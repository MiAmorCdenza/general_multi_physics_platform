/**
 * @file test_field.cpp
 * @brief Tests for the field module: shape, components, and readability.
 *
 * Test case ids match the @tests fields in the field headers byte for byte.
 *
 * The interesting cases here are the ones where a description is well formed but
 * the buffer is not usable: a null pointer, a span that stops one byte short, and
 * an f64 view with the wrong alignment. Each of those has to be *detected*,
 * because the alternative is reading past the end of a buffer or performing a
 * misaligned load.
 */
#include <catch2/catch_test_macros.hpp>

#include <qp/graph/field.hpp>

#include <array>
#include <cstdint>
#include <type_traits>
#include <vector>

using namespace qp::graph::field;

namespace {

/// @brief A dimension with a single axis exponent, for readable fixtures.
constexpr qp::abi::FieldDim dim_of(std::int8_t e0, std::int8_t e1 = 0, std::int8_t e2 = 0,
                               std::int8_t e3 = 0, std::int8_t e4 = 0, std::int8_t e5 = 0,
                               std::int8_t e6 = 0) {
    return qp::abi::FieldDim{e0, e1, e2, e3, e4, e5, e6};
}

/// @brief A line field of `n` f32 scalars, backed by `storage`.
FieldValue make_line_f32(const std::vector<float>& storage, std::uint32_t n) {
    FieldValue v;
    v.desc = qp::abi::make_lattice(qp::abi::LatticeKind::line, qp::abi::ComponentKind::scalar,
                               qp::abi::ElementType::f32, dim_of(1), n);
    v.data = storage.data();
    v.bytes = storage.size() * sizeof(float);
    return v;
}

/// @brief A line field of `n` 3-component f32 vectors, backed by `storage`.
FieldValue make_line_vec3_f32(const std::vector<float>& storage, std::uint32_t n) {
    FieldValue v;
    v.desc = qp::abi::make_lattice(qp::abi::LatticeKind::line, qp::abi::ComponentKind::vector,
                               qp::abi::ElementType::f32, dim_of(0, 0, 0, 1), n);
    v.data = storage.data();
    v.bytes = storage.size() * sizeof(float);
    return v;
}

}  // namespace

// ===========================================================================
// Kind and component vocabulary
// ===========================================================================

TEST_CASE("field.kind_matches_abi", "[field]") {
    // The two enumerations describe the same four shapes and must not drift.
    STATIC_REQUIRE(static_cast<std::uint8_t>(Kind::Point) ==
                   static_cast<std::uint8_t>(qp::abi::LatticeKind::point));
    STATIC_REQUIRE(static_cast<std::uint8_t>(Kind::Line) ==
                   static_cast<std::uint8_t>(qp::abi::LatticeKind::line));
    STATIC_REQUIRE(static_cast<std::uint8_t>(Kind::Plane) ==
                   static_cast<std::uint8_t>(qp::abi::LatticeKind::plane));
    STATIC_REQUIRE(static_cast<std::uint8_t>(Kind::Volume) ==
                   static_cast<std::uint8_t>(qp::abi::LatticeKind::volume));

    // Round trip through the abi vocabulary, both directions.
    for (const qp::abi::LatticeKind k : {qp::abi::LatticeKind::point, qp::abi::LatticeKind::line,
                                     qp::abi::LatticeKind::plane, qp::abi::LatticeKind::volume}) {
        REQUIRE(abi_kind(kind_of(k)) == k);
    }
    for (const Kind k : {Kind::Point, Kind::Line, Kind::Plane, Kind::Volume}) {
        REQUIRE(kind_of(abi_kind(k)) == k);
    }
}

TEST_CASE("field.components.count", "[field]") {
    STATIC_REQUIRE(component_count(Components::Scalar) == 1);
    STATIC_REQUIRE(component_count(Components::Vector) == 3);

    // And they agree with the abi notion of component count.
    STATIC_REQUIRE(component_count(components_of(qp::abi::ComponentKind::scalar)) ==
                   qp::abi::component_count(qp::abi::ComponentKind::scalar));
    STATIC_REQUIRE(component_count(components_of(qp::abi::ComponentKind::vector)) ==
                   qp::abi::component_count(qp::abi::ComponentKind::vector));
    STATIC_REQUIRE(abi_component(Components::Scalar) == qp::abi::ComponentKind::scalar);
    STATIC_REQUIRE(abi_component(Components::Vector) == qp::abi::ComponentKind::vector);
}

TEST_CASE("field.components.valid_range", "[field]") {
    REQUIRE(has_component(Components::Scalar, 0));
    REQUIRE_FALSE(has_component(Components::Scalar, 1));

    REQUIRE(has_component(Components::Vector, 0));
    REQUIRE(has_component(Components::Vector, 1));
    REQUIRE(has_component(Components::Vector, 2));
    REQUIRE_FALSE(has_component(Components::Vector, 3));

    // The boundary is exact: a huge index must not be accepted by wrapping.
    REQUIRE_FALSE(has_component(Components::Vector, 0xFFFFFFFFu));
}

// ===========================================================================
// Point counts and byte spans
// ===========================================================================

TEST_CASE("field.layout.point_counts", "[field]") {
    // point: always exactly one, whatever the counts say. A single value is not
    // an empty lattice, and a plugin that sets counts on a point field must not
    // silently turn it into a multi-point field.
    const qp::abi::LatticeDesc point = qp::abi::make_lattice(
        qp::abi::LatticeKind::point, qp::abi::ComponentKind::scalar, qp::abi::ElementType::f32, dim_of(1), 7, 7, 7);
    REQUIRE(qp::abi::point_count(point) == 1);

    const qp::abi::LatticeDesc line = qp::abi::make_lattice(
        qp::abi::LatticeKind::line, qp::abi::ComponentKind::scalar, qp::abi::ElementType::f32, dim_of(1), 5);
    REQUIRE(qp::abi::point_count(line) == 5);

    const qp::abi::LatticeDesc plane = qp::abi::make_lattice(
        qp::abi::LatticeKind::plane, qp::abi::ComponentKind::scalar, qp::abi::ElementType::f32, dim_of(1), 3, 4);
    REQUIRE(qp::abi::point_count(plane) == 12);

    const qp::abi::LatticeDesc volume = qp::abi::make_lattice(
        qp::abi::LatticeKind::volume, qp::abi::ComponentKind::vector, qp::abi::ElementType::f32, dim_of(1), 2, 3, 4);
    REQUIRE(qp::abi::point_count(volume) == 24);
}

TEST_CASE("field.layout.byte_span", "[field]") {
    // A scalar f32 line of 8 points is 32 bytes.
    const qp::abi::LatticeDesc scalar = qp::abi::make_lattice(
        qp::abi::LatticeKind::line, qp::abi::ComponentKind::scalar, qp::abi::ElementType::f32, dim_of(1), 8);
    REQUIRE(qp::abi::data_bytes(scalar) == 32);

    // The same shape in f64 doubles, and a vector field triples again.
    const qp::abi::LatticeDesc scalar64 = qp::abi::make_lattice(
        qp::abi::LatticeKind::line, qp::abi::ComponentKind::scalar, qp::abi::ElementType::f64, dim_of(1), 8);
    REQUIRE(qp::abi::data_bytes(scalar64) == 64);

    const qp::abi::LatticeDesc vector = qp::abi::make_lattice(
        qp::abi::LatticeKind::line, qp::abi::ComponentKind::vector, qp::abi::ElementType::f32, dim_of(1), 8);
    REQUIRE(qp::abi::data_bytes(vector) == 96);

    const qp::abi::LatticeDesc vector64 = qp::abi::make_lattice(
        qp::abi::LatticeKind::line, qp::abi::ComponentKind::vector, qp::abi::ElementType::f64, dim_of(1), 8);
    REQUIRE(qp::abi::data_bytes(vector64) == 192);

    // The stride agrees with the span, which is what is_consistent checks.
    for (const qp::abi::LatticeDesc& d : {scalar, scalar64, vector, vector64}) {
        REQUIRE(d.spacing_bytes == qp::abi::expected_spacing(d));
        REQUIRE(qp::abi::data_bytes(d) ==
                static_cast<std::uint64_t>(d.spacing_bytes) * qp::abi::point_count(d));
    }
}

TEST_CASE("field.value_points", "[field]") {
    std::vector<float> storage(8, 0.0F);
    const FieldValue v = make_line_f32(storage, 8);
    REQUIRE(v.point_count() == 8);
    REQUIRE(v.required_bytes() == 32);
    REQUIRE(v.kind() == Kind::Line);
    REQUIRE(v.is_scalar());
    REQUIRE_FALSE(v.is_vector());
}

TEST_CASE("field.value_bytes", "[field]") {
    // A large volume grid: the span must be computed in 64 bits. If it were
    // computed in 32, 1024^3 points of 3 f32 each would wrap to a small number
    // and read as a valid, tiny field.
    const qp::abi::LatticeDesc huge = qp::abi::make_lattice(
        qp::abi::LatticeKind::volume, qp::abi::ComponentKind::vector, qp::abi::ElementType::f64, dim_of(1),
        1024, 1024, 1024);
    const std::uint64_t expected = 1024ULL * 1024ULL * 1024ULL * 3ULL * 8ULL;
    REQUIRE(qp::abi::point_count(huge) == 1024ULL * 1024ULL * 1024ULL);
    REQUIRE(qp::abi::data_bytes(huge) == expected);
    REQUIRE(expected > 0xFFFFFFFFULL);   // the point of the test
}

// ===========================================================================
// Validity
// ===========================================================================

TEST_CASE("field.value_is_valid", "[field]") {
    std::vector<float> storage(8, 0.0F);

    SECTION("a consistent description with a covering buffer is readable") {
        const FieldValue v = make_line_f32(storage, 8);
        REQUIRE(is_valid_field(v));
        REQUIRE(is_readable(v));
    }

    SECTION("a well formed description is valid even without a buffer") {
        // The two questions are deliberately separate: is the shape describable,
        // and can it be read. A plugin that has not run yet has the first without
        // the second, and a caller must be able to say which.
        FieldValue v = make_line_f32(storage, 8);
        v.data = nullptr;
        REQUIRE(is_valid_field(v));
        REQUIRE_FALSE(is_readable(v));
    }

    SECTION("an inconsistent description is neither valid nor readable") {
        FieldValue v = make_line_f32(storage, 8);
        v.desc.reserved = 1;
        REQUIRE_FALSE(is_valid_field(v));
        REQUIRE_FALSE(is_readable(v));
    }

    SECTION("a span one byte short is not readable") {
        FieldValue v = make_line_f32(storage, 8);
        v.bytes = v.required_bytes() - 1;
        REQUIRE(is_valid_field(v));
        REQUIRE_FALSE(is_readable(v));
    }

    SECTION("a span exactly the required size is readable") {
        FieldValue v = make_line_f32(storage, 8);
        v.bytes = v.required_bytes();
        REQUIRE(is_readable(v));
    }

    SECTION("a zero-point line is rejected as inconsistent") {
        // count[0] == 0 cannot describe anything, and accepting it would make
        // "empty" and "malformed" indistinguishable.
        FieldValue v = make_line_f32(storage, 0);
        REQUIRE_FALSE(is_valid_field(v));
        REQUIRE_FALSE(is_readable(v));
    }

    SECTION("a point field ignores the counts but not the stride") {
        FieldValue v;
        v.desc = qp::abi::make_lattice(qp::abi::LatticeKind::point, qp::abi::ComponentKind::scalar,
                                   qp::abi::ElementType::f32, dim_of(1), 99);
        v.data = storage.data();
        v.bytes = 4;
        REQUIRE(v.point_count() == 1);
        REQUIRE(is_readable(v));
    }
}

TEST_CASE("field.value_is_valid_f64_alignment", "[field]") {
    // A plugin can hand over a view into the middle of a packed buffer. For f32
    // that is legal (4-byte alignment is all a float needs); for f64 it is not,
    // and the resulting load is undefined behaviour rather than a wrong number.
    alignas(8) std::array<double, 8> aligned{1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0};
    const std::uint64_t total = aligned.size() * sizeof(double);

    FieldValue good;
    good.desc = qp::abi::make_lattice(qp::abi::LatticeKind::line, qp::abi::ComponentKind::scalar,
                                  qp::abi::ElementType::f64, dim_of(1), 8);
    good.data = aligned.data();
    good.bytes = total;
    REQUIRE(is_readable(good));
    REQUIRE(get_component(good, 7, 0) == 8.0);

    // Offset by 4 bytes: still inside the buffer, but no longer 8-byte aligned.
    const auto* misaligned = reinterpret_cast<const void*>(
        reinterpret_cast<const char*>(aligned.data()) + 4);
    FieldValue bad;
    bad.desc = qp::abi::make_lattice(qp::abi::LatticeKind::line, qp::abi::ComponentKind::scalar,
                                 qp::abi::ElementType::f64, dim_of(1), 7);
    bad.data = misaligned;
    bad.bytes = total - 4;
    REQUIRE(is_valid_field(bad));          // the shape is fine
    REQUIRE_FALSE(is_readable(bad));       // the pointer is not

    // f32 has no such problem: 4-byte alignment is implied by any float pointer.
    std::vector<float> floats(8, 1.0F);
    FieldValue f32_misaligned = make_line_f32(floats, 7);
    f32_misaligned.data = reinterpret_cast<const char*>(floats.data()) + 4;
    f32_misaligned.bytes = floats.size() * sizeof(float) - 4;
    REQUIRE(is_readable(f32_misaligned));
}

// ===========================================================================
// Component access
// ===========================================================================

TEST_CASE("field.component_access", "[field]") {
    SECTION("scalar f32") {
        const std::vector<float> storage{1.5F, -2.5F, 3.25F};
        const FieldValue v = make_line_f32(storage, 3);
        REQUIRE(is_readable(v));
        REQUIRE(get_component(v, 0, 0) == 1.5);
        REQUIRE(get_component(v, 1, 0) == -2.5);
        REQUIRE(get_component(v, 2, 0) == 3.25);
    }

    SECTION("scalar f64 is not narrowed") {
        // The scalar measurement chain is f64 (ADR-0005); reading it through a
        // double is exact, and reading an f32 field widens exactly. Neither
        // direction may lose a bit.
        const std::vector<double> storage{0.1, 0.2, 0.3};
        FieldValue v;
        v.desc = qp::abi::make_lattice(qp::abi::LatticeKind::line, qp::abi::ComponentKind::scalar,
                                   qp::abi::ElementType::f64, dim_of(1), 3);
        v.data = storage.data();
        v.bytes = storage.size() * sizeof(double);
        REQUIRE(is_readable(v));
        REQUIRE(get_component(v, 0, 0) == 0.1);
        REQUIRE(get_component(v, 1, 0) == 0.2);
        REQUIRE(get_component(v, 2, 0) == 0.3);
    }

    SECTION("vector components stay in their own lane") {
        // Point 1 is (10, 11, 12): the three components must not bleed into each
        // other, which is what a wrong stride produces.
        const std::vector<float> storage{0.0F, 0.0F, 0.0F,
                                         10.0F, 11.0F, 12.0F,
                                         20.0F, 21.0F, 22.0F};
        const FieldValue v = make_line_vec3_f32(storage, 3);
        REQUIRE(v.is_vector());
        REQUIRE(is_readable(v));
        REQUIRE(v.required_bytes() == 36);
        REQUIRE(get_component(v, 0, 0) == 0.0);
        REQUIRE(get_component(v, 0, 2) == 0.0);
        REQUIRE(get_component(v, 1, 0) == 10.0);
        REQUIRE(get_component(v, 1, 1) == 11.0);
        REQUIRE(get_component(v, 1, 2) == 12.0);
        REQUIRE(get_component(v, 2, 2) == 22.0);
    }
}

TEST_CASE("field.components.rejects_out_of_range", "[field]") {
    // Out-of-range access returns 0.0 rather than whatever sits next in memory.
    // The alternative is a silent read of a neighbouring sample, which looks like
    // plausible data and is therefore much worse than a zero.
    const std::vector<float> storage{7.0F, 8.0F, 9.0F};
    const FieldValue scalar = make_line_f32(storage, 3);
    REQUIRE(get_component(scalar, 0, 1) == 0.0);      // scalar has no y
    REQUIRE(get_component(scalar, 3, 0) == 0.0);      // past the end
    REQUIRE(get_component(scalar, 100, 0) == 0.0);

    const std::vector<float> vec_storage{1.0F, 2.0F, 3.0F};
    const FieldValue vec = make_line_vec3_f32(vec_storage, 1);
    REQUIRE(get_component(vec, 0, 2) == 3.0);         // in range
    REQUIRE(get_component(vec, 0, 3) == 0.0);         // one past the last
    REQUIRE(get_component(vec, 1, 0) == 0.0);         // past the last point

    // The largest possible indices must not wrap into range.
    REQUIRE(get_component(scalar, 0xFFFFFFFFFFFFFFFFULL, 0) == 0.0);
    REQUIRE(get_component(scalar, 0, 0xFFFFFFFFu) == 0.0);
}

TEST_CASE("field.dimension_is_per_component", "[field]") {
    // The dimension describes one component, not one point. A scalar field of
    // length 100 and a vector field of length 100 with the same FieldDim carry
    // the same physical quantity per component -- which is why the dimension
    // must not be multiplied by the component count anywhere in this module.
    const qp::abi::FieldDim velocity = dim_of(1, 0, 0, 0, 0, 0, -1);   // length / time

    std::vector<float> scalar_storage(4, 1.0F);
    FieldValue scalar;
    scalar.desc = qp::abi::make_lattice(qp::abi::LatticeKind::line, qp::abi::ComponentKind::scalar,
                                    qp::abi::ElementType::f32, velocity, 4);
    scalar.data = scalar_storage.data();
    scalar.bytes = scalar_storage.size() * sizeof(float);

    std::vector<float> vector_storage(12, 1.0F);
    FieldValue vec;
    vec.desc = qp::abi::make_lattice(qp::abi::LatticeKind::line, qp::abi::ComponentKind::vector,
                                 qp::abi::ElementType::f32, velocity, 4);
    vec.data = vector_storage.data();
    vec.bytes = vector_storage.size() * sizeof(float);

    // Compare the seven exponents rather than the whole struct: abi::FieldDim is
    // a C-subset POD with no comparison operator (adding one would be ABI surface
    // for no benefit), and spelling the exponents out says what "same dimension"
    // actually means.
    const auto same_dim = [](qp::abi::FieldDim a, qp::abi::FieldDim b) {
        return a.L == b.L && a.M == b.M && a.T == b.T && a.I == b.I &&
               a.Th == b.Th && a.N == b.N && a.J == b.J;
    };
    REQUIRE(same_dim(scalar.dimension(), velocity));
    REQUIRE(same_dim(vec.dimension(), velocity));
    REQUIRE(same_dim(scalar.dimension(), vec.dimension()));

    // Same points, same dimension, but the byte spans differ by the component
    // count -- the difference lives in the span, never in the dimension.
    REQUIRE(scalar.point_count() == vec.point_count());
    REQUIRE(vec.required_bytes() == scalar.required_bytes() * 3);
}

TEST_CASE("field.value_is_trivially_copyable", "[field]") {
    // A FieldValue is passed by value through evaluation; if it were not a POD
    // the hot path would acquire a copy constructor invocation per node.
    STATIC_REQUIRE(std::is_trivially_copyable_v<FieldValue>);
    STATIC_REQUIRE(std::is_standard_layout_v<FieldValue>);
    STATIC_REQUIRE(sizeof(FieldValue) <= 64);

    // A default-constructed value is an invalid, empty field rather than an
    // uninitialised one: reading it must be well defined.
    const FieldValue empty{};
    REQUIRE_FALSE(is_valid_field(empty));
    REQUIRE_FALSE(is_readable(empty));
    REQUIRE(empty.point_count() == 1);   // "point" kind, but no data
    REQUIRE(get_component(empty, 0, 0) == 0.0);
}

/// @brief A self-consistent volume of f64 vectors on a `3 x 2 x 2` grid: 12 points, 36 values.
[[nodiscard]] qp::abi::LatticeDesc volume_desc(std::uint32_t nx = 3, std::uint32_t ny = 2,
                                               std::uint32_t nz = 2) {
    qp::abi::FieldDim tesla;
    tesla.M = 1;
    tesla.T = -2;
    tesla.I = -1;
    return qp::abi::make_lattice(qp::abi::LatticeKind::volume, qp::abi::ComponentKind::vector,
                                 qp::abi::ElementType::f64, tesla, nx, ny, nz);
}

TEST_CASE("field.set.publishes_and_reads_back", "[field]") {
    // **The gap this type closes.** A `field_handle` port value carries a `LatticeDesc` and no data, by design;
    // without a publisher, nothing anywhere could turn a handle into samples. So the case that matters is the
    // round trip: publish, then read the same numbers back through the field vocabulary -- not through the
    // store's own API, because a store that returned its own idea of the data would prove nothing about what a
    // kernel would see.
    FieldSet fields;
    REQUIRE(fields.size() == 0);
    REQUIRE(fields.bytes() == 0);
    REQUIRE_FALSE(fields.contains(FieldKey{1, 1}));

    const auto desc = volume_desc();
    std::vector<double> samples(36);
    for (std::size_t i = 0; i < samples.size(); ++i) samples[i] = static_cast<double>(i) * 0.5;

    REQUIRE(fields.publish(FieldKey{1, 1}, desc, samples));
    REQUIRE(fields.size() == 1);
    REQUIRE(fields.contains(FieldKey{1, 1}));

    const FieldValue view = fields.view(FieldKey{1, 1});
    REQUIRE(is_readable(view));
    REQUIRE(view.kind() == Kind::Volume);
    REQUIRE(view.is_vector());
    REQUIRE(view.desc.element == qp::abi::ElementType::f64);
    REQUIRE(view.point_count() == 12);
    REQUIRE(view.required_bytes() == 36 * sizeof(double));
    // The dimension survives the round trip, because it is what makes the samples a magnetic field rather than
    // twelve vectors of nothing in particular.
    REQUIRE(view.dimension().M == 1);
    REQUIRE(view.dimension().T == -2);
    REQUIRE(view.dimension().I == -1);
    // And the values, at both ends of the layout: point `(i * ny + j) * nz + k`, then component.
    REQUIRE(get_component(view, 0, 0) == 0.0);
    REQUIRE(get_component(view, 0, 2) == 1.0);
    REQUIRE(get_component(view, 11, 0) == 16.5);
    REQUIRE(get_component(view, 11, 2) == 17.5);

    REQUIRE(fields.bytes() == 36 * sizeof(double));

    // The store is the owner, so the caller's vector may go away. That is the property `abi::FieldBuffer`'s
    // "the publisher keeps it alive" sentence delegates, and the one a store that kept a pointer would fail
    // while passing every assertion above.
    samples.clear();
    samples.shrink_to_fit();
    REQUIRE(get_component(fields.view(FieldKey{1, 1}), 11, 2) == 17.5);

    // An absent key is an **unreadable** value, not an error and not a zero field: "no field was baked here" and
    // "the field is zero" are the same force and different experiments.
    const FieldValue absent = fields.view(FieldKey{9, 9});
    REQUIRE_FALSE(is_readable(absent));

    // The set is keyed by publisher, so two nodes with the same grid shape do not collide -- the failure a
    // descriptor-keyed store would have, and the one that would silently run one node's field twice.
    std::vector<double> other(36, 7.0);
    REQUIRE(fields.publish(FieldKey{2, 1}, desc, std::move(other)));
    REQUIRE(fields.size() == 2);
    REQUIRE(get_component(fields.view(FieldKey{1, 1}), 0, 0) == 0.0);
    REQUIRE(get_component(fields.view(FieldKey{2, 1}), 0, 0) == 7.0);

    // A default-constructed key names nothing and is never publishable: node index zero is the graph's own
    // invalid slot, and a store that accepted it would have an entry nobody could name.
    REQUIRE_FALSE(kNoField.valid());
    REQUIRE_FALSE(fields.publish(kNoField, desc, std::vector<double>(36, 1.0)));
    REQUIRE_FALSE(fields.contains(kNoField));
}

TEST_CASE("field.set.refuses_what_it_cannot_own", "[field]") {
    // Every refusal is a shape that would otherwise hand a kernel a view it reads past the end of, or a
    // precision nobody chose. A store that "took what it was given" would be a corruption path rather than a
    // container, which is the same reasoning `abi::is_consistent` exists for one layer down.
    FieldSet fields;
    const auto desc = volume_desc();
    std::vector<double> right_size(36, 1.0);

    // A descriptor that is not self-consistent: a volume whose byte stride contradicts its component type.
    qp::abi::LatticeDesc broken = desc;
    broken.spacing_bytes = 4;
    REQUIRE_FALSE(fields.publish(FieldKey{1, 1}, broken, right_size));
    REQUIRE(fields.size() == 0);

    // The wrong element type. ADR-0005 makes f32 the field default and the particle path uses f64; a store that
    // narrowed silently here would decide the precision of a run in a fallback nobody wrote down.
    qp::abi::LatticeDesc single = desc;
    single.element = qp::abi::ElementType::f32;
    single.spacing_bytes = qp::abi::expected_spacing(single);
    REQUIRE_FALSE(fields.publish(FieldKey{1, 1}, single, right_size));

    // A sample count that is not the descriptor's own, in both directions: too few is a read past the end and
    // too many is a claim about data the descriptor will never describe. Neither is a "close enough".
    REQUIRE_FALSE(fields.publish(FieldKey{1, 1}, desc, std::vector<double>(35, 1.0)));
    REQUIRE_FALSE(fields.publish(FieldKey{1, 1}, desc, std::vector<double>(37, 1.0)));
    REQUIRE_FALSE(fields.publish(FieldKey{1, 1}, desc, std::vector<double>{}));
    REQUIRE(fields.size() == 0);

    // The count is `points x components`, not `points`: a scalar-sized vector for a vector field is exactly the
    // off-by-three a hand-written check gets wrong.
    REQUIRE_FALSE(fields.publish(FieldKey{1, 1}, desc, std::vector<double>(12, 1.0)));

    // And the shape it does accept comes up, so the refusals above are not a store that refuses everything.
    REQUIRE(fields.publish(FieldKey{1, 1}, desc, right_size));
    REQUIRE(fields.size() == 1);
}

TEST_CASE("field.set.a_rebake_replaces_the_samples", "[field]") {
    // A re-bake is the ordinary case rather than the exception: a parameter changed, the field domain recomputed,
    // and the run that follows must read the new samples. A store that refused the second publish would make
    // "turn the tilt knob" a no-op after the first bake, which is the kind of failure a user reports as "the
    // simulation is stuck" with nothing in any log.
    FieldSet fields;
    const auto desc = volume_desc();
    const FieldKey key{4, 1};

    REQUIRE(fields.publish(key, desc, std::vector<double>(36, 1.0)));
    REQUIRE(get_component(fields.view(key), 0, 0) == 1.0);
    REQUIRE(fields.size() == 1);

    REQUIRE(fields.publish(key, desc, std::vector<double>(36, 2.0)));
    REQUIRE(get_component(fields.view(key), 0, 0) == 2.0);
    REQUIRE(get_component(fields.view(key), 11, 2) == 2.0);
    // Replaced, not appended: a store that kept both would grow without bound across a knob's worth of edits.
    REQUIRE(fields.size() == 1);

    // A re-bake may also change the **shape**, which is why the entry's descriptor is replaced and not only its
    // samples: a user who made the grid finer must get the grid they asked for, and a stale descriptor would
    // describe the new samples with the old counts.
    const auto finer = volume_desc(5, 2, 2);
    REQUIRE(fields.publish(key, finer, std::vector<double>(60, 3.0)));
    REQUIRE(fields.view(key).desc.count[0] == 5);
    REQUIRE(fields.view(key).point_count() == 20);
    REQUIRE(get_component(fields.view(key), 19, 2) == 3.0);
    REQUIRE(fields.bytes() == 60 * sizeof(double));
}

TEST_CASE("field.set.keys_are_ordered_and_distinct", "[field]") {
    // The order is not cosmetic: a report that enumerated a run's baked fields, or a cache that walked them,
    // would be a different report on every run if the container's iteration order were unspecified. The entries
    // are kept sorted by key for that reason, and this case is what pins it.
    FieldSet fields;
    const auto desc = volume_desc();
    const std::vector<double> samples(36, 1.0);

    REQUIRE(fields.publish(FieldKey{3, 2}, desc, samples));
    REQUIRE(fields.publish(FieldKey{1, 1}, desc, samples));
    REQUIRE(fields.publish(FieldKey{3, 1}, desc, samples));
    REQUIRE(fields.publish(FieldKey{2, 7}, desc, samples));

    const std::vector<FieldKey> keys = fields.keys();
    REQUIRE(keys.size() == 4);
    REQUIRE(keys[0] == (FieldKey{1, 1}));
    REQUIRE(keys[1] == (FieldKey{2, 7}));
    REQUIRE(keys[2] == (FieldKey{3, 1}));
    REQUIRE(keys[3] == (FieldKey{3, 2}));
    for (std::size_t i = 1; i < keys.size(); ++i) REQUIRE(keys[i - 1] < keys[i]);

    // The comparisons are a total order on distinct keys and an equivalence on equal ones, which is what the
    // binary search above assumes and what a hand-written `==` alone would not give.
    REQUIRE(FieldKey{1, 1} == FieldKey{1, 1});
    REQUIRE(FieldKey{1, 1} != FieldKey{1, 2});
    REQUIRE(FieldKey{1, 2} != FieldKey{2, 1});
    REQUIRE_FALSE(FieldKey{1, 1} < FieldKey{1, 1});
    REQUIRE(FieldKey{1, 1} < FieldKey{1, 2});
    REQUIRE(FieldKey{1, 9} < FieldKey{2, 1});   // node dominates port, so the order is a sweep over publishers

    // Erase removes one entry and leaves the rest readable, which is the half a `clear`-only store would fail:
    // a user who deletes one field node must not have the whole run's bake disappear.
    REQUIRE(fields.erase(FieldKey{3, 1}));
    REQUIRE_FALSE(fields.contains(FieldKey{3, 1}));
    REQUIRE(fields.size() == 3);
    REQUIRE(is_readable(fields.view(FieldKey{3, 2})));
    REQUIRE(is_readable(fields.view(FieldKey{1, 1})));
    REQUIRE_FALSE(fields.erase(FieldKey{3, 1}));   // erasing twice is not an error, it is a false
    REQUIRE_FALSE(fields.erase(FieldKey{99, 1}));

    fields.clear();
    REQUIRE(fields.size() == 0);
    REQUIRE(fields.keys().empty());
    REQUIRE(fields.bytes() == 0);
    REQUIRE_FALSE(is_readable(fields.view(FieldKey{1, 1})));
}
