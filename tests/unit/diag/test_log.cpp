/**
 * @file test_log.cpp
 * @brief Tests for the unified logging facade (logging.hpp and logging.cpp).
 *
 * Test case ids match the @tests fields in the diag headers byte for byte.
 *
 * Timestamps are asserted exactly, because time is fully injectable: the
 * formatter takes every time field from its clock. What is asserted for the
 * production clock is only its range, never a value.
 */
#include <catch2/catch_test_macros.hpp>

#include <qp/diag.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

/// @brief Upper bound on a JSON number literal before any conversion.
constexpr std::size_t kMaxNumberChars = 32;

std::string from_u8(const std::u8string& s) {
    return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}

std::string octal_clear_text(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    // Explicit unsigned char: a plain `char` may be signed, and comparing a
    // negative value against 0x20 would be a bug as well as a warning.
    for (const char raw : in) {
        const auto c = static_cast<unsigned char>(raw);
        if (c < 0x20 || c == 0x7F) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\x%02x", static_cast<unsigned>(c));
            out += buf;
        } else {
            out += raw;
        }
    }
    return out;
}

/// @brief Minimal JSON reader: enough to prove a log line is well formed.
///
/// A regex would not do. `\\` and `\"` are indistinguishable to one, so a
/// message containing a backslash would desynchronise it from the real string
/// boundaries and hide exactly the escaping bugs these tests exist to catch.
class JsonReader final {
public:
    explicit JsonReader(std::string_view text) : text_(text) {}

    bool parse_object() {
        skip_space();
        if (!consume('{')) return false;
        if (peek() == '}') {
            ++pos_;
            return true;
        }
        for (;;) {
            skip_space();
            if (!parse_string()) return false;
            skip_space();
            if (!consume(':')) return false;
            if (!parse_value()) return false;
            skip_space();
            if (consume(',')) continue;
            if (consume('}')) return true;
            return false;
        }
    }

    [[nodiscard]] bool at_end() const {
        std::size_t i = pos_;
        while (i < text_.size() && is_space(text_[i])) ++i;
        return i == text_.size();
    }

private:
    static bool is_space(char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r';
    }

    [[nodiscard]] char peek() const { return pos_ < text_.size() ? text_[pos_] : '\0'; }

    void skip_space() {
        while (pos_ < text_.size() && is_space(text_[pos_])) ++pos_;
    }

    bool consume(char expected) {
        if (peek() != expected) return false;
        ++pos_;
        return true;
    }

    bool parse_value() {
        switch (peek()) {
            case '"': return parse_string();
            case '{': return parse_object();
            case 't': return consume_literal("true");
            case 'f': return consume_literal("false");
            case 'n': return consume_literal("null");
            default: return parse_number();
        }
    }

    bool consume_literal(std::string_view literal) {
        if (text_.compare(pos_, literal.size(), literal) != 0) return false;
        pos_ += literal.size();
        return true;
    }

    bool parse_string() {
        if (!consume('"')) return false;
        while (pos_ < text_.size()) {
            const char c = text_[pos_++];
            if (c == '"') return true;
            // A raw control byte inside a string is illegal JSON. This is the
            // check that catches a missing escape.
            if (static_cast<unsigned char>(c) < 0x20) return false;
            if (c != '\\') continue;
            if (pos_ >= text_.size()) return false;
            const char e = text_[pos_++];
            switch (e) {
                case '"': case '\\': case '/': case 'b':
                case 'f': case 'n': case 'r': case 't':
                    break;
                case 'u': {
                    if (pos_ + 4 > text_.size()) return false;
                    for (int i = 0; i < 4; ++i) {
                        const char h = text_[pos_ + static_cast<std::size_t>(i)];
                        const bool hex = (h >= '0' && h <= '9') || (h >= 'a' && h <= 'f') ||
                                         (h >= 'A' && h <= 'F');
                        if (!hex) return false;
                    }
                    pos_ += 4;
                    break;
                }
                default: return false;
            }
        }
        return false;
    }

    bool parse_number() {
        const std::size_t start = pos_;
        if (peek() == '-') ++pos_;
        while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
        if (peek() == '.') {
            ++pos_;
            while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
        }
        if (peek() == 'e' || peek() == 'E') {
            ++pos_;
            if (peek() == '+' || peek() == '-') ++pos_;
            while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
        }
        const std::size_t len = pos_ - start;
        if (len == 0 || len > kMaxNumberChars) return false;
        const std::string literal(text_.substr(start, len));
        char* end = nullptr;
        const double value = std::strtod(literal.c_str(), &end);
        return end == literal.c_str() + literal.size() && std::isfinite(value);
    }

    std::string_view text_;
    std::size_t pos_ = 0;
};

bool is_valid_json_object(const std::string& line) {
    JsonReader reader(line);
    return reader.parse_object() && reader.at_end();
}

/// @brief A deterministic clock: 2026-09-11T22:13:45Z.
///
/// Hard coded rather than computed with mktime, which is local-time dependent
/// and would make the expected strings depend on the machine's time zone.
constexpr qp::diag::UnixTime kFixedEpoch = 1789164825;

qp::diag::Timestamp fixed_clock() noexcept { return qp::diag::Timestamp{kFixedEpoch, 0}; }

/// @brief A clock that advances one second per call.
///
/// Needed by the append test: it writes the same diagnostic twice and compares
/// the two records. With a real clock that comparison races the millisecond
/// counter and fails about one run in a thousand; two distinct but predictable
/// timestamps keep the records identical apart from the timestamp.
qp::diag::Timestamp ticking_clock() noexcept {
    static qp::diag::UnixTime ticks = 0;
    return qp::diag::Timestamp{kFixedEpoch + ticks++, 0};
}

/// @brief Replaces the 24-character `"ts"` value with a placeholder.
std::string strip_timestamp(const std::string& line) {
    const std::string key = "\"ts\":\"";
    const std::size_t begin = line.find(key);
    if (begin == std::string::npos) return line;
    const std::size_t value_begin = begin + key.size();
    const std::size_t value_end = line.find('"', value_begin);
    if (value_end == std::string::npos) return line;
    return line.substr(0, value_begin) + "<ts>" + line.substr(value_end);
}

qp::diag::Diagnostic sample_diagnostic() {
    return qp::diag::Diagnostic{qp::diag::ErrorCode::dimension_mismatch, "expected m/s, got m/s^2",
                                qp::diag::SourceId{"units"}};
}

/// @brief Temporary file that removes itself, so a failing test leaves no litter.
class TempFile final {
public:
    explicit TempFile(const std::string& name) {
        dir_ = std::filesystem::temp_directory_path() /
               ("qp_diag_log_" + std::to_string(static_cast<unsigned long long>(
                                     std::chrono::steady_clock::now().time_since_epoch().count())));
        std::filesystem::create_directories(dir_);
        path_ = dir_ / name;
    }

    ~TempFile() {
        std::error_code ignored;
        std::filesystem::remove_all(dir_, ignored);
    }

    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;

    [[nodiscard]] std::string string_path() const { return from_u8(path_.u8string()); }

    [[nodiscard]] std::string read_all() const {
        std::ifstream in(path_, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }

private:
    std::filesystem::path dir_;
    std::filesystem::path path_;
};

}  // namespace

// ===========================================================================
// Severity classification
// ===========================================================================

TEST_CASE("diag.log.severity_names_are_stable", "[diag][log]") {
    // The three strings are part of the log schema; consumers grep for them.
    STATIC_REQUIRE(qp::diag::to_string(qp::diag::Severity::info) == "info");
    STATIC_REQUIRE(qp::diag::to_string(qp::diag::Severity::warning) == "warning");
    STATIC_REQUIRE(qp::diag::to_string(qp::diag::Severity::error) == "error");
    STATIC_REQUIRE(sizeof(qp::diag::Severity) == 1);
}

TEST_CASE("diag.log.severity_is_total", "[diag][log][property]") {
    using namespace qp::diag;
    // Every code maps to a severity, and severity agrees with the domain.
    const ErrorCode all[] = {
        ErrorCode::ok, ErrorCode::invalid_argument, ErrorCode::malformed_document,
        ErrorCode::unsupported_version, ErrorCode::missing_field, ErrorCode::out_of_range,
        ErrorCode::unknown_port_type, ErrorCode::type_mismatch, ErrorCode::dimension_mismatch,
        ErrorCode::unit_mismatch, ErrorCode::unknown_node, ErrorCode::unknown_port,
        ErrorCode::cycle_detected, ErrorCode::duplicate_connection, ErrorCode::not_connected,
        ErrorCode::graph_busy, ErrorCode::plugin_not_found, ErrorCode::plugin_incompatible,
        ErrorCode::plugin_load_failed, ErrorCode::plugin_capability_missing,
        ErrorCode::run_not_found, ErrorCode::seed_required, ErrorCode::dataset_empty,
        ErrorCode::fit_failed, ErrorCode::internal_error, ErrorCode::not_implemented,
        ErrorCode::cancelled,
    };
    for (const ErrorCode code : all) {
        const Severity s = severity_of(code);
        switch (domain_of(code)) {
            case ErrorDomain::input:
            case ErrorDomain::typing:
                REQUIRE(s == Severity::info);
                break;
            case ErrorDomain::graph:
            case ErrorDomain::plugin:
            case ErrorDomain::runtime:
                REQUIRE(s == Severity::warning);
                break;
            case ErrorDomain::internal:
                REQUIRE(s == Severity::error);
                break;
        }
    }
}

TEST_CASE("diag.diagnostic.severity_is_derived_from_domain", "[diag][log]") {
    using namespace qp::diag;
    const Diagnostic input_problem{ErrorCode::missing_field, "no seed"};
    const Diagnostic typing_problem{ErrorCode::dimension_mismatch, "wrong dimension"};
    const Diagnostic graph_problem{ErrorCode::cycle_detected, "loop"};
    const Diagnostic internal_problem{ErrorCode::internal_error, "invariant broken"};

    REQUIRE(input_problem.severity() == Severity::info);
    REQUIRE(typing_problem.severity() == Severity::info);
    REQUIRE(graph_problem.severity() == Severity::warning);
    REQUIRE(internal_problem.severity() == Severity::error);
}

TEST_CASE("diag.consequence_names_are_stable", "[diag][log]") {
    using namespace qp::diag;
    STATIC_REQUIRE(to_string(Consequence::recoverable) == "recoverable");
    STATIC_REQUIRE(to_string(Consequence::degraded) == "degraded");
    STATIC_REQUIRE(to_string(Consequence::run_aborted) == "run_aborted");
    STATIC_REQUIRE(to_string(Consequence::fatal) == "fatal");
}

TEST_CASE("diag.error_domain_names_are_stable", "[diag][log]") {
    using namespace qp::diag;
    STATIC_REQUIRE(to_string(ErrorDomain::input) == "input");
    STATIC_REQUIRE(to_string(ErrorDomain::typing) == "typing");
    STATIC_REQUIRE(to_string(ErrorDomain::graph) == "graph");
    STATIC_REQUIRE(to_string(ErrorDomain::plugin) == "plugin");
    STATIC_REQUIRE(to_string(ErrorDomain::runtime) == "runtime");
    STATIC_REQUIRE(to_string(ErrorDomain::internal) == "internal");
}

// ===========================================================================
// ISO 8601 timestamps
// ===========================================================================

namespace {

/// @brief Asserts the `YYYY-MM-DDTHH:MM:SS.mmmZ` shape of a timestamp.
void require_iso8601_shape(const std::string& s) {
    REQUIRE(s.size() == 24);
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        const bool digit = c >= '0' && c <= '9';
        switch (i) {
            case 4: case 7: REQUIRE(c == '-'); break;
            case 10: REQUIRE(c == 'T'); break;
            case 13: case 16: REQUIRE(c == ':'); break;
            case 19: REQUIRE(c == '.'); break;
            case 23: REQUIRE(c == 'Z'); break;
            default: REQUIRE(digit); break;
        }
    }
}

std::string digits_at(const std::string& s, std::size_t pos, std::size_t count) {
    return s.substr(pos, count);
}

}  // namespace

TEST_CASE("diag.log.iso8601_epoch", "[diag][log]") {
    REQUIRE(qp::diag::to_iso8601_utc(0, 0) == "1970-01-01T00:00:00.000Z");
    REQUIRE(qp::diag::to_iso8601_utc(0, 1) == "1970-01-01T00:00:00.001Z");
    REQUIRE(qp::diag::to_iso8601_utc(1, 0) == "1970-01-01T00:00:01.000Z");
    REQUIRE(qp::diag::to_iso8601_utc(59, 999) == "1970-01-01T00:00:59.999Z");
    // 1970-01-02T00:00:00Z
    REQUIRE(qp::diag::to_iso8601_utc(86400, 0) == "1970-01-02T00:00:00.000Z");
}

TEST_CASE("diag.log.iso8601_width_and_shape", "[diag][log]") {
    const qp::diag::UnixTime samples[] = {0, 1, 999, 1'000'000, 1'789'683'225, 4'102'444'800};
    for (const qp::diag::UnixTime t : samples) {
        const std::string s = qp::diag::to_iso8601_utc(t, 7);
        INFO("t=" << t << " s=" << s);
        require_iso8601_shape(s);
        // Fixed width is the whole point: month and day must be zero padded.
        REQUIRE(digits_at(s, 5, 2) != "  ");
    }

    // Zero padding is observable in the first decade of the epoch.
    REQUIRE(qp::diag::to_iso8601_utc(0, 5)[20] == '0');
    REQUIRE(qp::diag::to_iso8601_utc(0, 5)[21] == '0');
    REQUIRE(qp::diag::to_iso8601_utc(0, 5)[22] == '5');
}

TEST_CASE("diag.log.iso8601_is_lexically_sortable", "[diag][log][property]") {
    // Fixed width means string comparison equals chronological comparison. This
    // is what lets a log file be sorted as plain text.
    const qp::diag::UnixTime sorted[] = {0,       1,          59,        60,
                                         3600,    86'399,     86'400,    1'000'000,
                                         1'789'683'225};
    for (std::size_t i = 0; i + 1 < std::size(sorted); ++i) {
        const std::string a = qp::diag::to_iso8601_utc(sorted[i], 999);
        const std::string b = qp::diag::to_iso8601_utc(sorted[i + 1], 0);
        INFO("a=" << a << " b=" << b);
        REQUIRE(a < b);
    }

    // Milliseconds sort within a second too.
    REQUIRE(qp::diag::to_iso8601_utc(100, 0) < qp::diag::to_iso8601_utc(100, 1));
    REQUIRE(qp::diag::to_iso8601_utc(100, 9) < qp::diag::to_iso8601_utc(100, 10));
    REQUIRE(qp::diag::to_iso8601_utc(100, 999) < qp::diag::to_iso8601_utc(101, 0));
}

TEST_CASE("diag.log.iso8601_clamps_millis", "[diag][log]") {
    // Out-of-range milliseconds are clamped rather than rejected: a log line is
    // diagnostic data, and a bad counter must not produce malformed output.
    REQUIRE(qp::diag::to_iso8601_utc(0, -1) == qp::diag::to_iso8601_utc(0, 0));
    REQUIRE(qp::diag::to_iso8601_utc(0, 1000) == qp::diag::to_iso8601_utc(0, 999));
    REQUIRE(qp::diag::to_iso8601_utc(0, 123456) == qp::diag::to_iso8601_utc(0, 999));
    require_iso8601_shape(qp::diag::to_iso8601_utc(0, 1000));
}

// ===========================================================================
// JSON escaping
// ===========================================================================

TEST_CASE("diag.log.json_escape_quotes", "[diag][log]") {
    using qp::diag::json_escape;
    REQUIRE(json_escape("") == "");
    REQUIRE(json_escape("plain") == "plain");
    REQUIRE(json_escape("\"") == "\\\"");
    REQUIRE(json_escape("a\"b") == "a\\\"b");
    REQUIRE(json_escape("\\") == "\\\\");
    // A backslash-escaped quote must stay two characters apart, not merge.
    REQUIRE(json_escape("\\\"") == "\\\\\\\"");
    REQUIRE(json_escape("say \"hi\"") == "say \\\"hi\\\"");
}

TEST_CASE("diag.log.json_escape_controls", "[diag][log]") {
    using qp::diag::json_escape;
    REQUIRE(json_escape("\n") == "\\n");
    REQUIRE(json_escape("\r") == "\\r");
    REQUIRE(json_escape("\t") == "\\t");
    REQUIRE(json_escape("\b") == "\\b");
    REQUIRE(json_escape("\f") == "\\f");
    REQUIRE(json_escape(std::string("\0", 1)) == "\\u0000");
    REQUIRE(json_escape(std::string("\x01", 1)) == "\\u0001");
    REQUIRE(json_escape(std::string("\x1f", 1)) == "\\u001f");
    REQUIRE(json_escape(std::string("\x7f", 1)) == std::string("\x7f", 1));  // DEL is legal raw

    // No output may contain a raw control byte.
    const std::string hostile = "a\nb\tc\rd\x01e";
    const std::string escaped = json_escape(hostile);
    for (const char raw : escaped) {
        REQUIRE(static_cast<unsigned char>(raw) >= 0x20);
    }
}

TEST_CASE("diag.log.json_escape_plain_text_is_unchanged", "[diag][log][property]") {
    using qp::diag::json_escape;
    // Digits, punctuation (other than quote and backslash) and UTF-8 must pass
    // through byte for byte: otherwise every log line grows for no reason.
    const std::string plain =
        "node 3: gain = 4.5e-3; port [in.0] -> {out}; path C:/tmp/x.json; \xe2\x88\x86t = 1";
    REQUIRE(json_escape(plain) == plain);

    // Valid multi-byte sequences survive exactly.
    const std::string utf8 = "\xc3\xa9\xe2\x82\xac\xf0\x9f\x94\xac";  // e-acute, euro, a 4-byte char
    REQUIRE(json_escape(utf8) == utf8);
    // A 2-byte sequence that is the shortest encoding of U+0080 is legal.
    REQUIRE(json_escape("\xc2\x80") == "\xc2\x80");
}

TEST_CASE("diag.log.json_escape_replaces_invalid_utf8", "[diag][log]") {
    using qp::diag::json_escape;
    // A lone continuation byte.
    REQUIRE(json_escape("\x80") == "?");
    // A truncated 3-byte sequence: two replacement characters, one per bad byte.
    REQUIRE(json_escape("\xe2\x82") == "??");
    // An overlong encoding of '/' is not legal UTF-8.
    REQUIRE(json_escape("\xc0\xaf") == "??");
    // Surrogate half U+D800.
    REQUIRE(json_escape("\xed\xa0\x80") == "???");
    // Code point above U+10FFFF.
    REQUIRE(json_escape("\xf5\x80\x80\x80") == "????");
    // Valid text around an invalid byte keeps its valid parts.
    REQUIRE(json_escape("a\x80z") == "a?z");
    REQUIRE(json_escape("caf\xc3\xa9\x80") == "caf\xc3\xa9?");
}

// ===========================================================================
// JSON record shape
// ===========================================================================

TEST_CASE("diag.log.format_json_has_trailing_newline", "[diag][log]") {
    const qp::diag::JsonFormatter formatter{&fixed_clock};
    const std::string line = formatter.format(sample_diagnostic());
    REQUIRE_FALSE(line.empty());
    REQUIRE(line.back() == '\n');
    // Exactly one newline: the record is one line, and any newline inside a
    // value must have been escaped.
    REQUIRE(std::count(line.begin(), line.end(), '\n') == 1);
}

TEST_CASE("diag.log.format_json_minimal", "[diag][log]") {
    const qp::diag::JsonFormatter formatter{&fixed_clock};
    const qp::diag::Diagnostic d{qp::diag::ErrorCode::cycle_detected, "loop"};
    const std::string line = formatter.format(d);

    // Exact byte-for-byte comparison, including the timestamp and its field
    // order. This is the test that fails if time is only half injectable.
    const std::string expected =
        "{\"ts\":\"2026-09-11T22:13:45.000Z\",\"level\":\"warning\","
        "\"code\":\"cycle_detected\",\"domain\":\"graph\",\"source\":\"\","
        "\"consequence\":\"degraded\",\"msg\":\"loop\"}\n";
    REQUIRE(line == expected);
}

TEST_CASE("diag.log.clock_is_injectable", "[diag][log]") {
    // The formatter must take **every** time field from the injected clock, not
    // just the seconds. If it reads the milliseconds from the wall clock, two
    // consecutive records differ in a way no test can predict, and the whole
    // `ts` field stops being assertable.
    struct Advancing {
        static qp::diag::Timestamp next() noexcept {
            static int calls = 0;
            // 27 calls per second: two records 27 ms apart whatever the machine
            // is doing, and no dependence on how fast the test runs.
            const int step = calls++;
            return qp::diag::Timestamp{kFixedEpoch, step * 27};
        }
    };

    const qp::diag::JsonFormatter formatter{&Advancing::next};
    const qp::diag::Diagnostic d{qp::diag::ErrorCode::cycle_detected, "loop"};

    const std::string first = formatter.format(d);
    const std::string second = formatter.format(d);

    REQUIRE(first.find("\"ts\":\"2026-09-11T22:13:45.000Z\"") != std::string::npos);
    REQUIRE(second.find("\"ts\":\"2026-09-11T22:13:45.027Z\"") != std::string::npos);
    // Everything except the timestamp is identical.
    REQUIRE(strip_timestamp(first) == strip_timestamp(second));

    // A default-constructed formatter falls back to the real clock and still
    // produces a well-shaped record.
    const qp::diag::JsonFormatter real;
    const std::string line = real.format(d);
    REQUIRE(line.size() > 60);
    REQUIRE(is_valid_json_object(line.substr(0, line.size() - 1)));
}

TEST_CASE("diag.log.format_json_all_fields", "[diag][log]") {
    const qp::diag::JsonFormatter formatter{&fixed_clock};
    const qp::diag::Diagnostic d{
        qp::diag::ErrorCode::dimension_mismatch, "expected m/s, got m/s^2",
        qp::diag::SourceId{"units"}, qp::diag::Consequence::run_aborted};
    const std::string line = formatter.format(d);

    REQUIRE(line.find("\"level\":\"info\"") != std::string::npos);
    REQUIRE(line.find("\"code\":\"dimension_mismatch\"") != std::string::npos);
    REQUIRE(line.find("\"domain\":\"typing\"") != std::string::npos);
    REQUIRE(line.find("\"source\":\"units\"") != std::string::npos);
    REQUIRE(line.find("\"consequence\":\"run_aborted\"") != std::string::npos);
    REQUIRE(line.find("\"msg\":\"expected m/s, got m/s^2\"") != std::string::npos);
    REQUIRE(line.find("\"ts\":\"2026-09-11T22:13:45.000Z\"") != std::string::npos);

    // Every documented field is present exactly once.
    for (const char* field : {"ts", "level", "code", "domain", "source", "consequence", "msg"}) {
        const std::string needle = std::string("\"") + field + "\":";
        const auto first = line.find(needle);
        REQUIRE(first != std::string::npos);
        REQUIRE(line.find(needle, first + 1) == std::string::npos);
    }
}

TEST_CASE("diag.log.format_json_is_parseable", "[diag][log]") {
    const qp::diag::JsonFormatter formatter{&fixed_clock};

    // Adversarial messages: a naive escaper fails at least one of these.
    const std::vector<std::string> messages = {
        "",
        "plain",
        "quote \" inside",
        "backslash \\ inside",
        "both \\\" together",
        "line\nbreak",
        "tab\tand\r\ncrlf",
        std::string("control \x01 and \x1f here"),
        "utf8 \xc3\xa9\xe2\x82\xac",
        "invalid \x80 byte",
        "trailing backslash \\",
    };

    for (const std::string& message : messages) {
        const qp::diag::Diagnostic d{qp::diag::ErrorCode::fit_failed, message,
                                     qp::diag::SourceId{"fit\""}};
        const std::string line = formatter.format(d);
        INFO("message=[" << octal_clear_text(message) << "] line=[" << octal_clear_text(line)
                         << "]");
        // Strip the single trailing newline, then require a complete object.
        REQUIRE_FALSE(line.empty());
        REQUIRE(line.back() == '\n');
        const std::string body = line.substr(0, line.size() - 1);
        REQUIRE(body.find('\n') == std::string::npos);
        REQUIRE(is_valid_json_object(body));
    }
}

TEST_CASE("diag.log.format_json_escapes_message", "[diag][log]") {
    const qp::diag::JsonFormatter formatter{&fixed_clock};
    const qp::diag::Diagnostic d{qp::diag::ErrorCode::internal_error, "a\"b\\c\nd",
                                 qp::diag::SourceId{"core"}};
    const std::string line = formatter.format(d);

    REQUIRE(line.find("\"msg\":\"a\\\"b\\\\c\\nd\"") != std::string::npos);
    // The raw forms must not appear anywhere.
    REQUIRE(line.find("a\"b") == std::string::npos);
    REQUIRE(line.find("c\nd") == std::string::npos);
}

// ===========================================================================
// File sink
// ===========================================================================

TEST_CASE("diag.log.file_sink_writes_jsonl", "[diag][log]") {
    const TempFile file{"run.jsonl"};
    {
        qp::diag::JsonFileSink sink{file.string_path(), &fixed_clock};
        REQUIRE(sink.is_open());
        REQUIRE(sink.written() == 0);
        REQUIRE(std::string(sink.name()) == "json_file");

        sink.emit(sample_diagnostic());
        sink.emit(qp::diag::Diagnostic{qp::diag::ErrorCode::cycle_detected, "loop"});
        REQUIRE(sink.written() == 2);
    }

    // Read only after the sink is destroyed. On Windows a second handle cannot
    // reliably read a file that is still open for append.
    const std::string content = file.read_all();
    REQUIRE_FALSE(content.empty());
    REQUIRE(content.back() == '\n');
    REQUIRE(std::count(content.begin(), content.end(), '\n') == 2);

    std::size_t start = 0;
    int records = 0;
    while (start < content.size()) {
        const std::size_t end = content.find('\n', start);
        REQUIRE(end != std::string::npos);
        const std::string line = content.substr(start, end - start);
        INFO("line=[" << octal_clear_text(line) << "]");
        REQUIRE(is_valid_json_object(line));
        ++records;
        start = end + 1;
    }
    REQUIRE(records == 2);
}

TEST_CASE("diag.log.file_sink_appends", "[diag][log]") {
    const TempFile file{"append.jsonl"};
    const qp::diag::Diagnostic d{qp::diag::ErrorCode::run_not_found, "no such run"};

    {
        qp::diag::JsonFileSink first{file.string_path(), &ticking_clock};
        REQUIRE(first.is_open());
        first.emit(d);
        REQUIRE(first.written() == 1);
    }
    {
        // A second process-run must accumulate evidence, not overwrite it.
        qp::diag::JsonFileSink second{file.string_path(), &ticking_clock};
        REQUIRE(second.is_open());
        second.emit(d);
        REQUIRE(second.written() == 1);
    }

    const std::string content = file.read_all();
    REQUIRE(std::count(content.begin(), content.end(), '\n') == 2);
    const std::size_t first_end = content.find('\n');
    const std::size_t second_end = content.find('\n', first_end + 1);
    const std::string first_record = content.substr(0, first_end);
    const std::string second_record =
        content.substr(first_end + 1, second_end - first_end - 1);

    // The first run is still there byte for byte, with the second appended after
    // it. The two records differ only in the timestamp, which is exactly the
    // property "append" has to have.
    REQUIRE(first_record != second_record);
    REQUIRE(strip_timestamp(first_record) == strip_timestamp(second_record));
    REQUIRE(first_record.find("\"code\":\"run_not_found\"") != std::string::npos);
}

TEST_CASE("diag.log.file_sink_records_are_one_line", "[diag][log]") {
    const TempFile file{"oneline.jsonl"};
    {
        qp::diag::JsonFileSink sink{file.string_path(), &fixed_clock};
        REQUIRE(sink.is_open());

        // A message that would split the file if escaping were skipped.
        sink.emit(qp::diag::Diagnostic{qp::diag::ErrorCode::fit_failed,
                                       "line one\nline two\r\n{\"fake\":\"record\"}"});
        sink.emit(qp::diag::Diagnostic{qp::diag::ErrorCode::dataset_empty, ""});
        REQUIRE(sink.written() == 2);
    }

    const std::string content = file.read_all();
    REQUIRE(std::count(content.begin(), content.end(), '\n') == 2);
    // The injected fake record must appear escaped, not as a field.
    REQUIRE(content.find("\\nline two") != std::string::npos);
    REQUIRE(content.find("{\"fake\"") == std::string::npos);
    REQUIRE(content.find("\\\"fake\\\"") != std::string::npos);
}

TEST_CASE("diag.log.file_sink_unopenable_path_is_inert", "[diag][log]") {
    // Logging must never become a failure source: a bad path yields an inert
    // sink, not an exception and not a crash.
    const TempFile dir{"sub"};
    const std::string as_directory = from_u8(
        (std::filesystem::path(dir.string_path()).parent_path() / "no_such_dir").u8string());

    qp::diag::JsonFileSink sink{as_directory + "/nested/run.jsonl", &fixed_clock};
    REQUIRE_FALSE(sink.is_open());
    sink.emit(sample_diagnostic());  // Must be a no-op.
    REQUIRE(sink.written() == 0);
}

TEST_CASE("diag.log.now_is_sane", "[diag][log]") {
    // Guards against a unit mix-up (seconds vs milliseconds) in the clock.
    const qp::diag::UnixTime now = qp::diag::now_unix_seconds();
    REQUIRE(now > 1'600'000'000);   // after 2020-09-13
    REQUIRE(now < 4'102'444'800);   // before 2100-01-01

    const int millis = qp::diag::now_unix_millis();
    REQUIRE(millis >= 0);
    REQUIRE(millis <= 999);

    // Both spellings of the production clock agree, and the millisecond field
    // stays in range.
    const qp::diag::Timestamp a = qp::diag::system_clock();
    const qp::diag::Timestamp b = qp::diag::system_clock_seconds();
    REQUIRE(a.seconds > 1'600'000'000);
    REQUIRE(a.millis >= 0);
    REQUIRE(a.millis <= 999);
    REQUIRE(b.seconds > 1'600'000'000);
}
