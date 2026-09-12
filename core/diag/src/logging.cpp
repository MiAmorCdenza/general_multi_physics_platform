/**
 * @file logging.cpp
 * @brief Implementation of the JSON Lines logging facade.
 */
#include <qp/diag/logging.hpp>

#include <chrono>
#include <cstdio>
#include <ctime>

namespace qp::diag {
namespace {

/// @brief Portable UTC breakdown. Returns false when the input is unusable.
///
/// Keyed on the platform rather than the compiler: MinGW defines `_MSC_VER`
/// for compatibility but does not provide `gmtime_s`, only `gmtime_r`.
bool to_utc_tm(UnixTime seconds, std::tm& out) noexcept {
    const auto t = static_cast<std::time_t>(seconds);
#if defined(_WIN32)
    return ::gmtime_s(&out, &t) == 0;
#else
    return ::gmtime_r(&t, &out) != nullptr;
#endif
}

/// @brief Appends an unsigned value, zero padded to `width`.
void append_padded(std::string& out, unsigned value, int width) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%0*u", width, value);
    out += buf;
}

/// @brief Number of bytes in the UTF-8 sequence introduced by `lead`.
///
/// Returns 0 when `lead` cannot start a sequence, which also covers the
/// continuation-byte and 0xF8..0xFF ranges.
int utf8_sequence_length(unsigned char lead) noexcept {
    if (lead < 0x80) return 1;
    if (lead < 0xC2) return 0;   // continuation byte, or an overlong 2-byte lead
    if (lead < 0xE0) return 2;
    if (lead < 0xF0) return 3;
    if (lead < 0xF5) return 4;
    return 0;
}

/// @brief True when `c` is a UTF-8 continuation byte (10xxxxxx).
bool is_continuation(unsigned char c) noexcept { return (c & 0xC0) == 0x80; }

/// @brief Whether the `len` bytes starting at `i` form a legal UTF-8 sequence.
///
/// The one place legality is decided. `json_escape` replaces an illegal byte with `?` and
/// `is_valid_utf8` refuses a string that contains one; a second copy of these rules would let the two
/// drift, and the drift would look like a document that validates and then saves corrupted.
bool is_legal_sequence(std::string_view in, std::size_t i, int len) noexcept {
    if (len <= 1) return false;   // 0 means the lead byte cannot start a sequence
    if (i + static_cast<std::size_t>(len) > in.size()) return false;   // truncated
    for (int k = 1; k < len; ++k) {
        if (!is_continuation(static_cast<unsigned char>(in[i + static_cast<std::size_t>(k)]))) {
            return false;
        }
    }
    // Reject overlong encodings, surrogates and out-of-range code points, which
    // are structurally well formed but not legal UTF-8.
    const auto c = static_cast<unsigned char>(in[i]);
    if (len == 3) {
        const auto c1 = static_cast<unsigned char>(in[i + 1]);
        if (c == 0xE0 && c1 < 0xA0) return false;
        if (c == 0xED && c1 > 0x9F) return false;
    } else if (len == 4) {
        const auto c1 = static_cast<unsigned char>(in[i + 1]);
        if (c == 0xF0 && c1 < 0x90) return false;
        if (c == 0xF4 && c1 > 0x8F) return false;
    }
    return true;
}

}  // namespace

Severity Diagnostic::severity() const noexcept { return severity_of(code_); }

std::string to_iso8601_utc(UnixTime seconds, int millis) noexcept {
    // Clamp rather than reject: a log timestamp is diagnostic data, and a
    // caller with a bad millisecond counter should still get a well-formed
    // record instead of an exception in the middle of error reporting.
    if (millis < 0) millis = 0;
    if (millis > 999) millis = 999;

    std::tm tmv{};
    if (!to_utc_tm(seconds, tmv)) {
        // Out-of-range timestamps still produce a well-formed sentinel. Fixing
        // the clock is the caller's problem; emitting malformed JSON here would
        // break every downstream parser.
        return "1970-01-01T00:00:00.000Z";
    }

    std::string out;
    out.reserve(24);
    append_padded(out, static_cast<unsigned>(tmv.tm_year + 1900), 4);
    out += '-';
    append_padded(out, static_cast<unsigned>(tmv.tm_mon + 1), 2);
    out += '-';
    append_padded(out, static_cast<unsigned>(tmv.tm_mday), 2);
    out += 'T';
    append_padded(out, static_cast<unsigned>(tmv.tm_hour), 2);
    out += ':';
    append_padded(out, static_cast<unsigned>(tmv.tm_min), 2);
    out += ':';
    append_padded(out, static_cast<unsigned>(tmv.tm_sec), 2);
    out += '.';
    append_padded(out, static_cast<unsigned>(millis), 3);
    out += 'Z';
    return out;
}

UnixTime now_unix_seconds() noexcept {
    return static_cast<UnixTime>(std::time(nullptr));
}

int now_unix_millis() noexcept {
    using namespace std::chrono;
    const auto since_epoch = system_clock::now().time_since_epoch();
    const auto ms = duration_cast<milliseconds>(since_epoch).count();
    auto remainder = static_cast<long long>(ms % 1000);
    if (remainder < 0) remainder += 1000;
    return static_cast<int>(remainder);
}

Timestamp system_clock() noexcept {
    return Timestamp{now_unix_seconds(), now_unix_millis()};
}

Timestamp system_clock_seconds() noexcept { return system_clock(); }

std::string json_escape(const std::string& in) noexcept {
    std::string out;
    out.reserve(in.size() + 8);

    std::size_t i = 0;
    while (i < in.size()) {
        const auto c = static_cast<unsigned char>(in[i]);

        // ASCII fast path. Everything below 0x80 is either copied verbatim or
        // escaped, and nothing here can split a multi-byte sequence.
        if (c < 0x80) {
            switch (c) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                case '\b': out += "\\b"; break;
                case '\f': out += "\\f"; break;
                default:
                    if (c < 0x20) {
                        char buf[8];
                        std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                        out += buf;
                    } else {
                        out += static_cast<char>(c);
                    }
                    break;
            }
            ++i;
            continue;
        }

        // Validate the whole sequence before copying it. Passing invalid bytes
        // through would produce a file no JSON parser accepts, which loses the
        // entire log rather than one character.
        const int len = utf8_sequence_length(c);
        if (is_legal_sequence(in, i, len)) {
            out.append(in, i, static_cast<std::size_t>(len));
            i += static_cast<std::size_t>(len);
        } else {
            // One replacement character per invalid byte. Collapsing a run
            // would hide how much data was lost.
            out += '?';
            ++i;
        }
    }
    return out;
}

bool is_valid_utf8(std::string_view in) noexcept {
    std::size_t i = 0;
    while (i < in.size()) {
        const auto c = static_cast<unsigned char>(in[i]);
        if (c < 0x80) {
            ++i;
            continue;
        }
        const int len = utf8_sequence_length(c);
        if (!is_legal_sequence(in, i, len)) return false;
        i += static_cast<std::size_t>(len);
    }
    return true;
}

std::string JsonFormatter::format(const Diagnostic& d) const noexcept {
    const Timestamp now = clock_();
    std::string out;
    out.reserve(256);
    out += "{\"ts\":\"";
    out += to_iso8601_utc(now.seconds, now.millis);
    out += "\",\"level\":\"";
    out += to_string(d.severity());
    out += "\",\"code\":\"";
    out += to_string(d.code());
    out += "\",\"domain\":\"";
    out += to_string(d.domain());
    out += "\",\"source\":\"";
    out += json_escape(d.source().value);
    out += "\",\"consequence\":\"";
    out += to_string(d.consequence());
    out += "\",\"msg\":\"";
    out += json_escape(d.message());
    out += "\"}\n";
    return out;
}

JsonFileSink::JsonFileSink(const std::string& path, Clock clock) noexcept
    : formatter_(clock) {
#if defined(_WIN32)
    // fopen is deprecated by MSVC but still works; the secure variant avoids a
    // warning that /W4 /WX would otherwise turn into an error. MinGW also
    // accepts it, so the platform check is the right one here.
    if (::fopen_s(&file_, path.c_str(), "ab") != 0) {
        file_ = nullptr;
    }
#else
    file_ = std::fopen(path.c_str(), "ab");
#endif
}

JsonFileSink::~JsonFileSink() {
    if (file_ != nullptr) {
        std::fclose(file_);
        file_ = nullptr;
    }
}

void JsonFileSink::emit(const Diagnostic& d) noexcept {
    if (file_ == nullptr) return;
    const std::string line = formatter_.format(d);
    const std::size_t written = std::fwrite(line.data(), 1, line.size(), file_);
    // Flush after every record: a crash is exactly when the log matters most,
    // so buffering would lose the records that explain the crash.
    std::fflush(file_);
    if (written == line.size()) ++written_;
}

}  // namespace qp::diag
