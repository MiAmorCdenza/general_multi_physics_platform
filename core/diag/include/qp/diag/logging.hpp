/**
 * @file logging.hpp
 * @brief Unified logging: one JSON Lines record per event.
 *
 * ## Why JSON Lines
 *
 * A log line has to serve three consumers at once: a human reading a
 * terminal, a script grouping by severity, and a bug report that must be
 * reproducible from its log. Free-form text serves only the first.
 *
 * JSON Lines (one complete JSON object per line) keeps the file streamable
 * and appendable while remaining trivially parseable:
 *
 * ```text
 * {"ts":"2026-09-11T22:13:45.123Z","level":"error","code":"dimension_mismatch",
 *  "domain":"typing","source":"validate","consequence":"recoverable","msg":"..."}
 * ```
 *
 * One record per line means a truncated final line costs exactly one
 * record, never the whole file. A JSON array would not survive truncation.
 *
 * ## Field contract
 *
 * | Field | Type | Meaning |
 * |---|---|---|
 * | `ts` | string | ISO 8601 UTC, milliseconds, fixed width |
 * | `level` | string | `info` / `warning` / `error` |
 * | `code` | string | stable identifier; safe to grep and alert on |
 * | `domain` | string | which subsystem the error came from |
 * | `source` | string | plugin or module that emitted it |
 * | `consequence` | string | how badly the run is affected |
 * | `msg` | string | human-readable detail |
 *
 * `code` is the machine key; `msg` is for humans and may be reworded. Never
 * alert on `msg`.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   Every record is exactly one line and parses as JSON
 * @errors      noexcept
 * @frozen      yes; the field set is part of the log schema
 */
#pragma once

#include <qp/diag/diagnostic.hpp>
#include <qp/diag/error.hpp>
#include <qp/diag/sink.hpp>

#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>
#include <string_view>

namespace qp::diag {

/// @brief Seconds since the Unix epoch, UTC. Negative values are pre-1970.
using UnixTime = std::int64_t;

/**
 * @brief Log severity, as written to the `level` field.
 *
 * Severity is a **presentation** classification: it decides which stream a
 * record is routed to and how loudly a viewer shows it. It is not a control
 * signal — `Consequence` is what the host acts on. Deriving severity from the
 * error domain keeps the two consistent instead of letting every call site
 * invent its own level.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   Mapping from ErrorCode is total and stable
 * @errors      noexcept
 * @frozen      yes (the strings are part of the log schema)
 * @tests       diag.log.severity_names_are_stable, diag.log.severity_is_total
 */
enum class Severity : std::uint8_t {
    /// Normal operation or a user-correctable input problem.
    info = 0,
    /// Something degraded but the run can continue.
    warning = 1,
    /// The run or the process state is compromised.
    error = 2,
};

/// @brief Stable short name for `Severity`, as written to the log.
[[nodiscard]] constexpr std::string_view to_string(Severity s) noexcept {
    switch (s) {
        case Severity::info: return "info";
        case Severity::warning: return "warning";
        case Severity::error: return "error";
    }
    return "unknown";
}

/**
 * @brief Classifies an error code for logging.
 *
 * Input and typing problems are reported at `info`: from the platform's point
 * of view they are the expected result of a user editing a graph, not an
 * incident. Graph, plugin and runtime problems degrade or abort a run, so they
 * are `warning`. Internal errors compromise process state and are `error`.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        Returns a value for every ErrorDomain
 * @invariant   Same code always yields the same severity
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       diag.log.severity_is_total
 */
[[nodiscard]] constexpr Severity severity_of(ErrorCode code) noexcept {
    switch (domain_of(code)) {
        case ErrorDomain::input:
        case ErrorDomain::typing:
            return Severity::info;
        case ErrorDomain::graph:
        case ErrorDomain::plugin:
        case ErrorDomain::runtime:
            return Severity::warning;
        case ErrorDomain::internal:
            return Severity::error;
    }
    return Severity::error;
}

/**
 * @brief Formats a timestamp as ISO 8601 UTC with millisecond precision.
 *
 * Shape: `2026-09-11T22:13:45.123Z`
 *
 * Three deliberate properties:
 *   - **UTC only.** Local-time logs cannot be correlated across machines and
 *     their ordering changes across a DST boundary.
 *   - **Fixed width.** Every field is zero padded, so lexical order equals
 *     chronological order; the log file is sortable as plain text.
 *   - Millisecond precision is the ceiling: the current 32-bit toolchain has
 *     one-second `time_t` resolution, so finer precision would be fabricated.
 *
 * @ownership   pure
 * @thread      any
 * @pre         millis is in [0, 999]; out-of-range values are clamped
 * @post        24-character string ending in 'Z'
 * @invariant   Same input yields the same string
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      yes
 * @tests       diag.log.iso8601_epoch, diag.log.iso8601_width_and_shape,
 *              diag.log.iso8601_is_lexically_sortable,
 *              diag.log.iso8601_clamps_millis
 */
[[nodiscard]] std::string to_iso8601_utc(UnixTime seconds, int millis = 0) noexcept;

/// @brief Current wall-clock seconds. Separated so tests can control time.
[[nodiscard]] UnixTime now_unix_seconds() noexcept;

/// @brief Milliseconds within the current second, in [0, 999].
[[nodiscard]] int now_unix_millis() noexcept;

/**
 * @brief Escapes a string for embedding in a JSON string literal (RFC 8259).
 *
 * Escapes `"` and `\`, uses the short form for control characters that have
 * one (`\n`, `\r`, `\t`, `\b`, `\f`) and `\u00XX` for the rest. Bytes that
 * are not valid UTF-8 become `?`: emitting invalid UTF-8 would produce a
 * file no JSON parser accepts, which is worse than losing one character.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        Result contains no raw control byte and no unescaped quote
 * @invariant   Safe input is returned unchanged
 * @errors      noexcept
 * @complexity  O(len)
 * @nondet      none
 * @frozen      no
 * @tests       diag.log.json_escape_quotes, diag.log.json_escape_controls,
 *              diag.log.json_escape_plain_text_is_unchanged,
 *              diag.log.json_escape_replaces_invalid_utf8
 */
[[nodiscard]] std::string json_escape(const std::string& in) noexcept;

/**
 * @brief Whether every byte of `in` is part of a legal UTF-8 sequence.
 *
 * The predicate `json_escape` acts on, exposed because a caller that must not lose data has to ask
 * *before* escaping. Escaping maps an illegal byte to `?`, which is the right answer for a log line --
 * a log nobody can parse loses everything -- and the wrong answer for a document, where it is a silent
 * edit to the user's data. Such a caller refuses the string instead, and needs this to decide.
 *
 * One place decides legality, so the two cannot disagree: the sequence checks below are the same code
 * `json_escape` uses, not a second opinion. A string this rejects is exactly a string escaping would
 * modify -- asserted, for inputs with nothing else to escape, by
 * `diag.log.valid_utf8_agrees_with_json_escape`.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        true exactly when the bytes are well-formed UTF-8: no truncated sequence, no overlong
 *              encoding, no surrogate, no code point above U+10FFFF, no stray continuation byte
 * @invariant   Depends only on the bytes
 * @errors      noexcept
 * @complexity  O(len)
 * @nondet      none
 * @frozen      no
 * @tests       diag.log.valid_utf8_accepts_legal_sequences,
 *              diag.log.valid_utf8_rejects_illegal_ones,
 *              diag.log.valid_utf8_agrees_with_json_escape
 */
[[nodiscard]] bool is_valid_utf8(std::string_view in) noexcept;

/**
 * @brief One instant, split into whole seconds and milliseconds within them.
 *
 * @ownership   pure (a value type)
 * @thread      any
 * @pre         millis is in [0, 999]
 * @post        none
 * @invariant   none
 * @errors      noexcept
 * @frozen      yes
 * @tests       diag.log.clock_is_injectable
 */
struct Timestamp final {
    UnixTime seconds = 0;
    int millis = 0;
};

/**
 * @brief Clock used by the JSON formatter.
 *
 * A function pointer rather than a virtual interface: the formatter stores one
 * and calls it once per record, and a pointer is trivially copyable, which
 * keeps JsonFileSink copyable-by-value semantics out of the picture.
 *
 * Returning both fields is deliberate. An earlier version returned only the
 * seconds and read the milliseconds from the wall clock directly, which made
 * the injected clock only half a clock: every formatted record still varied
 * between runs, so a test could not assert a full log line and the append test
 * failed roughly one run in a thousand when two records landed in different
 * milliseconds. Injecting time has to mean injecting *all* of it.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   none
 * @errors      noexcept
 * @frozen      no
 * @tests       diag.log.clock_is_injectable
 */
using Clock = Timestamp (*)() noexcept;

/// @brief Current wall-clock time, split as Timestamp documents.
[[nodiscard]] Timestamp system_clock() noexcept;

/// @brief Current wall-clock seconds, Unix epoch. Kept for callers that only
///        need the seconds and for the tests that pin the real clock's range.
[[nodiscard]] UnixTime now_unix_seconds() noexcept;

/// @brief Milliseconds within the current second, in [0, 999].
[[nodiscard]] int now_unix_millis() noexcept;

/// @brief The clock used by the JSON formatter in production.
[[nodiscard]] Timestamp system_clock_seconds() noexcept;

/**
 * @brief Renders diagnostics as JSON Lines.
 *
 * @ownership   owns
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   Output contains one '\n' per formatted record and nothing else
 * @errors      noexcept
 * @frozen      yes
 * @tests       diag.log.format_json_minimal, diag.log.format_json_all_fields,
 *              diag.log.format_json_has_trailing_newline,
 *              diag.log.format_json_is_parseable,
 *              diag.log.format_json_escapes_message
 */
class JsonFormatter final {
public:
    /// @brief `clock` may be null, in which case the system clock is used.
    explicit JsonFormatter(Clock clock = nullptr) noexcept
        : clock_(clock == nullptr ? &system_clock_seconds : clock) {}

    /**
     * @brief Formats one diagnostic as a single JSON line.
     *
     * @ownership   pure
     * @thread      any
     * @pre         none
     * @post        Returned string ends with '\n' and contains no other '\n'
     *              except the escaped ones inside string values
     * @invariant   Field order is stable so logs diff cleanly
     * @errors      noexcept; allocation failure terminates, because a log
     *              record that cannot be built must not become a new failure
     *              source inside error reporting
     * @complexity  O(len)
     * @nondet      only through the injected clock
     * @frozen      no
     * @tests       diag.log.format_json_minimal, diag.log.format_json_all_fields
     */
    [[nodiscard]] std::string format(const Diagnostic& d) const noexcept;

private:
    Clock clock_;
};

/**
 * @brief Appends JSON Lines records to a file.
 *
 * Opens in append mode so that consecutive runs accumulate into one file
 * instead of overwriting evidence. Flushes after every record: a crash is
 * exactly the situation where the log matters most, and a buffered record
 * would be lost.
 *
 * @ownership   owns
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   Each write appends exactly one line
 * @errors      noexcept; a failed open leaves the sink inert rather than
 *              throwing, because logging must never become a failure source
 * @frozen      no
 * @tests       diag.log.file_sink_writes_jsonl, diag.log.file_sink_appends,
 *              diag.log.file_sink_records_are_one_line
 */
class JsonFileSink final : public ISink {
public:
    /// @brief Opens `path` for append. Check `is_open()` before relying on it.
    explicit JsonFileSink(const std::string& path, Clock clock = nullptr) noexcept;

    ~JsonFileSink() override;

    JsonFileSink(const JsonFileSink&) = delete;
    JsonFileSink& operator=(const JsonFileSink&) = delete;

    void emit(const Diagnostic& d) noexcept override;
    [[nodiscard]] const char* name() const noexcept override { return "json_file"; }

    [[nodiscard]] bool is_open() const noexcept { return file_ != nullptr; }
    [[nodiscard]] std::uint64_t written() const noexcept { return written_; }

private:
    std::FILE* file_ = nullptr;
    JsonFormatter formatter_;
    std::uint64_t written_ = 0;
};

}  // namespace qp::diag
