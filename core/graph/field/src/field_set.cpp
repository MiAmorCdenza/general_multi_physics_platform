/**
 * @file field_set.cpp
 * @brief The store's four operations, and the two invariants that make them safe to hand out views from.
 *
 * Small on purpose. The interesting decisions are in the header -- f64 only, keyed by publisher, the vector taken
 * by value -- and what is left here is arithmetic that has to be exactly right:
 *
 *   1. **the byte count is the descriptor's, not the caller's.** A `publish` that trusted the vector's length
 *      would accept a 100-point descriptor with 99 points of data and hand a kernel a view that reads one past
 *      the end; `abi::data_bytes` is the only authority on how big a described lattice is, and it is what the
 *      check uses;
 *   2. **an entry's samples are never relocated.** `std::vector<Entry>` moves `Entry` objects when it grows, and
 *      moving a `std::vector<double>` moves a pointer rather than the bytes, so a `FieldValue` already handed
 *      out stays valid. That is a property of the container rather than of this code, which is exactly why it is
 *      written down: a later change from `std::vector<Entry>` to something that copies samples into place would
 *      break every view in flight, and it would break them silently.
 */
#include <qp/graph/field/field_set.hpp>

#include <algorithm>
#include <cstddef>
#include <utility>

namespace qp::graph::field {

bool FieldSet::publish(FieldKey key, const qp::abi::LatticeDesc& desc, std::vector<double> samples) {
    if (!key.valid()) return false;
    if (!qp::abi::is_consistent(desc)) return false;
    // f64 only, and a refusal rather than a narrowing: see the header's reopening condition. A bake that
    // produced f32 and got a handle back anyway would have had its precision chosen by a fallback.
    if (desc.element != qp::abi::ElementType::f64) return false;

    const std::uint64_t points = qp::abi::point_count(desc);
    const std::uint64_t expected = points * qp::abi::component_count(desc.component);
    if (static_cast<std::uint64_t>(samples.size()) != expected) return false;

    const auto slot = std::lower_bound(entries_.begin(), entries_.end(), key,
                                       [](const Entry& e, FieldKey k) { return e.key < k; });
    if (slot != entries_.end() && slot->key == key) {
        // A re-bake: replace the shape and the samples, keep the entry's place in the order.
        slot->desc = desc;
        slot->samples = std::move(samples);
        return true;
    }
    Entry entry;
    entry.key = key;
    entry.desc = desc;
    entry.samples = std::move(samples);
    entries_.insert(slot, std::move(entry));
    return true;
}

FieldValue FieldSet::view(FieldKey key) const noexcept {
    FieldValue out;
    const Entry* entry = find(key);
    if (entry == nullptr) return out;
    out.desc = entry->desc;
    out.data = entry->samples.data();
    out.bytes = qp::abi::data_bytes(entry->desc);
    return out;
}

bool FieldSet::contains(FieldKey key) const noexcept { return find(key) != nullptr; }

bool FieldSet::erase(FieldKey key) noexcept {
    const auto slot = std::lower_bound(entries_.begin(), entries_.end(), key,
                                       [](const Entry& e, FieldKey k) { return e.key < k; });
    if (slot == entries_.end() || !(slot->key == key)) return false;
    entries_.erase(slot);
    return true;
}

void FieldSet::clear() noexcept { entries_.clear(); }

std::vector<FieldKey> FieldSet::keys() const {
    std::vector<FieldKey> out;
    out.reserve(entries_.size());
    for (const Entry& entry : entries_) out.push_back(entry.key);
    return out;
}

std::uint64_t FieldSet::bytes() const noexcept {
    std::uint64_t total = 0;
    for (const Entry& entry : entries_) total += qp::abi::data_bytes(entry.desc);
    return total;
}

const FieldSet::Entry* FieldSet::find(FieldKey key) const noexcept {
    const auto slot = std::lower_bound(entries_.begin(), entries_.end(), key,
                                       [](const Entry& e, FieldKey k) { return e.key < k; });
    if (slot == entries_.end() || !(slot->key == key)) return nullptr;
    return &*slot;
}

}  // namespace qp::graph::field
