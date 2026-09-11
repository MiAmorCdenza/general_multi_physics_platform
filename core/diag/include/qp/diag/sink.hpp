/**
 * @file sink.hpp
 * @brief 诊断出口：诊断产生后去哪儿。
 *
 * 为什么出口是**接口**而不是全局日志对象：
 *   1. "可变全局状态"是归属不清最常见的物理形态（enforcement.md §3 的
 *      `qp-no-mutable-global` 规则）。诊断出口若是全局单例，测试之间会互相污染。
 *   2. 不同宿主需要不同出口：CLI 打到 stderr，GUI 收到面板里，
 *      测试收集到断言列表里。三者不该互相知道。
 *   3. 多线程求值下，"谁来同步这个全局对象"会变成无法回答的问题。
 *
 * @ownership   observes（Sink 不拥有诊断，只读后即可丢弃）
 * @thread      any（实现方负责线程安全；求值线程与主线程都可能产生诊断）
 * @pre         none
 * @post        none
 * @invariant   实现方不得保存传入 Diagnostic 的引用
 * @errors      emit 不得抛出（宿主在最坏情况下也要能记录）
 * @frozen      是（接口形状冻结）
 * @tests       diag.sink.collecting_sink, diag.sink.null_sink
 */
#pragma once

#include <qp/diag/diagnostic.hpp>

#include <cstddef>
#include <mutex>
#include <utility>
#include <vector>

namespace qp::diag {

/**
 * @brief 诊断出口接口。
 *
 * 实现方约定：
 *   - `emit` 必须 noexcept 语义（不抛）；内部失败自行吞掉。
 *   - **不得保存** `const Diagnostic&`；需要留存必须拷贝。
 */
class ISink {
public:
    ISink() = default;
    virtual ~ISink() = default;
    ISink(const ISink&) = delete;
    ISink& operator=(const ISink&) = delete;
    ISink(ISink&&) = delete;
    ISink& operator=(ISink&&) = delete;

    /// @brief 接收一条诊断。实现方负责线程安全。
    virtual void emit(const Diagnostic& d) noexcept = 0;

    /// @brief 出口的稳定短名，用于诊断"日志去哪了"这类问题。
    [[nodiscard]] virtual const char* name() const noexcept = 0;
};

/// @brief 丢弃一切诊断。用于不需要诊断的场景（性能基准、纯计算测试）。
class NullSink final : public ISink {
public:
    void emit(const Diagnostic&) noexcept override {}
    [[nodiscard]] const char* name() const noexcept override { return "null"; }
};

/**
 * @brief 收集诊断到内存。测试与"运行结束统一展示"的宿主使用。
 *
 * 线程安全：`emit` 与 `items` 都加锁（诊断可能来自求值线程）。
 *
 * @ownership   owns（拷贝保存每条诊断）
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   items() 的顺序是 emit 的先后顺序
 * @errors      emit noexcept（分配失败即 terminate；诊断出口不允许成为新的失败源）
 * @frozen      否
 * @tests       diag.sink.collecting_sink, diag.sink.collecting_sink_thread_safe
 */
class CollectingSink final : public ISink {
public:
    void emit(const Diagnostic& d) noexcept override {
        // 刻意不做异常处理：分配失败即 terminate。
        // 诊断出口若自己会失败，"错误处理"就变成了错误的来源。
        const std::lock_guard<std::mutex> lock(mutex_);
        items_.push_back(d);
    }

    [[nodiscard]] const char* name() const noexcept override { return "collecting"; }

    /// @brief 已收集的诊断数量。
    [[nodiscard]] std::size_t size() const noexcept {
        const std::lock_guard<std::mutex> lock(mutex_);
        return items_.size();
    }
    [[nodiscard]] bool empty() const noexcept {
        const std::lock_guard<std::mutex> lock(mutex_);
        return items_.empty();
    }

    /// @brief 已收集诊断的**副本**。返回副本而不是引用：引用会在锁释放后失效。
    [[nodiscard]] std::vector<Diagnostic> items() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return items_;
    }

    /// @brief 清空。测试用例之间必须清空，否则会互相污染。
    void clear() noexcept {
        const std::lock_guard<std::mutex> lock(mutex_);
        items_.clear();
    }

    /// @brief 是否收到过至少一条指定错误码的诊断。
    [[nodiscard]] bool contains(ErrorCode code) const noexcept {
        const std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& d : items_) {
            if (d.code() == code) return true;
        }
        return false;
    }

    /// @brief 收到的诊断中最严重的后果级别。
    [[nodiscard]] Consequence worst_consequence() const noexcept {
        const std::lock_guard<std::mutex> lock(mutex_);
        auto worst = Consequence::recoverable;
        for (const auto& d : items_) {
            if (d.consequence() > worst) worst = d.consequence();
        }
        return worst;
    }

private:
    mutable std::mutex mutex_;   ///< mutable：const 查询也要加锁
    std::vector<Diagnostic> items_;
};

}  // namespace qp::diag
