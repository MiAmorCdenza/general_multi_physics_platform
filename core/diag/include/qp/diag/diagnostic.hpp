/**
 * @file diagnostic.hpp
 * @brief 结构化诊断：插件出错必须能被**归因给用户**。
 *
 * 设计意图（对应 plan-tree.md §2.5）：
 *   一条无主日志等于没有信息。诊断必须携带：
 *   - 哪里出的（code + 域）
 *   - 有多严重（consequence）
 *   - 是谁出的（source：插件/模块标识）
 *   - 附带的可选上下文（node_id / port_id 等，用稳定字符串而非指针）
 *
 * 刻意**不携带**指向运行时对象的指针或引用：诊断会被跨线程、跨进程、
 * 跨语言传递，任何 live 对象引用都会变成悬垂。
 */
#pragma once

#include <qp/diag/error.hpp>

#include <optional>
#include <string>
#include <utility>

namespace qp::diag {

/// @brief 诊断的来源标识：哪个插件/模块产生的。
///
/// 用稳定字符串而非指针——插件可能已被卸载，诊断仍要能打印出来。
struct SourceId final {
    std::string value{};

    [[nodiscard]] bool empty() const noexcept { return value.empty(); }
    [[nodiscard]] friend bool operator==(const SourceId& a, const SourceId& b) noexcept {
        return a.value == b.value;
    }
};

/**
 * @brief 一条结构化诊断。
 *
 * @ownership   owns（持有自己的字符串副本；不引用任何外部对象）
 * @thread      any
 * @pre         code != ErrorCode::ok
 * @post        none
 * @invariant   code 与 consequence 一经构造不再变化
 * @errors      noexcept（除构造时的内存分配；分配失败即 std::terminate）
 * @complexity  —
 * @nondet      none
 * @frozen      否（可加字段；已有字段语义冻结）
 * @tests       diag.diagnostic.construction, diag.diagnostic.stable_text,
 *              diag.diagnostic.no_live_references
 */
class Diagnostic final {
public:
    /// @brief 构造。`consequence` 缺省时按错误域推断。
    ///
    /// 只有一个构造函数：早期还提供了三参数重载，与四参数版（第四参有默认值）
    /// 在 `Diagnostic{a, b, c}` 形式下**产生歧义**，编译期就报错。
    /// 一个构造函数 + 默认参数已能表达全部用法。
    Diagnostic(ErrorCode code, std::string message, SourceId source = {},
               std::optional<Consequence> consequence = std::nullopt)
        : code_(code),
          consequence_(consequence.value_or(default_consequence(code))),
          source_(std::move(source)),
          message_(std::move(message)) {}

    [[nodiscard]] ErrorCode code() const noexcept { return code_; }
    [[nodiscard]] Consequence consequence() const noexcept { return consequence_; }
    [[nodiscard]] ErrorDomain domain() const noexcept { return domain_of(code_); }
    [[nodiscard]] const SourceId& source() const noexcept { return source_; }
    [[nodiscard]] const std::string& message() const noexcept { return message_; }

    /// @brief 覆盖后果级别。返回自身引用以便链式书写。
    Diagnostic& with_consequence(Consequence c) noexcept {
        consequence_ = c;
        return *this;
    }

    /// @brief 附加一段上下文说明（不改 code / consequence）。
    Diagnostic& with_detail(std::string detail) {
        if (!detail.empty()) {
            if (!message_.empty()) message_ += "；";
            message_ += std::move(detail);
        }
        return *this;
    }

    /// @brief 构造用户可见的一行文本。
    ///
    /// 格式：`<source>: <code> — <message>`
    /// 这是**展示**，不是判定依据；判定一律用 code。
    ///
    /// @ownership   pure
    /// @thread      any
    /// @pre         none
    /// @post        返回非空字符串；source 为空时省略前缀
    /// @invariant   同一诊断每次调用返回同一字符串
    /// @errors      noexcept；分配失败即 std::terminate
    /// @complexity  O(len)
    /// @nondet      none
    /// @frozen      否（展示格式可变）
    /// @tests       diag.diagnostic.stable_text
    [[nodiscard]] std::string to_text() const {
        std::string out;
        if (!source_.empty()) {
            out += source_.value;
            out += ": ";
        }
        out += to_string(code_);
        out += " — ";
        out += message_;
        return out;
    }

private:
    ErrorCode code_;
    Consequence consequence_;
    SourceId source_;
    std::string message_;
};

}  // namespace qp::diag
