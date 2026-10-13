// =============================================================================
// lock_manager.h — Row-level shared/exclusive locks with wait-die.
//
// Strict two-phase locking (strict 2PL)
//   Growing phase : a transaction acquires locks as it reads (S) and writes (X).
//   Shrinking     : it releases them ALL only at commit/abort.
//   Holding X locks until the end prevents other transactions from reading
//   uncommitted data (no dirty reads) and gives conflict-serializable schedules
//   for the rows touched.
//
// Compatibility:        held S   held X
//            request S    OK      conflict
//            request X  conflict  conflict
//
// Deadlock PREVENTION with wait-die (transaction id = start timestamp):
//   if the requester is OLDER than every conflicting holder -> it WAITS
//   otherwise (it is younger)                                -> it DIES (aborts)
// Waits only ever go old -> young, so a cycle in the waits-for graph is
// impossible, hence no deadlock and no need for a detector thread.
//
// Locks are NOT latches: locks protect logical data for a whole transaction;
// latches (std::mutex) protect in-memory structures for a few instructions.
// We never block on a lock while holding a latch.
// =============================================================================
#pragma once

#include <condition_variable>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include "concurrency/transaction.h"

namespace coredb {

class LockManager {
public:
    // Block (or die) until `txn` holds at least a shared lock on `rid`.
    void lockShared(Transaction& txn, const Rid& rid);

    // Block (or die) until `txn` holds an exclusive lock on `rid`.
    // Upgrades S -> X if the transaction already holds S.
    void lockExclusive(Transaction& txn, const Rid& rid);

    // Non-blocking X lock: returns false instead of waiting. Used by INSERT to
    // skip slots that are still locked by an in-flight deleter.
    bool tryLockExclusive(Transaction& txn, const Rid& rid);

    // Releases every lock held by `txn` and wakes up waiters.
    void unlockAll(Transaction& txn);

private:
    struct LockState {
        std::unordered_set<txn_id_t> shared;  // holders of S
        txn_id_t exclusive = NO_TXN;          // holder of X (at most one)
    };

    // Returns the ids of transactions (other than `me`) that block the request.
    static std::vector<txn_id_t> conflicts(const LockState& s, txn_id_t me, bool want_exclusive);

    // Wait-die decision: wait while older than all conflicting holders, else die.
    void waitOrDie(std::unique_lock<std::mutex>& lock, Transaction& txn,
                   const std::vector<txn_id_t>& blockers, const Rid& rid);

    std::mutex mutex_;
    std::condition_variable cv_;  // Signalled whenever any lock is released.
    std::unordered_map<Rid, LockState, RidHash> table_;
};

}  // namespace coredb
