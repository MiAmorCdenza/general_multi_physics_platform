/**
 * @file cache.hpp
 * @brief 节点级内容寻址缓存。
 *
 * ## 为什么是"内容寻址"而不是"版本号失效"
 *
 * 版本号失效（图上任何改动 → 清空缓存）在课堂现场是不可用的：
 * 老师调一个节点的参数，整张图的烘焙结果全被丢掉，于是每次调节都要重算。
 *
 * 内容寻址的语义是"**同样的输入 → 同样的输出**"：
 *   - 改一个参数 → 只有下游的键变了，无关分支照旧命中；
 *   - 撤销回上一步 → 之前的键重新出现，**立刻命中**（不需要重算）。
 *
 * 第二条尤其重要：撤销/重做在课堂演示里是高频操作，
 * 若每次撤销都触发重算，演示节奏就断了。
 *
 * ## 键里必须有世代号
 *
 * `NodeId` 是 `(index, generation)`。删掉槽位 3 的节点再新建一个，
 * 新节点也占槽位 3 但世代不同。若键里只放 index，
 * **新节点会命中旧节点的缓存**——这是最难查的一类错误：
 * 它不崩溃，只是算出别人的结果。
 *
 * ## 淘汰
 *
 * 缓存有容量上限。超限时按"最久未使用"淘汰。
 * 上限存在的理由：参数扫描会持续产生新键，
 * 无上限的缓存会在长时间运行后吃掉整台机器的内存。
 *
 * @ownership   owns（拥有缓存的全部值副本）
 * @thread      main（求值在单线程进行；见 EvalContext 的说明）
 * @pre         none
 * @post        none
 * @invariant   缓存不改变任何值；命中与未命中对调用方语义相同
 * @errors      noexcept（容量为 0 即禁用缓存）
 * @frozen      否
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
 * @brief 缓存键：内容寻址 + 世代 + 端口号。
 *
 * @ownership   owns
 * @thread      any（构造后只读）
 * @pre         none
 * @post        none
 * @invariant   `equals` 为真 ⟹ 两条键描述同一次计算
 * @errors      noexcept
 * @frozen      否
 * @tests       graph.eval.cache_key_equality, graph.eval.cache_key_generation_matters,
 *              graph.eval.cache_key_param_change_differs
 */
struct CacheKey final {
    /// 节点身份（含世代）。**必须含世代**，否则槽位复用会命中别人的结果。
    NodeId node{};
    /// 节点类型名（同一个槽位可能被改类型——虽然当前设计不允许，
    /// 但键里带上它成本极低而收益明确）。
    std::string type_name;
    /// 参数与输入的内容哈希。
    ValueHash content = kFnvOffsetBasis;
    /// 参数与输入的规范化文本。用于哈希碰撞时的精确判定。
    std::string content_text;

    [[nodiscard]] bool equals(const CacheKey& other) const noexcept {
        return node == other.node && type_name == other.type_name &&
               content == other.content && content_text == other.content_text;
    }
};

/// @brief `CacheKey` 的哈希适配（用内容哈希，不需要再算一遍文本）。
struct CacheKeyHash final {
    [[nodiscard]] std::size_t operator()(const CacheKey& k) const noexcept {
        // 直接复用内容哈希：它已经把节点身份混进去了
        return static_cast<std::size_t>(k.content);
    }
};

/// @brief `CacheKey` 的相等适配（精确比较，不只比哈希）。
struct CacheKeyEq final {
    [[nodiscard]] bool operator()(const CacheKey& a, const CacheKey& b) const noexcept {
        return a.equals(b);
    }
};

/**
 * @brief 一个缓存条目：键 + 值 + 使用序号（用于 LRU）。
 */
struct CacheEntry final {
    CacheKey key{};
    std::vector<std::pair<PortNumber, qp::ports::Value>> outputs;
    std::uint64_t last_used = 0;
};

/**
 * @brief 节点级内容寻址缓存。
 *
 * @ownership   owns
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   命中数与未命中数之和等于查询次数
 * @errors      noexcept
 * @frozen      否
 * @tests       graph.eval.cache_store_and_fetch, graph.eval.cache_miss_on_first_lookup,
 *              graph.eval.cache_eviction_lru, graph.eval.cache_disabled_when_zero,
 *              graph.eval.cache_clear, graph.eval.cache_stats_are_consistent,
 *              graph.eval.cache_undo_redo_hits
 */
class EvalCache final {
public:
    /// @brief 构造。`capacity == 0` 表示禁用缓存（每次都未命中）。
    explicit EvalCache(std::size_t capacity = 256) noexcept : capacity_(capacity) {}

    /**
     * @brief 查询。命中返回条目指针，未命中返回 nullptr。
     *
     * @ownership   observes（返回指针指向缓存内部，**下一次 put 之后可能失效**）
     * @thread      main
     * @pre         none
     * @post        命中时该条目的 last_used 被更新
     * @invariant   不修改缓存的内容集合
     * @errors      noexcept
     * @complexity  O(1) 摊销
     * @nondet      none
     * @frozen      否
     * @tests       graph.eval.cache_store_and_fetch, graph.eval.cache_miss_on_first_lookup
     */
    [[nodiscard]] const CacheEntry* find(const CacheKey& key) noexcept;

    /**
     * @brief 存入（或覆盖）一个条目。超限时按 LRU 淘汰。
     *
     * @ownership   owns（复制键与值）
     * @thread      main
     * @pre         none
     * @post        之后 `find(key)` 必然命中
     * @invariant   条目数不超过 capacity（capacity 为 0 时恒为 0）
     * @errors      noexcept；分配失败即 std::terminate
     *              （缓存不是失败源：它自己失败时不应把错误掺进求值结果）
     * @complexity  O(1) 摊销；淘汰时 O(n)
     * @nondet      none
     * @frozen      否
     * @tests       graph.eval.cache_eviction_lru, graph.eval.cache_disabled_when_zero
     */
    void put(CacheKey key, std::vector<std::pair<PortNumber, qp::ports::Value>> outputs) noexcept;

    /// @brief 清空全部条目（统计量保留）。
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
