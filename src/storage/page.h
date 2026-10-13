// =============================================================================
// page.h — An in-memory frame holding one disk page plus its bookkeeping.
//
// Layout convention shared by EVERY page type in CoreDB:
//     bytes [0, 8)  : page LSN — LSN of the last log record applied to this page.
// The buffer pool reads this field to enforce the write-ahead-log rule before
// writing a dirty page back to disk. Pages that are never logged (index and
// catalog pages) simply leave it at 0.
// =============================================================================
#pragma once

#include <cstring>

#include "common/config.h"

namespace coredb {

class Page {
public:
    Page() { reset(); }

    char*       data()       { return data_; }
    const char* data() const { return data_; }

    page_id_t id() const { return page_id_; }

    lsn_t lsn() const {
        lsn_t v;
        std::memcpy(&v, data_, sizeof(v));
        return v;
    }
    void setLsn(lsn_t v) { std::memcpy(data_, &v, sizeof(v)); }

private:
    friend class BufferPoolManager;  // Only the pool manages identity and pin state.

    void reset() {
        std::memset(data_, 0, PAGE_SIZE);
        page_id_   = INVALID_PAGE_ID;
        pin_count_ = 0;
        is_dirty_  = false;
    }

    // alignas(8) so page layouts can safely overlay structs with 8-byte fields.
    alignas(8) char data_[PAGE_SIZE];
    page_id_t page_id_;
    int       pin_count_;   // >0 => someone is using it => must not be evicted.
    bool      is_dirty_;    // Modified since it was read from disk?
};

// Size of the header every page reserves for its LSN.
constexpr std::size_t PAGE_LSN_BYTES = sizeof(lsn_t);

}  // namespace coredb
