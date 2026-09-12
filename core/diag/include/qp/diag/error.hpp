/**
 * @file error.hpp
 * @brief Error codes and consequence levels: one platform-wide failure taxonomy.
 *
 * Design intent: a plugin failure must be **attributable to the user**, not turn into
 * an ownerless log line. So an error is not free text but a layered enumeration:
 *   - ErrorCode says "what went wrong" (for program branches and test assertions)
 *   - Consequence says "what happens then" (for the host to degrade or abort)
 *   - Free text is supplementary only and is never the basis for a decision
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   Every error code belongs to exactly one error domain
 * @errors      noexcept
 * @complexity  —
 * @nondet      none
 * @frozen      yes (a published enumerator value can never be renumbered)
 * @tests       diag.error_code_values_are_stable, diag.error_domain_mapping
 */
#pragma once

#include <cstdint>
#include <string_view>

namespace qp::diag {

/// @brief Error code. Values must never be reordered -- each is a stable id, not a counter.
///
/// Domain-tagged: high 8 bits are the domain, low 8 bits the index, so a new domain shifts no value.
enum class ErrorCode : std::uint16_t {
    ok = 0,

    // -- 1xxx input and formatting --------------------------------------------
    invalid_argument = 0x0101,
    malformed_document = 0x0102,
    unsupported_version = 0x0103,
    missing_field = 0x0104,
    out_of_range = 0x0105,

    // -- 2xxx types and dimensions --------------------------------------------
    unknown_port_type = 0x0201,
    type_mismatch = 0x0202,
    dimension_mismatch = 0x0203,
    unit_mismatch = 0x0204,

    // -- 3xxx graph structure -------------------------------------------------
    unknown_node = 0x0301,
    unknown_port = 0x0302,
    cycle_detected = 0x0303,
    duplicate_connection = 0x0304,
    not_connected = 0x0305,
    graph_busy = 0x0306,

    // -- 4xxx plugins ---------------------------------------------------------
    plugin_not_found = 0x0401,
    plugin_incompatible = 0x0402,
    plugin_load_failed = 0x0403,
    plugin_capability_missing = 0x0404,
    /// The plugin misbehaved **while the host was calling it**: it raised, or it returned something that
    /// does not match what it declared. The host caught it and carried on; the plugin's answer is not
    /// trustworthy.
    ///
    /// Separate from the plugin's own refusals on purpose. A plugin that returns "I cannot do this" is
    /// working; a plugin that raises is broken, and the two need different words in a report and different
    /// handling in the host -- one is a limitation a user can act on, the other is a defect to report.
    plugin_fault = 0x0405,
    /// The host stopped calling this plugin after repeated faults, and is reporting the consequence rather
    /// than the cause. A caller that sees this should ask what happened earlier; the run itself continues.
    plugin_quarantined = 0x0406,

    // -- 5xxx runs and data ---------------------------------------------------
    run_not_found = 0x0501,
    seed_required = 0x0502,
    dataset_empty = 0x0503,
    fit_failed = 0x0504,

    // -- 9xxx internal --------------------------------------------------------
    internal_error = 0x0901,
    not_implemented = 0x0902,
    cancelled = 0x0903,
};

/**
 * @brief Consequence level of a failure. The host decides degrade, retry, or abort from it.
 *
 * This is the line between "handling an error" and "reporting an error":
 *   ErrorCode is for the program; Consequence is for the **scheduling decision**.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   The order is the severity (comparable and sortable)
 * @errors      noexcept
 * @complexity  —
 * @nondet      none
 * @frozen      yes
 * @tests       diag.consequence_ordering
 */
enum class Consequence : std::uint8_t {
    /// Ignorable: the operation did nothing, but system state is intact.
    recoverable = 0,
    /// This node/plugin failed this time; the rest of the graph can carry on.
    degraded = 1,
    /// This run cannot continue, but the process and the document are intact.
    run_aborted = 2,
    /// Document or process state is untrustworthy; execution must stop.
    fatal = 3,
};

/// @brief Error domain. Groups error codes for display to the user.
enum class ErrorDomain : std::uint8_t {
    input = 0,
    typing = 1,
    graph = 2,
    plugin = 3,
    runtime = 4,
    internal = 5,
};

/**
 * @brief Returns the domain of an error code.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        Domain for the high 8 bits of code; ok maps to internal (meaningless, total-only)
 * @invariant   All error codes inside one domain report the same domain number
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       diag.error_domain_mapping
 */
[[nodiscard]] constexpr ErrorDomain domain_of(ErrorCode code) noexcept {
    const auto v = static_cast<std::uint16_t>(code);
    if (v == 0) return ErrorDomain::internal;
    switch (v >> 8) {
        case 0x01: return ErrorDomain::input;
        case 0x02: return ErrorDomain::typing;
        case 0x03: return ErrorDomain::graph;
        case 0x04: return ErrorDomain::plugin;
        case 0x05: return ErrorDomain::runtime;
        default:   return ErrorDomain::internal;
    }
}

/**
 * @brief Stable short name of an error code. Used by logs, test assertions, and serialization.
 *
 * Free text is not the basis for a decision; this short name is.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        Returns a non-empty stable identifier (ASCII, underscore separated)
 * @invariant   One code always yields one string; distinct codes never share a string
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      yes (renaming it breaks compatibility)
 * @tests       diag.error_code_values_are_stable
 */
[[nodiscard]] constexpr std::string_view to_string(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::ok: return "ok";
        case ErrorCode::invalid_argument: return "invalid_argument";
        case ErrorCode::malformed_document: return "malformed_document";
        case ErrorCode::unsupported_version: return "unsupported_version";
        case ErrorCode::missing_field: return "missing_field";
        case ErrorCode::out_of_range: return "out_of_range";
        case ErrorCode::unknown_port_type: return "unknown_port_type";
        case ErrorCode::type_mismatch: return "type_mismatch";
        case ErrorCode::dimension_mismatch: return "dimension_mismatch";
        case ErrorCode::unit_mismatch: return "unit_mismatch";
        case ErrorCode::unknown_node: return "unknown_node";
        case ErrorCode::unknown_port: return "unknown_port";
        case ErrorCode::cycle_detected: return "cycle_detected";
        case ErrorCode::duplicate_connection: return "duplicate_connection";
        case ErrorCode::not_connected: return "not_connected";
        case ErrorCode::graph_busy: return "graph_busy";
        case ErrorCode::plugin_not_found: return "plugin_not_found";
        case ErrorCode::plugin_incompatible: return "plugin_incompatible";
        case ErrorCode::plugin_load_failed: return "plugin_load_failed";
        case ErrorCode::plugin_capability_missing: return "plugin_capability_missing";
        case ErrorCode::plugin_fault: return "plugin_fault";
        case ErrorCode::plugin_quarantined: return "plugin_quarantined";
        case ErrorCode::run_not_found: return "run_not_found";
        case ErrorCode::seed_required: return "seed_required";
        case ErrorCode::dataset_empty: return "dataset_empty";
        case ErrorCode::fit_failed: return "fit_failed";
        case ErrorCode::internal_error: return "internal_error";
        case ErrorCode::not_implemented: return "not_implemented";
        case ErrorCode::cancelled: return "cancelled";
    }
    return "unknown";
}

/**
 * @brief Stable short name for `Consequence`. Safe to grep and alert on.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        Returns a non-empty ASCII identifier
 * @invariant   Distinct values never share a name
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      yes (renaming breaks log consumers)
 * @tests       diag.consequence_names_are_stable, diag.error_domain_names_are_stable
 */
[[nodiscard]] constexpr std::string_view to_string(Consequence c) noexcept {
    switch (c) {
        case Consequence::recoverable: return "recoverable";
        case Consequence::degraded: return "degraded";
        case Consequence::run_aborted: return "run_aborted";
        case Consequence::fatal: return "fatal";
    }
    return "unknown";
}

/**
 * @brief Stable short name for `ErrorDomain`.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        Returns a non-empty ASCII identifier
 * @invariant   Distinct values never share a name
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      yes
 * @tests       diag.error_domain_names_are_stable
 */
[[nodiscard]] constexpr std::string_view to_string(ErrorDomain d) noexcept {
    switch (d) {
        case ErrorDomain::input: return "input";
        case ErrorDomain::typing: return "typing";
        case ErrorDomain::graph: return "graph";
        case ErrorDomain::plugin: return "plugin";
        case ErrorDomain::runtime: return "runtime";
        case ErrorDomain::internal: return "internal";
    }
    return "unknown";
}

/// @brief Default consequence level of each error code. A plugin may override it case by case.
[[nodiscard]] constexpr Consequence default_consequence(ErrorCode code) noexcept {
    switch (domain_of(code)) {
        case ErrorDomain::input:
        case ErrorDomain::typing:
            return Consequence::recoverable;   // the user just changes a parameter
        case ErrorDomain::graph:
            return Consequence::degraded;      // this node fails, the rest carries on
        case ErrorDomain::plugin:
            return Consequence::degraded;      // a failed plugin must not sink the document
        case ErrorDomain::runtime:
            return Consequence::run_aborted;
        case ErrorDomain::internal:
            return Consequence::fatal;
    }
    return Consequence::fatal;
}

}  // namespace qp::diag
