#include "buffer/lru_replacer.h"

namespace coredb {

std::optional<frame_id_t> LruReplacer::victim() {
    if (lru_list_.empty()) return std::nullopt;
    frame_id_t frame = lru_list_.back();
    lru_list_.pop_back();
    index_.erase(frame);
    return frame;
}

void LruReplacer::pin(frame_id_t frame) {
    auto it = index_.find(frame);
    if (it == index_.end()) return;
    lru_list_.erase(it->second);
    index_.erase(it);
}

void LruReplacer::unpin(frame_id_t frame) {
    if (index_.count(frame)) return;  // Already a candidate; keep its position.
    lru_list_.push_front(frame);
    index_[frame] = lru_list_.begin();
}

}  // namespace coredb
