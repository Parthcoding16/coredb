// =============================================================================
// bplus_tree.h — Disk-based B+ tree index: int64 key -> RID.
//
// Shape
//   * Internal nodes hold separator keys and child page ids; they only route.
//   * Leaf nodes hold (key, RID) pairs, sorted, and are chained left-to-right
//     through `next_leaf`, so a range scan descends once, then walks sideways.
//   * Every node is exactly one 4 KB page fetched through the buffer pool, so
//     the tree can be much larger than memory. With ~250 entries per node a
//     tree of height 3 already indexes ~15 million keys; a lookup touches 3 pages.
//
// Operations
//   insert : descend to the leaf, insert in sorted position; if the leaf
//            overflows, split it in half and push the separator up. Splits can
//            cascade up to the root; a root split grows the tree by one level
//            (B+ trees grow at the ROOT, which keeps all leaves at equal depth).
//   remove : delete the entry from its leaf. Under-full nodes are NOT merged —
//            a common simplification (PostgreSQL's btree also avoids eager
//            merging); space is reused by later inserts.
//
// Concurrency: one reader/writer latch for the whole tree (std::shared_mutex):
// lookups and scans run in parallel, modifications are exclusive. Latch
// crabbing would allow concurrent writers and is the natural next step.
//
// Recovery: index pages are not logged. After a crash the index is rebuilt
// from the (logged, recovered) table heap — see Catalog::rebuildIndexes().
// =============================================================================
#pragma once

#include <limits>
#include <optional>
#include <shared_mutex>
#include <utility>
#include <vector>

#include "buffer/buffer_pool_manager.h"

namespace coredb {

class BPlusTree {
public:
    // max sizes of 0 mean "as many entries as fit in a page". Tests pass tiny
    // values (e.g. 3) to force deep trees and many splits with few keys.
    BPlusTree(BufferPoolManager* bpm, page_id_t root_page_id,
              int leaf_max_size = 0, int internal_max_size = 0);

    std::optional<Rid> get(int64_t key);

    // Returns false (and changes nothing) if `key` already exists.
    bool insert(int64_t key, const Rid& rid);

    // Removes `key` only if it currently maps to `expected`. Returns success.
    bool remove(int64_t key, const Rid& expected);

    // All entries with lo <= key <= hi, in key order.
    std::vector<std::pair<int64_t, Rid>> rangeScan(
        int64_t lo = std::numeric_limits<int64_t>::min(),
        int64_t hi = std::numeric_limits<int64_t>::max());

    page_id_t rootPageId();
    int height();  // 0 for an empty tree.

private:
    // Walks from the root to the leaf that should contain `key`, recording the
    // internal pages visited (needed to propagate splits upward).
    page_id_t findLeaf(int64_t key, std::vector<page_id_t>* path);
    void insertIntoParent(std::vector<page_id_t>& path, page_id_t left,
                          int64_t separator, page_id_t right);

    BufferPoolManager* bpm_;
    page_id_t root_;
    int leaf_max_;
    int internal_max_;
    std::shared_mutex latch_;
};

}  // namespace coredb
