/**
 * @file diagnostic.hpp
 * @brief Structured diagnostics: a plugin error must be **attributable to the user**.
 *
 * Design intent (see plan-tree.md section 2.5):
 *   A log line with no owner carries no information. A diagnostic must carry:
 *   - where it came from (code + domain)
 *   - how severe it is (consequence)
 *   - who produced it (source: plugin/module identifier)
 *   - optional attached context (node_id / port_id, as stable strings, not pointers)
 *
 * It deliberately **carries no** pointer or reference to a runtime object: diagnostics
 * travel across threads, processes and languages, so any live reference dangles.
 */
#pragma once

#include <qp/diag/error.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace qp::diag {

/// @brief Log severity. Defined in logging.hpp; forward declared so that a
///        diagnostic can report its severity without depending on the log
///        schema.
enum class Severity : std::uint8_t;

/// @brief Source identifier of a diagnostic: which plugin/module produced it.
///
/// A stable string, not a pointer -- the plugin may be gone but the text still prints.
struct SourceId final {
    std::string value{};

    [[nodiscard]] bool empty() const noexcept { return value.empty(); }
    [[nodiscard]] friend bool operator==(const SourceId& a, const SourceId& b) noexcept {
        return a.value == b.value;
    }
};

/**
 * @brief One structured diagnostic.
 *
 * @ownership   owns (holds its own string copies; references no external object)
 * @thread      any
 * @pre         code != ErrorCode::ok
 * @post        none
 * @invariant   code and consequence never change after construction
 * @errors      noexcept (except allocation at construction; failure calls std::terminate)
 * @complexity  -
 * @nondet      none
 * @frozen      no (fields may be added; existing field semantics are frozen)
 * @tests       diag.diagnostic.construction, diag.diagnostic.stable_text,
 *              diag.diagnostic.no_live_references,
 *              diag.diagnostic.severity_is_derived_from_domain
 */
class Diagnostic final {
public:
    /// @brief Constructs. A defaulted `consequence` is inferred from the error domain.
    ///
    /// Only one constructor: an earlier three-parameter overload was ambiguous with
    /// the four-parameter form (defaulted fourth argument) in `Diagnostic{a, b, c}`
    /// and failed to compile. One constructor plus defaults covers every usage.
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

    /// @brief Log severity, derived from the error domain.
    ///
    /// Declared here and defined in logging.hpp so that this header keeps its
    /// "no dependency on the log schema" property; diagnostics exist whether or
    /// not anyone logs them.
    ///
    /// @ownership   pure
    /// @thread      any
    /// @pre         none
    /// @post        Returns the severity implied by code()
    /// @invariant   Same diagnostic always reports the same severity
    /// @errors      noexcept
    /// @complexity  O(1)
    /// @nondet      none
    /// @frozen      no
    /// @tests       diag.diagnostic.severity_is_derived_from_domain
    [[nodiscard]] Severity severity() const noexcept;

    /// @brief Overrides the consequence. Returns `*this` so calls can be chained.
    Diagnostic& with_consequence(Consequence c) noexcept {
        consequence_ = c;
        return *this;
    }

    /// @brief Attaches a context note (does not change code / consequence).
    Diagnostic& with_detail(std::string detail) {
        if (!detail.empty()) {
            if (!message_.empty()) message_ += "；";
            message_ += std::move(detail);
        }
        return *this;
    }

    /// @brief Builds the single user-visible line of text.
    ///
    /// Format: `<source>: <code> - <message>`
    /// This is **display**, not the basis for a decision; always decide on code.
    ///
    /// @ownership   pure
    /// @thread      any
    /// @pre         none
    /// @post        Returns a non-empty string; the prefix is omitted when source is empty
    /// @invariant   The same diagnostic returns the same string on every call
    /// @errors      noexcept; allocation failure calls std::terminate
    /// @complexity  O(len)
    /// @nondet      none
    /// @frozen      no (the display format may change)
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
