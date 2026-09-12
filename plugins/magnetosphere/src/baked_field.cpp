/**
 * @file baked_field.cpp
 * @brief The index arithmetic, the trilinear blend, and the clamp.
 *
 * The layout is the one `abi::LatticeDesc` describes, written here once: point `(i, j, k)` is
 * `(i * ny + j) * nz + k`, and a vector's three components follow consecutively. `field::get_component` computes
 * the same offset from the same description, so a kernel that reads through the field vocabulary and this file's
 * own `sample` are reading the same memory -- which is the property that makes the table usable by both.
 */
#include <qp/plugins/magnetosphere/baked_field.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace qp::plugins::magnetosphere {
namespace {

namespace abi = qp::abi;

/// @brief Largest index not past the end, for a table of `n` nodes. `n` is at least 2 for a built table.
[[nodiscard]] std::uint32_t last_index(std::uint32_t n) noexcept { return n > 0 ? n - 1 : 0; }

}  // namespace

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

qp::graph::field::FieldValue BakedField::view() const noexcept {
    qp::graph::field::FieldValue out;
    if (empty() || nx_ < 2 || ny_ < 2 || nz_ < 2) {
        // An empty or degenerate table has no valid description: `abi::is_consistent` refuses a volume with a
        // zero count, and a one-node axis has no interval to interpolate over. Refusing the description is what
        // stops a kernel from being handed a table it would read incorrectly rather than not at all.
        return out;
    }
    // A field's dimension is the **same seven exponents as the quantity**: a magnetic field is
    // kg / (A s^2), which in this unit system is M = 1, T = -2, I = -1.
    abi::FieldDim tesla;
    tesla.M = 1;
    tesla.T = -2;
    tesla.I = -1;
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

    // Factional index along each axis, clamped into `[0, n - 1]`. Clamping here rather than at the value is what
    // makes an out-of-range point read the **boundary** rather than an extrapolation: the fractional part pins to
    // 0 or `n - 2`, so the blend is between the two outermost nodes.
    const double fx = (point.x - origin_.x) / spacing_.x;
    const double fy = (point.y - origin_.y) / spacing_.y;
    const double fz = (point.z - origin_.z) / spacing_.z;
    const double last_x = static_cast<double>(last_index(nx_));
    const double last_y = static_cast<double>(last_index(ny_));
    const double last_z = static_cast<double>(last_index(nz_));

    const bool outside = fx < 0.0 || fy < 0.0 || fz < 0.0 || fx > last_x || fy > last_y || fz > last_z;
    if (outside) ++clamped_;

    const double cx = fx < 0.0 ? 0.0 : (fx > last_x ? last_x : fx);
    const double cy = fy < 0.0 ? 0.0 : (fy > last_y ? last_y : fy);
    const double cz = fz < 0.0 ? 0.0 : (fz > last_z ? last_z : fz);

    // The cell's lower corner and the blend weights. `floor` of a value already inside `[0, n-1]` is at most
    // `n-2` after the guard, so `i + 1 <= n - 1` and the eight reads are always in range.
    const auto lower = [](double f, std::uint32_t n) -> std::uint32_t {
        const double floor_f = std::floor(f);
        const double max_lower = static_cast<double>(n >= 2 ? n - 2 : 0);
        const double clamped = floor_f < 0.0 ? 0.0 : (floor_f > max_lower ? max_lower : floor_f);
        return static_cast<std::uint32_t>(clamped);
    };
    const std::uint32_t i = lower(cx, nx_);
    const std::uint32_t j = lower(cy, ny_);
    const std::uint32_t k = lower(cz, nz_);
    const double tx = cx - static_cast<double>(i);
    const double ty = cy - static_cast<double>(j);
    const double tz = cz - static_cast<double>(k);
    const double s = 1.0 - tx;
    const double t = 1.0 - ty;
    const double u = 1.0 - tz;

    const auto at = [this](std::uint32_t a, std::uint32_t b, std::uint32_t c) noexcept -> const double* {
        return data_.data() + ((static_cast<std::size_t>(a) * ny_ + b) * nz_ + c) * 3;
    };
    const double* v000 = at(i, j, k);
    const double* v100 = at(i + 1, j, k);
    const double* v010 = at(i, j + 1, k);
    const double* v110 = at(i + 1, j + 1, k);
    const double* v001 = at(i, j, k + 1);
    const double* v101 = at(i + 1, j, k + 1);
    const double* v011 = at(i, j + 1, k + 1);
    const double* v111 = at(i + 1, j + 1, k + 1);

    Vec3 out{};
    double* out_components[3] = {&out.x, &out.y, &out.z};
    for (int component = 0; component < 3; ++component) {
        const double w000 = v000[component] * s * t * u;
        const double w100 = v100[component] * tx * t * u;
        const double w010 = v010[component] * s * ty * u;
        const double w110 = v110[component] * tx * ty * u;
        const double w001 = v001[component] * s * t * tz;
        const double w101 = v101[component] * tx * t * tz;
        const double w011 = v011[component] * s * ty * tz;
        const double w111 = v111[component] * tx * ty * tz;
        *out_components[component] = w000 + w100 + w010 + w110 + w001 + w101 + w011 + w111;
    }
    return out;
}

}  // namespace qp::plugins::magnetosphere
