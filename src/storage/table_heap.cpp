#include "storage/table_heap.h"

namespace coredb {

page_id_t TableHeap::createFirstPage(BufferPoolManager& bpm, DiskManager& disk,
                                     std::size_t record_size) {
    PageGuard guard = bpm.newPage();
    TablePage(guard.data()).init(static_cast<uint16_t>(record_size));
    guard.markDirty();
    page_id_t pid = guard.id();
    guard.release();
    bpm.flushPage(pid);  // Structural change: forced, not logged.
    disk.sync();
    return pid;
}

TableHeap::TableHeap(TableInfo* owner, const Schema* schema, page_id_t first_page,
                     BufferPoolManager* bpm, DiskManager* disk, LogManager* log, LockManager* locks)
    : owner_(owner), schema_(schema), first_page_(first_page),
      bpm_(bpm), disk_(disk), log_(log), locks_(locks) {
    // Walk the chain once to learn the page list and which pages have room.
    for (page_id_t pid = first_page_; pid != INVALID_PAGE_ID;) {
        PageGuard guard = bpm_->fetchPage(pid);
        TablePage tp(guard.data());
        pages_.push_back(pid);
        for (uint16_t s = 0; s < tp.numSlots(); ++s) {
            if (!tp.isUsed(s)) { has_free_slots_.insert(pid); break; }
        }
        pid = tp.nextPageId();
    }
}

std::vector<char> TableHeap::imageOf(const Tuple& tuple) const {
    std::vector<char> image(1 + schema_->recordSize());
    image[0] = 1;
    schema_->serialize(tuple, image.data() + 1);
    return image;
}

void TableHeap::writeSlot(const Rid& rid, const std::vector<char>& after, Transaction& txn,
                          bool add_to_write_set) {
    PageGuard guard = bpm_->fetchPage(rid.page_id);
    TablePage tp(guard.data());

    LogRecord rec;
    rec.txn = txn.id();
    rec.type = LogType::Write;
    rec.rid = rid;
    rec.before = tp.slotImage(rid.slot);
    rec.after = after;
    lsn_t lsn = log_->append(rec);          // 1. log first ...

    tp.writeSlotImage(rid.slot, after);     // 2. ... then change the page
    guard.page()->setLsn(lsn);
    guard.markDirty();

    if (add_to_write_set) {
        txn.writeSet().push_back({owner_, rid, std::move(rec.before), after});
    }
}

page_id_t TableHeap::appendPage() {
    PageGuard fresh = bpm_->newPage();
    TablePage(fresh.data()).init(static_cast<uint16_t>(schema_->recordSize()));
    fresh.markDirty();
    page_id_t new_pid = fresh.id();
    fresh.release();

    page_id_t last = pages_.back();
    {
        PageGuard tail = bpm_->fetchPage(last);
        TablePage(tail.data()).setNextPageId(new_pid);
        tail.markDirty();
    }
    // Force both pages so the chain is durable before anyone logs a change on
    // the new page; recovery can then always find it.
    bpm_->flushPage(new_pid);
    bpm_->flushPage(last);
    disk_->sync();

    pages_.push_back(new_pid);
    has_free_slots_.insert(new_pid);
    return new_pid;
}

Rid TableHeap::insertTuple(const Tuple& tuple, Transaction& txn) {
    std::lock_guard<std::mutex> latch(latch_);
    const std::vector<char> image = imageOf(tuple);

    auto tryPage = [&](page_id_t pid) -> std::optional<Rid> {
        PageGuard guard = bpm_->fetchPage(pid);
        TablePage tp(guard.data());
        bool any_free = false;
        for (uint16_t s = 0; s < tp.numSlots(); ++s) {
            if (tp.isUsed(s)) continue;
            any_free = true;
            Rid rid{pid, s};
            // An empty slot may still be X-locked by a transaction that deleted
            // its row and has not committed; reusing it would break that
            // transaction's rollback. tryLock skips such slots without waiting.
            if (locks_->tryLockExclusive(txn, rid)) return rid;
        }
        if (!any_free) has_free_slots_.erase(pid);
        return std::nullopt;
    };

    std::optional<Rid> rid;
    for (auto it = has_free_slots_.begin(); it != has_free_slots_.end() && !rid;) {
        page_id_t pid = *it++;  // advance first: tryPage may erase pid
        rid = tryPage(pid);
    }
    if (!rid) rid = tryPage(appendPage());
    if (!rid) throw DbException("internal error: fresh page has no free slot");

    writeSlot(*rid, image, txn, /*add_to_write_set=*/true);
    return *rid;
}

void TableHeap::deleteTuple(const Rid& rid, Transaction& txn) {
    std::lock_guard<std::mutex> latch(latch_);
    writeSlot(rid, emptyImage(), txn, true);
    has_free_slots_.insert(rid.page_id);
}

void TableHeap::updateTuple(const Rid& rid, const Tuple& tuple, Transaction& txn) {
    std::lock_guard<std::mutex> latch(latch_);
    writeSlot(rid, imageOf(tuple), txn, true);
}

void TableHeap::restoreImage(const Rid& rid, const std::vector<char>& image, Transaction& txn) {
    std::lock_guard<std::mutex> latch(latch_);
    writeSlot(rid, image, txn, /*add_to_write_set=*/false);
    if (image[0] == 0) has_free_slots_.insert(rid.page_id);
}

std::optional<Tuple> TableHeap::getTuple(const Rid& rid) {
    std::lock_guard<std::mutex> latch(latch_);
    PageGuard guard = bpm_->fetchPage(rid.page_id);
    TablePage tp(guard.data());
    if (rid.slot >= tp.numSlots() || !tp.isUsed(rid.slot)) return std::nullopt;
    return schema_->deserialize(tp.record(rid.slot));
}

std::vector<Rid> TableHeap::scanRids() {
    std::lock_guard<std::mutex> latch(latch_);
    std::vector<Rid> rids;
    for (page_id_t pid : pages_) {
        PageGuard guard = bpm_->fetchPage(pid);
        TablePage tp(guard.data());
        for (uint16_t s = 0; s < tp.numSlots(); ++s) {
            if (tp.isUsed(s)) rids.push_back({pid, s});
        }
    }
    return rids;
}

}  // namespace coredb
