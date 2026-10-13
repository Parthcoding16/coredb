#include "catalog/catalog.h"

#include <cstring>
#include <mutex>

namespace coredb {

namespace {

// Bounds-checked sequential writer/reader over the catalog page.
class PageWriter {
public:
    explicit PageWriter(char* page) : p_(page + PAGE_LSN_BYTES), end_(page + PAGE_SIZE) {}
    template <typename T>
    void put(const T& v) { need(sizeof(T)); std::memcpy(p_, &v, sizeof(T)); p_ += sizeof(T); }
    void putString(const std::string& s) {
        put(static_cast<uint8_t>(s.size()));
        need(s.size());
        std::memcpy(p_, s.data(), s.size());
        p_ += s.size();
    }
private:
    void need(std::size_t n) {
        if (static_cast<std::size_t>(end_ - p_) < n) throw DbException("catalog page is full");
    }
    char* p_;
    char* end_;
};

class PageReader {
public:
    explicit PageReader(const char* page) : p_(page + PAGE_LSN_BYTES) {}
    template <typename T>
    T get() { T v; std::memcpy(&v, p_, sizeof(T)); p_ += sizeof(T); return v; }
    std::string getString() {
        auto n = get<uint8_t>();
        std::string s(p_, n);
        p_ += n;
        return s;
    }
private:
    const char* p_;
};

}  // namespace

Catalog::Catalog(BufferPoolManager* bpm, DiskManager* disk, LogManager* log, LockManager* locks)
    : bpm_(bpm), disk_(disk), log_(log), locks_(locks) {}

void Catalog::initialize() {
    PageGuard g = bpm_->newPage();
    if (g.id() != CATALOG_PAGE_ID) throw DbException("catalog must be page 0 of a new database");
    g.release();
    persist();
}

void Catalog::persist() {
    PageGuard g = bpm_->fetchPage(CATALOG_PAGE_ID);
    std::memset(g.data(), 0, PAGE_SIZE);
    PageWriter w(g.data());
    w.put(static_cast<uint32_t>(tables_.size()));
    for (auto& [name, info] : tables_) {
        w.putString(name);
        w.put(info->heap->firstPageId());
        w.put(info->hasIndex() ? info->index->rootPageId() : INVALID_PAGE_ID);
        w.put(static_cast<int32_t>(info->schema.pkColumn()));
        w.put(static_cast<uint16_t>(info->schema.numColumns()));
        for (const Column& c : info->schema.columns()) {
            w.putString(c.name);
            w.put(static_cast<uint8_t>(c.type));
            w.put(c.length);
        }
    }
    g.markDirty();
}

void Catalog::load() {
    PageGuard g = bpm_->fetchPage(CATALOG_PAGE_ID);
    PageReader r(g.data());
    auto count = r.get<uint32_t>();
    struct Pending { std::string name; Schema schema; page_id_t first; page_id_t root; };
    std::vector<Pending> pending;
    for (uint32_t t = 0; t < count; ++t) {
        Pending p;
        p.name = r.getString();
        p.first = r.get<page_id_t>();
        p.root = r.get<page_id_t>();
        auto pk = r.get<int32_t>();
        auto ncols = r.get<uint16_t>();
        std::vector<Column> cols;
        for (uint16_t i = 0; i < ncols; ++i) {
            Column c;
            c.name = r.getString();
            c.type = static_cast<TypeId>(r.get<uint8_t>());
            c.length = r.get<uint16_t>();
            cols.push_back(c);
        }
        p.schema = Schema(std::move(cols), pk);
        pending.push_back(std::move(p));
    }
    g.release();  // unpin page 0 before opening tables (they fetch pages too)
    for (auto& p : pending) openTable(p.name, std::move(p.schema), p.first, p.root);
}

void Catalog::openTable(const std::string& name, Schema schema, page_id_t first_page,
                        page_id_t index_root) {
    auto info = std::make_unique<TableInfo>();
    info->name = name;
    info->schema = std::move(schema);
    info->heap = std::make_unique<TableHeap>(info.get(), &info->schema, first_page,
                                             bpm_, disk_, log_, locks_);
    if (info->schema.pkColumn() >= 0) {
        info->index = std::make_unique<BPlusTree>(bpm_, index_root);
    }
    tables_[name] = std::move(info);
}

TableInfo& Catalog::createTable(const std::string& name, const std::vector<Column>& columns,
                                int pk_column) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    if (tables_.count(name)) throw DbException("table '" + name + "' already exists");
    if (columns.empty()) throw DbException("a table needs at least one column");
    Schema schema(columns, pk_column);
    if (schema.recordSize() > TablePage::maxRecordSize()) {
        throw DbException("row too large: " + std::to_string(schema.recordSize()) + " bytes");
    }
    page_id_t first = TableHeap::createFirstPage(*bpm_, *disk_, schema.recordSize());
    openTable(name, std::move(schema), first, INVALID_PAGE_ID);

    persist();  // DDL is forced: write page 0 and fsync right away.
    bpm_->flushPage(CATALOG_PAGE_ID);
    disk_->sync();
    return *tables_[name];
}

TableInfo* Catalog::getTable(const std::string& name) {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto it = tables_.find(name);
    return it == tables_.end() ? nullptr : it->second.get();
}

std::vector<TableInfo*> Catalog::tables() {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    std::vector<TableInfo*> out;
    for (auto& [name, info] : tables_) out.push_back(info.get());
    return out;
}

void Catalog::rebuildIndexes() {
    for (auto& [name, info] : tables_) {
        if (!info->hasIndex()) continue;
        info->index = std::make_unique<BPlusTree>(bpm_, INVALID_PAGE_ID);
        for (const Rid& rid : info->heap->scanRids()) {
            if (auto tuple = info->heap->getTuple(rid)) info->index->insert(info->keyOf(*tuple), rid);
        }
    }
}

}  // namespace coredb
