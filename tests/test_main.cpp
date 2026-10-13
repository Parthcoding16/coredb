// =============================================================================
// test_main.cpp — Self-contained test suite (no external framework needed).
//
//   make test        or        ./build/coredb_tests
//
// Unit tests cover each layer in isolation; integration tests drive the engine
// through SQL. The two most important ones:
//   * crash_fault_injection : 40 rounds of random committed / rolled-back /
//     in-flight transactions, each ended by a simulated crash, then checked
//     against an in-memory model of what SHOULD have survived.
//   * concurrent_bank_transfers : 8 threads move money between accounts under
//     contention; wait-die aborts are retried; the total must never change.
// =============================================================================
#include <atomic>
#include <cstdio>
#include <functional>
#include <iostream>
#include <map>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "buffer/buffer_pool_manager.h"
#include "execution/session.h"
#include "index/bplus_tree.h"
#include "recovery/log_manager.h"
#include "sql/parser.h"

using namespace coredb;

// ---- Tiny test framework --------------------------------------------------------------

namespace {

struct TestCase { const char* name; std::function<void()> fn; };
std::vector<TestCase>& registry() { static std::vector<TestCase> r; return r; }
struct Registrar { Registrar(const char* n, std::function<void()> f) { registry().push_back({n, std::move(f)}); } };

struct CheckFailure : std::runtime_error { using std::runtime_error::runtime_error; };

#define TEST(name) \
    void test_##name(); \
    Registrar registrar_##name(#name, test_##name); \
    void test_##name()

#define CHECK(cond) \
    do { if (!(cond)) throw CheckFailure(std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": CHECK(" #cond ") failed"); } while (0)

#define CHECK_THROWS(stmt) \
    do { bool thrown = false; try { stmt; } catch (const DbException&) { thrown = true; } CHECK(thrown); } while (0)

// A fresh database path; removes leftovers from earlier runs.
std::string tempDb(const std::string& tag) {
    std::string path = "/tmp/coredb_test_" + tag + "_" + std::to_string(::getpid()) + ".db";
    std::remove(path.c_str());
    std::remove((path + ".wal").c_str());
    return path;
}

std::size_t rowCount(Session& s, const std::string& sql) { return s.execute(sql).rows.size(); }

}  // namespace

// ---- Buffer pool ---------------------------------------------------------------------------

TEST(lru_replacer_evicts_least_recently_unpinned) {
    LruReplacer r;
    r.unpin(1); r.unpin(2); r.unpin(3);
    r.pin(1);                       // 1 is in use again
    CHECK(r.victim() == 2);
    r.unpin(1);
    CHECK(r.victim() == 3);
    CHECK(r.victim() == 1);
    CHECK(!r.victim().has_value());
}

TEST(buffer_pool_pins_evicts_and_writes_back) {
    std::string path = tempDb("bpm");
    DiskManager disk(path);
    BufferPoolManager bpm(3, &disk, nullptr);

    page_id_t first;
    {
        PageGuard a = bpm.newPage();
        first = a.id();
        std::snprintf(a.data() + 100, 32, "hello page");
        a.markDirty();
        PageGuard b = bpm.newPage();
        PageGuard c = bpm.newPage();
        CHECK_THROWS(bpm.newPage());  // all 3 frames pinned
    }                                 // guards unpin here (RAII)

    for (int i = 0; i < 5; ++i) bpm.newPage();  // forces `first` out of memory
    PageGuard again = bpm.fetchPage(first);     // read back from disk
    CHECK(std::string(again.data() + 100) == "hello page");
    CHECK(bpm.misses() >= 1);
}

// ---- B+ tree ---------------------------------------------------------------------------------

TEST(bplus_tree_splits_lookups_ranges_and_removes) {
    std::string path = tempDb("btree");
    DiskManager disk(path);
    BufferPoolManager bpm(64, &disk, nullptr);
    BPlusTree tree(&bpm, INVALID_PAGE_ID, /*leaf_max=*/4, /*internal_max=*/4);  // tiny nodes => deep tree

    std::vector<int64_t> keys(5000);
    for (int i = 0; i < 5000; ++i) keys[i] = i * 3;  // multiples of 3
    std::shuffle(keys.begin(), keys.end(), std::mt19937(42));
    for (int64_t k : keys) CHECK(tree.insert(k, Rid{static_cast<page_id_t>(k), 7}));
    CHECK(!tree.insert(keys[0], Rid{1, 1}));  // duplicate rejected
    CHECK(tree.height() >= 5);

    for (int64_t k : keys) {
        auto rid = tree.get(k);
        CHECK(rid && rid->page_id == k && rid->slot == 7);
    }
    CHECK(!tree.get(1).has_value());  // not a multiple of 3

    auto range = tree.rangeScan(100, 200);  // 102, 105, ..., 198
    CHECK(range.size() == 33);
    for (std::size_t i = 0; i < range.size(); ++i) CHECK(range[i].first == 102 + 3 * static_cast<int64_t>(i));

    for (int64_t k : keys) if (k % 2 == 0) CHECK(tree.remove(k, Rid{static_cast<page_id_t>(k), 7}));
    CHECK(!tree.remove(3, Rid{999, 7}));  // wrong RID => not removed
    auto all = tree.rangeScan();
    CHECK(all.size() == 2500);
    for (auto& [k, rid] : all) CHECK(k % 2 != 0);
}

// ---- Log manager -------------------------------------------------------------------------------

TEST(wal_ignores_torn_tail) {
    std::string path = tempDb("wal") + ".wal";
    std::remove(path.c_str());
    {
        LogManager log(path);
        for (int i = 0; i < 10; ++i) {
            LogRecord r;
            r.txn = 1;
            r.type = LogType::Write;
            r.rid = {5, static_cast<uint16_t>(i)};
            r.before = {0, 0};
            r.after = {1, static_cast<char>(i)};
            log.append(r);
        }
        log.flushAll();
    }
    FILE* f = std::fopen(path.c_str(), "ab");  // simulate a half-written record
    std::fwrite("\x40\x00\x00\x00garbage", 1, 11, f);
    std::fclose(f);

    LogManager reopened(path);
    auto records = reopened.readAll();
    CHECK(records.size() == 10);
    CHECK(records.back().after[1] == 9);
    CHECK(reopened.nextLsn() == 11);  // LSNs continue after the last good record
}

// ---- SQL front end -----------------------------------------------------------------------------

TEST(parser_accepts_grammar_and_reports_errors) {
    auto stmt = Parser("SELECT name, age FROM users WHERE id >= 10 AND name != 'x' LIMIT 5;").parse();
    auto* sel = std::get_if<SelectStmt>(&stmt);
    CHECK(sel && sel->columns.size() == 2 && sel->where.size() == 2 && sel->limit == 5);
    CHECK(sel->where[1].op == CompareOp::Ne && sel->where[1].value.asString() == "x");

    auto upd = Parser("update t set a = a - 5, b = 'it''s' where id = -3").parse();
    auto* u = std::get_if<UpdateStmt>(&upd);
    CHECK(u && u->assignments[0].source_column == "a" && u->assignments[0].value.asInt() == -5);
    CHECK(u->assignments[1].value.asString() == "it's" && u->where[0].value.asInt() == -3);

    CHECK_THROWS(Parser("SELECT FROM t").parse());
    CHECK_THROWS(Parser("INSERT INTO t VALUES (1, 'unterminated)").parse());
    CHECK_THROWS(Parser("CREATE TABLE t (a FLOAT)").parse());
}

TEST(planner_picks_index_or_sequential_scan) {
    std::string path = tempDb("plan");
    Database db(path);
    Session s(db);
    s.execute("CREATE TABLE t (id INT PRIMARY KEY, v INT)");
    TableInfo& t = *db.catalog().getTable("t");
    auto plan = [&](const std::string& where) {
        return planScan(t, std::get<SelectStmt>(Parser("SELECT * FROM t WHERE " + where).parse()).where);
    };
    CHECK(plan("id = 5").kind == ScanPlan::Kind::IndexPoint);
    CHECK(plan("id > 5 AND id <= 9").kind == ScanPlan::Kind::IndexRange);
    CHECK(plan("id > 5 AND id <= 9").lo == 6 && plan("id > 5 AND id <= 9").hi == 9);
    CHECK(plan("v = 5").kind == ScanPlan::Kind::SeqScan);
    CHECK(plan("id != 5").kind == ScanPlan::Kind::SeqScan);
    CHECK(plan("id > 9 AND id < 3").empty);
}

// ---- End-to-end SQL ------------------------------------------------------------------------------

TEST(sql_crud_constraints_and_persistence) {
    std::string path = tempDb("crud");
    {
        Database db(path);
        Session s(db);
        s.execute("CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR(16), age INT)");
        s.execute("INSERT INTO users VALUES (1,'ada',36), (2,'linus',28), (3,'grace',45)");
        CHECK(rowCount(s, "SELECT * FROM users WHERE age > 30") == 2);
        CHECK(s.execute("UPDATE users SET age = age + 1 WHERE id = 2").message == "UPDATE 1");
        CHECK(s.execute("SELECT age FROM users WHERE id = 2").rows[0][0] == "29");
        CHECK_THROWS(s.execute("INSERT INTO users VALUES (2,'dup',1)"));            // PK
        CHECK_THROWS(s.execute("INSERT INTO users VALUES (9,'way too long a name',1)"));
        CHECK_THROWS(s.execute("INSERT INTO users VALUES ('x','y',1)"));             // types
        CHECK_THROWS(s.execute("SELECT * FROM nope"));
        CHECK(s.execute("DELETE FROM users WHERE name = 'ada'").message == "DELETE 1");
        s.execute("INSERT INTO users VALUES (1,'ada2',37)");  // key reusable after delete
        for (int i = 100; i < 1100; ++i) s.execute("INSERT INTO users VALUES (" + std::to_string(i) + ",'u',1)");
    }
    // Clean shutdown, then reopen: everything must still be there.
    Database db(path);
    CHECK(!db.recoveryStats().ran);
    Session s(db);
    CHECK(rowCount(s, "SELECT * FROM users") == 1003);
    CHECK(s.execute("SELECT name FROM users WHERE id = 1").rows[0][0] == "ada2");
    CHECK(rowCount(s, "SELECT * FROM users WHERE id >= 500 AND id < 600") == 100);
}

TEST(rollback_restores_rows_and_index) {
    std::string path = tempDb("rollback");
    Database db(path);
    Session s(db);
    s.execute("CREATE TABLE t (id INT PRIMARY KEY, v INT)");
    s.execute("INSERT INTO t VALUES (1,10), (2,20), (3,30)");
    s.execute("BEGIN");
    s.execute("DELETE FROM t WHERE id = 1");
    s.execute("INSERT INTO t VALUES (1, 99)");  // same key again, same txn
    s.execute("UPDATE t SET v = 0 WHERE id >= 2");
    s.execute("INSERT INTO t VALUES (4, 40)");
    s.execute("ROLLBACK");
    auto r = s.execute("SELECT id, v FROM t WHERE id >= 1");
    CHECK(r.rows.size() == 3);
    CHECK(r.rows[0][1] == "10" && r.rows[1][1] == "20" && r.rows[2][1] == "30");
    CHECK(rowCount(s, "SELECT * FROM t WHERE id = 4") == 0);
    // An error inside an explicit transaction rolls back the whole transaction.
    s.execute("BEGIN");
    s.execute("UPDATE t SET v = 7 WHERE id = 1");
    CHECK_THROWS(s.execute("INSERT INTO t VALUES (2, 0)"));
    CHECK(!s.inTransaction());
    CHECK(s.execute("SELECT v FROM t WHERE id = 1").rows[0][0] == "10");
}

TEST(wait_die_and_deferred_index_delete) {
    std::string path = tempDb("waitdie");
    Database db(path);
    Session older(db), younger(db);
    older.execute("CREATE TABLE t (id INT PRIMARY KEY, v INT)");
    older.execute("INSERT INTO t VALUES (5, 50)");

    older.execute("BEGIN");    // smaller id => older
    younger.execute("BEGIN");
    older.execute("DELETE FROM t WHERE id = 5");
    // Younger wants the row the older one X-locked: under wait-die it dies.
    bool died = false;
    try { younger.execute("INSERT INTO t VALUES (5, 1)"); }
    catch (const TransactionAbortException&) { died = true; }
    CHECK(died && !younger.inTransaction());
    older.execute("ROLLBACK");
    CHECK(older.execute("SELECT v FROM t WHERE id = 5").rows[0][0] == "50");

    older.execute("DELETE FROM t WHERE id = 5");      // autocommit
    younger.execute("INSERT INTO t VALUES (5, 2)");    // stale entry cleaned up
    CHECK(older.execute("SELECT v FROM t WHERE id = 5").rows[0][0] == "2");
}

// ---- Crash recovery ---------------------------------------------------------------------------

TEST(recovery_keeps_committed_and_discards_uncommitted) {
    std::string path = tempDb("recovery");
    {
        Database db(path, Options{4});  // tiny pool => uncommitted dirty pages get stolen to disk
        Session s(db);
        s.execute("CREATE TABLE t (id INT PRIMARY KEY, v INT)");
        for (int i = 0; i < 2000; ++i) s.execute("INSERT INTO t VALUES (" + std::to_string(i) + ", 1)");
        s.execute("BEGIN");
        s.execute("UPDATE t SET v = 999 WHERE id >= 0");  // touches every page; never commits
        s.execute("INSERT INTO t VALUES (5000, 5000)");
        s.abandonTransactionForCrashTest();
        db.simulateCrash();
    }
    Database db(path);
    CHECK(db.recoveryStats().ran);
    CHECK(db.recoveryStats().loser_txns == 1 && db.recoveryStats().undone > 0);
    Session s(db);
    CHECK(rowCount(s, "SELECT * FROM t") == 2000);
    CHECK(rowCount(s, "SELECT * FROM t WHERE v = 999") == 0);
    CHECK(rowCount(s, "SELECT * FROM t WHERE id = 5000") == 0);
    CHECK(rowCount(s, "SELECT * FROM t WHERE id = 1234") == 1);  // index rebuilt
}

TEST(crash_fault_injection_matches_model) {
    std::string path = tempDb("fault");
    std::mt19937 rng(7);
    std::map<int64_t, int64_t> model;  // committed state that must survive
    {
        Database db(path);
        Session(db).execute("CREATE TABLE kv (k INT PRIMARY KEY, v INT)");
    }
    for (int round = 0; round < 40; ++round) {
        {
            Database db(path, Options{8});  // tiny pool: lots of eviction (steal)
            Session s(db);
            auto randomTxn = [&](std::map<int64_t, int64_t>& state) {
                s.execute("BEGIN");
                for (int op = 0; op < 20; ++op) {
                    int64_t k = rng() % 300, v = rng() % 1000;
                    std::string ks = std::to_string(k), vs = std::to_string(v);
                    if (state.count(k)) {
                        if (rng() % 2) { s.execute("UPDATE kv SET v = " + vs + " WHERE k = " + ks); state[k] = v; }
                        else { s.execute("DELETE FROM kv WHERE k = " + ks); state.erase(k); }
                    } else {
                        s.execute("INSERT INTO kv VALUES (" + ks + ", " + vs + ")"); state[k] = v;
                    }
                }
            };
            for (int t = 0; t < 5; ++t) {
                std::map<int64_t, int64_t> next = model;
                randomTxn(next);
                if (rng() % 3) { s.execute("COMMIT"); model = next; }
                else s.execute("ROLLBACK");
            }
            std::map<int64_t, int64_t> lost = model;
            randomTxn(lost);  // in flight at the crash
            s.abandonTransactionForCrashTest();
            db.simulateCrash();
        }
        Database db(path, Options{8});
        Session s(db);
        auto rows = s.execute("SELECT k, v FROM kv").rows;
        std::map<int64_t, int64_t> actual;
        for (auto& r : rows) actual[std::stoll(r[0])] = std::stoll(r[1]);
        CHECK(actual == model);
        for (auto& [k, v] : model) {  // the rebuilt index must agree with the heap
            CHECK(s.execute("SELECT v FROM kv WHERE k = " + std::to_string(k)).rows.at(0).at(0) == std::to_string(v));
        }
    }
}

// ---- Concurrency -------------------------------------------------------------------------------

TEST(concurrent_bank_transfers_preserve_total) {
    std::string path = tempDb("bank");
    Database db(path);
    constexpr int ACCOUNTS = 10, THREADS = 8, TRANSFERS = 150;
    {
        Session s(db);
        s.execute("CREATE TABLE accounts (id INT PRIMARY KEY, balance INT)");
        for (int i = 0; i < ACCOUNTS; ++i) s.execute("INSERT INTO accounts VALUES (" + std::to_string(i) + ", 1000)");
    }
    std::atomic<int> aborts{0};
    std::vector<std::thread> workers;
    for (int w = 0; w < THREADS; ++w) {
        workers.emplace_back([&, w] {
            Session s(db);
            std::mt19937 rng(w);
            for (int i = 0; i < TRANSFERS; ++i) {
                int from = rng() % ACCOUNTS, to = (from + 1 + rng() % (ACCOUNTS - 1)) % ACCOUNTS;
                std::string amount = std::to_string(1 + rng() % 50);
                while (true) {  // retry until the transfer commits
                    try {
                        s.execute("BEGIN");
                        s.execute("UPDATE accounts SET balance = balance - " + amount + " WHERE id = " + std::to_string(from));
                        s.execute("UPDATE accounts SET balance = balance + " + amount + " WHERE id = " + std::to_string(to));
                        s.execute("COMMIT");
                        break;
                    } catch (const TransactionAbortException&) {
                        ++aborts;
                        std::this_thread::yield();
                    }
                }
            }
        });
    }
    for (auto& t : workers) t.join();

    Session s(db);
    int64_t total = 0;
    for (auto& row : s.execute("SELECT balance FROM accounts").rows) total += std::stoll(row[0]);
    CHECK(total == ACCOUNTS * 1000);
    std::cout << "    (" << THREADS * TRANSFERS << " transfers, " << aborts.load() << " wait-die aborts retried) ";
}

TEST(concurrent_inserts_all_land) {
    std::string path = tempDb("inserts");
    Database db(path);
    Session(db).execute("CREATE TABLE t (id INT PRIMARY KEY, v INT)");
    std::vector<std::thread> workers;
    for (int w = 0; w < 4; ++w) {
        workers.emplace_back([&, w] {
            Session s(db);
            for (int i = 0; i < 500; ++i) s.execute("INSERT INTO t VALUES (" + std::to_string(w * 1000 + i) + ", 0)");
        });
    }
    for (auto& t : workers) t.join();
    Session s(db);
    CHECK(rowCount(s, "SELECT * FROM t") == 2000);
    CHECK(rowCount(s, "SELECT * FROM t WHERE id >= 1000 AND id < 2000") == 500);
}

// ---- Runner ------------------------------------------------------------------------------------

int main() {
    int passed = 0, failed = 0;
    for (const TestCase& t : registry()) {
        std::cout << "[ RUN  ] " << t.name << std::flush;
        try {
            t.fn();
            std::cout << "\r[ PASS ] " << t.name << '\n';
            ++passed;
        } catch (const std::exception& e) {
            std::cout << "\r[ FAIL ] " << t.name << "\n         " << e.what() << '\n';
            ++failed;
        }
    }
    std::cout << '\n' << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
