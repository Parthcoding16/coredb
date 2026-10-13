// =============================================================================
// lru_replacer.h — Chooses which buffer-pool frame to evict.
//
// Only UNPINNED frames are candidates. The replacer keeps them in a doubly
// linked list ordered by the time they were unpinned (most recent at front)
// plus a hash map from frame id to list node, so every operation is O(1):
//
//     unpin(f)  -> push f to the front     (it was just used)
//     pin(f)    -> remove f from the list  (in use, not evictable)
//     victim()  -> pop from the back       (least recently used)
//
// This is the same list + hashmap structure as the classic "LRU cache"
// interview problem.
// =============================================================================
#pragma once

#include <list>
#include <optional>
#include <unordered_map>

#include "common/config.h"

namespace coredb {

class LruReplacer {
public:
    // Removes and returns the least-recently-used evictable frame, if any.
    std::optional<frame_id_t> victim();

    // Frame became pinned: it must not be evicted.
    void pin(frame_id_t frame);

    // Frame's pin count dropped to 0: it is now an eviction candidate.
    void unpin(frame_id_t frame);

    std::size_t size() const { return lru_list_.size(); }

private:
    std::list<frame_id_t> lru_list_;  // front = most recent, back = LRU
    std::unordered_map<frame_id_t, std::list<frame_id_t>::iterator> index_;
};

}  // namespace coredb
