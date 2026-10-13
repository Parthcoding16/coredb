#include "recovery/recovery_manager.h"

#include <unordered_set>

#include "storage/table_page.h"

namespace coredb {

RecoveryStats RecoveryManager::recover() {
    RecoveryStats stats;
    const std::vector<LogRecord> records = log_->readAll();
    stats.log_records = records.size();
    if (records.empty()) return stats;
    stats.ran = true;

    // ---- 1. ANALYSIS: who finished, who did not? ---------------------------------
    std::unordered_set<txn_id_t> started, finished;
    for (const LogRecord& r : records) {
        if (r.type == LogType::Begin) started.insert(r.txn);
        if (r.type == LogType::Commit) { finished.insert(r.txn); ++stats.committed_txns; }
        if (r.type == LogType::Abort) finished.insert(r.txn);
    }
    std::unordered_set<txn_id_t> losers;
    for (txn_id_t t : started) {
        if (!finished.count(t)) losers.insert(t);
    }
    stats.loser_txns = losers.size();

    // ---- 2. REDO: repeat history -------------------------------------------------
    for (const LogRecord& r : records) {
        if (r.type != LogType::Write) continue;
        PageGuard g = bpm_->fetchPage(r.rid.page_id);
        // Page LSN >= record LSN  =>  this change already reached disk. Skip.
        // This test is what makes redo idempotent.
        if (g.page()->lsn() >= r.lsn) continue;
        TablePage(g.data()).writeSlotImage(r.rid.slot, r.after);
        g.page()->setLsn(r.lsn);
        g.markDirty();
        ++stats.redone;
    }

    // ---- 3. UNDO: roll back losers, newest change first ---------------------------
    for (auto it = records.rbegin(); it != records.rend(); ++it) {
        if (it->type != LogType::Write || !losers.count(it->txn)) continue;
        PageGuard g = bpm_->fetchPage(it->rid.page_id);
        TablePage(g.data()).writeSlotImage(it->rid.slot, it->before);
        g.markDirty();
        ++stats.undone;
    }
    return stats;
}

}  // namespace coredb
