// =============================================================================
// main.cpp — Interactive shell for CoreDB.
//
//   ./coredb [database-file]        (default: coredb.db)
//
// SQL statements end with ';' and may span several lines. Meta-commands start
// with '.', e.g. .help, .tables, .stats, .crash. See printHelp() below.
// =============================================================================
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>

#include "execution/session.h"

using namespace coredb;

namespace {

void printHelp() {
    std::cout <<
        "SQL (end with ';'):\n"
        "  CREATE TABLE t (id INT PRIMARY KEY, name VARCHAR(32), age INT);\n"
        "  INSERT INTO t VALUES (1, 'ada', 36), (2, 'linus', 28);\n"
        "  SELECT * FROM t WHERE id >= 1 AND age > 30 LIMIT 10;\n"
        "  UPDATE t SET age = age + 1 WHERE id = 2;\n"
        "  DELETE FROM t WHERE name = 'ada';\n"
        "  EXPLAIN SELECT * FROM t WHERE id = 1;\n"
        "  BEGIN;  ...  COMMIT;   or   ROLLBACK;\n"
        "Meta-commands:\n"
        "  .tables          list tables\n"
        "  .schema <table>  show a table's columns\n"
        "  .stats           buffer pool / disk / log statistics\n"
        "  .checkpoint      flush all pages and truncate the WAL\n"
        "  .crash           kill the process WITHOUT flushing (then restart to see recovery)\n"
        "  .quit            clean shutdown\n";
}

void printResult(const ResultSet& r) {
    if (!r.columns.empty()) {
        std::vector<std::size_t> width(r.columns.size());
        for (std::size_t i = 0; i < r.columns.size(); ++i) {
            width[i] = r.columns[i].size();
            for (const auto& row : r.rows) width[i] = std::max(width[i], row[i].size());
        }
        auto line = [&] {
            std::cout << '+';
            for (std::size_t w : width) std::cout << std::string(w + 2, '-') << '+';
            std::cout << '\n';
        };
        auto print = [&](const std::vector<std::string>& cells) {
            std::cout << '|';
            for (std::size_t i = 0; i < cells.size(); ++i) {
                std::cout << ' ' << std::left << std::setw(static_cast<int>(width[i])) << cells[i] << " |";
            }
            std::cout << '\n';
        };
        line();
        print(r.columns);
        line();
        for (const auto& row : r.rows) print(row);
        line();
    }
    if (!r.message.empty()) std::cout << r.message << '\n';
}

void printStats(Database& db) {
    BufferPoolManager& bpm = db.bufferPool();
    const double total = static_cast<double>(bpm.hits() + bpm.misses());
    std::cout << "buffer pool : " << bpm.poolSize() << " frames, " << bpm.hits() << " hits, "
              << bpm.misses() << " misses, hit rate "
              << std::fixed << std::setprecision(1) << (total > 0 ? 100.0 * bpm.hits() / total : 0.0) << "%\n"
              << "disk        : " << db.disk().numPages() << " pages, " << db.disk().reads()
              << " reads, " << db.disk().writes() << " writes\n"
              << "wal         : next LSN " << db.log().nextLsn() << ", " << db.log().flushCount() << " fsyncs\n";
}

bool handleMeta(const std::string& cmd, Database& db, Session& session) {
    if (cmd == ".quit" || cmd == ".exit") return false;
    if (cmd == ".help") {
        printHelp();
    } else if (cmd == ".tables") {
        for (TableInfo* t : db.catalog().tables()) {
            std::cout << t->name << "  (" << t->heap->numPages() << " heap pages"
                      << (t->hasIndex() ? ", B+ tree height " + std::to_string(t->index->height()) : "") << ")\n";
        }
    } else if (cmd.rfind(".schema", 0) == 0) {
        std::string name = cmd.size() > 8 ? cmd.substr(8) : "";
        TableInfo* t = db.catalog().getTable(name);
        if (!t) { std::cout << "no such table: '" << name << "'\n"; return true; }
        for (std::size_t i = 0; i < t->schema.numColumns(); ++i) {
            const Column& c = t->schema.column(i);
            std::cout << "  " << c.name << " " << c.typeName()
                      << (static_cast<int>(i) == t->schema.pkColumn() ? " PRIMARY KEY" : "") << '\n';
        }
    } else if (cmd == ".stats") {
        printStats(db);
    } else if (cmd == ".checkpoint") {
        if (session.inTransaction()) std::cout << "error: finish the transaction first\n";
        else { db.checkpoint(); std::cout << "checkpoint complete\n"; }
    } else if (cmd == ".crash") {
        std::cout << "simulating a crash: exiting without flushing pages or the log buffer" << std::endl;
        std::_Exit(1);  // no destructors, no flushing — like pulling the plug
    } else {
        std::cout << "unknown command; try .help\n";
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string path = argc > 1 ? argv[1] : "coredb.db";
    try {
        Database db(path);
        const RecoveryStats& rs = db.recoveryStats();
        if (rs.ran) {
            std::cout << "crash recovery: " << rs.log_records << " log records, " << rs.redone
                      << " redone, " << rs.undone << " undone, " << rs.committed_txns
                      << " committed txns kept, " << rs.loser_txns << " in-flight txns rolled back\n";
        }
        std::cout << "CoreDB — " << path << "  (type .help)\n";

        Session session(db);
        std::string buffer, line;
        while (true) {
            std::cout << (buffer.empty() ? (session.inTransaction() ? "coredb*> " : "coredb> ") : "   ...> ")
                      << std::flush;
            if (!std::getline(std::cin, line)) break;
            if (buffer.empty() && !line.empty() && line[0] == '.') {
                if (!handleMeta(line, db, session)) break;
                continue;
            }
            buffer += line + "\n";
            if (line.find(';') == std::string::npos) continue;

            const auto start = std::chrono::steady_clock::now();
            try {
                printResult(session.execute(buffer));
                const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - start).count();
                std::cout << "Time: " << std::fixed << std::setprecision(3) << us / 1000.0 << " ms\n";
            } catch (const std::exception& e) {
                std::cout << "error: " << e.what() << '\n';
            }
            buffer.clear();
        }
        std::cout << "bye\n";
    } catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << '\n';
        return 1;
    }
    return 0;
}
