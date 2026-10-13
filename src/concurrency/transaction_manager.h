// =============================================================================
// transaction_manager.h — BEGIN / COMMIT / ABORT.
//
// COMMIT
//   1. append COMMIT to the log and fsync it  <- the durability point:
//      once this returns, the transaction survives any crash, even though its
//      data pages may still be only in memory ("no-force" policy)
//   2. apply postponed index deletions
//   3. release all locks (end of strict 2PL)
//
// ABORT (rollback)
//   1. walk the write set BACKWARDS, restoring each before-image; every
//      restore is itself logged, so a crash later replays the rollback too
//   2. remove index entries of rows this transaction inserted
//   3. log ABORT and release all locks
// =============================================================================
#pragma once

#include <atomic>
#include <memory>

#include "concurrency/lock_manager.h"
#include "recovery/log_manager.h"

namespace coredb {

class TransactionManager {
public:
    TransactionManager(LogManager* log, LockManager* locks, txn_id_t first_id = 1)
        : log_(log), locks_(locks), next_id_(first_id) {}

    std::unique_ptr<Transaction> begin();
    void commit(Transaction& txn);
    void abort(Transaction& txn);

    int activeCount() const { return active_.load(); }

private:
    LogManager* log_;
    LockManager* locks_;
    std::atomic<txn_id_t> next_id_;
    std::atomic<int> active_{0};
};

}  // namespace coredb
