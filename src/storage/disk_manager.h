// =============================================================================
// disk_manager.h — Raw page I/O against a single data file.
//
// The disk manager knows nothing about what a page contains. It only moves
// fixed-size 4 KB blocks between the file and caller-provided memory, using
// POSIX pread/pwrite (positional I/O: no shared file offset, so concurrent
// callers never race on lseek) and fsync for durability.
// =============================================================================
#pragma once

#include <atomic>
#include <mutex>
#include <string>

#include "common/config.h"

namespace coredb {

class DiskManager {
public:
    explicit DiskManager(const std::string& path);
    ~DiskManager();

    DiskManager(const DiskManager&) = delete;             // Owns a file descriptor:
    DiskManager& operator=(const DiskManager&) = delete;  // copying would double-close it.

    // Reads page `pid` into `out` (PAGE_SIZE bytes). Bytes past EOF read as 0.
    void readPage(page_id_t pid, char* out);

    // Writes PAGE_SIZE bytes from `in` to page `pid`. Goes to the OS page cache;
    // call sync() to force it onto stable storage.
    void writePage(page_id_t pid, const char* in);

    // Extends the file by one zeroed page and returns its id. Extending eagerly
    // means a page id is never handed out twice, even across a crash.
    page_id_t allocatePage();

    // fsync(): blocks until all written pages are durable.
    void sync();

    page_id_t numPages() const { return num_pages_.load(); }
    uint64_t  reads()  const { return reads_.load(); }
    uint64_t  writes() const { return writes_.load(); }

private:
    int fd_ = -1;
    std::mutex alloc_mutex_;                 // Serialises allocatePage().
    std::atomic<page_id_t> num_pages_{0};
    std::atomic<uint64_t> reads_{0}, writes_{0};
};

}  // namespace coredb
