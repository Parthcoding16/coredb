#include "storage/disk_manager.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace coredb {

namespace {
[[noreturn]] void ioFail(const std::string& what) {
    throw DbException("I/O error: " + what + ": " + std::strerror(errno));
}
}  // namespace

DiskManager::DiskManager(const std::string& path) {
    fd_ = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd_ < 0) ioFail("open " + path);

    struct stat st {};
    if (::fstat(fd_, &st) != 0) ioFail("fstat");
    num_pages_ = static_cast<page_id_t>(st.st_size / PAGE_SIZE);
}

DiskManager::~DiskManager() {
    if (fd_ >= 0) ::close(fd_);
}

void DiskManager::readPage(page_id_t pid, char* out) {
    const off_t offset = static_cast<off_t>(pid) * PAGE_SIZE;
    ssize_t n = ::pread(fd_, out, PAGE_SIZE, offset);
    if (n < 0) ioFail("pread");
    // A short read means the page lies (partly) beyond EOF: treat as zeroes.
    if (static_cast<std::size_t>(n) < PAGE_SIZE) std::memset(out + n, 0, PAGE_SIZE - n);
    ++reads_;
}

void DiskManager::writePage(page_id_t pid, const char* in) {
    const off_t offset = static_cast<off_t>(pid) * PAGE_SIZE;
    ssize_t n = ::pwrite(fd_, in, PAGE_SIZE, offset);
    if (n != static_cast<ssize_t>(PAGE_SIZE)) ioFail("pwrite");
    ++writes_;
}

page_id_t DiskManager::allocatePage() {
    std::lock_guard<std::mutex> lock(alloc_mutex_);
    const page_id_t pid = num_pages_.load();
    char zeros[PAGE_SIZE] = {};
    writePage(pid, zeros);
    num_pages_ = pid + 1;
    return pid;
}

void DiskManager::sync() {
    if (::fsync(fd_) != 0) ioFail("fsync");
}

}  // namespace coredb
