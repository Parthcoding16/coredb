// =============================================================================
// recovery_manager.h — Crash recovery from the write-ahead log.
//
// The buffer pool uses STEAL (dirty pages of uncommitted transactions may be
// written to disk) and NO-FORCE (committed pages need not be on disk at commit).
// That is the fastest combination, and it is why recovery needs both phases:
//
//   ANALYSIS : scan the log; transactions with BEGIN but neither COMMIT nor
//              ABORT are "losers" (they were running at the crash).
//   REDO     : replay every WRITE in log order ("repeat history"), skipping a
//              record if the page LSN shows the page already contains it.
//              This restores committed changes that never reached disk.
//   UNDO     : walk the log backwards and restore the before-image of every
//              WRITE made by a loser. This removes uncommitted changes that
//              were stolen onto disk.
//
// Afterwards a checkpoint flushes all pages and truncates the log, so a crash
// DURING recovery is harmless: the log is still there and every step above is
// idempotent, so recovery simply runs again.
//
// This is a simplified ARIES: same redo/undo structure and page-LSN test, but
// no fuzzy checkpoints and no CLR undo-next chains (rollbacks are logged as
// ordinary writes, see TransactionManager::abort).
// =============================================================================
#pragma once

#include <cstddef>

#include "buffer/buffer_pool_manager.h"
#include "recovery/log_manager.h"

namespace coredb {

struct RecoveryStats {
    bool        ran = false;
    std::size_t log_records = 0;
    std::size_t redone = 0;          // WRITE records actually re-applied
    std::size_t undone = 0;          // WRITE records rolled back
    std::size_t committed_txns = 0;
    std::size_t loser_txns = 0;
};

class RecoveryManager {
public:
    RecoveryManager(BufferPoolManager* bpm, LogManager* log) : bpm_(bpm), log_(log) {}

    // Runs analysis, redo, and undo. The caller then rebuilds indexes and
    // checkpoints. Returns ran=false if the log was empty (clean shutdown).
    RecoveryStats recover();

private:
    BufferPoolManager* bpm_;
    LogManager* log_;
};

}  // namespace coredb
