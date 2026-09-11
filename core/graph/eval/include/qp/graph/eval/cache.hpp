/**
 * @file cache.hpp
 * @brief Node-level content-addressed cache.
 *
 * ## Why "content addressing" and not "invalidate by version number"
 *
 * Version-number invalidation (any graph edit -> clear the cache) is unusable in class:
 * a teacher tweaks one parameter, all bake results are lost, and every tweak recomputes.
 *
 * Content addressing means "**same input -> same output**":
 *   - change one parameter -> only downstream keys change; unrelated branches still hit;
 *   - undo back one step -> the earlier keys reappear and **hit immediately** (no recompute).
 *
 * The second point matters most: undo/redo is a high-frequency action in a lecture,
 * and recomputing on every undo would break the demonstration's rhythm.
 *
 * ## The key must carry the generation
 *
 * `NodeId` is `(index, generation)`. Delete the node in slot 3, create a new one, and
 * the new node also occupies slot 3 but with a different generation. If the key held
 * only the index, **the new node would hit the old node's cache** -- the hardest kind
 * of bug to find: it does not crash, it just returns someone else's results.
 *
 * ## Eviction
 *
 * The cache has a capacity limit and evicts least-recently-used entries beyond it.
 * The limit exists because parameter sweeps keep producing new keys, and an unbounded
 * cache would eat the whole machine's memory after a long run.
 *
 * @ownership   owns (owns every cached copy of a value)
 * @thread      main (evaluation is single-threaded; see EvalContext)
 * @pre         none
 * @post        none
 * @invariant   The cache never changes a value; a hit and a miss mean the same to callers
 * @errors      noexcept (capacity 0 disables the cache)
 * @frozen      no
 */
#pragma once

#include <qp/graph/eval/value_key.hpp>
#include <qp/graph/ir.hpp>

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

namespace qp::graph {

/**
 * @brief Cache key: content address + generation + port number.
 *
 * @ownership   owns
 * @thread      any (read-only after construction)
 * @pre         none
 * @post        none
 * @invariant   `equals` being true means both keys describe the same computation
 * @errors      noexcept
 * @frozen      no
 * @tests       graph.eval.cache_key_equality, graph.eval.cache_key_generation_matters,
 *              graph.eval.cache_key_param_change_differs
 */
struct CacheKey final {
    /// Node identity (with generation). **Required**: otherwise slot reuse hits another node.
    NodeId node{};
    /// Node type name (the same slot could be retyped -- the current design forbids
    /// it, but carrying the name in the key costs almost nothing and pays off).
    std::string type_name;
    /// Content hash of parameters and inputs.
    ValueHash content = kFnvOffsetBasis;
    /// Canonical text of parameters and inputs. Exact test when hashes collide.
    std::string content_text;

    [[nodiscard]] bool equals(const CacheKey& other) const noexcept {
        return node == other.node && type_name == other.type_name &&
               content == other.content && content_text == other.content_text;
    }
};

/// @brief Hash adapter for `CacheKey` (uses the content hash, no second pass over text).
struct CacheKeyHash final {
    [[nodiscard]] std::size_t operator()(const CacheKey& k) const noexcept {
        // Reuse the content hash directly: node identity is already mixed into it
        return static_cast<std::size_t>(k.content);
    }
};

/// @brief Equality adapter for `CacheKey` (exact compare, not just the hash).
struct CacheKeyEq final {
    [[nodiscard]] bool operator()(const CacheKey& a, const CacheKey& b) const noexcept {
        return a.equals(b);
    }
};

/**
 * @brief One cache entry: key + value + use sequence number (for LRU).
 */
struct CacheEntry final {
    CacheKey key{};
    std::vector<std::pair<PortNumber, qp::ports::Value>> outputs;
    std::uint64_t last_used = 0;
};

/**
 * @brief Node-level content-addressed cache.
 *
 * @ownership   owns
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   hits + misses equals the number of lookups
 * @errors      noexcept
 * @frozen      no
 * @tests       graph.eval.cache_store_and_fetch, graph.eval.cache_miss_on_first_lookup,
 *              graph.eval.cache_eviction_lru, graph.eval.cache_disabled_when_zero,
 *              graph.eval.cache_clear, graph.eval.cache_stats_are_consistent,
 *              graph.eval.cache_undo_redo_hits
 */
class EvalCache final {
public:
    /// @brief Constructs. `capacity == 0` disables the cache (every lookup misses).
    explicit EvalCache(std::size_t capacity = 256) noexcept : capacity_(capacity) {}

    /**
     * @brief Looks up. Returns the entry pointer on a hit, nullptr on a miss.
     *
     * @ownership   observes (points inside the cache; **may dangle after the next put**)
     * @thread      main
     * @pre         none
     * @post        The entry's last_used is updated on a hit
     * @invariant   Does not modify the cache's content set
     * @errors      noexcept
     * @complexity  O(1) amortized
     * @nondet      none
     * @frozen      no
     * @tests       graph.eval.cache_store_and_fetch, graph.eval.cache_miss_on_first_lookup
     */
    [[nodiscard]] const CacheEntry* find(const CacheKey& key) noexcept;

    /**
     * @brief Stores (or overwrites) one entry. Evicts LRU when over capacity.
     *
     * @ownership   owns (copies the key and the values)
     * @thread      main
     * @pre         none
     * @post        A later `find(key)` necessarily hits
     * @invariant   The entry count never exceeds capacity (always 0 when capacity is 0)
     * @errors      noexcept; allocation failure calls std::terminate
     *              (the cache is not a failure source: its own failure must not leak into results)
     * @complexity  O(1) amortized; O(n) when evicting
     * @nondet      none
     * @frozen      no
     * @tests       graph.eval.cache_eviction_lru, graph.eval.cache_disabled_when_zero
     */
    void put(CacheKey key, std::vector<std::pair<PortNumber, qp::ports::Value>> outputs) noexcept;

    /// @brief Clears every entry (statistics are kept).
    void clear() noexcept;

    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] std::uint64_t hits() const noexcept { return hits_; }
    [[nodiscard]] std::uint64_t misses() const noexcept { return misses_; }
    [[nodiscard]] std::uint64_t evictions() const noexcept { return evictions_; }

private:
    void evict_one();

    std::size_t capacity_;
    std::unordered_map<CacheKey, CacheEntry, CacheKeyHash, CacheKeyEq> entries_;
    std::uint64_t clock_ = 0;
    std::uint64_t hits_ = 0;
    std::uint64_t misses_ = 0;
    std::uint64_t evictions_ = 0;
};

}  // namespace qp::graph
