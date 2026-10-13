// =============================================================================
// transaction.h — Per-transaction state.
//
// A transaction remembers:
//   * which RIDs it has locked (so commit/abort can release them all at once —
//     the "shrinking phase" of STRICT two-phase locking happens only at the end)
//   * its write set: before/after images of every slot it changed, so an
//     ABORT can undo them in reverse order without reading the log file
//   * index deletions it has postponed until commit (see executor.cpp)
// =============================================================================
#pragma once

#include <set>
#include <vector>

#include "common/config.h"

namespace coredb {

struct TableInfo;

enum class TxnState { Active, Committed, Aborted };

struct WriteRecord {
    TableInfo* table;
    Rid rid;
    std::vector<char> before;
    std::vector<char> after;
};

struct PendingIndexDelete {
    TableInfo* table;
    int64_t key;
    Rid rid;
};

class Transaction {
public:
    explicit Transaction(txn_id_t id) : id_(id) {}

    txn_id_t id() const { return id_; }
    TxnState state() const { return state_; }
    void setState(TxnState s) { state_ = s; }

    std::set<Rid>& sharedLocks()    { return shared_locks_; }
    std::set<Rid>& exclusiveLocks() { return exclusive_locks_; }
    std::vector<WriteRecord>& writeSet() { return write_set_; }
    std::vector<PendingIndexDelete>& pendingIndexDeletes() { return pending_index_deletes_; }

private:
    txn_id_t id_;
    TxnState state_ = TxnState::Active;
    std::set<Rid> shared_locks_;
    std::set<Rid> exclusive_locks_;
    std::vector<WriteRecord> write_set_;
    std::vector<PendingIndexDelete> pending_index_deletes_;
};

}  // namespace coredb
