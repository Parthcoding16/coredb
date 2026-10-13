#include "recovery/log_manager.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace coredb {

namespace {

[[noreturn]] void ioFail(const std::string& what) {
    throw DbException("log I/O error: " + what + ": " + std::strerror(errno));
}

// FNV-1a: tiny, fast hash used as a checksum to detect torn records.
uint32_t checksum(const char* data, std::size_t n) {
    uint32_t h = 2166136261u;
    for (std::size_t i = 0; i < n; ++i) {
        h ^= static_cast<uint8_t>(data[i]);
        h *= 16777619u;
    }
    return h;
}

template <typename T>
void put(std::vector<char>& out, const T& v) {
    const char* p = reinterpret_cast<const char*>(&v);
    out.insert(out.end(), p, p + sizeof(T));
}

template <typename T>
bool get(const char*& p, const char* end, T& v) {
    if (end - p < static_cast<std::ptrdiff_t>(sizeof(T))) return false;
    std::memcpy(&v, p, sizeof(T));
    p += sizeof(T);
    return true;
}

std::vector<char> serializePayload(const LogRecord& r) {
    std::vector<char> out;
    put(out, r.lsn);
    put(out, r.txn);
    put(out, static_cast<uint8_t>(r.type));
    if (r.type == LogType::Write) {
        put(out, r.rid.page_id);
        put(out, r.rid.slot);
        put(out, static_cast<uint32_t>(r.before.size()));
        out.insert(out.end(), r.before.begin(), r.before.end());
        put(out, static_cast<uint32_t>(r.after.size()));
        out.insert(out.end(), r.after.begin(), r.after.end());
    }
    return out;
}

bool deserializePayload(const char* p, const char* end, LogRecord& r) {
    uint8_t type = 0;
    if (!get(p, end, r.lsn) || !get(p, end, r.txn) || !get(p, end, type)) return false;
    r.type = static_cast<LogType>(type);
    if (r.type == LogType::Write) {
        uint32_t n = 0;
        if (!get(p, end, r.rid.page_id) || !get(p, end, r.rid.slot) || !get(p, end, n)) return false;
        if (end - p < static_cast<std::ptrdiff_t>(n)) return false;
        r.before.assign(p, p + n);
        p += n;
        if (!get(p, end, n) || end - p < static_cast<std::ptrdiff_t>(n)) return false;
        r.after.assign(p, p + n);
    }
    return true;
}

constexpr off_t HEADER_SIZE = sizeof(lsn_t);

}  // namespace

LogManager::LogManager(const std::string& path) {
    fd_ = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd_ < 0) ioFail("open " + path);

    struct stat st {};
    if (::fstat(fd_, &st) != 0) ioFail("fstat");
    if (st.st_size < HEADER_SIZE) {
        // Fresh log: write the header (base LSN = 1).
        lsn_t base = 1;
        if (::pwrite(fd_, &base, sizeof(base), 0) != sizeof(base)) ioFail("write header");
        if (::fsync(fd_) != 0) ioFail("fsync");
        next_lsn_ = 1;
    } else {
        lsn_t base = 1;
        if (::pread(fd_, &base, sizeof(base), 0) != sizeof(base)) ioFail("read header");
        next_lsn_ = base;
        for (const LogRecord& r : readAll()) next_lsn_ = std::max(next_lsn_, r.lsn + 1);
    }
    durable_lsn_ = next_lsn_ - 1;
}

LogManager::~LogManager() {
    // Deliberately does NOT flush: an unflushed tail is exactly what a real
    // crash loses. Clean shutdown flushes explicitly via a checkpoint.
    if (fd_ >= 0) ::close(fd_);
}

lsn_t LogManager::append(LogRecord& rec) {
    std::lock_guard<std::mutex> lock(mutex_);
    rec.lsn = next_lsn_++;
    std::vector<char> payload = serializePayload(rec);
    put(buffer_, static_cast<uint32_t>(payload.size()));
    put(buffer_, checksum(payload.data(), payload.size()));
    buffer_.insert(buffer_.end(), payload.begin(), payload.end());
    return rec.lsn;
}

void LogManager::flushLocked() {
    if (buffer_.empty()) return;
    struct stat st {};
    if (::fstat(fd_, &st) != 0) ioFail("fstat");
    if (::pwrite(fd_, buffer_.data(), buffer_.size(), st.st_size) !=
        static_cast<ssize_t>(buffer_.size())) {
        ioFail("pwrite");
    }
    if (::fsync(fd_) != 0) ioFail("fsync");
    buffer_.clear();
    durable_lsn_ = next_lsn_ - 1;
    ++flushes_;
}

void LogManager::flushUntil(lsn_t lsn) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (lsn <= durable_lsn_) return;  // Already durable: nothing to do.
    flushLocked();
}

void LogManager::flushAll() {
    std::lock_guard<std::mutex> lock(mutex_);
    flushLocked();
}

std::vector<LogRecord> LogManager::readAll() const {
    std::vector<LogRecord> records;
    struct stat st {};
    if (::fstat(fd_, &st) != 0) ioFail("fstat");
    if (st.st_size <= HEADER_SIZE) return records;

    std::vector<char> bytes(st.st_size - HEADER_SIZE);
    if (::pread(fd_, bytes.data(), bytes.size(), HEADER_SIZE) != static_cast<ssize_t>(bytes.size())) {
        ioFail("pread");
    }
    const char* p = bytes.data();
    const char* end = p + bytes.size();
    while (true) {
        uint32_t size = 0, sum = 0;
        if (!get(p, end, size) || !get(p, end, sum)) break;
        if (end - p < static_cast<std::ptrdiff_t>(size)) break;  // torn tail
        if (checksum(p, size) != sum) break;                       // corrupt tail
        LogRecord r;
        if (!deserializePayload(p, p + size, r)) break;
        records.push_back(std::move(r));
        p += size;
    }
    return records;
}

void LogManager::truncate() {
    std::lock_guard<std::mutex> lock(mutex_);
    buffer_.clear();
    if (::ftruncate(fd_, 0) != 0) ioFail("ftruncate");
    lsn_t base = next_lsn_;  // Future LSNs keep increasing.
    if (::pwrite(fd_, &base, sizeof(base), 0) != sizeof(base)) ioFail("write header");
    if (::fsync(fd_) != 0) ioFail("fsync");
    durable_lsn_ = next_lsn_ - 1;
}

lsn_t LogManager::nextLsn() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return next_lsn_;
}

}  // namespace coredb
