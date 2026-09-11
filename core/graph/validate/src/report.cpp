/**
 * @file report.cpp
 * @brief Implementation of the validation report.
 */
#include <qp/graph/validate/report.hpp>

namespace qp::graph {

std::string Issue::to_text() const {
    std::string out = "[";
    out += to_string(severity);
    out += "] ";
    if (node.valid()) {
        out += "node#";
        out += std::to_string(node.index);
        out += "/";
        out += std::to_string(node.generation);
        if (port != 0) {
            out += is_output ? " out" : " in";
            out += std::to_string(port);
        }
        out += ": ";
    }
    out += message;
    if (!hint.empty()) {
        out += "（建议：";
        out += hint;
        out += "）";
    }
    return out;
}

void Report::error(qp::diag::ErrorCode code, std::string message, NodeId node, PortNumber port,
                   bool is_output, std::string hint) {
    Issue i{};
    i.severity = Severity::error;
    i.code = code;
    i.node = node;
    i.port = port;
    i.is_output = is_output;
    i.message = std::move(message);
    i.hint = std::move(hint);
    issues_.push_back(std::move(i));
}

void Report::warn(qp::diag::ErrorCode code, std::string message, NodeId node, PortNumber port,
                  bool is_output, std::string hint) {
    Issue i{};
    i.severity = Severity::warning;
    i.code = code;
    i.node = node;
    i.port = port;
    i.is_output = is_output;
    i.message = std::move(message);
    i.hint = std::move(hint);
    issues_.push_back(std::move(i));
}

bool Report::ok() const noexcept {
    for (const auto& i : issues_) {
        if (i.is_error()) return false;
    }
    return true;
}

std::size_t Report::count(Severity s) const noexcept {
    std::size_t n = 0;
    for (const auto& i : issues_) {
        if (i.severity == s) ++n;
    }
    return n;
}

Severity Report::worst() const noexcept {
    auto w = Severity::info;
    for (const auto& i : issues_) {
        if (i.severity > w) w = i.severity;
    }
    return w;
}

std::string Report::to_text() const {
    std::string out;
    for (const auto& i : issues_) {
        out += i.to_text();
        out += "\n";
    }
    return out;
}

}  // namespace qp::graph
