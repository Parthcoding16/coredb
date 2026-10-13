// =============================================================================
// bench.cpp — Quick throughput numbers for the README / resume.
//   make bench   or   ./build/coredb_bench
// Measures: bulk inserts in one transaction, autocommit inserts (one fsync
// each), point lookups via the B+ tree, and the same lookups via a full scan.
// =============================================================================
#include <chrono>
#include <cstdio>
#include <iostream>
#include <random>
#include <unistd.h>

#include "execution/session.h"

using namespace coredb;
using Clock = std::chrono::steady_clock;

static double secondsSince(Clock::time_point t) {
    return std::chrono::duration<double>(Clock::now() - t).count();
}

int main() {
    const std::string path = "/tmp/coredb_bench_" + std::to_string(::getpid()) + ".db";
    std::remove(path.c_str());
    std::remove((path + ".wal").c_str());
    Database db(path);
    Session s(db);
    s.execute("CREATE TABLE t (id INT PRIMARY KEY, name VARCHAR(16), v INT)");

    constexpr int N = 100000;
    auto t0 = Clock::now();
    s.execute("BEGIN");
    for (int i = 0; i < N; ++i) s.execute("INSERT INTO t VALUES (" + std::to_string(i) + ", 'row', " + std::to_string(i % 97) + ")");
    s.execute("COMMIT");
    double bulk = secondsSince(t0);

    constexpr int AUTO = 500;
    t0 = Clock::now();
    for (int i = 0; i < AUTO; ++i) s.execute("INSERT INTO t VALUES (" + std::to_string(N + i) + ", 'auto', 1)");
    double autocommit = secondsSince(t0);

    std::mt19937 rng(1);
    constexpr int LOOKUPS = 20000;
    t0 = Clock::now();
    for (int i = 0; i < LOOKUPS; ++i) s.execute("SELECT * FROM t WHERE id = " + std::to_string(rng() % N));
    double indexed = secondsSince(t0);

    constexpr int SCANS = 20;
    t0 = Clock::now();
    for (int i = 0; i < SCANS; ++i) s.execute("SELECT * FROM t WHERE v = -1");  // no index on v
    double scan = secondsSince(t0);

    TableInfo& t = *db.catalog().getTable("t");
    std::printf("rows                       : %d (heap pages: %zu, B+ tree height: %d)\n", N + AUTO, t.heap->numPages(), t.index->height());
    std::printf("bulk insert (1 txn)        : %.0f rows/s\n", N / bulk);
    std::printf("autocommit insert (fsync)  : %.0f txns/s\n", AUTO / autocommit);
    std::printf("point lookup via B+ tree   : %.1f us/query\n", indexed / LOOKUPS * 1e6);
    std::printf("full scan (100k rows)      : %.1f ms/query\n", scan / SCANS * 1e3);
    std::printf("index speedup vs scan      : %.0fx\n", (scan / SCANS) / (indexed / LOOKUPS));
    std::printf("buffer pool hit rate       : %.1f%%\n", 100.0 * db.bufferPool().hits() / (db.bufferPool().hits() + db.bufferPool().misses()));
}
