// =============================================================================
// table_heap.h — Stores a table's rows in a linked list of TablePages.
//
// Every physical change to a slot goes through writeSlot(), which
//   1. appends a WRITE log record (before + after image)  <- WAL first
//   2. applies the change to the page and stamps the page LSN
//   3. remembers the change in the transaction's write set (for rollback)
//
// Structural changes (adding a page to the chain) are NOT logged; instead they
// are forced to disk immediately (write + fsync). They are rare, and a page
// that ends up linked but unused after a crash is harmless empty space.
//
// Concurrency: a per-table latch (std::mutex) protects the physical page
// contents for the few microseconds of each operation. Row LOCKS, which last
// for the whole transaction, are taken by the executor BEFORE calling in here,
// so this class never blocks on a lock while holding its latch.
// =============================================================================
#pragma once

#include <mutex>
#include <optional>
#include <set>
#include <vector>

#include "buffer/buffer_pool_manager.h"
#include "catalog/schema.h"
#include "concurrency/lock_manager.h"
#include "recovery/log_manager.h"
#include "storage/table_page.h"

namespace coredb {

class TableHeap {
public:
    // Allocates and durably formats the first page of a new, empty heap.
    static page_id_t createFirstPage(BufferPoolManager& bpm, DiskManager& disk,
                                     std::size_t record_size);

    TableHeap(TableInfo* owner, const Schema* schema, page_id_t first_page,
              BufferPoolManager* bpm, DiskManager* disk, LogManager* log, LockManager* locks);

    // Inserts into a free slot and returns its RID. The new RID is X-locked
    // for `txn` (a slot still locked by another transaction is skipped).
    Rid insertTuple(const Tuple& tuple, Transaction& txn);

    // Caller must already hold an X lock on `rid`.
    void deleteTuple(const Rid& rid, Transaction& txn);
    void updateTuple(const Rid& rid, const Tuple& tuple, Transaction& txn);

    // Reads a row; nullopt if the slot is empty.
    std::optional<Tuple> getTuple(const Rid& rid);

    // Snapshot of every currently occupied RID (sequential scan source).
    std::vector<Rid> scanRids();

    // Rollback helper: puts `image` back into the slot. Logged (so a later
    // crash replays the rollback too) but not added to the write set.
    void restoreImage(const Rid& rid, const std::vector<char>& image, Transaction& txn);

    // Builds slot images: [used flag][record bytes].
    std::vector<char> imageOf(const Tuple& tuple) const;
    std::vector<char> emptyImage() const { return std::vector<char>(1 + schema_->recordSize(), 0); }

    page_id_t firstPageId() const { return first_page_; }
    std::size_t numPages() const { return pages_.size(); }

private:
    void writeSlot(const Rid& rid, const std::vector<char>& after, Transaction& txn,
                   bool add_to_write_set);
    page_id_t appendPage();  // latch held

    TableInfo* owner_;
    const Schema* schema_;
    page_id_t first_page_;
    BufferPoolManager* bpm_;
    DiskManager* disk_;
    LogManager* log_;
    LockManager* locks_;

    std::mutex latch_;
    std::vector<page_id_t> pages_;        // The chain, in order.
    std::set<page_id_t> has_free_slots_;  // Hint: pages that may have room.
};

}  // namespace coredb
