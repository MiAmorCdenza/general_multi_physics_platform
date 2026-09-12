/**
 * @file instrument.cpp
 * @brief The instrument registry.
 */
#include <qp/runtime/instrument/instrument.hpp>

#include <algorithm>

namespace qp::runtime {

diag::Result<void> InstrumentRegistry::add(IInstrument* instrument) noexcept {
    if (instrument == nullptr) return diag::ErrorCode::invalid_argument;

    const InstrumentDesc& desc = instrument->describe();
    if (desc.id.empty()) return diag::ErrorCode::invalid_argument;

    // Refused rather than replaced. Two devices under one id would make "what measured this" depend on
    // registration order, and a run's record of which instrument produced a reading would then name whichever
    // was registered last -- which is the same class of defect as a record that cannot be looked up.
    if (find(desc.id) != nullptr) return diag::ErrorCode::duplicate_connection;

    instruments_.push_back(instrument);
    return {};
}

diag::Result<void> InstrumentRegistry::remove(std::string_view id) noexcept {
    const auto it = std::find_if(instruments_.begin(), instruments_.end(),
                                 [id](const IInstrument* instrument) {
                                     return instrument->describe().id == id;
                                 });
    if (it == instruments_.end()) return diag::ErrorCode::unknown_node;
    instruments_.erase(it);
    return {};
}

IInstrument* InstrumentRegistry::find(std::string_view id) const noexcept {
    if (id.empty()) return nullptr;
    for (IInstrument* instrument : instruments_) {
        if (instrument->describe().id == id) return instrument;
    }
    return nullptr;
}

std::vector<IInstrument*> InstrumentRegistry::all() const noexcept { return instruments_; }

std::vector<IInstrument*> InstrumentRegistry::adjustable() const noexcept {
    std::vector<IInstrument*> out;
    out.reserve(instruments_.size());
    for (IInstrument* instrument : instruments_) {
        if (instrument->describe().adjustable) out.push_back(instrument);
    }
    return out;
}

}  // namespace qp::runtime
