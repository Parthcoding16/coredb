// =============================================================================
// table_page.h — Layout of a heap (table data) page.
//
//   offset 0   : lsn_t      page LSN            (8 bytes, shared convention)
//   offset 8   : page_id_t  next heap page      (4 bytes, -1 = last page)
//   offset 12  : uint16_t   record size         (2 bytes)
//   offset 14  : uint16_t   number of slots     (2 bytes)
//   offset 16  : slot[0], slot[1], ...          each = [1-byte used flag][record]
//
// Records are fixed size, so slot i lives at a computable offset and a RID
// (page, slot) never moves. A slot "image" = flag + record bytes; it is what
// the WAL stores as before/after images.
//
// TablePage is a non-owning VIEW over a page's bytes: it adds structure without
// copying anything.
// =============================================================================
#pragma once

#include <cstring>
#include <optional>
#include <vector>

#include "storage/page.h"

namespace coredb {

class TablePage {
public:
    static constexpr std::size_t HEADER_SIZE = 16;

    explicit TablePage(char* data) : data_(data) {}

    // Formats an empty page for records of `record_size` bytes.
    void init(uint16_t record_size) {
        std::memset(data_, 0, PAGE_SIZE);
        setNextPageId(INVALID_PAGE_ID);
        std::memcpy(data_ + 12, &record_size, 2);
        uint16_t slots = static_cast<uint16_t>((PAGE_SIZE - HEADER_SIZE) / (1 + record_size));
        std::memcpy(data_ + 14, &slots, 2);
    }

    static std::size_t maxRecordSize() { return PAGE_SIZE - HEADER_SIZE - 1; }

    page_id_t nextPageId() const { page_id_t v; std::memcpy(&v, data_ + 8, 4); return v; }
    void setNextPageId(page_id_t v) { std::memcpy(data_ + 8, &v, 4); }
    uint16_t recordSize() const { uint16_t v; std::memcpy(&v, data_ + 12, 2); return v; }
    uint16_t numSlots()   const { uint16_t v; std::memcpy(&v, data_ + 14, 2); return v; }
    std::size_t slotSize() const { return 1 + recordSize(); }

    bool isUsed(uint16_t slot) const { return slotPtr(slot)[0] != 0; }
    const char* record(uint16_t slot) const { return slotPtr(slot) + 1; }

    std::vector<char> slotImage(uint16_t slot) const {
        const char* p = slotPtr(slot);
        return std::vector<char>(p, p + slotSize());
    }
    void writeSlotImage(uint16_t slot, const std::vector<char>& image) {
        std::memcpy(slotPtr(slot), image.data(), slotSize());
    }

private:
    char* slotPtr(uint16_t slot) const { return data_ + HEADER_SIZE + slot * slotSize(); }
    char* data_;
};

}  // namespace coredb
