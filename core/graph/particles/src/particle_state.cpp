/**
 * @file particle_state.cpp
 * @brief The four arrays, and the descriptions that say what they hold.
 *
 * The descriptions are built once per call from a table, and the table is the file comment of the header written
 * a second time -- deliberately. A slot's dimension is the one fact in this module that no compiler checks: a
 * buffer of doubles is a buffer of doubles, and a kernel that divides a position by a mass because the executor
 * labelled the slot wrongly would produce numbers that look like metres per second. So the table is here, next
 * to the switch that reads it, and `particles.state.slots_are_double_precision` asserts what it says.
 */
#include <qp/graph/particles/particle_state.hpp>

#include <cstddef>
#include <utility>

namespace qp::graph::particles {
namespace {

namespace abi = qp::abi;

/// @brief `units::Dim` as the ABI's field dimension. The two are layout-compatible by construction.
[[nodiscard]] constexpr abi::FieldDim field_dim(qp::units::Dim d) noexcept {
    abi::FieldDim out;
    out.L = d.L;
    out.M = d.M;
    out.T = d.T;
    out.I = d.I;
    out.Th = d.Th;
    out.N = d.N;
    out.J = d.J;
    return out;
}

/// @brief What one slot holds: how many components, and of what.
struct SlotShape final {
    abi::ComponentKind component = abi::ComponentKind::scalar;
    abi::FieldDim dimension{};
};

/// @brief The shape of a slot. Total over the four enumerators.
///
/// The dimensions, written out: a length, a velocity, a charge per mass, and a pure number. The status is
/// dimensionless rather than given a made-up unit, because a status code with a unit would be a quantity, and
/// `field::is_valid_field` is entitled to be asked what a buffer holds and get a true answer.
[[nodiscard]] constexpr SlotShape shape_of(ParticleState::Slot slot) noexcept {
    switch (slot) {
        case ParticleState::Slot::position:
            return SlotShape{abi::ComponentKind::vector,
                             field_dim(qp::units::dims::length)};
        case ParticleState::Slot::velocity:
            return SlotShape{abi::ComponentKind::vector,
                             field_dim(qp::units::dims::velocity)};
        case ParticleState::Slot::charge_mass:
            return SlotShape{abi::ComponentKind::scalar,
                             field_dim(qp::units::dims::charge / qp::units::dims::mass)};
        case ParticleState::Slot::status:
            return SlotShape{abi::ComponentKind::scalar, abi::kDimensionless};
    }
    return SlotShape{};
}

/// @brief How many components a slot holds, from the same table. Used by the component accessors.
[[nodiscard]] constexpr std::size_t component_count_of(ParticleState::Slot slot) noexcept {
    return abi::component_count(shape_of(slot).component);
}

}  // namespace

const char* to_string(Status status) noexcept {
    switch (status) {
        case Status::live: return "live";
        case Status::absorbed: return "absorbed";
        case Status::escaped: return "escaped";
    }
    return "unknown";
}

ParticleState::ParticleState(std::size_t n) { resize(n); }

void ParticleState::resize(std::size_t n) {
    count_ = n;
    position_.assign(n * 3, 0.0);
    velocity_.assign(n * 3, 0.0);
    charge_mass_.assign(n, 0.0);
    // `assign` rather than `resize`, so a resize that reuses the allocation still clears the statuses. A
    // reallocation that kept the previous run's `absorbed` flags would make the new batch start half dead, and
    // the count a report quotes would be about particles that no longer exist.
    status_.assign(n, static_cast<double>(Status::live));
}

const double* ParticleState::data_of(Slot slot) const noexcept {
    switch (slot) {
        case Slot::position: return position_.data();
        case Slot::velocity: return velocity_.data();
        case Slot::charge_mass: return charge_mass_.data();
        case Slot::status: return status_.data();
    }
    return nullptr;
}

field::FieldValue ParticleState::slot(Slot which) noexcept {
    // The const overload does the work through `const_cast` rather than duplicating the description: two copies
    // of the table above is the drift the table's own comment warns about, and the constness of `this` says
    // nothing about whether a caller will write through the returned view.
    return const_cast<const ParticleState*>(this)->slot(which);
}

field::FieldValue ParticleState::slot(Slot which) const noexcept {
    const SlotShape shape = shape_of(which);
    field::FieldValue value;
    if (count_ == 0) {
        // An empty batch has **no** valid description, and that is the ABI's rule rather than a choice here:
        // `abi::is_consistent` refuses a line lattice with a zero count, because a lattice with no points is not
        // a lattice. So an empty batch reports an invalid view, and every caller that steps one is refused by
        // `BatchView::valid()` before a kernel ever sees it -- rather than being handed a description that
        // claims zero points and reads as a valid empty field.
        return value;
    }
    const double* data = data_of(which);
    if (data == nullptr) return value;

    value.desc = abi::make_lattice(abi::LatticeKind::line, shape.component, abi::ElementType::f64,
                                   shape.dimension, static_cast<std::uint32_t>(count_));
    value.data = data;
    value.bytes = abi::data_bytes(value.desc);
    return value;
}

double ParticleState::at(std::size_t i, std::size_t component, Slot which) const noexcept {
    if (i >= count_ || component >= component_count_of(which)) return 0.0;
    const double* data = data_of(which);
    if (data == nullptr) return 0.0;
    return data[i * component_count_of(which) + component];
}

void ParticleState::set(std::size_t i, std::size_t component, Slot which, double value) noexcept {
    if (i >= count_ || component >= component_count_of(which)) return;
    // The mutable overload's view is the only way to reach the array as a `double*`, and it is used here rather
    // than a second switch over the four vectors: one place decides which array a slot names.
    field::FieldValue view = slot(which);
    if (view.data == nullptr) return;
    auto* writable = const_cast<double*>(static_cast<const double*>(view.data));
    writable[i * component_count_of(which) + component] = value;
}

Status ParticleState::status_of(std::size_t i) const noexcept {
    if (i >= count_) return Status::escaped;
    const double raw = status_[i];
    // A status byte that is not one of the three is treated as `escaped`, for the same reason an out-of-range
    // index is: the only way to write one is a corrupt buffer or a kernel that wrote a number into a slot it
    // does not own, and reporting `live` for a particle in an unknown state would keep stepping it.
    if (raw == static_cast<double>(Status::live)) return Status::live;
    if (raw == static_cast<double>(Status::absorbed)) return Status::absorbed;
    return Status::escaped;
}

void ParticleState::set_status(std::size_t i, Status status) noexcept {
    if (i >= count_) return;
    status_[i] = static_cast<double>(status);
}

std::size_t ParticleState::live_count() const noexcept { return count_with(Status::live); }

std::size_t ParticleState::count_with(Status status) const noexcept {
    std::size_t total = 0;
    for (std::size_t i = 0; i < count_; ++i) {
        if (status_of(i) == status) ++total;
    }
    return total;
}

bool ParticleState::is_consistent() const noexcept {
    return position_.size() == count_ * 3 && velocity_.size() == count_ * 3 &&
           charge_mass_.size() == count_ && status_.size() == count_;
}

}  // namespace qp::graph::particles
