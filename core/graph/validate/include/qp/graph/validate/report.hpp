/**
 * @file report.hpp
 * @brief Validation result: a set of diagnostics **that carry a location**.
 *
 * ## Why validation must return locatable problems instead of one boolean
 *
 * One validation pass finds several problems at once (three nodes missing parameters, two wires whose
 * dimensions disagree). Returning only "invalid" would force the user to try them one by one -- a disaster
 * in front of a class. Stopping at the first problem would cost the user N fixes to open one experiment.
 *
 * Hence: **give every problem at once, each with enough information to locate it** (node handle +
 * port number + user-facing description). Locations use stable handles rather than pointers:
 * a report gets cached, printed, and sent to the UI thread, so any live reference would dangle.
 *
 * ## The three severity levels
 *
 * `error` blocks loading; `warning` allows loading but should be surfaced (for example an `any` port,
 * where type checking is already void); `info` is purely explanatory (for example "this node is bypassed").
 *
 * @ownership   owns
 * @thread      any (read-only after construction)
 * @pre         none
 * @post        none
 * @invariant   `ok()` is equivalent to "there is no problem at error level"
 * @errors      noexcept
 * @frozen      no
 */
#pragma once

#include <qp/diag/diagnostic.hpp>
#include <qp/graph/ir.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace qp::graph {

/// @brief Severity level of a problem.
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
 * @brief One validation problem.
 *
 * @ownership   owns (holds its own string and handles, references no graph)
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   the same problem returns the same string from `to_text()` every time
 * @errors      noexcept
 * @frozen      no
 * @tests       graph.validate.issue_text, graph.validate.issue_location
 */
struct Issue final {
    Severity severity = Severity::error;
    qp::diag::ErrorCode code = qp::diag::ErrorCode::invalid_argument;
    /// Node the problem belongs to. Invalid means it belongs to no single node (graph level, for example).
    NodeId node{};
    /// Port the problem belongs to. 0 means it belongs to no single port.
    PortNumber port = 0;
    /// Whether it is an output port. Meaningful only when port != 0.
    bool is_output = false;
    /// A one-line user-facing message.
    std::string message;
    /// Optional repair hint. When present, users can solve it themselves more easily.
    std::string hint;

    [[nodiscard]] bool is_error() const noexcept { return severity == Severity::error; }

    /// @brief Build the one user-visible line of text (including the location).
    [[nodiscard]] std::string to_text() const;
};

/**
 * @brief The complete result of one validation run.
 *
 * @ownership   owns
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   once `ok()` is true it never becomes false again (the result is immutable)
 * @errors      noexcept
 * @frozen      no
 * @tests       graph.validate.report_ok, graph.validate.report_collects_all,
 *              graph.validate.report_worst_severity
 */
class Report final {
public:
    void add(Issue issue) { issues_.push_back(std::move(issue)); }

    /// @brief Convenience adder: error.
    void error(qp::diag::ErrorCode code, std::string message, NodeId node = {},
               PortNumber port = 0, bool is_output = false, std::string hint = {});

    /// @brief Convenience adder: warning.
    void warn(qp::diag::ErrorCode code, std::string message, NodeId node = {},
              PortNumber port = 0, bool is_output = false, std::string hint = {});

    /// @brief Whether there is no problem at error level.
    [[nodiscard]] bool ok() const noexcept;

    [[nodiscard]] const std::vector<Issue>& issues() const noexcept { return issues_; }
    [[nodiscard]] std::size_t size() const noexcept { return issues_.size(); }
    [[nodiscard]] bool empty() const noexcept { return issues_.empty(); }

    /// @brief Number of problems at the given severity.
    [[nodiscard]] std::size_t count(Severity s) const noexcept;

    /// @brief The worst severity present. Returns info when there are no problems.
    [[nodiscard]] Severity worst() const noexcept;

    /// @brief Join all problems into multi-line text (one per line).
    [[nodiscard]] std::string to_text() const;

    void clear() noexcept { issues_.clear(); }

private:
    std::vector<Issue> issues_;
};

}  // namespace qp::graph
