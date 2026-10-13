#include "concurrency/lock_manager.h"

#include <algorithm>

namespace coredb {

std::vector<txn_id_t> LockManager::conflicts(const LockState& s, txn_id_t me, bool want_exclusive) {
    std::vector<txn_id_t> blockers;
    if (s.exclusive != NO_TXN && s.exclusive != me) blockers.push_back(s.exclusive);
    if (want_exclusive) {
        for (txn_id_t holder : s.shared) {
            if (holder != me) blockers.push_back(holder);
        }
    }
    return blockers;
}

void LockManager::waitOrDie(std::unique_lock<std::mutex>& lock, Transaction& txn,
                            const std::vector<txn_id_t>& blockers, const Rid& rid) {
    const txn_id_t oldest_blocker =
        *std::min_element(blockers.begin(), blockers.end());
    // Smaller id == started earlier == older.
    if (txn.id() < oldest_blocker) {
        cv_.wait(lock);  // Older than every holder: allowed to wait.
        return;
    }
    throw TransactionAbortException("transaction " + std::to_string(txn.id()) +
                                    " aborted by wait-die on row " + rid.toString() +
                                    " (younger than lock holder " +
                                    std::to_string(oldest_blocker) + ")");
}

void LockManager::lockShared(Transaction& txn, const Rid& rid) {
    std::unique_lock<std::mutex> lock(mutex_);
    while (true) {
        LockState& s = table_[rid];  // Re-lookup each loop: the map may change while we wait.
        if (s.exclusive == txn.id() || s.shared.count(txn.id())) return;  // Already covered.
        auto blockers = conflicts(s, txn.id(), /*want_exclusive=*/false);
        if (blockers.empty()) {
            s.shared.insert(txn.id());
            txn.sharedLocks().insert(rid);
            return;
        }
        waitOrDie(lock, txn, blockers, rid);
    }
}

void LockManager::lockExclusive(Transaction& txn, const Rid& rid) {
    std::unique_lock<std::mutex> lock(mutex_);
    while (true) {
        LockState& s = table_[rid];
        if (s.exclusive == txn.id()) return;
        auto blockers = conflicts(s, txn.id(), /*want_exclusive=*/true);
        if (blockers.empty()) {
            s.shared.erase(txn.id());  // Upgrade S -> X if we held S.
            s.exclusive = txn.id();
            txn.sharedLocks().erase(rid);
            txn.exclusiveLocks().insert(rid);
            return;
        }
        waitOrDie(lock, txn, blockers, rid);
    }
}

bool LockManager::tryLockExclusive(Transaction& txn, const Rid& rid) {
    std::lock_guard<std::mutex> lock(mutex_);
    LockState& s = table_[rid];
    if (s.exclusive == txn.id()) return true;
    if (!conflicts(s, txn.id(), /*want_exclusive=*/true).empty()) {
        if (s.exclusive == NO_TXN && s.shared.empty()) table_.erase(rid);
        return false;
    }
    s.shared.erase(txn.id());
    s.exclusive = txn.id();
    txn.sharedLocks().erase(rid);
    txn.exclusiveLocks().insert(rid);
    return true;
}

void LockManager::unlockAll(Transaction& txn) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto release = [&](const Rid& rid) {
            auto it = table_.find(rid);
            if (it == table_.end()) return;
            it->second.shared.erase(txn.id());
            if (it->second.exclusive == txn.id()) it->second.exclusive = NO_TXN;
            if (it->second.shared.empty() && it->second.exclusive == NO_TXN) table_.erase(it);
        };
        for (const Rid& rid : txn.sharedLocks()) release(rid);
        for (const Rid& rid : txn.exclusiveLocks()) release(rid);
        txn.sharedLocks().clear();
        txn.exclusiveLocks().clear();
    }
    cv_.notify_all();  // Waiters re-check their conditions.
}

}  // namespace coredb
