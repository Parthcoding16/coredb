// =============================================================================
// database.h — Owns and wires together every engine component.
//
//   Database("app.db") opens/creates two files:
//     app.db      data pages (page 0 = catalog, then heap and index pages)
//     app.db.wal  write-ahead log
//
// Startup sequence
//   1. open files; if the data file is empty, format the catalog page
//   2. run crash recovery if the log is non-empty (it is empty after a clean
//      shutdown, because shutdown takes a checkpoint)
//   3. load the catalog; if recovery ran, rebuild the indexes and checkpoint
//
// Components are held in unique_ptrs and destroyed in reverse declaration
// order, so nothing outlives what it points to.
// =============================================================================
#pragma once

#include <memory>
#include <string>

#include "catalog/catalog.h"
#include "concurrency/transaction_manager.h"
#include "recovery/recovery_manager.h"

namespace coredb {

struct Options {
    std::size_t pool_frames = DEFAULT_POOL_FRAMES;
};

class Database {
public:
    explicit Database(const std::string& path, Options options = {});
    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    Catalog&            catalog()      { return *catalog_; }
    TransactionManager& transactions() { return *txns_; }
    LockManager&        locks()        { return *locks_; }
    BufferPoolManager&  bufferPool()   { return *bpm_; }
    LogManager&         log()          { return *log_; }
    DiskManager&        disk()         { return *disk_; }
    const RecoveryStats& recoveryStats() const { return recovery_; }

    // Flushes every dirty page, fsyncs, then truncates the log. Requires that
    // no transaction is active (a "quiescent" checkpoint).
    void checkpoint();

    // Testing/demo hook: the destructor will skip the shutdown checkpoint, so
    // unflushed log records and dirty pages are lost exactly as in a crash.
    void simulateCrash() { crashed_ = true; }

private:
    bool crashed_ = false;
    std::unique_ptr<DiskManager> disk_;
    std::unique_ptr<LogManager> log_;
    std::unique_ptr<BufferPoolManager> bpm_;
    std::unique_ptr<LockManager> locks_;
    std::unique_ptr<Catalog> catalog_;
    std::unique_ptr<TransactionManager> txns_;
    RecoveryStats recovery_;
};

}  // namespace coredb
