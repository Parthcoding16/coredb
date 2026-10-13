// =============================================================================
// log_manager.h — The write-ahead log (WAL).
//
// Every change to a table page is first described by a log record. Records are
// appended to an in-memory buffer and forced to the log file with fsync when:
//   * a transaction commits   (durability: COMMIT is on disk before we say OK)
//   * a dirty page is evicted (WAL rule, enforced by the buffer pool)
//
// Log file layout
//   [ 8 bytes : base LSN ] [ record ] [ record ] ...
//   record = [u32 payload size][u32 checksum][payload]
//   payload = lsn, txn id, type, and for WRITE records the slot's
//             before-image and after-image (physical logging).
//
// The checksum lets recovery detect a torn final record (the machine died in
// the middle of a write) and stop reading there instead of replaying garbage.
// The base LSN keeps LSNs strictly increasing across log truncations, which
// matters because page LSNs on disk are compared against record LSNs.
// =============================================================================
#pragma once

#include <mutex>
#include <string>
#include <vector>

#include "common/config.h"

namespace coredb {

enum class LogType : uint8_t {
    Begin  = 1,
    Commit = 2,
    Abort  = 3,
    Write  = 4,  // Physical change to one tuple slot: before -> after.
};

struct LogRecord {
    lsn_t    lsn  = NO_LSN;
    txn_id_t txn  = NO_TXN;
    LogType  type = LogType::Begin;

    // Only meaningful for LogType::Write.
    Rid rid;
    std::vector<char> before;  // Slot contents before the change (for UNDO).
    std::vector<char> after;   // Slot contents after the change  (for REDO).
};

class LogManager {
public:
    explicit LogManager(const std::string& path);
    ~LogManager();

    LogManager(const LogManager&) = delete;
    LogManager& operator=(const LogManager&) = delete;

    // Assigns the next LSN to `rec`, buffers it, and returns the LSN.
    // Not durable until a flush covers it.
    lsn_t append(LogRecord& rec);

    // Makes every record with lsn <= `lsn` durable (write + fsync).
    void flushUntil(lsn_t lsn);
    void flushAll();

    // Reads every intact record from the log file (used by recovery).
    std::vector<LogRecord> readAll() const;

    // Discards the log (after a checkpoint has made all pages durable).
    // The next LSN continues from where it left off.
    void truncate();

    lsn_t    nextLsn() const;
    uint64_t flushCount() const { return flushes_; }

private:
    void flushLocked();  // mutex_ must be held

    int fd_ = -1;
    mutable std::mutex mutex_;
    std::vector<char> buffer_;     // Appended but not yet written records.
    lsn_t next_lsn_ = 1;
    lsn_t durable_lsn_ = 0;        // Highest LSN known to be on disk.
    uint64_t flushes_ = 0;
};

}  // namespace coredb
