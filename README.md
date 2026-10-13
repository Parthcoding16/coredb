# CoreDB

A disk-based relational database engine written from scratch in C++17. No external dependencies.

CoreDB stores tables in 4 KB pages behind an LRU buffer pool, indexes primary keys with a disk-based B+ tree, survives crashes through write-ahead logging with redo/undo recovery, and isolates concurrent transactions with strict two-phase locking and wait-die deadlock prevention. A small SQL layer (tokenizer, recursive-descent parser, rule-based planner) sits on top.

## Build and run

Requires Linux, macOS or WSL (it uses POSIX `pread`/`pwrite`/`fsync`) and g++ 9+ or clang 10+.

```bash
make            # builds ./build/coredb
make test       # runs the test suite
make bench      # prints throughput numbers
./build/coredb mydata.db
```

CMake works too: `cmake -B build && cmake --build build && ./build/coredb_tests`.

## Example session

```sql
coredb> CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR(20), age INT);
coredb> INSERT INTO users VALUES (1,'ada',36), (2,'linus',28), (3,'grace',45);
coredb> SELECT * FROM users WHERE age > 30;
coredb> EXPLAIN SELECT * FROM users WHERE id >= 2;
| Index Range Scan on users using B+ tree (2 <= id <= +inf) |
|   Filter: id >= 2                                         |
coredb> UPDATE users SET age = age + 1 WHERE id = 2;
coredb> BEGIN;
coredb*> DELETE FROM users WHERE id = 1;
coredb*> ROLLBACK;                 -- row 1 is back
coredb> .crash                     -- kill without flushing anything
$ ./build/coredb mydata.db
crash recovery: 3 log records, 1 redone, 0 undone, 1 committed txns kept, 0 in-flight txns rolled back
```

Meta-commands: `.help`, `.tables`, `.schema <t>`, `.stats`, `.checkpoint`, `.crash`, `.quit`.

## Architecture

```
 SQL text
   │  sql/parser        tokenizer + recursive-descent parser  →  AST (std::variant)
   ▼
 execution/session     planner (index point / index range / sequential scan) + executor
   │                   autocommit or BEGIN…COMMIT; any error rolls back the transaction
   ├──────────────► concurrency/lock_manager      row S/X locks, strict 2PL, wait-die
   ├──────────────► concurrency/transaction_mgr   commit (fsync COMMIT) / abort (undo write set)
   ▼
 catalog/              table metadata in page 0, schemas, fixed-width tuples
   ├── storage/table_heap   rows in slotted pages; every change logged first
   └── index/bplus_tree     int64 key → RID, leaf chain for range scans
   ▼
 buffer/buffer_pool_manager   frames + page table + LRU replacer, RAII PageGuard, WAL rule on eviction
   ▼
 storage/disk_manager  ◄──►  recovery/log_manager (WAL)  ◄──  recovery/recovery_manager (analysis/redo/undo)
```

| Directory | What it demonstrates |
| --- | --- |
| `storage/` | Page layout, positional I/O, fsync, fixed-size slots, RIDs |
| `buffer/` | Page caching, LRU (list + hashmap, O(1)), pin counts, dirty bits, steal policy |
| `index/` | B+ tree search, insert with leaf and internal splits, range scans over the leaf chain |
| `recovery/` | Write-ahead logging, LSNs, checksummed records, ARIES-style redo/undo |
| `concurrency/` | Shared/exclusive locks, strict 2PL, wait-die, latches vs locks |
| `sql/`, `execution/` | Lexing, recursive-descent parsing, rule-based query planning, EXPLAIN |

## Design decisions

| Decision | Choice | Main alternative | Why |
| --- | --- | --- | --- |
| Buffer replacement | LRU | Clock, LRU-K | Simple and O(1); Clock would cut lock contention |
| Buffer write policy | Steal + no-force | No-steal / force | Best performance; recovery handles both directions |
| Logging | Physical slot images | Logical (SQL-level) | Redo/undo are idempotent byte copies |
| Index recovery | Rebuilt from the heap after a crash | Log index pages | Far simpler; costs startup time after a crash only |
| Isolation | Strict 2PL on rows | MVCC | Clear semantics; readers block writers (MVCC avoids that) |
| Deadlocks | Wait-die prevention | Waits-for graph detection | No background detector thread needed |
| Row format | Fixed width | Slotted variable length | No compaction needed; wastes space on short strings |

## Tests

13 tests, all passing, also clean under AddressSanitizer, UndefinedBehaviorSanitizer and ThreadSanitizer:

- unit tests for the LRU replacer, buffer pool, B+ tree (5,000 keys, height ≥ 5 with tiny nodes), WAL torn-tail handling, parser and planner
- SQL integration tests for CRUD, constraints, persistence across restarts, rollback, and wait-die
- **crash fault injection:** 40 rounds of random committed, rolled-back and in-flight transactions, each ended by a simulated crash with an 8-frame buffer pool, checked against an in-memory model
- **concurrency:** 8 threads × 150 bank transfers with wait-die aborts retried, checking the total balance never changes; and 4 threads of concurrent inserts

## Benchmarks (`make bench`, one machine, numbers vary)

| Workload | Result |
| --- | --- |
| Bulk insert, one transaction | ~115,000 rows/s |
| Autocommit insert (one fsync per commit) | ~7,300 txns/s |
| Point lookup via B+ tree, 100k rows | ~150 µs/query including SQL parsing |
| Full scan, 100k rows | ~74 ms/query |
| Index vs scan speedup | ~490× |

## Known limitations

These are deliberate scope choices, each with a known next step:

- the B+ tree does not merge under-full nodes and uses one tree-wide latch (next step: latch crabbing)
- phantoms are possible because there are no range locks (next step: next-key locking)
- checkpoints are quiescent rather than fuzzy, and DDL is not transactional
- types are limited to INT and VARCHAR(n ≤ 255); there are no joins, ORDER BY or NULL
