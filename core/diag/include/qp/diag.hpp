/**
 * @file qp/diag.hpp
 * @brief The single entry point of the diag module.
 *
 * diag is an L0 foundation module and **depends on no other module inside core**.
 * It is the platform-wide error channel: every failing module and plugin reports here.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   This module adds no mutable global state (the sink is an interface, not a singleton)
 * @errors      noexcept
 * @complexity  —
 * @nondet      none
 * @frozen      yes (error code values and the Result shape)
 * @tests       diag.error_code_values_are_stable, diag.error_code_names_are_unique,
 *              diag.error_domain_mapping, diag.consequence_ordering,
 *              diag.consequence_names_are_stable, diag.error_domain_names_are_stable,
 *              diag.result.ok_value, diag.result.err_only, diag.result.layout,
 *              diag.result.value_or_fallback, diag.result.monadic_chaining,
 *              diag.result.void_specialization, diag.result.void_ok_is_truthy,
 *              diag.result.no_throw_on_access, diag.result.move_only_payload,
 *              diag.diagnostic.construction, diag.diagnostic.stable_text,
 *              diag.diagnostic.no_live_references,
 *              diag.diagnostic.severity_is_derived_from_domain,
 *              diag.sink.null_sink, diag.sink.collecting_sink,
 *              diag.sink.collecting_sink_thread_safe,
 *              diag.log.severity_names_are_stable, diag.log.severity_is_total,
 *              diag.log.clock_is_injectable,
 *              diag.log.iso8601_epoch, diag.log.iso8601_width_and_shape,
 *              diag.log.iso8601_is_lexically_sortable, diag.log.iso8601_clamps_millis,
 *              diag.log.json_escape_quotes, diag.log.json_escape_controls,
 *              diag.log.json_escape_plain_text_is_unchanged,
 *              diag.log.json_escape_replaces_invalid_utf8,
 *              diag.log.format_json_minimal, diag.log.format_json_all_fields,
 *              diag.log.format_json_has_trailing_newline,
 *              diag.log.format_json_is_parseable,
 *              diag.log.format_json_escapes_message,
 *              diag.log.file_sink_writes_jsonl, diag.log.file_sink_appends,
 *              diag.log.file_sink_records_are_one_line,
 *              diag.log.file_sink_unopenable_path_is_inert, diag.log.now_is_sane
 */
#pragma once

#include <qp/diag/contract.hpp>
#include <qp/diag/diagnostic.hpp>
#include <qp/diag/error.hpp>
#include <qp/diag/logging.hpp>
#include <qp/diag/result.hpp>
#include <qp/diag/sink.hpp>

namespace qp::diag {

/// @brief ABI version of the diag module. Bump when error code values or the Result shape change.
inline constexpr int kDiagAbiVersion = 1;

}  // namespace qp::diag
