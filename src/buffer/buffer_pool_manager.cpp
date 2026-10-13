#include "buffer/buffer_pool_manager.h"

#include "recovery/log_manager.h"

namespace coredb {

// ---- PageGuard ---------------------------------------------------------------

PageGuard& PageGuard::operator=(PageGuard&& other) noexcept {
    if (this != &other) {
        release();
        bpm_ = other.bpm_;
        page_ = other.page_;
        dirty_ = other.dirty_;
        other.bpm_ = nullptr;
        other.page_ = nullptr;
        other.dirty_ = false;
    }
    return *this;
}

void PageGuard::release() {
    if (page_ != nullptr) {
        bpm_->unpin(page_->id(), dirty_);
        page_ = nullptr;
        bpm_ = nullptr;
        dirty_ = false;
    }
}

// ---- BufferPoolManager ---------------------------------------------------------

BufferPoolManager::BufferPoolManager(std::size_t pool_size, DiskManager* disk, LogManager* log)
    : frames_(pool_size), disk_(disk), log_(log) {
    for (std::size_t i = 0; i < pool_size; ++i) free_list_.push_back(static_cast<frame_id_t>(i));
}

PageGuard BufferPoolManager::fetchPage(page_id_t pid) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Hit: page already resident. Pin it and remove it from eviction candidates.
    if (auto it = page_table_.find(pid); it != page_table_.end()) {
        Page& page = frames_[it->second];
        ++page.pin_count_;
        replacer_.pin(it->second);
        ++hits_;
        return PageGuard(this, &page);
    }

    // Miss: bring it in from disk.
    ++misses_;
    frame_id_t frame = acquireFrame();
    Page& page = frames_[frame];
    disk_->readPage(pid, page.data_);
    page.page_id_ = pid;
    page.pin_count_ = 1;
    page.is_dirty_ = false;
    page_table_[pid] = frame;
    return PageGuard(this, &page);
}

PageGuard BufferPoolManager::newPage() {
    std::lock_guard<std::mutex> lock(mutex_);
    frame_id_t frame = acquireFrame();
    page_id_t pid = disk_->allocatePage();
    Page& page = frames_[frame];
    page.reset();
    page.page_id_ = pid;
    page.pin_count_ = 1;
    page_table_[pid] = frame;
    return PageGuard(this, &page);
}

void BufferPoolManager::unpin(page_id_t pid, bool dirty) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = page_table_.find(pid);
    if (it == page_table_.end()) return;
    Page& page = frames_[it->second];
    if (dirty) page.is_dirty_ = true;
    if (page.pin_count_ > 0 && --page.pin_count_ == 0) replacer_.unpin(it->second);
}

frame_id_t BufferPoolManager::acquireFrame() {
    if (!free_list_.empty()) {
        frame_id_t frame = free_list_.front();
        free_list_.pop_front();
        return frame;
    }
    auto victim = replacer_.victim();
    if (!victim) {
        throw DbException("buffer pool exhausted: all " + std::to_string(frames_.size()) +
                          " frames are pinned");
    }
    Page& old = frames_[*victim];
    if (old.is_dirty_) writeBack(old);   // "Steal": may write uncommitted data (undo via WAL).
    page_table_.erase(old.page_id_);
    old.reset();
    return *victim;
}

void BufferPoolManager::writeBack(Page& page) {
    // Write-ahead logging rule: the log record describing the newest change on
    // this page must be durable BEFORE the page itself reaches disk.
    if (log_ != nullptr && page.lsn() != NO_LSN) log_->flushUntil(page.lsn());
    disk_->writePage(page.page_id_, page.data_);
    page.is_dirty_ = false;
}

void BufferPoolManager::flushPage(page_id_t pid) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = page_table_.find(pid);
    if (it == page_table_.end()) return;
    Page& page = frames_[it->second];
    if (page.is_dirty_) writeBack(page);
}

void BufferPoolManager::flushAll() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [pid, frame] : page_table_) {
        if (frames_[frame].is_dirty_) writeBack(frames_[frame]);
    }
}

}  // namespace coredb
