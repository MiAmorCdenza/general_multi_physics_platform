/**
 * @file report.hpp
 * @brief 校验结果：**带位置的**诊断集合。
 *
 * ## 为什么校验必须返回可定位的问题，而不是一个布尔值
 *
 * 一次校验会同时发现多个问题（三个节点缺参数、两条线量纲不符）。
 * 若只返回"不合法"，用户就得逐个试——在课堂现场那是灾难。
 * 若在第一个问题处停下，用户要修 N 次才能打开一个实验。
 *
 * 因此：**一次给全部问题，每条都带足够定位的信息**（节点句柄 + 端口号 +
 * 面向用户的描述）。定位信息用稳定的句柄而不是指针：
 * 报告会被缓存、被打印、被发到 UI 线程，任何 live 引用都会悬垂。
 *
 * ## 三个严重级别
 *
 * `error` 阻止加载；`warning` 允许加载但应提示（例如用了 `any` 端口，
 * 类型检查已失效）；`info` 纯说明（例如"这个节点处于绕过状态"）。
 *
 * @ownership   owns
 * @thread      any（构造后只读）
 * @pre         none
 * @post        none
 * @invariant   `ok()` 等价于"没有 error 级别的问题"
 * @errors      noexcept
 * @frozen      否
 */
#pragma once

#include <qp/diag/diagnostic.hpp>
#include <qp/graph/ir.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace qp::graph {

/// @brief 问题的严重级别。
enum class Severity : std::uint8_t {
    info = 0,
    warning = 1,
    error = 2,
};

[[nodiscard]] constexpr const char* to_string(Severity s) noexcept {
    switch (s) {
        case Severity::info: return "info";
        case Severity::warning: return "warning";
        case Severity::error: return "error";
    }
    return "unknown";
}

/**
 * @brief 一条校验问题。
 *
 * @ownership   owns（自持字符串与句柄，不引用图）
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   同一问题每次 `to_text()` 返回同一字符串
 * @errors      noexcept
 * @frozen      否
 * @tests       graph.validate.issue_text, graph.validate.issue_location
 */
struct Issue final {
    Severity severity = Severity::error;
    qp::diag::ErrorCode code = qp::diag::ErrorCode::invalid_argument;
    /// 问题所在节点。无效表示问题不属于某个具体节点（例如整图级别）。
    NodeId node{};
    /// 问题所在端口。0 表示不属于某个具体端口。
    PortNumber port = 0;
    /// 是否为输出端口。仅在 port != 0 时有意义。
    bool is_output = false;
    /// 面向用户的一句话。
    std::string message;
    /// 可选的修复建议。有则用户更容易自己解决。
    std::string hint;

    [[nodiscard]] bool is_error() const noexcept { return severity == Severity::error; }

    /// @brief 构造用户可见的一行文本（含位置）。
    [[nodiscard]] std::string to_text() const;
};

/**
 * @brief 一次校验的完整结果。
 *
 * @ownership   owns
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   一旦 `ok()` 为真，之后不会再变为假（结果不可变）
 * @errors      noexcept
 * @frozen      否
 * @tests       graph.validate.report_ok, graph.validate.report_collects_all,
 *              graph.validate.report_worst_severity
 */
class Report final {
public:
    void add(Issue issue) { issues_.push_back(std::move(issue)); }

    /// @brief 添加快捷方式：错误。
    void error(qp::diag::ErrorCode code, std::string message, NodeId node = {},
               PortNumber port = 0, bool is_output = false, std::string hint = {});

    /// @brief 添加快捷方式：警告。
    void warn(qp::diag::ErrorCode code, std::string message, NodeId node = {},
              PortNumber port = 0, bool is_output = false, std::string hint = {});

    /// @brief 是否没有 error 级别的问题。
    [[nodiscard]] bool ok() const noexcept;

    [[nodiscard]] const std::vector<Issue>& issues() const noexcept { return issues_; }
    [[nodiscard]] std::size_t size() const noexcept { return issues_.size(); }
    [[nodiscard]] bool empty() const noexcept { return issues_.empty(); }

    /// @brief 指定级别的问题数量。
    [[nodiscard]] std::size_t count(Severity s) const noexcept;

    /// @brief 最严重的级别。无问题时返回 info。
    [[nodiscard]] Severity worst() const noexcept;

    /// @brief 把所有问题拼成多行文本（一行一条）。
    [[nodiscard]] std::string to_text() const;

    void clear() noexcept { issues_.clear(); }

private:
    std::vector<Issue> issues_;
};

}  // namespace qp::graph
