/**
 * @file value_text.cpp
 * @brief The conversions, with the rounding done by `to_chars` rather than by a format string.
 *
 * Every numeric payload is written with `std::to_chars` in its shortest round-tripping form, and each kind gets
 * its **own** shortest form: `float` is formatted as a `float` and parsed as a `float`, never widened. The
 * alternative -- `std::to_string`, or going through `double` -- is wrong for a reason that is easy to miss and
 * hard to notice: the value comes back as the same *number* and not the same *value*, so a save-then-load can
 * change a parameter in the last bit, and `ports::Value`'s own contract (`static_cast<float>(to_double()) ==
 * as_f32()`) is the invariant that catches it.
 */
#include <qp/plugins/value_text/value_text.hpp>

#include <qp/diag/logging.hpp>
#include <qp/graph/ir/descriptor.hpp>

#include <charconv>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>

namespace qp::plugins::value_text {
namespace {

using qp::authoring::DocumentRefusal;
namespace ports = qp::ports;

/// @brief The shortest text that reads back as `value`.
[[nodiscard]] std::string shortest(double value) {
    char buffer[64]{};
    const auto written = std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::general);
    return std::string{buffer, static_cast<std::size_t>(written.ptr - buffer)};
}

/// @brief The shortest text that reads back as the **same float**, which is not the same as the shortest double.
[[nodiscard]] std::string shortest(float value) {
    char buffer[64]{};
    const auto written = std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::general);
    return std::string{buffer, static_cast<std::size_t>(written.ptr - buffer)};
}

/// @brief Reads a double, refusing the spellings that are not numbers.
///
/// `from_chars` rather than `strtod`, and the reason is locale rather than performance: `strtod` honours the
/// machine's decimal separator, so a file read on a machine that uses a comma would parse `12.5` as `12`. A file
/// written by this program must read back the same on any machine.
[[nodiscard]] bool parse_double(std::string_view text, double& out) noexcept {
    if (text.empty()) return false;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), out);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

[[nodiscard]] bool parse_float(std::string_view text, float& out) noexcept {
    if (text.empty()) return false;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), out);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

[[nodiscard]] bool parse_int(std::string_view text, std::int64_t& out) noexcept {
    if (text.empty()) return false;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), out);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

/// @brief The seven axes as `L,M,T,I,Th,N,J`.
[[nodiscard]] std::string format_dim(const qp::units::Dim& dim) {
    const std::int8_t values[7] = {dim.L, dim.M, dim.T, dim.I, dim.Th, dim.N, dim.J};
    std::string out;
    for (std::size_t i = 0; i < 7; ++i) {
        if (i != 0) out.push_back(',');
        out += std::to_string(static_cast<int>(values[i]));
    }
    return out;
}

[[nodiscard]] bool parse_dim(std::string_view text, qp::units::Dim& out) noexcept {
    std::int8_t values[7] = {0, 0, 0, 0, 0, 0, 0};
    std::size_t axis = 0;
    std::size_t start = 0;
    // The end of the piece last consumed, so "were there exactly seven?" is answered by whether anything is
    // left -- not by another `find`, which is how the first version got this wrong: after seven axes `start`
    // points at the seventh comma itself, so `find(',', start)` finds that same comma and an eight-axis payload
    // passed as well-formed.
    std::size_t consumed = 0;
    while (axis < 7) {
        if (start >= text.size()) break;
        const std::size_t comma = text.find(',', start);
        const std::size_t stop = comma == std::string_view::npos ? text.size() : comma;
        const std::string_view piece = text.substr(start, stop - start);
        int parsed = 0;
        const auto result = std::from_chars(piece.data(), piece.data() + piece.size(), parsed);
        if (result.ec != std::errc{} || result.ptr != piece.data() + piece.size()) return false;
        // The axes are `std::int8_t`, and a value outside that range would wrap silently -- which would turn a
        // typo in a hand-edited file into a plausible dimension.
        if (parsed < -128 || parsed > 127) return false;
        values[axis++] = static_cast<std::int8_t>(parsed);
        consumed = stop;
        if (comma == std::string_view::npos) break;
        start = comma + 1;
    }
    // Exactly seven axes, and nothing after them. Six is a file that lost one and eight is a file from a build
    // with a different dimension model -- and reading either as if it were seven would be a guess.
    if (axis != 7) return false;
    if (consumed != text.size()) return false;
    out = qp::units::Dim{values[0], values[1], values[2], values[3], values[4], values[5], values[6]};
    return true;
}

}  // namespace

DocumentRefusal to_text(const ports::Value& value, Tagged& out) {
    Tagged tagged;
    switch (value.kind()) {
        case ports::ValueKind::f64: {
            const double v = value.to_double();
            if (!std::isfinite(v)) return DocumentRefusal::non_finite_number;
            tagged.kind = "f64";
            tagged.payload = shortest(v);
            break;
        }
        case ports::ValueKind::f32: {
            const float v = value.as_f32();
            if (!std::isfinite(v)) return DocumentRefusal::non_finite_number;
            tagged.kind = "f32";
            // Formatted **as a float**, which is the whole reason this case exists: the shortest text that reads
            // back as this float is not in general the shortest text that reads back as its double.
            tagged.payload = shortest(v);
            break;
        }
        case ports::ValueKind::i64:
            tagged.kind = "i64";
            tagged.payload = std::to_string(value.as_i64());
            break;
        case ports::ValueKind::boolean:
            tagged.kind = "bool";
            tagged.payload = value.as_bool() ? "true" : "false";
            break;
        case ports::ValueKind::dimension:
            tagged.kind = "dim";
            tagged.payload = format_dim(value.as_dimension());
            break;
        case ports::ValueKind::text: {
            const std::string& s = value.as_text();
            if (!qp::diag::is_valid_utf8(s)) return DocumentRefusal::text_not_utf8;
            tagged.kind = "text";
            // Raw, not escaped. Escaping is the format's business, and a layer that escaped here would force
            // every format to unescape it -- which is how two escaping rules come to exist.
            tagged.payload = s;
            break;
        }
        case ports::ValueKind::field_handle:
            // A handle names a buffer in this process. It is not in the document, so it cannot be in a file, and
            // a handle to nothing would load as a node whose input silently changed.
            return DocumentRefusal::value_kind_not_supported;
        case ports::ValueKind::invalid:
            // An unset parameter. Writing it would produce a file that claims a value; the caller should omit the
            // parameter instead, which is what `qpjson` does.
            return DocumentRefusal::value_kind_not_supported;
    }
    out = std::move(tagged);
    return DocumentRefusal::ok;
}

DocumentRefusal from_text(const Tagged& tagged, ports::Value& out) {
    if (tagged.kind == "f64") {
        double v = 0.0;
        if (!parse_double(tagged.payload, v)) return DocumentRefusal::malformed;
        if (!std::isfinite(v)) return DocumentRefusal::non_finite_number;
        out = ports::Value{v};
        return DocumentRefusal::ok;
    }
    if (tagged.kind == "f32") {
        float v = 0.0f;
        if (!parse_float(tagged.payload, v)) return DocumentRefusal::malformed;
        if (!std::isfinite(v)) return DocumentRefusal::non_finite_number;
        out = ports::Value{v};
        return DocumentRefusal::ok;
    }
    if (tagged.kind == "i64") {
        std::int64_t v = 0;
        if (!parse_int(tagged.payload, v)) return DocumentRefusal::malformed;
        out = ports::Value{v};
        return DocumentRefusal::ok;
    }
    if (tagged.kind == "bool") {
        // Accepted spellings are exactly the ones `to_text` writes. A reader that also accepted `yes` or `1`
        // would be one more rule to keep in step with the writer, and a file that says `1` where a boolean is
        // expected is a file whose author meant something ambiguous.
        if (tagged.payload == "true") {
            out = ports::Value{true};
            return DocumentRefusal::ok;
        }
        if (tagged.payload == "false") {
            out = ports::Value{false};
            return DocumentRefusal::ok;
        }
        return DocumentRefusal::malformed;
    }
    if (tagged.kind == "dim") {
        qp::units::Dim dim{};
        if (!parse_dim(tagged.payload, dim)) return DocumentRefusal::malformed;
        out = ports::Value{dim};
        return DocumentRefusal::ok;
    }
    if (tagged.kind == "text") {
        if (!qp::diag::is_valid_utf8(tagged.payload)) return DocumentRefusal::text_not_utf8;
        out = ports::Value{tagged.payload};
        return DocumentRefusal::ok;
    }
    // An unknown kind is refused rather than skipped: a file naming a kind this build does not have is a file
    // written by a build that disagrees about what a value is, and reading the payload as something else would
    // be a guess.
    return DocumentRefusal::value_kind_not_supported;
}

}  // namespace qp::plugins::value_text
