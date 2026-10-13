#include "database.h"

#include <iostream>

namespace coredb {

Database::Database(const std::string& path, Options options) {
    disk_    = std::make_unique<DiskManager>(path);
    log_     = std::make_unique<LogManager>(path + ".wal");
    bpm_     = std::make_unique<BufferPoolManager>(options.pool_frames, disk_.get(), log_.get());
    locks_   = std::make_unique<LockManager>();
    catalog_ = std::make_unique<Catalog>(bpm_.get(), disk_.get(), log_.get(), locks_.get());
    txns_    = std::make_unique<TransactionManager>(log_.get(), locks_.get());

    if (disk_->numPages() == 0) {  // brand-new database
        catalog_->initialize();
        bpm_->flushPage(CATALOG_PAGE_ID);
        disk_->sync();
    }

    // Recovery works on raw heap pages, so it runs BEFORE the catalog opens the
    // tables (their free-space hints must reflect the recovered state).
    recovery_ = RecoveryManager(bpm_.get(), log_.get()).recover();
    catalog_->load();
    if (recovery_.ran) {
        catalog_->rebuildIndexes();  // index pages are not logged
        checkpoint();                // make the recovered state durable, empty the log
    }
}

Database::~Database() {
    if (crashed_) return;
    try {
        checkpoint();
    } catch (const std::exception& e) {
        // Never throw from a destructor; the log still allows recovery.
        std::cerr << "warning: shutdown checkpoint failed: " << e.what() << "\n";
    }
}

void Database::checkpoint() {
    if (txns_->activeCount() > 0) {
        throw DbException("cannot checkpoint while transactions are active");
    }
    catalog_->persist();  // current index roots -> page 0
    bpm_->flushAll();     // every dirty page -> disk (WAL rule applied per page)
    disk_->sync();        // ... and durable
    log_->truncate();     // only now is the log no longer needed
}

}  // namespace coredb
