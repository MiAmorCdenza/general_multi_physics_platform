/**
 * @file qpjson_format.cpp
 * @brief JSON document format: the writer, and a parser hardened for input the writer did not produce.
 *
 * The two halves are deliberately not symmetric in how much they trust their input. The writer is
 * handed a document the platform built, so its job is to report the three things JSON cannot carry and
 * otherwise succeed. The parser is handed whatever the user picked in a file dialog, so it assumes
 * nothing: no input is indexed without a size test, every member is matched against a fixed set, and
 * there is no path out of it that throws.
 *
 * There is also no recursion to bound. The document's shape is fixed -- document, graph, node,
 * parameter -- so the parser's call depth is a constant, and a file containing ten thousand nested
 * arrays is refused by the first `expect` that does not match rather than by a depth counter.
 */
#include <qp/plugins/qpjson/qpjson_format.hpp>

#include <qp/diag/logging.hpp>
#include <qp/graph/ir.hpp>
#include <qp/graph/structure/graph.hpp>
#include <qp/units/dim.hpp>

#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace qp::plugins::qpjson {
namespace {

namespace authoring = qp::authoring;
using authoring::DocumentRefusal;

/// @brief Appends a signed integer in its shortest decimal form.
void append_int(std::string& out, std::int64_t v) {
    char buf[24];
    const auto r = std::to_chars(buf, buf + sizeof(buf), v);
    out.append(buf, r.ptr);
}

/// @brief Appends an unsigned handle field (a slot index, a generation, a port number).
void append_handle(std::string& out, std::uint32_t v) {
    char buf[16];
    const auto r = std::to_chars(buf, buf + sizeof(buf), v);
    out.append(buf, r.ptr);
}

/// @brief Appends the shortest decimal form that reads back as the same double.
///
/// Shortest round-tripping rather than a fixed precision: `0.1` is written as `0.1` and not as
/// `0.10000000000000001`, which is what makes the file readable, and the value that comes back is
/// bit-identical, which is what makes the round-trip test meaningful. Returns false for a value JSON
/// cannot express -- there is no literal for an infinity or a NaN -- so the caller can refuse it.
[[nodiscard]] bool append_double(std::string& out, double v) {
    if (!std::isfinite(v)) return false;
    char buf[40];
    const auto r = std::to_chars(buf, buf + sizeof(buf), v);
    if (r.ec != std::errc{}) return false;
    out.append(buf, r.ptr);
    return true;
}

/// @brief Appends a float in the shortest form that reads back as the same float.
[[nodiscard]] bool append_float(std::string& out, float v) {
    if (!std::isfinite(v)) return false;
    char buf[24];
    const auto r = std::to_chars(buf, buf + sizeof(buf), v);
    if (r.ec != std::errc{}) return false;
    out.append(buf, r.ptr);
    return true;
}

/// @brief Appends a JSON string literal.
///
/// The escaping is `qp::diag::json_escape` rather than a second escaper written here: a document and a
/// log line quote the same way, and two implementations of one rule drift. `json_escape` replaces an
/// invalid byte with `?`, which is why every string is validated before the writer runs -- this
/// function is the second half of that decision, not a place where data may be lost.
void append_string(std::string& out, std::string_view s) {
    out += '"';
    out += qp::diag::json_escape(std::string{s});
    out += '"';
}

/// @brief The name a value kind is written under.
///
/// Spelled out rather than written as the enum's number: `"kind": 1` would make the file depend on
/// `ValueKind`'s numeric tags, and those are a C++ detail -- frozen for the ABI, but the file outlives
/// the build. A reader of the file should not need the enum in hand to know what `200` means.
[[nodiscard]] const char* kind_name(qp::ports::ValueKind kind) noexcept {
    switch (kind) {
        case qp::ports::ValueKind::invalid: return "invalid";
        case qp::ports::ValueKind::f64: return "f64";
        case qp::ports::ValueKind::f32: return "f32";
        case qp::ports::ValueKind::i64: return "i64";
        case qp::ports::ValueKind::boolean: return "bool";
        case qp::ports::ValueKind::text: return "text";
        case qp::ports::ValueKind::dimension: return "dim";
        case qp::ports::ValueKind::field_handle: return "field";
    }
    return "unknown";
}

/// @brief The value kind a name denotes, or null when the name is not one this format reads.
[[nodiscard]] const qp::ports::ValueKind* kind_from_name(std::string_view name) noexcept {
    struct Entry final {
        const char* name;
        qp::ports::ValueKind kind;
    };
    static constexpr Entry kEntries[] = {
        {"invalid", qp::ports::ValueKind::invalid}, {"f64", qp::ports::ValueKind::f64},
        {"f32", qp::ports::ValueKind::f32},         {"i64", qp::ports::ValueKind::i64},
        {"bool", qp::ports::ValueKind::boolean},    {"text", qp::ports::ValueKind::text},
        {"dim", qp::ports::ValueKind::dimension},
        // Named so a file holding one is refused with a sentence rather than "unknown kind": a build
        // that believed it could carry a field handle wrote this, and this build cannot.
        {"field", qp::ports::ValueKind::field_handle},
    };
    for (const Entry& entry : kEntries) {
        if (name == entry.name) return &entry.kind;
    }
    return nullptr;
}

// -- the write-side pre-pass -------------------------------------------------

/// @brief Finds the first thing this format cannot write, or `ok`.
///
/// A separate pass rather than an error return threaded through the writer, for the same reason
/// `check_export` is separate from an export: the question is "can this document be written at all", it
/// has to be answerable **before** any byte exists, and a caller must not be able to receive half a
/// document. It also puts every refusal in one place, so a reason cannot be forgotten in one branch.
[[nodiscard]] DocumentRefusal find_unwritable(const authoring::DocumentSource& source) {
    if (!qp::diag::is_valid_utf8(source.title)) return DocumentRefusal::text_not_utf8;

    for (const qp::graph::NodeSlot& slot : source.graph.slots()) {
        if (!slot.occupied) continue;
        const qp::graph::Node& node = slot.node;
        // An empty type name is a **pending** node -- reserved and not yet given a type -- and it is
        // written rather than refused: a document saved while the user is mid-edit is a document.
        if (!qp::diag::is_valid_utf8(node.type_name)) return DocumentRefusal::text_not_utf8;
        if (!qp::diag::is_valid_utf8(node.name)) return DocumentRefusal::text_not_utf8;

        for (const qp::graph::ParamValue& param : node.params) {
            switch (param.value.kind()) {
                case qp::ports::ValueKind::field_handle:
                    // A handle names a buffer in the running process. It is not in the document, so it
                    // cannot be in the file, and a handle to nothing would load as a node whose input
                    // silently changed.
                    return DocumentRefusal::value_kind_not_supported;
                case qp::ports::ValueKind::text:
                    if (!qp::diag::is_valid_utf8(param.value.as_text())) {
                        return DocumentRefusal::text_not_utf8;
                    }
                    break;
                case qp::ports::ValueKind::f64:
                    if (!std::isfinite(param.value.as_f64())) return DocumentRefusal::non_finite_number;
                    break;
                case qp::ports::ValueKind::f32:
                    if (!std::isfinite(param.value.as_f32())) return DocumentRefusal::non_finite_number;
                    break;
                default:
                    break;
            }
        }
    }

    for (const authoring::ViewId& view : source.layouts.view_ids()) {
        if (!qp::diag::is_valid_utf8(view)) return DocumentRefusal::text_not_utf8;
        if (!qp::diag::is_valid_utf8(source.layouts.get(view))) {
            return DocumentRefusal::text_not_utf8;
        }
    }
    return DocumentRefusal::ok;
}

// -- writing -----------------------------------------------------------------

void write_param(std::string& out, const qp::graph::ParamValue& param) {
    out += "{\"port\": ";
    append_handle(out, param.number);
    out += ", \"kind\": \"";
    out += kind_name(param.value.kind());
    out += '"';
    switch (param.value.kind()) {
        case qp::ports::ValueKind::invalid:
            // No `value` member at all. A default-constructed value is what "this parameter is not set"
            // means, and writing `"value": 0` would turn "never measured" into "measured zero" -- the
            // distinction this whole platform exists to keep.
            break;
        case qp::ports::ValueKind::f64:
            out += ", \"value\": ";
            (void)append_double(out, param.value.as_f64());
            break;
        case qp::ports::ValueKind::f32:
            out += ", \"value\": ";
            (void)append_float(out, param.value.as_f32());
            break;
        case qp::ports::ValueKind::i64:
            out += ", \"value\": ";
            append_int(out, param.value.as_i64());
            break;
        case qp::ports::ValueKind::boolean:
            out += param.value.as_bool() ? ", \"value\": true" : ", \"value\": false";
            break;
        case qp::ports::ValueKind::text:
            out += ", \"value\": ";
            append_string(out, param.value.as_text());
            break;
        case qp::ports::ValueKind::dimension: {
            // Seven exponents in `units::Dim`'s own order, which is frozen. A node parameter may be a
            // dimension -- a spring constant's dimension is data, not decoration -- so this is carried
            // rather than refused.
            const qp::units::Dim d = param.value.as_dimension();
            const qp::units::DimExp exps[7] = {d.L, d.M, d.T, d.I, d.Th, d.N, d.J};
            out += ", \"value\": [";
            for (int i = 0; i < 7; ++i) {
                if (i != 0) out += ", ";
                append_int(out, static_cast<std::int64_t>(exps[i]));
            }
            out += ']';
            break;
        }
        case qp::ports::ValueKind::field_handle:
            // Unreachable: find_unwritable refuses it before the writer runs. Kept explicit so the
            // switch stays total and a future kind cannot be silently omitted.
            break;
    }
    out += '}';
}

void write_node(std::string& out, const qp::graph::Node& node) {
    out += "{\"index\": ";
    append_handle(out, node.id.index);
    out += ", \"generation\": ";
    append_handle(out, node.id.generation);
    out += ", \"type\": ";
    append_string(out, node.type_name);
    out += ", \"name\": ";
    append_string(out, node.name);
    out += node.bypassed ? ", \"bypassed\": true" : ", \"bypassed\": false";
    out += ", \"order\": ";
    append_int(out, static_cast<std::int64_t>(node.order_hint));

    out += ", \"params\": [";
    for (std::size_t i = 0; i < node.params.size(); ++i) {
        if (i != 0) out += ", ";
        write_param(out, node.params[i]);
    }
    out += "]}";
}

/// @brief Writes one endpoint of an edge.
///
/// The direction is **not** in the file, and that is deliberate: the graph's own invariant is that an
/// edge runs from an output to an input, so a member saying which is which would be a member that can be
/// written wrongly. `from` is an output and `to` is an input by position, and a file cannot say
/// otherwise.
void write_endpoint(std::string& out, qp::graph::PortRef ref) {
    out += "{\"node\": ";
    append_handle(out, ref.node.index);
    out += ", \"generation\": ";
    append_handle(out, ref.node.generation);
    out += ", \"port\": ";
    append_handle(out, ref.port);
    out += '}';
}

void write_graph(std::string& out, const qp::graph::Graph& graph) {
    out += "{\n    \"nodes\": [";
    bool first = true;
    for (const qp::graph::NodeSlot& slot : graph.slots()) {
        if (!slot.occupied) continue;
        out += first ? "\n      " : ",\n      ";
        first = false;
        write_node(out, slot.node);
    }
    out += first ? "],\n" : "\n    ],\n";

    out += "    \"edges\": [";
    first = true;
    for (const qp::graph::Edge& edge : graph.edges()) {
        out += first ? "\n      " : ",\n      ";
        first = false;
        out += "{\"from\": ";
        write_endpoint(out, edge.from);
        out += ", \"to\": ";
        write_endpoint(out, edge.to);
        out += '}';
    }
    out += first ? "]\n" : "\n    ]\n";
    out += "  },\n";
}

void write_layouts(std::string& out, const authoring::ViewLayouts& layouts) {
    out += "  \"layouts\": [";
    bool first = true;
    for (const authoring::ViewId& view : layouts.view_ids()) {
        out += first ? "\n    " : ",\n    ";
        first = false;
        out += "{\"view\": ";
        append_string(out, view);
        out += ", \"text\": ";
        append_string(out, layouts.get(view));
        out += '}';
    }
    out += first ? "]\n" : "\n  ]\n";
}

/// @brief Writes a document that `find_unwritable` has already accepted.
void write_document(std::string& out, const authoring::DocumentSource& source) {
    out += "{\n  \"";
    out += QpJsonFormat::kMarkerKey;
    out += "\": ";
    append_int(out, QpJsonFormat::kVersion);
    out += ",\n  \"title\": ";
    append_string(out, source.title);
    out += ",\n  \"graph\": ";
    write_graph(out, source.graph);
    write_layouts(out, source.layouts);
    out += "}\n";
}

// -- reading -----------------------------------------------------------------

/**
 * @brief A cursor over the input, which never reads past the end and never throws.
 *
 * Every failure is recorded once, on the first error, because the first error is the one that names the
 * user's problem: a parser that keeps going after a syntax error reports whatever it tripped over
 * afterwards, and that is usually a symptom rather than the cause.
 */
class Reader final {
public:
    explicit Reader(std::string_view in) noexcept : in_(in) {}

    /// @brief Why parsing stopped, or `ok`.
    [[nodiscard]] DocumentRefusal error() const noexcept { return error_; }

    void skip_ws() noexcept {
        while (i_ < in_.size()) {
            const char c = in_[i_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++i_;
                continue;
            }
            break;
        }
    }

    [[nodiscard]] bool at_end() noexcept {
        skip_ws();
        return i_ >= in_.size();
    }

    /// @brief Whether the next non-space character is `c`, without consuming it.
    [[nodiscard]] bool peek_is(char c) noexcept {
        skip_ws();
        return i_ < in_.size() && in_[i_] == c;
    }

    /// @brief Whether the next characters are `text`, without consuming them.
    [[nodiscard]] bool peek_text(std::string_view text) noexcept {
        skip_ws();
        return in_.substr(i_, text.size()) == text;
    }

    /// @brief Consumes `text`, which `peek_text` has already matched.
    void take_text(std::string_view text) noexcept { i_ += text.size(); }

    /// @brief Consumes `expected`, or reports why it could not.
    ///
    /// Running out of input is reported as `truncated` and a wrong byte as `malformed`, because they are
    /// different user problems: one is a file that was cut short, the other is a file that is not what
    /// it says it is, and the fix for the first is to find the original copy.
    [[nodiscard]] bool expect(char expected) noexcept {
        skip_ws();
        if (i_ >= in_.size()) return fail(DocumentRefusal::truncated);
        if (in_[i_] != expected) return fail(DocumentRefusal::malformed);
        ++i_;
        return true;
    }

    /// @brief Records `why` as the failure, keeping the first one. Always returns false.
    [[nodiscard]] bool fail(DocumentRefusal why) noexcept {
        if (error_ == DocumentRefusal::ok) error_ = why;
        return false;
    }

    /// @brief Records the failure that fits the cursor: out of input, or a byte that does not fit.
    [[nodiscard]] bool fail_here() noexcept {
        return fail(i_ >= in_.size() ? DocumentRefusal::truncated : DocumentRefusal::malformed);
    }

    /// @brief Whether the marker literal is at the cursor, consuming it when it is.
    ///
    /// The literal is assembled from `kMarkerKey` rather than written out again, so the key is named in
    /// one place: a second copy would be a second thing to keep in step, and the failure mode -- a writer
    /// and a reader that disagree about the marker -- is a format that cannot open its own files.
    ///
    /// Running out of input *inside* the marker is reported as `truncated`, not as "not a document": the
    /// bytes so far match, so the file is this format's and was cut short, which is a different problem
    /// for the user and a different fix.
    [[nodiscard]] bool marker() noexcept {
        static const std::string literal =
            std::string{"\""} + QpJsonFormat::kMarkerKey + "\"";
        skip_ws();
        const std::string_view rest = in_.substr(i_);
        if (rest.size() < literal.size()) {
            if (literal.compare(0, rest.size(), rest) == 0) return fail(DocumentRefusal::truncated);
            return fail(DocumentRefusal::not_a_document);
        }
        if (rest.compare(0, literal.size(), literal) != 0) {
            return fail(DocumentRefusal::not_a_document);
        }
        i_ += literal.size();
        return true;
    }

    /// @brief Parses a JSON string, unescaping it into `out`.
    [[nodiscard]] bool string(std::string& out) noexcept {
        skip_ws();
        if (i_ >= in_.size()) return fail(DocumentRefusal::truncated);
        if (in_[i_] != '"') return fail(DocumentRefusal::malformed);
        ++i_;
        out.clear();
        while (true) {
            if (i_ >= in_.size()) return fail(DocumentRefusal::truncated);   // unterminated
            const char c = in_[i_];
            if (c == '"') {
                ++i_;
                // The parser enforces the same rule the writer does. A file this format cannot write
                // must not become one it can read: those bytes would end up in a view's layout payload,
                // a node's name or a text parameter, and the platform would then refuse to save the
                // document it had just loaded -- a file that can be opened and not saved.
                if (!qp::diag::is_valid_utf8(out)) return fail(DocumentRefusal::text_not_utf8);
                return true;
            }
            if (static_cast<unsigned char>(c) < 0x20) {
                // A raw control byte must be escaped in JSON, so its presence means this file was not
                // written by a JSON writer -- or was edited by something that is not one.
                return fail(DocumentRefusal::malformed);
            }
            if (c != '\\') {
                out += c;
                ++i_;
                continue;
            }
            ++i_;
            if (i_ >= in_.size()) return fail(DocumentRefusal::truncated);
            switch (in_[i_]) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u':
                    if (!unicode_escape(out)) return false;
                    break;
                default:
                    return fail(DocumentRefusal::malformed);
            }
            ++i_;
        }
    }
    /// @brief Parses an object member name and its colon.
    [[nodiscard]] bool key(std::string& out) noexcept {
        if (!string(out)) return false;
        return expect(':');
    }

    /// @brief Parses a number, requiring the whole token to be consumed.
    ///
    /// The conversion is `std::from_chars` rather than `strtod`, because `strtod` reads the locale's
    /// decimal point: a file holding `0.5` would load as `0` on a machine whose locale uses a comma, and
    /// the number would look like it had been rounded rather than misread. `from_chars` is
    /// locale-independent by definition.
    [[nodiscard]] bool double_value(double& out) noexcept {
        const std::string_view token = number_token();
        if (token.empty()) return false;
        const auto r = std::from_chars(token.data(), token.data() + token.size(), out);
        if (r.ec != std::errc{} || r.ptr != token.data() + token.size()) {
            return fail(DocumentRefusal::malformed);
        }
        return true;
    }

    /// @brief Parses an integer, refusing a token that holds a fraction or an exponent.
    [[nodiscard]] bool integer(std::int64_t& out) noexcept {
        const std::string_view token = number_token();
        if (token.empty()) return false;
        const auto r = std::from_chars(token.data(), token.data() + token.size(), out);
        if (r.ec != std::errc{} || r.ptr != token.data() + token.size()) {
            return fail(DocumentRefusal::malformed);
        }
        return true;
    }

    /// @brief Parses a positive integer that must fit an unsigned 32-bit handle field.
    [[nodiscard]] bool handle(std::uint32_t& out) noexcept {
        std::int64_t v = 0;
        if (!integer(v)) return false;
        if (v <= 0 || v > 0xFFFFFFFFll) return fail(DocumentRefusal::malformed);
        out = static_cast<std::uint32_t>(v);
        return true;
    }

    [[nodiscard]] bool boolean(bool& out) noexcept {
        if (peek_text("true")) {
            take_text("true");
            out = true;
            return true;
        }
        if (peek_text("false")) {
            take_text("false");
            out = false;
            return true;
        }
        return fail_here();
    }

private:
    /// @brief The raw number token at the cursor, or empty when there is none.
    ///
    /// Scanned structurally -- sign, digits, optional fraction, optional exponent -- and then handed to
    /// `from_chars`, which is what decides whether it is a number. Splitting it this way keeps the
    /// scanner's job to "where does the token end" and leaves "is it a number" in one place.
    [[nodiscard]] std::string_view number_token() noexcept {
        skip_ws();
        const std::size_t start = i_;
        if (i_ < in_.size() && in_[i_] == '-') ++i_;
        const std::size_t digits_start = i_;
        while (i_ < in_.size() && in_[i_] >= '0' && in_[i_] <= '9') ++i_;
        if (i_ == digits_start) {
            i_ = start;
            (void)fail_here();   // records why; the caller sees the empty token
            return {};
        }
        if (i_ < in_.size() && in_[i_] == '.') {
            ++i_;
            while (i_ < in_.size() && in_[i_] >= '0' && in_[i_] <= '9') ++i_;
        }
        if (i_ < in_.size() && (in_[i_] == 'e' || in_[i_] == 'E')) {
            ++i_;
            if (i_ < in_.size() && (in_[i_] == '+' || in_[i_] == '-')) ++i_;
            while (i_ < in_.size() && in_[i_] >= '0' && in_[i_] <= '9') ++i_;
        }
        return in_.substr(start, i_ - start);
    }

    /// @brief Parses the token after a `\u` and appends the code point as UTF-8.
    ///
    /// `i_` points at the `u` on entry and at the last hex digit on return; the caller's `++i_` then
    /// moves past it. Surrogate pairs are joined, and a surrogate on its own is refused: it is not a code
    /// point, and encoding one would put illegal UTF-8 into a document that this same format then
    /// refuses to write -- a file that could not have been produced here must not become one that can
    /// only be read here.
    [[nodiscard]] bool unicode_escape(std::string& out) noexcept {
        std::uint32_t code = 0;
        if (!hex4(i_ + 1, code)) return false;
        i_ += 4;

        if (code >= 0xD800 && code <= 0xDBFF) {
            // A high surrogate must be followed by its low half.
            if (i_ + 6 >= in_.size()) return fail(DocumentRefusal::truncated);
            if (in_[i_ + 1] != '\\' || in_[i_ + 2] != 'u') return fail(DocumentRefusal::malformed);
            std::uint32_t low = 0;
            if (!hex4(i_ + 3, low)) return false;
            if (low < 0xDC00 || low > 0xDFFF) return fail(DocumentRefusal::malformed);
            code = 0x10000u + ((code - 0xD800u) << 10) + (low - 0xDC00u);
            i_ += 6;
        } else if (code >= 0xDC00 && code <= 0xDFFF) {
            return fail(DocumentRefusal::malformed);
        }
        append_utf8(out, code);
        return true;
    }

    /// @brief Reads four hex digits starting at `pos`, or reports why it could not.
    [[nodiscard]] bool hex4(std::size_t pos, std::uint32_t& out) noexcept {
        if (pos + 4 > in_.size()) return fail(DocumentRefusal::truncated);
        std::uint32_t value = 0;
        for (std::size_t k = 0; k < 4; ++k) {
            const char c = in_[pos + k];
            std::uint32_t digit = 0;
            if (c >= '0' && c <= '9') {
                digit = static_cast<std::uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                digit = static_cast<std::uint32_t>(c - 'a') + 10u;
            } else if (c >= 'A' && c <= 'F') {
                digit = static_cast<std::uint32_t>(c - 'A') + 10u;
            } else {
                return fail(DocumentRefusal::malformed);
            }
            value = (value << 4) | digit;
        }
        out = value;
        return true;
    }

    /// @brief Appends one code point as UTF-8. `code` is a legal scalar value by construction.
    static void append_utf8(std::string& out, std::uint32_t code) {
        if (code < 0x80) {
            out += static_cast<char>(code);
        } else if (code < 0x800) {
            out += static_cast<char>(0xC0u | (code >> 6));
            out += static_cast<char>(0x80u | (code & 0x3Fu));
        } else if (code < 0x10000) {
            out += static_cast<char>(0xE0u | (code >> 12));
            out += static_cast<char>(0x80u | ((code >> 6) & 0x3Fu));
            out += static_cast<char>(0x80u | (code & 0x3Fu));
        } else {
            out += static_cast<char>(0xF0u | (code >> 18));
            out += static_cast<char>(0x80u | ((code >> 12) & 0x3Fu));
            out += static_cast<char>(0x80u | ((code >> 6) & 0x3Fu));
            out += static_cast<char>(0x80u | (code & 0x3Fu));
        }
    }

    std::string_view in_{};
    std::size_t i_ = 0;
    DocumentRefusal error_ = DocumentRefusal::ok;
};

/// @brief What a node object held, and which of its required members were present.
struct NodeFields final {
    std::uint32_t index = 0;
    std::uint32_t generation = 0;
    std::string type{};
    std::string name{};
    bool bypassed = false;
    std::int64_t order = 0;
    std::vector<qp::graph::ParamValue> params{};
    bool has_index = false;
    bool has_generation = false;
    bool has_type = false;
};

/// @brief Parses `{ "port": ..., "kind": ..., "value": ... }`.
///
/// `kind` is read before `value`, because that member is what tells the reader how to read the next one.
/// That is a real constraint on hand-written files, and it is stated rather than hidden: a file with
/// `value` first is refused, not guessed at.
[[nodiscard]] bool parse_param(Reader& r, qp::graph::ParamValue& out) noexcept {
    if (!r.expect('{')) return false;

    bool has_port = false;
    bool has_kind = false;
    bool has_value = false;
    qp::ports::Value value{};

    while (!r.peek_is('}')) {
        std::string name;
        if (!r.key(name)) return false;
        if (name == "port") {
            if (!r.handle(out.number)) return false;
            has_port = true;
        } else if (name == "kind") {
            std::string kind;
            if (!r.string(kind)) return false;
            const qp::ports::ValueKind* found = kind_from_name(kind);
            if (found == nullptr) return r.fail(DocumentRefusal::malformed);
            // A field handle is named rather than left as "unknown kind": a file holding one was written
            // by a build that believed it could carry it, and this one cannot.
            if (*found == qp::ports::ValueKind::field_handle) {
                return r.fail(DocumentRefusal::value_kind_not_supported);
            }
            switch (*found) {
                case qp::ports::ValueKind::f64: value = qp::ports::Value{0.0}; break;
                case qp::ports::ValueKind::f32: value = qp::ports::Value{0.0f}; break;
                case qp::ports::ValueKind::i64: value = qp::ports::Value{std::int64_t{0}}; break;
                case qp::ports::ValueKind::boolean: value = qp::ports::Value{false}; break;
                case qp::ports::ValueKind::text: value = qp::ports::Value{std::string{}}; break;
                case qp::ports::ValueKind::dimension: value = qp::ports::Value{qp::units::Dim{}}; break;
                case qp::ports::ValueKind::invalid:
                case qp::ports::ValueKind::field_handle:
                    // `invalid` carries no payload: it is the default-constructed value, and the absence
                    // of a `value` member is exactly what says "this parameter is not set".
                    value = qp::ports::Value{};
                    break;
            }
            has_kind = true;
        } else if (name == "value") {
            if (!has_kind) {
                // The member order is part of the format: without the kind, nothing says how to read
                // these bytes.
                return r.fail(DocumentRefusal::malformed);
            }
            switch (value.kind()) {
                case qp::ports::ValueKind::f64: {
                    double v = 0.0;
                    if (!r.double_value(v)) return false;
                    value = qp::ports::Value{v};
                    break;
                }
                case qp::ports::ValueKind::f32: {
                    double v = 0.0;
                    if (!r.double_value(v)) return false;
                    // Through double and back: the token was written from a float, whose shortest form
                    // is exactly representable, so the cast returns the same bits.
                    value = qp::ports::Value{static_cast<float>(v)};
                    break;
                }
                case qp::ports::ValueKind::i64: {
                    std::int64_t v = 0;
                    if (!r.integer(v)) return false;
                    value = qp::ports::Value{v};
                    break;
                }
                case qp::ports::ValueKind::boolean: {
                    bool v = false;
                    if (!r.boolean(v)) return false;
                    value = qp::ports::Value{v};
                    break;
                }
                case qp::ports::ValueKind::text: {
                    std::string v;
                    if (!r.string(v)) return false;
                    value = qp::ports::Value{std::move(v)};
                    break;
                }
                case qp::ports::ValueKind::dimension: {
                    if (!r.expect('[')) return false;
                    qp::units::DimExp exps[7] = {0, 0, 0, 0, 0, 0, 0};
                    for (int i = 0; i < 7; ++i) {
                        if (i != 0 && !r.expect(',')) return false;
                        std::int64_t e = 0;
                        if (!r.integer(e)) return false;
                        if (e < -128 || e > 127) return r.fail(DocumentRefusal::malformed);
                        exps[i] = static_cast<qp::units::DimExp>(e);
                    }
                    if (!r.expect(']')) return false;
                    value = qp::ports::Value{
                        qp::units::Dim{exps[0], exps[1], exps[2], exps[3], exps[4], exps[5], exps[6]}};
                    break;
                }
                default:
                    // A payload for a kind that has none: the file and this format disagree.
                    return r.fail(DocumentRefusal::malformed);
            }
            has_value = true;
        } else {
            return r.fail(DocumentRefusal::malformed);
        }

        if (!r.peek_is('}') && !r.expect(',')) return false;
    }

    if (!r.expect('}')) return false;
    if (!has_port || !has_kind) return r.fail(DocumentRefusal::malformed);
    // A kind that carries a payload must have one. `invalid` is the one kind that does not.
    if (!has_value && value.kind() != qp::ports::ValueKind::invalid) {
        return r.fail(DocumentRefusal::malformed);
    }
    out.value = std::move(value);
    return true;
}

/// @brief Parses `{ "index": ..., "generation": ..., "type": ..., ... }`.
[[nodiscard]] bool parse_node(Reader& r, NodeFields& out) noexcept {
    if (!r.expect('{')) return false;

    while (!r.peek_is('}')) {
        std::string name;
        if (!r.key(name)) return false;
        if (name == "index") {
            if (!r.handle(out.index)) return false;
            out.has_index = true;
        } else if (name == "generation") {
            if (!r.handle(out.generation)) return false;
            out.has_generation = true;
        } else if (name == "type") {
            if (!r.string(out.type)) return false;
            out.has_type = true;
        } else if (name == "name") {
            if (!r.string(out.name)) return false;
        } else if (name == "bypassed") {
            if (!r.boolean(out.bypassed)) return false;
        } else if (name == "order") {
            if (!r.integer(out.order)) return false;
        } else if (name == "params") {
            std::vector<qp::graph::PortNumber> seen;
            if (!r.expect('[')) return false;
            while (!r.peek_is(']')) {
                qp::graph::ParamValue param;
                if (!parse_param(r, param)) return false;
                // The graph's own invariant is one parameter per port, and `set_param` maintains it by
                // replacing. A file that breaks it would have one of the two silently replaced on load,
                // so it is refused instead.
                for (const qp::graph::PortNumber number : seen) {
                    if (number == param.number) return r.fail(DocumentRefusal::malformed);
                }
                seen.push_back(param.number);
                out.params.push_back(std::move(param));
                if (!r.peek_is(']') && !r.expect(',')) return false;
            }
            if (!r.expect(']')) return false;
        } else {
            return r.fail(DocumentRefusal::malformed);
        }

        if (!r.peek_is('}') && !r.expect(',')) return false;
    }

    if (!r.expect('}')) return false;
    // `index`, `generation` and `type` are required; everything else defaults. Required because a node
    // without an identity can be named by no edge and no view's layout; optional because the format
    // claims to be writable by hand, and a person writing one should not have to spell out
    // `"bypassed": false`.
    if (!out.has_index || !out.has_generation || !out.has_type) {
        return r.fail(DocumentRefusal::malformed);
    }
    return true;
}

/// @brief Parses one endpoint. The direction comes from the position, not from the file.
[[nodiscard]] bool parse_endpoint(Reader& r, qp::graph::PortRef& out,
                                 qp::graph::PortDirection direction) noexcept {
    if (!r.expect('{')) return false;
    bool has_node = false;
    bool has_generation = false;
    bool has_port = false;
    while (!r.peek_is('}')) {
        std::string name;
        if (!r.key(name)) return false;
        if (name == "node") {
            if (!r.handle(out.node.index)) return false;
            has_node = true;
        } else if (name == "generation") {
            if (!r.handle(out.node.generation)) return false;
            has_generation = true;
        } else if (name == "port") {
            if (!r.handle(out.port)) return false;
            has_port = true;
        } else {
            return r.fail(DocumentRefusal::malformed);
        }
        if (!r.peek_is('}') && !r.expect(',')) return false;
    }
    if (!r.expect('}')) return false;
    if (!has_node || !has_generation || !has_port) return r.fail(DocumentRefusal::malformed);
    out.direction = direction;
    return true;
}

[[nodiscard]] bool parse_edge(Reader& r, qp::graph::Edge& out) noexcept {
    if (!r.expect('{')) return false;
    bool has_from = false;
    bool has_to = false;
    while (!r.peek_is('}')) {
        std::string name;
        if (!r.key(name)) return false;
        if (name == "from") {
            if (!parse_endpoint(r, out.from, qp::graph::PortDirection::output)) return false;
            has_from = true;
        } else if (name == "to") {
            if (!parse_endpoint(r, out.to, qp::graph::PortDirection::input)) return false;
            has_to = true;
        } else {
            return r.fail(DocumentRefusal::malformed);
        }
        if (!r.peek_is('}') && !r.expect(',')) return false;
    }
    if (!r.expect('}')) return false;
    if (!has_from || !has_to) return r.fail(DocumentRefusal::malformed);
    return true;
}

/// @brief Maps a graph refusal to the document refusal that names the same problem.
[[nodiscard]] DocumentRefusal from_graph_error(qp::diag::ErrorCode code) noexcept {
    switch (code) {
        case qp::diag::ErrorCode::ok: return DocumentRefusal::ok;
        case qp::diag::ErrorCode::cycle_detected: return DocumentRefusal::cyclic_graph;
        case qp::diag::ErrorCode::unknown_node: return DocumentRefusal::dangling_edge;
        default: return DocumentRefusal::malformed;
    }
}

/// @brief Parses the `graph` member's contents into `graph`.
[[nodiscard]] bool parse_graph(Reader& r, qp::graph::Graph& graph) noexcept {
    // Nodes are collected first and restored before any edge, whoever wrote the file: an edge cannot be
    // restored before the node it names, and a file that lists edges first is not wrong, only
    // differently ordered.
    std::vector<NodeFields> nodes;
    std::vector<qp::graph::Edge> edges;

    if (!r.expect('{')) return false;
    while (!r.peek_is('}')) {
        std::string name;
        if (!r.key(name)) return false;
        if (name == "nodes") {
            if (!r.expect('[')) return false;
            while (!r.peek_is(']')) {
                NodeFields fields;
                if (!parse_node(r, fields)) return false;
                nodes.push_back(std::move(fields));
                if (!r.peek_is(']') && !r.expect(',')) return false;
            }
            if (!r.expect(']')) return false;
        } else if (name == "edges") {
            if (!r.expect('[')) return false;
            while (!r.peek_is(']')) {
                qp::graph::Edge edge;
                if (!parse_edge(r, edge)) return false;
                edges.push_back(edge);
                if (!r.peek_is(']') && !r.expect(',')) return false;
            }
            if (!r.expect(']')) return false;
        } else {
            return r.fail(DocumentRefusal::malformed);
        }
        if (!r.peek_is('}') && !r.expect(',')) return false;
    }
    if (!r.expect('}')) return false;

    for (const NodeFields& fields : nodes) {
        if (fields.order < -2147483648ll || fields.order > 2147483647ll) {
            return r.fail(DocumentRefusal::malformed);
        }
        qp::graph::Node node;
        node.id = qp::graph::NodeId{fields.index, fields.generation};
        node.type_name = fields.type;
        node.name = fields.name;
        node.bypassed = fields.bypassed;
        node.order_hint = static_cast<std::int32_t>(fields.order);
        node.params = fields.params;
        const auto restored = graph.restore_node(node);
        if (!restored.has_value()) {
            // `duplicate_connection` from restore_node means the slot is already taken. A repeated index
            // and generation is the document's own problem, and "two nodes claim the same handle" is
            // what the user has to fix -- not "the graph refused something".
            if (restored.error() == qp::diag::ErrorCode::duplicate_connection) {
                return r.fail(DocumentRefusal::duplicate_node);
            }
            return r.fail(from_graph_error(restored.error()));
        }
    }

    for (const qp::graph::Edge& edge : edges) {
        const auto restored = graph.restore_edge(edge);
        if (!restored.has_value()) {
            // Two edges into one input port is not a duplicate node, and saying so would send the user
            // looking in the wrong place.
            if (restored.error() == qp::diag::ErrorCode::duplicate_connection) {
                return r.fail(DocumentRefusal::duplicate_edge);
            }
            return r.fail(from_graph_error(restored.error()));
        }
    }
    return true;
}

/// @brief Parses the `layouts` member's contents into `layouts`.
[[nodiscard]] bool parse_layouts(Reader& r, authoring::ViewLayouts& layouts) noexcept {
    if (!r.expect('[')) return false;
    while (!r.peek_is(']')) {
        std::string view;
        std::string text;
        bool has_view = false;
        bool has_text = false;
        if (!r.expect('{')) return false;
        while (!r.peek_is('}')) {
            std::string name;
            if (!r.key(name)) return false;
            if (name == "view") {
                if (!r.string(view)) return false;
                has_view = true;
            } else if (name == "text") {
                if (!r.string(text)) return false;
                has_text = true;
            } else {
                return r.fail(DocumentRefusal::malformed);
            }
            if (!r.peek_is('}') && !r.expect(',')) return false;
        }
        if (!r.expect('}')) return false;
        if (!has_view || !has_text || view.empty()) return r.fail(DocumentRefusal::malformed);
        // A view has one layout. A file holding two would have one of them silently replaced -- the
        // same class of loss this format refuses everywhere else.
        if (layouts.has(view)) return r.fail(DocumentRefusal::malformed);
        layouts.set(view, std::move(text));
        if (!r.peek_is(']') && !r.expect(',')) return false;
    }
    return r.expect(']');
}

/// @brief Parses a whole document. Fills `out` only on success.
[[nodiscard]] DocumentRefusal parse_document(std::string_view bytes, authoring::DocumentSnapshot& out) {
    Reader r{bytes};
    if (r.at_end()) return DocumentRefusal::truncated;
    if (!r.peek_is('{')) return DocumentRefusal::not_a_document;
    if (!r.expect('{')) return DocumentRefusal::not_a_document;

    // The marker is required **first**, which is what makes "this is not a document" a distinct answer
    // from "this is a broken document": a JSON file that begins with something else is a different kind
    // of file, and telling its owner that their document is malformed would send them looking for a
    // syntax error that is not there.
    if (!r.marker()) return r.error();
    if (!r.expect(':')) return r.error();

    std::int64_t version = 0;
    if (!r.integer(version)) return r.error();
    if (version > QpJsonFormat::kVersion) return DocumentRefusal::unsupported_version;
    if (version < 1) return DocumentRefusal::malformed;

    authoring::DocumentSnapshot loaded;
    bool has_graph = false;

    while (!r.peek_is('}')) {
        if (!r.expect(',')) return r.error();
        std::string name;
        if (!r.key(name)) return r.error();
        if (name == "title") {
            if (!r.string(loaded.title)) return r.error();
        } else if (name == "graph") {
            if (!parse_graph(r, loaded.graph)) return r.error();
            has_graph = true;
        } else if (name == "layouts") {
            if (!parse_layouts(r, loaded.layouts)) return r.error();
        } else {
            // Unknown members are refused, not skipped: the version marker is the compatibility
            // mechanism, so a member this version does not define means the file and this build
            // disagree about what a document is, and loading the part they agree on would produce a
            // graph that is not the one that was saved.
            return DocumentRefusal::malformed;
        }
    }
    if (!r.expect('}')) return r.error();
    if (!r.at_end()) return DocumentRefusal::malformed;   // bytes after the document

    if (!has_graph) {
        // A document describes a graph. One without a graph is not an empty document; it is a file this
        // format does not define.
        return DocumentRefusal::malformed;
    }
    out = std::move(loaded);
    return DocumentRefusal::ok;
}

}  // namespace

const authoring::DocumentFormatDesc& QpJsonFormat::format() const noexcept {
    // A function-local static: the description is immutable, and a registry of formats is entitled to
    // hold the reference for as long as the process lives.
    static const authoring::DocumentFormatDesc desc = [] {
        authoring::DocumentFormatDesc d;
        d.name = "qp.document.json";
        d.label = "Experiment document (JSON)";
        d.extensions = {"qpd"};
        d.is_text = true;
        return d;
    }();
    return desc;
}

authoring::DocumentRefusal QpJsonFormat::to_bytes(const authoring::DocumentSource& source,
                                                 std::string& out) const noexcept {
    out.clear();
    const DocumentRefusal unwritable = find_unwritable(source);
    if (unwritable != DocumentRefusal::ok) return unwritable;

    // Built aside and moved in, so a refusal raised partway through could not leave a prefix behind even
    // if a later change made the writer able to fail.
    std::string built;
    built.reserve(256 + source.graph.node_count() * 96);
    write_document(built, source);
    out = std::move(built);
    return DocumentRefusal::ok;
}

authoring::DocumentRefusal QpJsonFormat::from_bytes(std::string_view bytes,
                                                   authoring::DocumentSnapshot& out) const noexcept {
    // Parsed into a local snapshot and moved into place only on success, so a caller whose load failed
    // still holds what it held before rather than a graph missing the nodes the parser had not reached.
    return parse_document(bytes, out);
}

}  // namespace qp::plugins::qpjson
