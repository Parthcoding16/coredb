// =============================================================================
// catalog.h — Table metadata ("the system catalog").
//
// The catalog lives in page 0 of the data file and records, for each table:
// name, columns, primary-key column, first heap page, and index root page.
//
// DDL (CREATE TABLE) is not transactional here: the catalog page is forced to
// disk immediately, so a created table survives any later crash.
// =============================================================================
#pragma once

#include <map>
#include <memory>
#include <shared_mutex>
#include <string>
#include <vector>

#include "catalog/schema.h"
#include "index/bplus_tree.h"
#include "storage/table_heap.h"

namespace coredb {

struct TableInfo {
    std::string name;
    Schema schema;
    std::unique_ptr<TableHeap> heap;
    std::unique_ptr<BPlusTree> index;  // null if the table has no primary key

    bool hasIndex() const { return index != nullptr; }
    int64_t keyOf(const Tuple& t) const { return t[schema.pkColumn()].asInt(); }
};

class Catalog {
public:
    Catalog(BufferPoolManager* bpm, DiskManager* disk, LogManager* log, LockManager* locks);

    // Formats page 0 for a brand-new database.
    void initialize();

    // Reads page 0 and opens every table.
    void load();

    // Creates a table (and its primary-key index, if any) and forces the
    // catalog to disk. Throws if the name is taken or the row is too large.
    TableInfo& createTable(const std::string& name, const std::vector<Column>& columns, int pk_column);

    TableInfo* getTable(const std::string& name);
    std::vector<TableInfo*> tables();

    // Serialises all metadata (including current index roots) into page 0.
    void persist();

    // Discards every index and rebuilds it by scanning its table heap.
    // Called after crash recovery, because index pages are not logged.
    void rebuildIndexes();

private:
    void openTable(const std::string& name, Schema schema, page_id_t first_page, page_id_t index_root);

    BufferPoolManager* bpm_;
    DiskManager* disk_;
    LogManager* log_;
    LockManager* locks_;
    // unique_ptr gives TableInfo a stable address: heaps, indexes, and
    // transactions keep raw pointers to it.
    std::map<std::string, std::unique_ptr<TableInfo>> tables_;
    std::shared_mutex mutex_;  // guards tables_ (lookups are shared, CREATE is exclusive)
};

}  // namespace coredb
