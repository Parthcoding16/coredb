#include "execution/session.h"

#include <algorithm>
#include <limits>

#include "sql/parser.h"

namespace coredb {

namespace {

constexpr int64_t KEY_MIN = std::numeric_limits<int64_t>::min();
constexpr int64_t KEY_MAX = std::numeric_limits<int64_t>::max();

// Overload-set helper for std::visit with lambdas.
template <class... Ts> struct Overloaded : Ts... { using Ts::operator()...; };
template <class... Ts> Overloaded(Ts...) -> Overloaded<Ts...>;

const char* opText(CompareOp op) {
    switch (op) {
        case CompareOp::Eq: return "=";
        case CompareOp::Ne: return "!=";
        case CompareOp::Lt: return "<";
        case CompareOp::Le: return "<=";
        case CompareOp::Gt: return ">";
        case CompareOp::Ge: return ">=";
    }
    return "?";
}

// Checks that every WHERE column exists and is compared with the right type.
void validateWhere(const TableInfo& table, const std::vector<Condition>& where) {
    for (const Condition& c : where) {
        auto idx = table.schema.indexOf(c.column);
        if (!idx) throw DbException("unknown column '" + c.column + "' in table '" + table.name + "'");
        if (table.schema.column(*idx).type != c.value.type()) {
            throw DbException("type mismatch in WHERE for column '" + c.column + "'");
        }
    }
}

bool matches(const TableInfo& table, const Tuple& tuple, const std::vector<Condition>& where) {
    for (const Condition& c : where) {
        int cmp = tuple[*table.schema.indexOf(c.column)].compare(c.value);
        bool ok = false;
        switch (c.op) {
            case CompareOp::Eq: ok = cmp == 0; break;
            case CompareOp::Ne: ok = cmp != 0; break;
            case CompareOp::Lt: ok = cmp < 0; break;
            case CompareOp::Le: ok = cmp <= 0; break;
            case CompareOp::Gt: ok = cmp > 0; break;
            case CompareOp::Ge: ok = cmp >= 0; break;
        }
        if (!ok) return false;
    }
    return true;
}

std::string describeWhere(const std::vector<Condition>& where) {
    std::string s;
    for (std::size_t i = 0; i < where.size(); ++i) {
        if (i) s += " AND ";
        const Value& v = where[i].value;
        s += where[i].column + " " + opText(where[i].op) + " " +
             (v.type() == TypeId::Varchar ? "'" + v.asString() + "'" : v.toString());
    }
    return s;
}

}  // namespace

// ---- Planner ---------------------------------------------------------------------------

ScanPlan planScan(const TableInfo& table, const std::vector<Condition>& where) {
    ScanPlan plan;
    if (!table.hasIndex()) return plan;  // SeqScan

    const std::string& pk = table.schema.column(table.schema.pkColumn()).name;
    int64_t lo = KEY_MIN, hi = KEY_MAX;
    bool uses_key = false;
    for (const Condition& c : where) {
        if (c.column != pk || c.value.type() != TypeId::Int) continue;
        const int64_t v = c.value.asInt();
        switch (c.op) {
            case CompareOp::Eq: lo = std::max(lo, v); hi = std::min(hi, v); break;
            case CompareOp::Ge: lo = std::max(lo, v); break;
            case CompareOp::Le: hi = std::min(hi, v); break;
            case CompareOp::Gt: if (v == KEY_MAX) plan.empty = true; else lo = std::max(lo, v + 1); break;
            case CompareOp::Lt: if (v == KEY_MIN) plan.empty = true; else hi = std::min(hi, v - 1); break;
            case CompareOp::Ne: continue;  // a B+ tree cannot help with !=
        }
        uses_key = true;
    }
    if (!uses_key) return plan;
    plan.kind = (lo == hi) ? ScanPlan::Kind::IndexPoint : ScanPlan::Kind::IndexRange;
    plan.lo = lo;
    plan.hi = hi;
    if (lo > hi) plan.empty = true;
    return plan;
}

std::string ScanPlan::describe(const TableInfo& table) const {
    const std::string pk = table.hasIndex() ? table.schema.column(table.schema.pkColumn()).name : "";
    auto bound = [](int64_t v, const char* inf) { return v == KEY_MIN || v == KEY_MAX ? std::string(inf) : std::to_string(v); };
    switch (kind) {
        case Kind::SeqScan:
            return "Sequential Scan on " + table.name;
        case Kind::IndexPoint:
            return "Index Point Lookup on " + table.name + " using B+ tree (" + pk + " = " + std::to_string(lo) + ")";
        case Kind::IndexRange:
            return "Index Range Scan on " + table.name + " using B+ tree (" + bound(lo, "-inf") +
                   " <= " + pk + " <= " + bound(hi, "+inf") + ")";
    }
    return "";
}

// ---- Session -----------------------------------------------------------------------------

Session::~Session() {
    if (txn_) {
        try { db_.transactions().abort(*txn_); } catch (...) {}
    }
}

TableInfo& Session::table(const std::string& name) {
    TableInfo* t = db_.catalog().getTable(name);
    if (!t) throw DbException("no such table: '" + name + "'");
    return *t;
}

std::vector<Rid> Session::candidates(TableInfo& table, const ScanPlan& plan) {
    if (plan.empty) return {};
    if (plan.kind == ScanPlan::Kind::SeqScan) return table.heap->scanRids();
    std::vector<Rid> rids;
    for (const auto& [key, rid] : table.index->rangeScan(plan.lo, plan.hi)) rids.push_back(rid);
    return rids;
}

ResultSet Session::execute(const std::string& sql) {
    Statement stmt = Parser(sql).parse();
    TransactionManager& tm = db_.transactions();

    // Transaction control and DDL are handled up front.
    if (std::holds_alternative<BeginStmt>(stmt)) {
        if (txn_) throw DbException("a transaction is already in progress");
        txn_ = tm.begin();
        return {{}, {}, "BEGIN (transaction " + std::to_string(txn_->id()) + ")"};
    }
    if (std::holds_alternative<CommitStmt>(stmt)) {
        if (!txn_) throw DbException("no transaction in progress");
        tm.commit(*txn_);
        txn_.reset();
        return {{}, {}, "COMMIT"};
    }
    if (std::holds_alternative<RollbackStmt>(stmt)) {
        if (!txn_) throw DbException("no transaction in progress");
        tm.abort(*txn_);
        txn_.reset();
        return {{}, {}, "ROLLBACK"};
    }
    if (auto* create = std::get_if<CreateTableStmt>(&stmt)) {
        if (txn_) throw DbException("CREATE TABLE is not allowed inside a transaction");
        return runCreate(*create);
    }
    if (auto* explain = std::get_if<ExplainStmt>(&stmt)) return runExplain(*explain);

    // DML: run inside the open transaction, or in a fresh autocommit one.
    std::unique_ptr<Transaction> autocommit;
    Transaction* txn = txn_.get();
    if (!txn) {
        autocommit = tm.begin();
        txn = autocommit.get();
    }
    try {
        ResultSet result = run(stmt, *txn);
        if (autocommit) tm.commit(*autocommit);
        return result;
    } catch (const TransactionAbortException&) {
        tm.abort(*txn);
        txn_.reset();
        throw;
    } catch (const DbException& e) {
        tm.abort(*txn);
        const bool was_explicit = txn_ != nullptr;
        txn_.reset();
        if (was_explicit) throw DbException(std::string(e.what()) + " (transaction rolled back)");
        throw;
    }
}

ResultSet Session::run(const Statement& stmt, Transaction& txn) {
    return std::visit(Overloaded{
        [&](const InsertStmt& s) { return runInsert(s, txn); },
        [&](const SelectStmt& s) { return runSelect(s, txn); },
        [&](const DeleteStmt& s) { return runDelete(s, txn); },
        [&](const UpdateStmt& s) { return runUpdate(s, txn); },
        [&](const auto&) -> ResultSet { throw DbException("internal error: unexpected statement"); },
    }, stmt);
}

ResultSet Session::runCreate(const CreateTableStmt& s) {
    std::vector<Column> columns;
    int pk = -1;
    for (const ColumnDef& def : s.columns) {
        for (const Column& c : columns) {
            if (c.name == def.name) throw DbException("duplicate column '" + def.name + "'");
        }
        if (def.primary_key) {
            if (pk != -1) throw DbException("only one PRIMARY KEY column is supported");
            if (def.type != TypeId::Int) throw DbException("PRIMARY KEY must be an INT column");
            pk = static_cast<int>(columns.size());
        }
        columns.push_back({def.name, def.type, def.length});
    }
    db_.catalog().createTable(s.table, columns, pk);
    return {{}, {}, "CREATE TABLE" + std::string(pk >= 0 ? " (with B+ tree index on primary key)" : "")};
}

ResultSet Session::runInsert(const InsertStmt& s, Transaction& txn) {
    TableInfo& t = table(s.table);
    LockManager& locks = db_.locks();
    for (const auto& values : s.rows) {
        Tuple tuple = t.schema.validate(values);
        int64_t key = 0;
        if (t.hasIndex()) {
            key = t.keyOf(tuple);
            if (auto existing = t.index->get(key)) {
                // The entry may belong to a live row (duplicate!) or be stale:
                // left behind by a committed delete, whose index removal is
                // deferred. Lock the row first so an in-flight delete is
                // resolved (committed or rolled back) before we decide.
                locks.lockShared(txn, *existing);
                auto row = t.heap->getTuple(*existing);
                if (row && t.keyOf(*row) == key) {
                    throw DbException("duplicate primary key " + std::to_string(key) + " in table '" + t.name + "'");
                }
                t.index->remove(key, *existing);  // stale entry: clean it up
            }
        }
        Rid rid = t.heap->insertTuple(tuple, txn);
        if (t.hasIndex() && !t.index->insert(key, rid)) {
            // A concurrent transaction inserted the same key in between.
            throw DbException("duplicate primary key " + std::to_string(key) + " in table '" + t.name + "'");
        }
    }
    return {{}, {}, "INSERT " + std::to_string(s.rows.size())};
}

ResultSet Session::runSelect(const SelectStmt& s, Transaction& txn) {
    TableInfo& t = table(s.table);
    validateWhere(t, s.where);

    std::vector<std::size_t> projection;
    ResultSet result;
    if (s.columns.empty()) {
        for (std::size_t i = 0; i < t.schema.numColumns(); ++i) projection.push_back(i);
    } else {
        for (const std::string& name : s.columns) {
            auto idx = t.schema.indexOf(name);
            if (!idx) throw DbException("unknown column '" + name + "'");
            projection.push_back(*idx);
        }
    }
    for (std::size_t i : projection) result.columns.push_back(t.schema.column(i).name);

    const ScanPlan plan = planScan(t, s.where);
    for (const Rid& rid : candidates(t, plan)) {
        if (s.limit && static_cast<int64_t>(result.rows.size()) >= *s.limit) break;
        db_.locks().lockShared(txn, rid);  // may wait, or abort via wait-die
        auto tuple = t.heap->getTuple(rid);
        if (!tuple || !matches(t, *tuple, s.where)) continue;
        std::vector<std::string> row;
        for (std::size_t i : projection) row.push_back((*tuple)[i].toString());
        result.rows.push_back(std::move(row));
    }
    result.message = "(" + std::to_string(result.rows.size()) + " row" + (result.rows.size() == 1 ? "" : "s") + ")";
    return result;
}

ResultSet Session::runDelete(const DeleteStmt& s, Transaction& txn) {
    TableInfo& t = table(s.table);
    validateWhere(t, s.where);
    std::size_t count = 0;
    for (const Rid& rid : candidates(t, planScan(t, s.where))) {
        db_.locks().lockExclusive(txn, rid);
        auto tuple = t.heap->getTuple(rid);
        if (!tuple || !matches(t, *tuple, s.where)) continue;
        t.heap->deleteTuple(rid, txn);
        // Keep the index entry until COMMIT: if we roll back, the row comes
        // back and must still be findable; meanwhile inserters of the same key
        // see the entry, lock the row, and wait for us to finish.
        if (t.hasIndex()) txn.pendingIndexDeletes().push_back({&t, t.keyOf(*tuple), rid});
        ++count;
    }
    return {{}, {}, "DELETE " + std::to_string(count)};
}

ResultSet Session::runUpdate(const UpdateStmt& s, Transaction& txn) {
    TableInfo& t = table(s.table);
    validateWhere(t, s.where);

    // Validate the SET list once, before touching any row.
    for (const Assignment& a : s.assignments) {
        auto idx = t.schema.indexOf(a.column);
        if (!idx) throw DbException("unknown column '" + a.column + "'");
        if (static_cast<int>(*idx) == t.schema.pkColumn()) throw DbException("updating the primary key is not supported");
        if (a.source_column) {
            auto src = t.schema.indexOf(*a.source_column);
            if (!src) throw DbException("unknown column '" + *a.source_column + "'");
            if (t.schema.column(*src).type != TypeId::Int || t.schema.column(*idx).type != TypeId::Int) {
                throw DbException("arithmetic in SET needs INT columns");
            }
        }
    }

    std::size_t count = 0;
    for (const Rid& rid : candidates(t, planScan(t, s.where))) {
        db_.locks().lockExclusive(txn, rid);
        auto tuple = t.heap->getTuple(rid);
        if (!tuple || !matches(t, *tuple, s.where)) continue;
        std::vector<Value> values = *tuple;
        for (const Assignment& a : s.assignments) {
            const std::size_t idx = *t.schema.indexOf(a.column);
            values[idx] = a.source_column
                ? Value::integer((*tuple)[*t.schema.indexOf(*a.source_column)].asInt() + a.value.asInt())
                : a.value;
        }
        t.heap->updateTuple(rid, t.schema.validate(values), txn);
        ++count;
    }
    return {{}, {}, "UPDATE " + std::to_string(count)};
}

ResultSet Session::runExplain(const ExplainStmt& s) {
    auto [name, where] = std::visit([](const auto& stmt) {
        return std::make_pair(stmt.table, stmt.where);
    }, s.inner);
    TableInfo& t = table(name);
    validateWhere(t, where);
    ScanPlan plan = planScan(t, where);

    ResultSet r;
    r.columns = {"plan"};
    r.rows.push_back({plan.empty ? "Empty Result (contradictory key bounds)" : plan.describe(t)});
    if (!where.empty()) r.rows.push_back({"  Filter: " + describeWhere(where)});
    return r;
}

}  // namespace coredb
