/**
 * @file baked_field.cpp
 * @brief The index arithmetic, the trilinear blend, and the clamp -- once, for both kinds of caller.
 *
 * The layout is the one `abi::LatticeDesc` describes, written here once: point `(i, j, k)` is
 * `(i * ny + j) * nz + k`, and a vector's three components follow consecutively. `field::get_component` computes
 * the same offset from the same description, so a kernel that reads through the field vocabulary and this file's
 * own `sample` are reading the same memory -- which is the property that makes the table usable by both.
 *
 * The blend is written as a **template over the element type** and then wrapped by two free functions, because
 * two different things hold a baked table: a baker holds a `BakedField`, and everything downstream of the plugin
 * boundary holds a `field::FieldValue`. They must not each have their own trilinear sampler: two copies of this
 * arithmetic are two answers to "what is the field between two nodes", and the disagreement would show up as a
 * particle curving slightly differently from where an emitter launched it -- which no test of either half would
 * catch.
 *
 * ADR-0005 makes `f32` the default for field data and the particle path uses `f64`, so the samples can be either
 * and the read dispatches on the descriptor. A sampler that cast to `double*` and read an `f32` table would read
 * two samples as one number.
 */
#include <qp/plugins/magnetosphere/baked_field.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace qp::plugins::magnetosphere {
namespace {

namespace abi = qp::abi;
namespace gfield = qp::graph::field;

/// @brief Largest index not past the end, for a table of `n` nodes. `n` is at least 2 for a built table.
[[nodiscard]] std::uint32_t last_index(std::uint32_t n) noexcept { return n > 0 ? n - 1 : 0; }

/// @brief The lower cell index along one axis, for a fractional index already inside the grid.
[[nodiscard]] std::uint32_t lower_index(double fractional, std::uint32_t n) noexcept {
    const double floor_f = std::floor(fractional);
    const double max_lower = static_cast<double>(n >= 2 ? n - 2 : 0);
    const double clamped = floor_f < 0.0 ? 0.0 : (floor_f > max_lower ? max_lower : floor_f);
    return static_cast<std::uint32_t>(clamped);
}

/// @brief One axis of a trilinear read: the lower index and the fraction between it and the next node.
struct Cell final {
    std::uint32_t lower = 0;
    double fraction = 0.0;
};

/// @brief Where `point` falls along one axis, clamped so a sample outside the grid reads the boundary node.
[[nodiscard]] Cell cell_of(double point, double origin, double spacing, std::uint32_t n) noexcept {
    const double last = static_cast<double>(last_index(n));
    const double fractional = (point - origin) / spacing;
    // Clamped, not extrapolated, and the two tests are **negated** on purpose: a particle position can reach
    // infinity, and `fractional` is then a NaN, which compares false against every bound. Written as
    // `f < 0 ? 0 : (f > last ? last : f)` a NaN falls through both arms and reaches
    // `static_cast<std::uint32_t>`, which is undefined behaviour rather than a wrong number.
    double clamped = 0.0;
    if (fractional > last) {
        clamped = last;
    } else if (fractional > 0.0) {
        clamped = fractional;
    }
    Cell cell;
    cell.lower = lower_index(clamped, n);
    cell.fraction = clamped - static_cast<double>(cell.lower);
    return cell;
}

/// @brief Whether a description is a volume of at least two nodes an axis, which is what a blend needs.
[[nodiscard]] bool is_volume(const gfield::FieldValue& table, bool want_vector) noexcept {
    if (!gfield::is_readable(table)) return false;
    if (table.kind() != gfield::Kind::Volume) return false;
    if (table.is_vector() != want_vector) return false;
    return table.desc.count[0] >= 2 && table.desc.count[1] >= 2 && table.desc.count[2] >= 2;
}

/// @brief The trilinear weight of one corner of a cell, from its bit pattern: bit 0 is `x`, bit 1 is `y`, bit 2
///        is `z`.
///
/// The corner order this defines is the order the two reads below list their eight pointers in, and it is the
/// only place the two have to agree: a weight applied to the wrong corner is a field that is right at every node
/// and wrong everywhere between them.
[[nodiscard]] constexpr double weight_of(int corner, double tx, double ty, double tz) noexcept {
    const double wx = (corner & 1) != 0 ? tx : 1.0 - tx;
    const double wy = (corner & 2) != 0 ? ty : 1.0 - ty;
    const double wz = (corner & 4) != 0 ? tz : 1.0 - tz;
    return wx * wy * wz;
}

/// @brief The trilinear blend of one vector volume at `point` (SI), for the element type `T`.
template <typename T>
[[nodiscard]] Vec3 read_vector(const gfield::FieldValue& table, const Vec3& origin, const Vec3& spacing,
                               const Vec3& point) noexcept {
    const std::uint32_t nx = table.desc.count[0];
    const std::uint32_t ny = table.desc.count[1];
    const std::uint32_t nz = table.desc.count[2];
    const auto* data = static_cast<const T*>(table.data);
    if (data == nullptr) return Vec3{};

    const Cell cx = cell_of(point.x, origin.x, spacing.x, nx);
    const Cell cy = cell_of(point.y, origin.y, spacing.y, ny);
    const Cell cz = cell_of(point.z, origin.z, spacing.z, nz);
    const std::uint32_t i = cx.lower;
    const std::uint32_t j = cy.lower;
    const std::uint32_t k = cz.lower;

    const auto at = [data, ny, nz](std::uint32_t a, std::uint32_t b, std::uint32_t c) noexcept -> const T* {
        return data + ((static_cast<std::size_t>(a) * ny + b) * nz + c) * 3;
    };
    const T* corner[8] = {at(i, j, k),         at(i + 1, j, k),         at(i, j + 1, k),
                          at(i + 1, j + 1, k), at(i, j, k + 1),         at(i + 1, j, k + 1),
                          at(i, j + 1, k + 1), at(i + 1, j + 1, k + 1)};
    const double tx = cx.fraction;
    const double ty = cy.fraction;
    const double tz = cz.fraction;
    // The eight weights are written out rather than looped over a bit pattern: this is the innermost arithmetic
    // in the kit -- eight reads and eight multiplies per component per sub-step -- and a loop with
    // `(index >> bit) & 1` in it costs more than the lines it saves.
    const double weight[8] = {weight_of(0, tx, ty, tz), weight_of(1, tx, ty, tz), weight_of(2, tx, ty, tz),
                              weight_of(3, tx, ty, tz), weight_of(4, tx, ty, tz), weight_of(5, tx, ty, tz),
                              weight_of(6, tx, ty, tz), weight_of(7, tx, ty, tz)};
    Vec3 out{};
    double* component[3] = {&out.x, &out.y, &out.z};
    for (int axis = 0; axis < 3; ++axis) {
        double sum = 0.0;
        for (int corner_index = 0; corner_index < 8; ++corner_index) {
            sum += static_cast<double>(corner[corner_index][axis]) * weight[corner_index];
        }
        *component[axis] = sum;
    }
    return out;
}

/// @brief The trilinear blend of one scalar volume at `point` (SI), for the element type `T`.
template <typename T>
[[nodiscard]] double read_scalar(const gfield::FieldValue& table, const Vec3& origin, const Vec3& spacing,
                                 const Vec3& point) noexcept {
    const std::uint32_t nx = table.desc.count[0];
    const std::uint32_t ny = table.desc.count[1];
    const std::uint32_t nz = table.desc.count[2];
    const auto* data = static_cast<const T*>(table.data);
    if (data == nullptr) return 0.0;

    const Cell cx = cell_of(point.x, origin.x, spacing.x, nx);
    const Cell cy = cell_of(point.y, origin.y, spacing.y, ny);
    const Cell cz = cell_of(point.z, origin.z, spacing.z, nz);
    const std::uint32_t i = cx.lower;
    const std::uint32_t j = cy.lower;
    const std::uint32_t k = cz.lower;
    const auto at = [data, ny, nz](std::uint32_t a, std::uint32_t b, std::uint32_t c) noexcept -> double {
        return static_cast<double>(data[(static_cast<std::size_t>(a) * ny + b) * nz + c]);
    };
    const double tx = cx.fraction;
    const double ty = cy.fraction;
    const double tz = cz.fraction;
    const double weight[8] = {weight_of(0, tx, ty, tz), weight_of(1, tx, ty, tz), weight_of(2, tx, ty, tz),
                              weight_of(3, tx, ty, tz), weight_of(4, tx, ty, tz), weight_of(5, tx, ty, tz),
                              weight_of(6, tx, ty, tz), weight_of(7, tx, ty, tz)};
    const double corner[8] = {at(i, j, k),         at(i + 1, j, k),         at(i, j + 1, k),
                              at(i + 1, j + 1, k), at(i, j, k + 1),         at(i + 1, j, k + 1),
                              at(i, j + 1, k + 1), at(i + 1, j + 1, k + 1)};
    double sum = 0.0;
    for (int corner_index = 0; corner_index < 8; ++corner_index) sum += corner[corner_index] * weight[corner_index];
    return sum;
}

}  // namespace

Vec3 sample_baked(const gfield::FieldValue& table, const Vec3& origin_m, const Vec3& spacing_m,
                  const Vec3& point_m) noexcept {
    if (!is_volume(table, /*want_vector=*/true)) return Vec3{};
    if (table.desc.element == abi::ElementType::f32) return read_vector<float>(table, origin_m, spacing_m, point_m);
    return read_vector<double>(table, origin_m, spacing_m, point_m);
}

double sample_baked_scalar(const gfield::FieldValue& table, const Vec3& origin_m, const Vec3& spacing_m,
                           const Vec3& point_m) noexcept {
    if (!is_volume(table, /*want_vector=*/false)) return 0.0;
    if (table.desc.element == abi::ElementType::f32) {
        return read_scalar<float>(table, origin_m, spacing_m, point_m);
    }
    return read_scalar<double>(table, origin_m, spacing_m, point_m);
}

BakedField::BakedField(Vec3 origin, Vec3 spacing, std::uint32_t nx, std::uint32_t ny, std::uint32_t nz)
    : origin_(origin), spacing_(spacing), nx_(nx), ny_(ny), nz_(nz) {
    const std::size_t points = static_cast<std::size_t>(nx) * ny * nz;
    data_.assign(points * 3, 0.0);
}

std::size_t BakedField::offset_of(std::uint32_t i, std::uint32_t j, std::uint32_t k) const noexcept {
    if (i >= nx_ || j >= ny_ || k >= nz_) return data_.size();
    const std::size_t point = (static_cast<std::size_t>(i) * ny_ + j) * nz_ + k;
    return point * 3;
}

Vec3 BakedField::node_position(std::uint32_t i, std::uint32_t j, std::uint32_t k) const noexcept {
    return Vec3{origin_.x + static_cast<double>(i) * spacing_.x,
                origin_.y + static_cast<double>(j) * spacing_.y,
                origin_.z + static_cast<double>(k) * spacing_.z};
}

void BakedField::set_node(std::uint32_t i, std::uint32_t j, std::uint32_t k, const Vec3& value) noexcept {
    const std::size_t offset = offset_of(i, j, k);
    if (offset >= data_.size()) return;
    data_[offset + 0] = value.x;
    data_[offset + 1] = value.y;
    data_[offset + 2] = value.z;
}

gfield::FieldValue BakedField::view() const noexcept {
    // A field's dimension is the **same seven exponents as the quantity**: a magnetic field is kg / (A s^2),
    // which in this unit system is M = 1, T = -2, I = -1.
    return view(tesla_dimension());
}

gfield::FieldValue BakedField::view(const abi::FieldDim dimension) const noexcept {
    gfield::FieldValue out;
    if (empty() || nx_ < 2 || ny_ < 2 || nz_ < 2) {
        // An empty or degenerate table has no valid description: `abi::is_consistent` refuses a volume with a
        // zero count, and a one-node axis has no interval to interpolate over. Refusing the description is what
        // stops a kernel from being handed a table it would read incorrectly rather than not at all.
        return out;
    }
    const abi::FieldDim& tesla = dimension;
    out.desc = abi::make_lattice(abi::LatticeKind::volume, abi::ComponentKind::vector,
                                 abi::ElementType::f64, tesla, nx_, ny_, nz_);
    out.data = data_.data();
    out.bytes = abi::data_bytes(out.desc);
    return out;
}

Vec3 BakedField::sample(const Vec3& point) const noexcept {
    if (empty() || nx_ < 2 || ny_ < 2 || nz_ < 2) return Vec3{};
    if (!is_finite(point)) {
        // A non-finite position is not a place, and answering it with the corner value would hide the model
        // failure that produced it. Counted as clamped, because "this sample is not usable" is the same finding
        // from the caller's point of view.
        ++clamped_;
        return Vec3{};
    }

    // The clamp **accounting** is this class's and the blend is the free function's: one implementation of each,
    // and the count stays a property of the table rather than of whoever sampled it.
    const double fx = (point.x - origin_.x) / spacing_.x;
    const double fy = (point.y - origin_.y) / spacing_.y;
    const double fz = (point.z - origin_.z) / spacing_.z;
    const double last_x = static_cast<double>(last_index(nx_));
    const double last_y = static_cast<double>(last_index(ny_));
    const double last_z = static_cast<double>(last_index(nz_));
    if (fx < 0.0 || fy < 0.0 || fz < 0.0 || fx > last_x || fy > last_y || fz > last_z) ++clamped_;

    return sample_baked(view(), origin_, spacing_, point);
}

}  // namespace qp::plugins::magnetosphere
