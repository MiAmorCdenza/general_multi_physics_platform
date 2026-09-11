/**
 * @file cache.cpp
 * @brief 内容寻址缓存的实现。
 */
#include <qp/graph/eval/cache.hpp>

#include <algorithm>

namespace qp::graph {

const CacheEntry* EvalCache::find(const CacheKey& key) noexcept {
    if (capacity_ == 0) {
        ++misses_;
        return nullptr;
    }
    const auto it = entries_.find(key);
    if (it == entries_.end()) {
        ++misses_;
        return nullptr;
    }
    ++hits_;
    it->second.last_used = ++clock_;
    return &it->second;
}

void EvalCache::put(CacheKey key,
                    std::vector<std::pair<PortNumber, qp::ports::Value>> outputs) noexcept {
    if (capacity_ == 0) return;

    const auto it = entries_.find(key);
    if (it != entries_.end()) {
        it->second.outputs = std::move(outputs);
        it->second.last_used = ++clock_;
        return;
    }

    // 先淘汰再插入：保证插入后不会短暂超限
    while (entries_.size() >= capacity_) {
        evict_one();
    }

    CacheEntry e{};
    e.key = std::move(key);
    e.outputs = std::move(outputs);
    e.last_used = ++clock_;
    entries_.emplace(e.key, std::move(e));
}

void EvalCache::evict_one() {
    if (entries_.empty()) return;
    auto victim = entries_.begin();
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
        if (it->second.last_used < victim->second.last_used) victim = it;
    }
    entries_.erase(victim);
    ++evictions_;
}

void EvalCache::clear() noexcept { entries_.clear(); }

}  // namespace qp::graph
