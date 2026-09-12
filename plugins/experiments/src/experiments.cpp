/**
 * @file experiments.cpp
 * @brief The envelope: a marker, a version, a name, and one nested `.qpd` document.
 *
 * The whole file is one scanner plus a delegation. The scanner is deliberately small and deliberately dumb: it
 * finds the `document` member and hands its **raw text** to the document format, so there is exactly one parser
 * for graph JSON in this repository and this file cannot disagree with it about what a node is. What the scanner
 * does have to get right is where that member ends, which means matching braces through strings and their
 * escapes -- the one place a naive scanner returns a plausible wrong answer instead of a refusal.
 */
#include <qp/plugins/experiments/experiments.hpp>

#include <qp/graph/ir/descriptor.hpp>
#include <qp/graph/structure/graph.hpp>

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace qp::plugins::experiments {
namespace {

using qp::authoring::DocumentRefusal;

/// @brief Whether `c` is whitespace JSON ignores.
[[nodiscard]] bool is_space(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/// @brief Moves `i` past whitespace.
void skip_space(std::string_view text, std::size_t& i) noexcept {
    while (i < text.size() && is_space(text[i])) ++i;
}

/// @brief Reads a JSON string starting at `text[i] == '"'`, unescaping only what a name can hold.
///
/// `\uXXXX` is refused rather than decoded, and this is the one place the envelope is stricter than JSON: an
/// escape only this file reads is an escape no writer of this format produces, and a second unescaper for a name
/// is not worth its bugs. The refusal surfaces as `malformed`, which is the right answer for bytes that are not
/// this format's.
[[nodiscard]] bool read_string(std::string_view text, std::size_t& i, std::string& out) {
    if (i >= text.size() || text[i] != '"') return false;
    ++i;
    std::string built;
    while (i < text.size()) {
        const char c = text[i];
        if (c == '"') {
            ++i;
            out = std::move(built);
            return true;
        }
        if (c == '\\') {
            if (i + 1 >= text.size()) return false;
            switch (text[i + 1]) {
                case '"': built.push_back('"'); break;
                case '\\': built.push_back('\\'); break;
                case '/': built.push_back('/'); break;
                case 'b': built.push_back('\b'); break;
                case 'f': built.push_back('\f'); break;
                case 'n': built.push_back('\n'); break;
                case 'r': built.push_back('\r'); break;
                case 't': built.push_back('\t'); break;
                default: return false;
            }
            i += 2;
            continue;
        }
        built.push_back(c);
        ++i;
    }
    return false;
}

/// @brief Reads the integer starting at `i`, if there is one.
[[nodiscard]] bool read_int(std::string_view text, std::size_t& i, std::int64_t& out) {
    skip_space(text, i);
    const std::size_t start = i;
    if (i < text.size() && (text[i] == '-' || text[i] == '+')) ++i;
    std::int64_t value = 0;
    bool any = false;
    while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
        value = value * 10 + (text[i] - '0');
        ++i;
        any = true;
    }
    if (!any) {
        i = start;
        return false;
    }
    out = value;
    return true;
}

/// @brief Reads a member key, i.e. a string followed by a colon.
[[nodiscard]] bool read_key(std::string_view text, std::size_t& i, std::string& out) {
    skip_space(text, i);
    if (!read_string(text, i, out)) return false;
    skip_space(text, i);
    if (i >= text.size() || text[i] != ':') return false;
    ++i;
    return true;
}

/**
 * @brief Finds the extent of the JSON value starting at `i`, as a **text** range.
 *
 * For an object or an array this is a brace or bracket match that steps over strings and their escapes; for a
 * scalar it is the scalar's own end. Returns false when the text ends first, which is the `truncated` case rather
 * than the `malformed` one -- and returning the wrong one of those two sends the user looking in the wrong place.
 */
[[nodiscard]] bool slice_value(std::string_view text, std::size_t& i, std::size_t& begin, std::size_t& end) {
    skip_space(text, i);
    begin = i;
    if (i >= text.size()) return false;

    const char first = text[i];
    if (first == '{' || first == '[') {
        const char open = first;
        const char close = first == '{' ? '}' : ']';
        int depth = 0;
        while (i < text.size()) {
            const char c = text[i];
            if (c == '"') {
                // Through the string, escapes and all: a brace inside a string is data, and skipping this is
                // the difference between a wrong answer and a refusal.
                std::string ignored;
                if (!read_string(text, i, ignored)) return false;
                continue;
            }
            if (c == open) {
                ++depth;
            } else if (c == close) {
                --depth;
                if (depth == 0) {
                    ++i;
                    end = i;
                    return true;
                }
            }
            ++i;
        }
        return false;
    }
    if (first == '"') {
        std::string ignored;
        if (!read_string(text, i, ignored)) return false;
        end = i;
        return true;
    }
    // A literal: a number, `true`, `false` or `null`. Runs to the first character that cannot be part of one.
    while (i < text.size() && !is_space(text[i]) && text[i] != ',' && text[i] != '}' && text[i] != ']') ++i;
    end = i;
    return true;
}

/// @brief What the envelope holds, once it has been scanned. The document is filled, not returned.
struct Envelope final {
    /// The document, already parsed by the delegated format.
    qp::authoring::DocumentSnapshot* document = nullptr;
    /// Where the name goes, or null when the caller does not want it.
    std::string* name = nullptr;
    /// Where the description goes, or null.
    std::string* description = nullptr;
};

/**
 * @brief Scans the envelope, filling whatever `out` asks for.
 *
 * One scanner for both entry points. `from_bytes` wants the document and nothing else, and `read` wants the
 * document plus the two strings, and a second scanner for the second caller would be a second statement of the
 * envelope's shape -- which is the defect this plugin exists to avoid one level up, so it is not repeated here.
 */
[[nodiscard]] DocumentRefusal parse_envelope(std::string_view bytes, const qp::authoring::IDocumentFormat& inner,
                                             Envelope out) {
    std::size_t i = 0;
    skip_space(bytes, i);
    if (i >= bytes.size()) return DocumentRefusal::truncated;
    if (bytes[i] != '{') return DocumentRefusal::not_a_document;
    ++i;

    // The marker is required **first**, for the reason the document format gives about its own: a file that
    // begins with something else is a different kind of file, and calling it malformed would send its owner
    // looking for a syntax error that is not there.
    std::string key;
    if (!read_key(bytes, i, key)) return DocumentRefusal::not_a_document;
    if (key != ExperimentFormat::kMarkerKey) return DocumentRefusal::not_a_document;

    std::int64_t version = 0;
    if (!read_int(bytes, i, version)) return DocumentRefusal::malformed;
    if (version > ExperimentFormat::kVersion) return DocumentRefusal::unsupported_version;
    if (version < 1) return DocumentRefusal::malformed;

    bool has_document = false;
    while (true) {
        skip_space(bytes, i);
        if (i >= bytes.size()) return DocumentRefusal::truncated;
        if (bytes[i] == '}') {
            ++i;
            break;
        }
        if (bytes[i] != ',') return DocumentRefusal::malformed;
        ++i;
        if (!read_key(bytes, i, key)) return DocumentRefusal::malformed;

        if (key == "document") {
            std::size_t begin = 0;
            std::size_t end = 0;
            if (!slice_value(bytes, i, begin, end)) return DocumentRefusal::malformed;
            // Handed over as text, and parsed by the format that wrote it. This file never looks inside, and a
            // refusal about the graph is therefore the graph format's own sentence.
            const DocumentRefusal loaded =
                inner.from_bytes(bytes.substr(begin, end - begin), *out.document);
            if (loaded != DocumentRefusal::ok) return loaded;
            has_document = true;
            continue;
        }
        if (key == "name" || key == "description") {
            std::string text;
            skip_space(bytes, i);
            if (!read_string(bytes, i, text)) return DocumentRefusal::malformed;
            std::string* target = key == "name" ? out.name : out.description;
            if (target != nullptr) *target = std::move(text);
            continue;
        }
        // An unknown member is refused rather than skipped, for the reason the document format gives: the
        // version marker is the compatibility mechanism, so a member outside this version's shape means the
        // file and this build disagree about what an experiment is.
        return DocumentRefusal::malformed;
    }

    skip_space(bytes, i);
    if (i != bytes.size()) return DocumentRefusal::malformed;   // bytes after the envelope
    if (!has_document) {
        // An experiment without a document is not an empty experiment; it is a file this format does not define.
        return DocumentRefusal::malformed;
    }
    return DocumentRefusal::ok;
}

/// @brief Whether an input port is a setting rather than a signal.
///
/// `connectable == false` is the descriptor's own word for it, and it is the definition this file uses rather than
/// a rule of its own: an input a wire can feed is an input a student does not set.
[[nodiscard]] bool is_setting(const qp::graph::PortDesc& port) noexcept {
    return !port.connectable && port.number != 0;
}

/// @brief Whether the value kind a port presents can be described as a number.
[[nodiscard]] bool is_numeric_kind(qp::ports::ValueKind kind) noexcept {
    return kind == qp::ports::ValueKind::f64 || kind == qp::ports::ValueKind::f32 ||
           kind == qp::ports::ValueKind::i64 || kind == qp::ports::ValueKind::dimension;
}

}  // namespace

ExperimentFormat::ExperimentFormat(const qp::authoring::IDocumentFormat& inner,
                                   const qp::graph::INodeCatalog& catalog) noexcept
    : inner_(&inner), catalog_(&catalog) {}

const qp::authoring::DocumentFormatDesc& ExperimentFormat::format() const noexcept {
    static const qp::authoring::DocumentFormatDesc desc = [] {
        qp::authoring::DocumentFormatDesc d;
        d.name = "qp.experiment.json";
        d.label = "Teaching experiment (JSON)";
        d.extensions = {"qpx"};
        d.is_text = true;
        return d;
    }();
    return desc;
}

qp::authoring::DocumentRefusal ExperimentFormat::to_bytes(const qp::authoring::DocumentSource& source,
                                                          std::string& out) const noexcept {
    out.clear();

    // The document first, through the format that owns the marshalling. A refusal here is the document's own --
    // a live field handle, a string that is not UTF-8, a non-finite number -- and reporting it unchanged is the
    // point: this envelope adds no requirement the document does not already have to meet.
    std::string document;
    const DocumentRefusal written = inner_->to_bytes(source, document);
    if (written != DocumentRefusal::ok) return written;

    std::string built;
    built.reserve(document.size() + 64);
    built += "{\"";
    built += kMarkerKey;
    built += "\": ";
    built += std::to_string(kVersion);
    built += ",\n  \"document\": ";
    built += document;
    built += "}\n";
    out = std::move(built);
    return DocumentRefusal::ok;
}

qp::authoring::DocumentRefusal ExperimentFormat::from_bytes(std::string_view bytes,
                                                            qp::authoring::DocumentSnapshot& out) const
    noexcept {
    // Parsed into a local snapshot and moved into place only on success, so a caller whose load failed still
    // holds what it held before rather than a graph missing the nodes the parser had not reached.
    qp::authoring::DocumentSnapshot loaded;
    Envelope envelope;
    envelope.document = &loaded;
    const DocumentRefusal scanned = parse_envelope(bytes, *inner_, envelope);
    if (scanned != DocumentRefusal::ok) return scanned;
    out = std::move(loaded);
    return DocumentRefusal::ok;
}

qp::authoring::DocumentRefusal ExperimentFormat::read(std::string_view bytes, Experiment& out) const noexcept {
    Experiment loaded;
    Envelope envelope;
    envelope.document = &loaded.document;
    envelope.name = &loaded.name;
    envelope.description = &loaded.description;
    const DocumentRefusal scanned = parse_envelope(bytes, *inner_, envelope);
    if (scanned != DocumentRefusal::ok) return scanned;

    // The parameters are **derived from the document**, never read from the envelope. A file cannot claim a
    // setting the graph has no input for, and adding a setting port to a node type makes it appear here with no
    // edit to this file -- which is the property that keeps a teacher's assignment from disagreeing with its
    // graph. What the envelope carries is the part the document cannot know: which experiment this is.
    const qp::graph::Graph& graph = loaded.document.graph;
    for (const qp::graph::NodeSlot& slot : graph.slots()) {
        if (!slot.occupied) continue;
        const qp::graph::Node& node = slot.node;
        const qp::graph::NodeDesc* type = catalog_->find(node.type_name);
        if (type == nullptr) continue;
        for (const qp::graph::PortDesc& port : type->inputs) {
            if (!is_setting(port)) continue;
            // A setting the graph's own edges feed is not a setting for this experiment: the wire is the
            // assignment. The descriptor says what the port **is**; the graph says what this file did with it.
            const qp::graph::PortRef input{node.id, port.number, qp::graph::PortDirection::input};
            if (graph.incoming(input) != nullptr) continue;

            ExperimentParameter parameter;
            parameter.node_index = node.id.index;
            parameter.node_generation = node.id.generation;
            parameter.type_name = node.type_name;
            parameter.node_name = node.name;
            parameter.port = port.number;
            parameter.name = port.name;
            parameter.label = port.label;
            parameter.unit = port.unit_symbol;
            parameter.bounded = port.has_range;
            parameter.min_value = port.min_value;
            parameter.max_value = port.max_value;
            for (const qp::graph::ParamValue& stored : node.params) {
                if (stored.number != port.number) continue;
                parameter.numeric = is_numeric_kind(stored.value.kind());
                parameter.value = stored.value.to_double();
                break;
            }
            loaded.parameters.push_back(std::move(parameter));
        }
    }

    out = std::move(loaded);
    return DocumentRefusal::ok;
}

}  // namespace qp::plugins::experiments
