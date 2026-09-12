/**
 * @file guard.cpp
 * @brief The fault log: counting what misbehaved, and deciding when to stop asking.
 */
#include <qp/plugin/guard.hpp>

#include <algorithm>

namespace qp::plugin {

Fault FaultLog::record(std::string label, diag::ErrorCode code, std::string what) {
    Fault fault;
    fault.label = label;

    // Counted from the log rather than from a counter beside it: one source of truth for "how many times
    // has this label failed", so a fault that was recorded cannot be missing from the count and a count
    // cannot exist without a fault behind it.
    fault.count = static_cast<int>(std::count_if(
        faults_.begin(), faults_.end(),
        [&label](const Fault& f) { return f.label == label; })) + 1;
    fault.code = code;
    fault.what = std::move(what);
    fault.quarantined = fault.count >= kFaultLimit;
    faults_.push_back(std::move(fault));
    return faults_.back();
}

bool FaultLog::is_quarantined(std::string_view label) const noexcept {
    int count = 0;
    for (const Fault& fault : faults_) {
        if (fault.label == label) ++count;
    }
    return count >= kFaultLimit;
}

std::size_t FaultLog::quarantined_count() const noexcept {
    std::size_t n = 0;
    for (const Fault& fault : faults_) {
        if (fault.quarantined) ++n;
    }
    return n;
}

}  // namespace qp::plugin
