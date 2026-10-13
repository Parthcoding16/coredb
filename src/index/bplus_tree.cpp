#include "index/bplus_tree.h"

#include <algorithm>
#include <mutex>

namespace coredb {

namespace {

// ---- On-page layout -------------------------------------------------------------

struct NodeHeader {
    lsn_t     lsn;        // unused by the index, kept for the shared page convention
    uint8_t   is_leaf;
    uint8_t   pad[3];
    int32_t   size;       // number of entries (leaf) or children (internal)
    page_id_t next_leaf;  // leaf chain; INVALID_PAGE_ID for internal nodes / last leaf
    int32_t   pad2;
};

struct LeafEntry {
    int64_t   key;
    page_id_t page_id;
    uint16_t  slot;
    uint16_t  pad;
};

// Internal node with n children stores n entries; entry[0].key is unused, so
// child i covers keys in [entry[i].key, entry[i+1].key).
struct InternalEntry {
    int64_t   key;
    page_id_t child;
    int32_t   pad;
};

static_assert(sizeof(NodeHeader) == 24 && sizeof(LeafEntry) == 16 && sizeof(InternalEntry) == 16);

constexpr int ENTRY_CAPACITY = (PAGE_SIZE - sizeof(NodeHeader)) / 16;

// Non-owning typed view over a node page.
struct Node {
    char* d;
    NodeHeader& h() { return *reinterpret_cast<NodeHeader*>(d); }
    LeafEntry* leaf() { return reinterpret_cast<LeafEntry*>(d + sizeof(NodeHeader)); }
    InternalEntry* inner() { return reinterpret_cast<InternalEntry*>(d + sizeof(NodeHeader)); }
    bool isLeaf() { return h().is_leaf != 0; }

    void initLeaf() { h() = NodeHeader{NO_LSN, 1, {}, 0, INVALID_PAGE_ID, 0}; }
    void initInternal() { h() = NodeHeader{NO_LSN, 0, {}, 0, INVALID_PAGE_ID, 0}; }

    // Index of the first leaf entry with key >= `key` (binary search).
    int leafLowerBound(int64_t key) {
        LeafEntry* b = leaf();
        return static_cast<int>(std::lower_bound(b, b + h().size, key,
            [](const LeafEntry& e, int64_t k) { return e.key < k; }) - b);
    }

    // Index of the child to follow for `key` (binary search over separators).
    int childIndex(int64_t key) {
        InternalEntry* b = inner();
        auto it = std::upper_bound(b + 1, b + h().size, key,
            [](int64_t k, const InternalEntry& e) { return k < e.key; });
        return static_cast<int>(it - b) - 1;
    }
};

}  // namespace

// ---- BPlusTree ---------------------------------------------------------------------

BPlusTree::BPlusTree(BufferPoolManager* bpm, page_id_t root_page_id, int leaf_max_size,
                     int internal_max_size)
    : bpm_(bpm), root_(root_page_id),
      // One spare entry is reserved: a node may hold max+1 entries for an
      // instant, right before it is split.
      leaf_max_(leaf_max_size > 0 ? leaf_max_size : ENTRY_CAPACITY - 1),
      internal_max_(internal_max_size > 0 ? internal_max_size : ENTRY_CAPACITY - 1) {
    if (leaf_max_ < 2 || internal_max_ < 3 || leaf_max_ >= ENTRY_CAPACITY ||
        internal_max_ >= ENTRY_CAPACITY) {
        throw DbException("invalid B+ tree node size");
    }
}

page_id_t BPlusTree::rootPageId() {
    std::shared_lock<std::shared_mutex> lock(latch_);
    return root_;
}

int BPlusTree::height() {
    std::shared_lock<std::shared_mutex> lock(latch_);
    int h = 0;
    for (page_id_t pid = root_; pid != INVALID_PAGE_ID; ++h) {
        PageGuard g = bpm_->fetchPage(pid);
        Node n{g.data()};
        pid = n.isLeaf() ? INVALID_PAGE_ID : n.inner()[0].child;
    }
    return h;
}

page_id_t BPlusTree::findLeaf(int64_t key, std::vector<page_id_t>* path) {
    page_id_t pid = root_;
    while (true) {
        PageGuard g = bpm_->fetchPage(pid);
        Node n{g.data()};
        if (n.isLeaf()) return pid;
        if (path) path->push_back(pid);
        pid = n.inner()[n.childIndex(key)].child;
    }
}

std::optional<Rid> BPlusTree::get(int64_t key) {
    std::shared_lock<std::shared_mutex> lock(latch_);
    if (root_ == INVALID_PAGE_ID) return std::nullopt;
    PageGuard g = bpm_->fetchPage(findLeaf(key, nullptr));
    Node n{g.data()};
    int i = n.leafLowerBound(key);
    if (i < n.h().size && n.leaf()[i].key == key) {
        return Rid{n.leaf()[i].page_id, n.leaf()[i].slot};
    }
    return std::nullopt;
}

bool BPlusTree::insert(int64_t key, const Rid& rid) {
    std::unique_lock<std::shared_mutex> lock(latch_);

    if (root_ == INVALID_PAGE_ID) {  // First key: the root is a single leaf.
        PageGuard g = bpm_->newPage();
        Node n{g.data()};
        n.initLeaf();
        n.leaf()[0] = {key, rid.page_id, rid.slot, 0};
        n.h().size = 1;
        g.markDirty();
        root_ = g.id();
        return true;
    }

    std::vector<page_id_t> path;
    PageGuard g = bpm_->fetchPage(findLeaf(key, &path));
    Node leaf{g.data()};
    int pos = leaf.leafLowerBound(key);
    int size = leaf.h().size;
    if (pos < size && leaf.leaf()[pos].key == key) return false;  // duplicate

    // Shift right and insert in sorted position.
    std::move_backward(leaf.leaf() + pos, leaf.leaf() + size, leaf.leaf() + size + 1);
    leaf.leaf()[pos] = {key, rid.page_id, rid.slot, 0};
    leaf.h().size = ++size;
    g.markDirty();
    if (size <= leaf_max_) return true;

    // Overflow: split the leaf. The upper half moves to a new right sibling.
    PageGuard rg = bpm_->newPage();
    Node right{rg.data()};
    right.initLeaf();
    int mid = size / 2;
    std::copy(leaf.leaf() + mid, leaf.leaf() + size, right.leaf());
    right.h().size = size - mid;
    leaf.h().size = mid;
    right.h().next_leaf = leaf.h().next_leaf;  // splice into the leaf chain
    leaf.h().next_leaf = rg.id();
    rg.markDirty();

    // For leaves the separator is COPIED up: it still exists in the right leaf.
    int64_t separator = right.leaf()[0].key;
    page_id_t left_id = g.id(), right_id = rg.id();
    g.release();
    rg.release();
    insertIntoParent(path, left_id, separator, right_id);
    return true;
}

void BPlusTree::insertIntoParent(std::vector<page_id_t>& path, page_id_t left,
                                 int64_t separator, page_id_t right) {
    if (path.empty()) {  // `left` was the root: grow the tree by one level.
        PageGuard g = bpm_->newPage();
        Node root{g.data()};
        root.initInternal();
        root.inner()[0] = {0, left, 0};
        root.inner()[1] = {separator, right, 0};
        root.h().size = 2;
        g.markDirty();
        root_ = g.id();
        return;
    }

    page_id_t parent_id = path.back();
    path.pop_back();
    PageGuard g = bpm_->fetchPage(parent_id);
    Node parent{g.data()};
    int size = parent.h().size;
    int i = 0;
    while (parent.inner()[i].child != left) ++i;  // position of `left`

    std::move_backward(parent.inner() + i + 1, parent.inner() + size, parent.inner() + size + 1);
    parent.inner()[i + 1] = {separator, right, 0};
    parent.h().size = ++size;
    g.markDirty();
    if (size <= internal_max_) return;

    // Split the internal node. The middle key is MOVED up (not copied):
    // internal nodes only route, so it need not stay at this level.
    PageGuard rg = bpm_->newPage();
    Node sibling{rg.data()};
    sibling.initInternal();
    int mid = size / 2;
    int64_t push_up = parent.inner()[mid].key;
    std::copy(parent.inner() + mid, parent.inner() + size, sibling.inner());
    sibling.h().size = size - mid;
    parent.h().size = mid;
    rg.markDirty();

    page_id_t sibling_id = rg.id();
    g.release();
    rg.release();
    insertIntoParent(path, parent_id, push_up, sibling_id);
}

bool BPlusTree::remove(int64_t key, const Rid& expected) {
    std::unique_lock<std::shared_mutex> lock(latch_);
    if (root_ == INVALID_PAGE_ID) return false;
    PageGuard g = bpm_->fetchPage(findLeaf(key, nullptr));
    Node leaf{g.data()};
    int pos = leaf.leafLowerBound(key);
    int size = leaf.h().size;
    if (pos >= size || leaf.leaf()[pos].key != key) return false;
    const LeafEntry& e = leaf.leaf()[pos];
    if (Rid{e.page_id, e.slot} != expected) return false;

    std::move(leaf.leaf() + pos + 1, leaf.leaf() + size, leaf.leaf() + pos);
    leaf.h().size = size - 1;
    g.markDirty();
    return true;
}

std::vector<std::pair<int64_t, Rid>> BPlusTree::rangeScan(int64_t lo, int64_t hi) {
    std::shared_lock<std::shared_mutex> lock(latch_);
    std::vector<std::pair<int64_t, Rid>> out;
    if (root_ == INVALID_PAGE_ID || lo > hi) return out;

    page_id_t pid = findLeaf(lo, nullptr);
    bool first = true;
    while (pid != INVALID_PAGE_ID) {
        PageGuard g = bpm_->fetchPage(pid);
        Node leaf{g.data()};
        int i = first ? leaf.leafLowerBound(lo) : 0;
        first = false;
        for (; i < leaf.h().size; ++i) {
            const LeafEntry& e = leaf.leaf()[i];
            if (e.key > hi) return out;
            out.emplace_back(e.key, Rid{e.page_id, e.slot});
        }
        pid = leaf.h().next_leaf;  // walk sideways along the leaf chain
    }
    return out;
}

}  // namespace coredb
