#include "concurrency/transaction_manager.h"

#include "catalog/catalog.h"

namespace coredb {

std::unique_ptr<Transaction> TransactionManager::begin() {
    auto txn = std::make_unique<Transaction>(next_id_++);
    LogRecord rec;
    rec.txn = txn->id();
    rec.type = LogType::Begin;
    log_->append(rec);
    ++active_;
    return txn;
}

void TransactionManager::commit(Transaction& txn) {
    LogRecord rec;
    rec.txn = txn.id();
    rec.type = LogType::Commit;
    lsn_t lsn = log_->append(rec);
    log_->flushUntil(lsn);  // Durability point: COMMIT record is on disk.

    // Rows deleted by this transaction can now lose their index entries.
    // remove() is conditional, so an entry already re-pointed at a newer row
    // with the same key is left alone.
    for (const PendingIndexDelete& d : txn.pendingIndexDeletes()) {
        d.table->index->remove(d.key, d.rid);
    }

    locks_->unlockAll(txn);
    txn.setState(TxnState::Committed);
    --active_;
}

void TransactionManager::abort(Transaction& txn) {
    auto& writes = txn.writeSet();
    for (auto it = writes.rbegin(); it != writes.rend(); ++it) {
        TableInfo& table = *it->table;
        table.heap->restoreImage(it->rid, it->before, txn);

        if (!table.hasIndex()) continue;
        const bool was_insert = it->before[0] == 0 && it->after[0] != 0;
        const bool was_delete = it->before[0] != 0 && it->after[0] == 0;
        if (was_insert) {
            // Undo an insert: its index entry must go.
            Tuple t = table.schema.deserialize(it->after.data() + 1);
            table.index->remove(table.keyOf(t), it->rid);
        } else if (was_delete) {
            // Undo a delete: make sure the restored row is indexed again
            // (no-op if its entry is still there, which is the usual case).
            Tuple t = table.schema.deserialize(it->before.data() + 1);
            table.index->insert(table.keyOf(t), it->rid);
        }
    }

    LogRecord rec;
    rec.txn = txn.id();
    rec.type = LogType::Abort;
    log_->append(rec);  // No fsync needed: losing it just means recovery undoes again.

    locks_->unlockAll(txn);
    txn.setState(TxnState::Aborted);
    --active_;
}

}  // namespace coredb
