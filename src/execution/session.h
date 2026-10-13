// =============================================================================
// session.h — One client connection: parses SQL, plans, and executes it.
//
// Transactions
//   * Without BEGIN, every statement runs in its own transaction (autocommit).
//   * Inside BEGIN ... COMMIT, any error rolls back the WHOLE transaction
//     (PostgreSQL behaves the same way), so the database never holds a
//     half-applied multi-statement change.
//
// Planner (rule-based)
//   If the WHERE clause constrains the primary key (=, <, <=, >, >=), use the
//   B+ tree: an IndexPoint lookup for equality, an IndexRange scan otherwise.
//   Else fall back to a SeqScan of the heap. In every case all WHERE
//   conditions are re-checked on the fetched row (a residual filter), which
//   also discards stale index entries.
//
// Locking discipline (never wait for a lock while holding a latch)
//   SELECT : S lock each candidate row, then read it.
//   UPDATE / DELETE : X lock each candidate row, then read and modify it.
//   INSERT : the heap X-locks the new slot.
// =============================================================================
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "database.h"
#include "sql/ast.h"

namespace coredb {

struct ResultSet {
    std::vector<std::string> columns;
    std::vector<std::vector<std::string>> rows;
    std::string message;  // e.g. "INSERT 3", "COMMIT"
};

// How the executor will find candidate rows.
struct ScanPlan {
    enum class Kind { SeqScan, IndexPoint, IndexRange };
    Kind kind = Kind::SeqScan;
    int64_t lo = 0, hi = 0;  // inclusive key bounds for index plans
    bool empty = false;      // contradictory bounds, e.g. id > 5 AND id < 3

    std::string describe(const TableInfo& table) const;
};

// Exposed for tests and EXPLAIN.
ScanPlan planScan(const TableInfo& table, const std::vector<Condition>& where);

class Session {
public:
    explicit Session(Database& db) : db_(db) {}
    ~Session();  // rolls back an open transaction

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // Parses and runs one statement. Throws DbException on error; a
    // TransactionAbortException means the transaction was rolled back by
    // wait-die and may be retried.
    ResultSet execute(const std::string& sql);

    bool inTransaction() const { return txn_ != nullptr; }

    // Testing hook: forget the open transaction WITHOUT rolling it back, as a
    // crash would. Only call right before Database::simulateCrash().
    void abandonTransactionForCrashTest() { txn_.reset(); }

private:
    ResultSet run(const Statement& stmt, Transaction& txn);
    ResultSet runCreate(const CreateTableStmt& s);
    ResultSet runInsert(const InsertStmt& s, Transaction& txn);
    ResultSet runSelect(const SelectStmt& s, Transaction& txn);
    ResultSet runDelete(const DeleteStmt& s, Transaction& txn);
    ResultSet runUpdate(const UpdateStmt& s, Transaction& txn);
    ResultSet runExplain(const ExplainStmt& s);

    TableInfo& table(const std::string& name);
    std::vector<Rid> candidates(TableInfo& table, const ScanPlan& plan);

    Database& db_;
    std::unique_ptr<Transaction> txn_;  // explicit transaction, if any
};

}  // namespace coredb
