// =============================================================================
// buffer_pool_manager.h — Caches disk pages in a fixed number of memory frames.
//
// Upper layers never touch the disk directly. They ask the pool for a page;
// the pool returns it from memory (hit) or reads it into a free frame (miss),
// evicting the least-recently-used unpinned page if the pool is full.
//
// Key invariants
//   * A pinned page (pin_count > 0) is never evicted.
//   * A dirty page is written back before its frame is reused.
//   * WAL rule: before a dirty page is written, the log is flushed at least up
//     to that page's LSN. Otherwise a crash could leave on disk a change whose
//     log record (needed to undo it) was lost.
//
// Pages are handed out wrapped in a PageGuard (RAII): the guard unpins the page
// in its destructor, so a forgotten unpin — the classic buffer-pool leak — is
// impossible, even when an exception unwinds the stack.
// =============================================================================
#pragma once

#include <atomic>
#include <list>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "buffer/lru_replacer.h"
#include "storage/disk_manager.h"
#include "storage/page.h"

namespace coredb {

class LogManager;
class BufferPoolManager;

// Move-only RAII handle to a pinned page.
class PageGuard {
public:
    PageGuard() = default;
    PageGuard(BufferPoolManager* bpm, Page* page) : bpm_(bpm), page_(page) {}
    ~PageGuard() { release(); }

    PageGuard(const PageGuard&) = delete;
    PageGuard& operator=(const PageGuard&) = delete;
    PageGuard(PageGuard&& other) noexcept { *this = std::move(other); }
    PageGuard& operator=(PageGuard&& other) noexcept;

    Page*       page()       { return page_; }
    char*       data()       { return page_->data(); }
    const char* data() const { return page_->data(); }
    page_id_t   id()   const { return page_->id(); }

    // Call after modifying the page so the pool knows to write it back.
    void markDirty() { dirty_ = true; }

    // Unpins early (normally the destructor does this).
    void release();

private:
    BufferPoolManager* bpm_ = nullptr;
    Page* page_ = nullptr;
    bool dirty_ = false;
};

class BufferPoolManager {
public:
    // `log` may be null (e.g. in unit tests that do not need WAL).
    BufferPoolManager(std::size_t pool_size, DiskManager* disk, LogManager* log);

    // Returns the page, pinned. Throws if every frame is pinned.
    PageGuard fetchPage(page_id_t pid);

    // Allocates a brand-new zeroed page on disk and returns it, pinned.
    PageGuard newPage();

    // Writes one page to disk if it is resident (respecting the WAL rule).
    void flushPage(page_id_t pid);

    // Writes every dirty resident page to disk (used by checkpoints).
    void flushAll();

    // Statistics for the REPL's .stats command.
    uint64_t hits()   const { return hits_.load(); }
    uint64_t misses() const { return misses_.load(); }
    std::size_t poolSize() const { return frames_.size(); }

private:
    friend class PageGuard;
    void unpin(page_id_t pid, bool dirty);

    // Finds a frame to (re)use: free list first, else an LRU victim.
    // Must be called with mutex_ held. Writes the victim back if dirty.
    frame_id_t acquireFrame();

    void writeBack(Page& page);  // WAL-safe write of one frame (mutex_ held).

    std::vector<Page> frames_;
    std::unordered_map<page_id_t, frame_id_t> page_table_;  // resident pages
    std::list<frame_id_t> free_list_;                        // never-used frames
    LruReplacer replacer_;
    DiskManager* disk_;
    LogManager* log_;
    std::mutex mutex_;  // One latch protects all pool metadata.
    std::atomic<uint64_t> hits_{0}, misses_{0};
};

}  // namespace coredb
