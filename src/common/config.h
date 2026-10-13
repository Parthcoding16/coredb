// =============================================================================
// config.h — Engine-wide constants, primitive types, and exception classes.
//
// Every other module includes this file, so it stays tiny and dependency-free.
// =============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>

namespace coredb {

// ---- Sizes ------------------------------------------------------------------

// One page = the unit of I/O between disk and memory. 4 KB matches the
// typical OS virtual-memory page and the sector-aligned block size of most
// SSDs, so one logical page maps to one physical read/write.
constexpr std::size_t PAGE_SIZE = 4096;

// Default number of in-memory frames in the buffer pool (256 * 4 KB = 1 MB).
constexpr std::size_t DEFAULT_POOL_FRAMES = 256;

// ---- Primitive identifiers ---------------------------------------------------

using page_id_t  = int32_t;   // Page number inside the data file.
using frame_id_t = int32_t;   // Slot number inside the buffer pool.
using txn_id_t   = int64_t;   // Transaction id; also its wait-die timestamp.
using lsn_t      = int64_t;   // Log sequence number (monotonic across restarts).

constexpr page_id_t INVALID_PAGE_ID = -1;
constexpr lsn_t     NO_LSN          = 0;    // Real LSNs start at 1.
constexpr txn_id_t  NO_TXN          = 0;    // Real transaction ids start at 1.

// The catalog (table metadata) always lives in page 0 of the data file.
constexpr page_id_t CATALOG_PAGE_ID = 0;

// ---- Record identifier -------------------------------------------------------

// A RID uniquely names one tuple slot: (page, slot-within-page).
// Indexes map keys -> RIDs, and the lock manager locks RIDs.
struct Rid {
    page_id_t page_id = INVALID_PAGE_ID;
    uint16_t  slot    = 0;

    bool operator==(const Rid& o) const { return page_id == o.page_id && slot == o.slot; }
    bool operator!=(const Rid& o) const { return !(*this == o); }
    bool operator<(const Rid& o) const {
        return page_id != o.page_id ? page_id < o.page_id : slot < o.slot;
    }
    std::string toString() const {
        return "(" + std::to_string(page_id) + "," + std::to_string(slot) + ")";
    }
};

struct RidHash {
    std::size_t operator()(const Rid& r) const noexcept {
        return std::hash<int64_t>{}((static_cast<int64_t>(r.page_id) << 16) | r.slot);
    }
};

// ---- Errors ------------------------------------------------------------------

// Any user-visible error (bad SQL, constraint violation, ...).
class DbException : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Thrown by the lock manager when wait-die decides a transaction must "die".
// The caller rolls the transaction back and may retry it.
class TransactionAbortException : public DbException {
public:
    using DbException::DbException;
};

}  // namespace coredb
