/**
 * @file registry.cpp
 * @brief Implementation of the kernel registry.
 */
#include <qp/graph/kernels/registry.hpp>

#include <algorithm>
#include <cstddef>

namespace qp::graph::kernels {
namespace {

/// @brief The sentinel index meaning "not found".
constexpr std::size_t kNotFound = static_cast<std::size_t>(-1);

}  // namespace

std::size_t KernelRegistry::index_of(KernelId id) const noexcept {
    if (!id.valid()) return kNotFound;
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        if (slots_[i].id == id) return i;
    }
    return kNotFound;
}

std::size_t KernelRegistry::index_of_name(std::string_view name) const noexcept {
    if (name.empty()) return kNotFound;
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        if (slots_[i].name == name) return i;
    }
    return kNotFound;
}

diag::Result<KernelId> KernelRegistry::declare(std::string_view name) {
    // Result<T> carries an ErrorCode only, by design: a failure that a caller can
    // branch on is a code, and free text is never a judgement basis. The code is
    // the contract; a message would be a second, weaker one.
    if (name.empty()) return diag::ErrorCode::invalid_argument;

    // Refusing a duplicate rather than overwriting: an overwritten kernel would
    // silently change what an existing plan operation does, and a graph saved
    // against the first plugin would then run the second plugin's physics.
    if (index_of_name(name) != kNotFound) return diag::ErrorCode::duplicate_connection;

    Slot slot;
    slot.id = KernelId{next_index_++};
    slot.name = name;
    slot.committed = false;
    slots_.push_back(slot);
    return slot.id;
}

diag::Result<void> KernelRegistry::commit(KernelId id, const KernelDesc& desc) {
    const std::size_t i = index_of(id);
    if (i == kNotFound) return diag::ErrorCode::unknown_node;
    if (slots_[i].committed) return diag::ErrorCode::duplicate_connection;
    if (!desc.valid()) return diag::ErrorCode::invalid_argument;

    // The name is the identity a saved document refers to. Committing a
    // different name than the one reserved would make the reservation a lie.
    if (desc.name != slots_[i].name) return diag::ErrorCode::plugin_incompatible;

    slots_[i].desc = desc;
    slots_[i].committed = true;
    return {};
}

diag::Result<void> KernelRegistry::drop(KernelId id) {
    const std::size_t i = index_of(id);
    if (i == kNotFound) return diag::ErrorCode::unknown_node;
    slots_.erase(slots_.begin() + static_cast<std::ptrdiff_t>(i));
    return {};
}

diag::Result<KernelId> KernelRegistry::add(const KernelDesc& desc) {
    auto declared = declare(desc.name);
    if (!declared) return declared.error();

    const KernelId id = declared.value();
    auto committed = commit(id, desc);
    if (!committed) {
        // Leave no trace: a reservation that outlives a failed registration is
        // exactly the "reserved forever" state the two-phase commit exists to
        // prevent.
        const auto dropped = drop(id);
        (void)dropped;
        return committed.error();
    }
    return id;
}

const KernelDesc* KernelRegistry::find(KernelId id) const noexcept {
    const std::size_t i = index_of(id);
    if (i == kNotFound || !slots_[i].committed) return nullptr;
    return &slots_[i].desc;
}

const KernelDesc* KernelRegistry::find_by_name(std::string_view name) const noexcept {
    const std::size_t i = index_of_name(name);
    if (i == kNotFound || !slots_[i].committed) return nullptr;
    return &slots_[i].desc;
}

std::size_t KernelRegistry::size() const noexcept {
    return static_cast<std::size_t>(
        std::count_if(slots_.begin(), slots_.end(), [](const Slot& s) { return s.committed; }));
}

std::size_t KernelRegistry::reserved_count() const noexcept {
    return static_cast<std::size_t>(
        std::count_if(slots_.begin(), slots_.end(), [](const Slot& s) { return !s.committed; }));
}

std::vector<KernelDesc> KernelRegistry::list() const noexcept {
    // Reservation order, not sorted. The editor lists kernels in the order the
    // plugins registered them; sorting would be a second, redundant ordering
    // rule that a user could not predict.
    std::vector<KernelDesc> out;
    out.reserve(size());
    for (const Slot& s : slots_) {
        if (s.committed) out.push_back(s.desc);
    }
    return out;
}

}  // namespace qp::graph::kernels
