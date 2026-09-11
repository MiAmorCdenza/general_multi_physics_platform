/**
 * @file test_abi_layout.cpp
 * @brief Static assertions for the ABI layout. **This is the authoritative enforcer of the ABI contract.**
 *
 * Why these assertions cannot be omitted:
 *   a plugin `.dll` is built separately, so the host cannot spot a layout mismatch at compile time.
 *   Change the layout -> the assertion fails first -> the author must face compatibility and bump the version.
 *
 * Relation to `tests/ABI_LAYOUT.md`:
 *   that document is a description for external language bindings; this file is its **enforcer**.
 *   When the two disagree, this file wins and the document is corrected immediately.
 */
#include <catch2/catch_test_macros.hpp>

#include <qp/abi.hpp>
#include <qp/units.hpp>

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string>
#include <type_traits>

using namespace qp::abi;

// ===========================================================================
// Version constants: the values are frozen
// ===========================================================================

static_assert(kAbiMajor == 1, "changing the ABI major version requires updating tests/ABI_LAYOUT.md and all bindings");
static_assert(kAbiMinor == 0);
static_assert(kFieldBufferLayout == 1);
static_assert(kLatticeDescLayout == 1);
static_assert(kLittleEndian);

// ===========================================================================
// FieldDim: 7 bytes, alignof 1
// ===========================================================================

static_assert(sizeof(FieldDim) == 7);
static_assert(alignof(FieldDim) == 1);
static_assert(std::is_trivially_copyable_v<FieldDim>);
static_assert(std::is_standard_layout_v<FieldDim>);
static_assert(offsetof(FieldDim, L) == 0);
static_assert(offsetof(FieldDim, M) == 1);
static_assert(offsetof(FieldDim, T) == 2);
static_assert(offsetof(FieldDim, I) == 3);
static_assert(offsetof(FieldDim, Th) == 4);
static_assert(offsetof(FieldDim, N) == 5);
static_assert(offsetof(FieldDim, J) == 6);

// ===========================================================================
// LatticeDesc: 32 bytes, alignof 4
// ===========================================================================

static_assert(sizeof(LatticeDesc) == 32, "changing the LatticeDesc size requires bumping kLatticeDescLayout");
static_assert(alignof(LatticeDesc) == 4);
static_assert(std::is_trivially_copyable_v<LatticeDesc>);
static_assert(std::is_standard_layout_v<LatticeDesc>);
static_assert(offsetof(LatticeDesc, dimension) == 0);
static_assert(offsetof(LatticeDesc, component) == 7);
static_assert(offsetof(LatticeDesc, element) == 8);
static_assert(offsetof(LatticeDesc, kind) == 9);
static_assert(offsetof(LatticeDesc, padding) == 10);
static_assert(offsetof(LatticeDesc, count) == 12);
static_assert(offsetof(LatticeDesc, spacing_bytes) == 24);
static_assert(offsetof(LatticeDesc, reserved) == 28);

// The underlying types of the enums are ABI too
static_assert(sizeof(LatticeKind) == 1);
static_assert(sizeof(ComponentKind) == 1);
static_assert(sizeof(ElementType) == 1);
static_assert(static_cast<int>(LatticeKind::point) == 0);
static_assert(static_cast<int>(LatticeKind::line) == 1);
static_assert(static_cast<int>(LatticeKind::plane) == 2);
static_assert(static_cast<int>(LatticeKind::volume) == 3);
static_assert(static_cast<int>(ComponentKind::scalar) == 0);
static_assert(static_cast<int>(ComponentKind::vector) == 1);
static_assert(static_cast<int>(ElementType::f32) == 0);
static_assert(static_cast<int>(ElementType::f64) == 1);

// ===========================================================================
// FieldBuffer: 80 bytes on 64-bit / 56 bytes on 32-bit
// ===========================================================================

static_assert(offsetof(FieldBuffer, lattice) == 0);
static_assert(offsetof(FieldBuffer, magic) == 32);
static_assert(offsetof(FieldBuffer, layout) == 36);
static_assert(offsetof(FieldBuffer, abi_major) == 38);
static_assert(offsetof(FieldBuffer, writer_seq) == 40);
static_assert(offsetof(FieldBuffer, flags) == 44);

// Platform-independent part
static_assert(sizeof(FieldBuffer::magic) == 4);
static_assert(sizeof(FieldBuffer::layout) == 2);
static_assert(sizeof(FieldBuffer::abi_major) == 2);
static_assert(sizeof(FieldBuffer::writer_seq) == 4);
static_assert(sizeof(FieldBuffer::flags) == 4);
static_assert(sizeof(FieldBuffer::data_bytes) == 8);
static_assert(sizeof(FieldBuffer::capacity_bytes) == 8);
// reserved pads the tail to 8-byte alignment; the element count varies by platform, the size is always 8
static_assert(sizeof(FieldBuffer::reserved) == 8);

// Platform-dependent part: branch on pointer size
//
// Note: on 32-bit a uint64 field still needs 8-byte alignment, so there is padding after data.
#if INTPTR_MAX == INT64_MAX
static_assert(offsetof(FieldBuffer, data) == 48);
static_assert(offsetof(FieldBuffer, data_bytes) == 56);
static_assert(offsetof(FieldBuffer, capacity_bytes) == 64);
static_assert(offsetof(FieldBuffer, reserved) == 72);
static_assert(sizeof(FieldBuffer) == 80, "a 64-bit layout change requires bumping kFieldBufferLayout");
static_assert(alignof(FieldBuffer) == 8);
#elif INTPTR_MAX == INT32_MAX
static_assert(offsetof(FieldBuffer, data) == 48);
static_assert(offsetof(FieldBuffer, data_bytes) == 56);   // 8-byte alignment padding
static_assert(offsetof(FieldBuffer, capacity_bytes) == 64);
static_assert(offsetof(FieldBuffer, reserved) == 72);
static_assert(sizeof(FieldBuffer) == 80, "a 32-bit layout change requires bumping kFieldBufferLayout");
static_assert(alignof(FieldBuffer) == 8);
#else
#error "unexpected pointer width: this ABI declares only 32-bit and 64-bit layouts"
#endif

static_assert(kFieldBufferMagic == 0x51504642U);

// ===========================================================================
// Guard against duplicate definitions: abi::FieldDim and units::Dim must agree
// ===========================================================================
//
// abi deliberately does not include units (see field_dim.hpp), at the cost of a duplicated layout.
// This group of assertions guards that duplication: any drift fails at compile time.

static_assert(sizeof(FieldDim) == sizeof(qp::units::Dim));
static_assert(alignof(FieldDim) == alignof(qp::units::Dim));
static_assert(std::is_same_v<DimExp, qp::units::DimExp>);
static_assert(kDimensionless.L == qp::units::Dim::none().L);
static_assert(kDimensionless.M == qp::units::Dim::none().M);
static_assert(kDimensionless.T == qp::units::Dim::none().T);
static_assert(kDimensionless.I == qp::units::Dim::none().I);
static_assert(kDimensionless.Th == qp::units::Dim::none().Th);
static_assert(kDimensionless.N == qp::units::Dim::none().N);
static_assert(kDimensionless.J == qp::units::Dim::none().J);

// ===========================================================================
// Runtime tests
// ===========================================================================

// -- The "named" test items for the layout assertions ------------------------
//
// The static_asserts above already enforce this at compile time. These TEST_CASEs exist so that the
// @tests entries in the contracts **have something to name** (the gate verifies the ids really exist),
// and so that "the layout was checked" appears explicitly in the test report.

TEST_CASE("abi.field_buffer.size_and_alignment", "[abi][layout]") {
  #if INTPTR_MAX == INT64_MAX
    REQUIRE(sizeof(FieldBuffer) == 80);
  #elif INTPTR_MAX == INT32_MAX
    REQUIRE(sizeof(FieldBuffer) == 80);
  #endif
    REQUIRE(alignof(FieldBuffer) == 8);
    REQUIRE(kFieldBufferAlign == 8);
    REQUIRE(kFieldBufferLayout == 1);
}

TEST_CASE("abi.field_buffer.field_offsets", "[abi][layout]") {
    REQUIRE(offsetof(FieldBuffer, lattice) == 0);
    REQUIRE(offsetof(FieldBuffer, magic) == 32);
    REQUIRE(offsetof(FieldBuffer, layout) == 36);
    REQUIRE(offsetof(FieldBuffer, abi_major) == 38);
    REQUIRE(offsetof(FieldBuffer, writer_seq) == 40);
    REQUIRE(offsetof(FieldBuffer, flags) == 44);
    REQUIRE(offsetof(FieldBuffer, data) == 48);
    REQUIRE(offsetof(FieldBuffer, data_bytes) == 56);
    REQUIRE(offsetof(FieldBuffer, capacity_bytes) == 64);
    REQUIRE(offsetof(FieldBuffer, reserved) == 72);
}

TEST_CASE("abi.lattice.size_and_alignment", "[abi][layout]") {
    REQUIRE(sizeof(LatticeDesc) == 32);
    REQUIRE(alignof(LatticeDesc) == 4);
    REQUIRE(kLatticeDescLayout == 1);
}

TEST_CASE("abi.lattice.field_offsets", "[abi][layout]") {
    REQUIRE(offsetof(LatticeDesc, dimension) == 0);
    REQUIRE(offsetof(LatticeDesc, component) == 7);
    REQUIRE(offsetof(LatticeDesc, element) == 8);
    REQUIRE(offsetof(LatticeDesc, kind) == 9);
    REQUIRE(offsetof(LatticeDesc, padding) == 10);
    REQUIRE(offsetof(LatticeDesc, count) == 12);
    REQUIRE(offsetof(LatticeDesc, spacing_bytes) == 24);
    REQUIRE(offsetof(LatticeDesc, reserved) == 28);
}

TEST_CASE("abi.lattice.trivially_copyable", "[abi][layout]") {
    REQUIRE(std::is_trivially_copyable_v<LatticeDesc>);
    REQUIRE(std::is_standard_layout_v<LatticeDesc>);
    REQUIRE(sizeof(FieldDim) == 7);
    REQUIRE(alignof(FieldDim) == 1);
    REQUIRE(std::is_trivially_copyable_v<FieldDim>);
}

TEST_CASE("abi.dim_matches_units_dim", "[abi][layout]") {
    // abi deliberately does not include units (see field_dim.hpp).
    // This group of assertions guards that duplication -- drift fails here.
    REQUIRE(sizeof(FieldDim) == sizeof(qp::units::Dim));
    REQUIRE(alignof(FieldDim) == alignof(qp::units::Dim));
    const qp::units::Dim u{1, 0, -1, 0, 0, 0, 0};
    const FieldDim a{1, 0, -1, 0, 0, 0, 0};
    REQUIRE(a.L == u.L);
    REQUIRE(a.M == u.M);
    REQUIRE(a.T == u.T);
    REQUIRE(a.I == u.I);
    REQUIRE(a.Th == u.Th);
    REQUIRE(a.N == u.N);
    REQUIRE(a.J == u.J);
}

TEST_CASE("abi.version.values_are_frozen", "[abi]") {
    REQUIRE(kAbiMajor == 1);
    REQUIRE(kAbiMajor == 1);
    REQUIRE(kHostVersion.major == kAbiMajor);
    REQUIRE(kHostVersion.minor == kAbiMinor);
    STATIC_REQUIRE(sizeof(Version) == 6);
}

TEST_CASE("abi.version.compatibility_matrix", "[abi]") {
    constexpr Version host{1, 0, 0};
    // Exactly the same
    REQUIRE(is_compatible(host, Version{1, 0, 0}));
    // Host minor version newer -> old plugins work (backward compatible)
    REQUIRE(is_compatible(Version{1, 3, 0}, Version{1, 0, 0}));
    REQUIRE(is_compatible(Version{1, 3, 0}, Version{1, 3, 9}));   // patch does not participate
    // Plugin minor version newer -> reject (the plugin may use capabilities the host lacks)
    REQUIRE_FALSE(is_compatible(Version{1, 0, 0}, Version{1, 1, 0}));
    // Different major version -> reject in both directions
    REQUIRE_FALSE(is_compatible(Version{1, 0, 0}, Version{2, 0, 0}));
    REQUIRE_FALSE(is_compatible(Version{2, 0, 0}, Version{1, 0, 0}));
}

TEST_CASE("abi.version.rejects_newer_major", "[abi]") {
    REQUIRE(check_compatible(Version{1, 0, 0}, Version{2, 0, 0}) == CompatVerdict::host_too_old);
}

TEST_CASE("abi.version.rejects_older_major", "[abi]") {
    REQUIRE(check_compatible(Version{2, 0, 0}, Version{1, 0, 0}) == CompatVerdict::plugin_too_old);
}

TEST_CASE("abi.version.accepts_newer_minor", "[abi]") {
    REQUIRE(check_compatible(Version{1, 5, 0}, Version{1, 2, 0}) == CompatVerdict::compatible);
    REQUIRE(check_compatible(Version{1, 5, 0}, Version{1, 5, 0}) == CompatVerdict::compatible);
    REQUIRE(check_compatible(Version{1, 0, 0}, Version{1, 1, 0}) == CompatVerdict::plugin_too_new);
}

TEST_CASE("abi.version.rejects_layout_mismatch", "[abi]") {
    // Same major and minor, but a different layout version -> must reject
    REQUIRE(check_compatible(Version{1, 0, 0}, Version{1, 0, 0}, /*host_layout=*/1,
                             /*plugin_layout=*/2) == CompatVerdict::layout_mismatch);
    REQUIRE_FALSE(is_compatible(Version{1, 0, 0}, Version{1, 0, 0}, 1, 2));
    // The layout check takes priority over the version check
    REQUIRE(check_compatible(Version{1, 0, 0}, Version{9, 9, 9}, 1, 2) ==
            CompatVerdict::layout_mismatch);
}

TEST_CASE("abi.version.reflexive", "[abi][property]") {
    // Reflexivity: any version compared with itself is compatible
    constexpr Version samples[] = {{0, 0, 0}, {1, 0, 0}, {1, 2, 3}, {2, 0, 0}, {65535, 65535, 65535}};
    for (Version v : samples) {
        REQUIRE(is_compatible(v, v));
        REQUIRE(check_compatible(v, v) == CompatVerdict::compatible);
    }
}

TEST_CASE("abi.version.verdict_names", "[abi]") {
    REQUIRE(std::string(to_string(CompatVerdict::compatible)) == "compatible");
    REQUIRE(std::string(to_string(CompatVerdict::host_too_old)) == "host_too_old");
    REQUIRE(std::string(to_string(CompatVerdict::plugin_too_old)) == "plugin_too_old");
    REQUIRE(std::string(to_string(CompatVerdict::plugin_too_new)) == "plugin_too_new");
    REQUIRE(std::string(to_string(CompatVerdict::layout_mismatch)) == "layout_mismatch");
    STATIC_REQUIRE(sizeof(CompatVerdict) == 1);
}

// -- Lattice-derived quantities ----------------------------------------------

TEST_CASE("abi.lattice.default_is_point_scalar", "[abi]") {
    constexpr LatticeDesc d{};
    STATIC_REQUIRE(d.kind == LatticeKind::point);
    STATIC_REQUIRE(d.component == ComponentKind::scalar);
    STATIC_REQUIRE(d.element == ElementType::f32);
    REQUIRE(point_count(d) == 1);
    REQUIRE(data_bytes(d) == 4);   // 1 point x 1 component x 4 bytes
}

TEST_CASE("abi.lattice.point_count", "[abi]") {
    const auto line = make_lattice(LatticeKind::line, ComponentKind::scalar, ElementType::f32,
                                   kDimensionless, 100);
    REQUIRE(point_count(line) == 100);

    const auto plane = make_lattice(LatticeKind::plane, ComponentKind::scalar, ElementType::f32,
                                    kDimensionless, 64, 32);
    REQUIRE(point_count(plane) == 64u * 32u);

    const auto vol = make_lattice(LatticeKind::volume, ComponentKind::vector, ElementType::f32,
                                  kDimensionless, 16, 8, 4);
    REQUIRE(point_count(vol) == 16u * 8u * 4u);

    // A point lattice is always 1, even when every count is 0
    const auto pt = make_lattice(LatticeKind::point, ComponentKind::scalar, ElementType::f32,
                                 kDimensionless);
    REQUIRE(point_count(pt) == 1);
}

TEST_CASE("abi.lattice.data_bytes", "[abi]") {
    // Real scenario: a 256x128x64 3D vector field, float32
    // This is where "one field is about 19MB" comes from, and the basis of the float32 decision in ADR-0005
    const auto vol = make_lattice(LatticeKind::volume, ComponentKind::vector, ElementType::f32,
                                  kDimensionless, 256, 128, 64);
    const std::uint64_t points = 256ull * 128ull * 64ull;
    REQUIRE(points == 2'097'152ull);
    REQUIRE(data_bytes(vol) == points * 3ull * 4ull);
    // About 24MB (the "about 19MB" in the old project notes is another set of dimensions, same order)

    // A scalar field is only 1/3
    const auto scalar = make_lattice(LatticeKind::volume, ComponentKind::scalar, ElementType::f32,
                                     kDimensionless, 256, 128, 64);
    REQUIRE(data_bytes(scalar) * 3 == data_bytes(vol));

    // f64 is twice f32 -> the quantitative reason ADR-0005 rejects double for fields
    const auto f64 = make_lattice(LatticeKind::volume, ComponentKind::vector, ElementType::f64,
                                  kDimensionless, 256, 128, 64);
    REQUIRE(data_bytes(f64) == data_bytes(vol) * 2);
}

TEST_CASE("abi.lattice.consistency_check", "[abi]") {
    const auto good = make_lattice(LatticeKind::plane, ComponentKind::vector, ElementType::f32,
                                   kDimensionless, 10, 20);
    REQUIRE(is_consistent(good));

    // Spacing does not match -> inconsistent
    LatticeDesc bad_spacing = good;
    bad_spacing.spacing_bytes = 4;   // a vector should be 12
    REQUIRE_FALSE(is_consistent(bad_spacing));

    // Non-zero reserved -> inconsistent (external bindings must zero it)
    LatticeDesc bad_reserved = good;
    bad_reserved.reserved = 1;
    REQUIRE_FALSE(is_consistent(bad_reserved));

    // Non-zero padding -> inconsistent
    LatticeDesc bad_padding = good;
    bad_padding.padding = 1;
    REQUIRE_FALSE(is_consistent(bad_padding));

    // A dimension count of 0 -> inconsistent
    LatticeDesc zero_dim = make_lattice(LatticeKind::plane, ComponentKind::scalar, ElementType::f32,
                                        kDimensionless, 10, 0);
    REQUIRE_FALSE(is_consistent(zero_dim));

    // point requires no counts
    REQUIRE(is_consistent(make_lattice(LatticeKind::point, ComponentKind::scalar, ElementType::f32,
                                       kDimensionless)));
}

TEST_CASE("abi.lattice.dimension_is_carried", "[abi]") {
    // The dimension of velocity: L=1, T=-1
    const FieldDim velocity{1, 0, -1, 0, 0, 0, 0};
    const auto v = make_lattice(LatticeKind::line, ComponentKind::vector, ElementType::f32, velocity,
                                100);
    REQUIRE(v.dimension.L == 1);
    REQUIRE(v.dimension.T == -1);
    REQUIRE(v.dimension.M == 0);
}

// -- flags bit operations ----------------------------------------------------

TEST_CASE("abi.field_buffer.flags_are_bitwise", "[abi]") {
    STATIC_REQUIRE(static_cast<std::uint32_t>(BufferFlags::valid) == 1U);
    STATIC_REQUIRE(static_cast<std::uint32_t>(BufferFlags::tombstone) == 2U);
    STATIC_REQUIRE(static_cast<std::uint32_t>(BufferFlags::from_cache) == 4U);

    const auto both = BufferFlags::valid | BufferFlags::from_cache;
    REQUIRE(has_flag(both, BufferFlags::valid));
    REQUIRE(has_flag(both, BufferFlags::from_cache));
    REQUIRE_FALSE(has_flag(both, BufferFlags::tombstone));
    REQUIRE_FALSE(has_flag(static_cast<std::uint32_t>(BufferFlags::none), BufferFlags::valid));
}

// -- FieldBuffer behavior ----------------------------------------------------

TEST_CASE("abi.field_buffer.trivially_copyable", "[abi]") {
    // It contains a std::atomic, so copy semantics must be provided explicitly.
    // Passing the descriptor by value is a design premise of this struct, so these assertions are needed.
    STATIC_REQUIRE(std::is_copy_constructible_v<FieldBuffer>);
    STATIC_REQUIRE(std::is_copy_assignable_v<FieldBuffer>);
    STATIC_REQUIRE(std::is_move_constructible_v<FieldBuffer>);
    STATIC_REQUIRE(std::is_move_assignable_v<FieldBuffer>);
    STATIC_REQUIRE(std::is_nothrow_copy_constructible_v<FieldBuffer>);
}

TEST_CASE("abi.field_buffer.copy_is_a_second_handle", "[abi]") {
    float payload[3] = {1.0f, 2.0f, 3.0f};
    FieldBuffer a;
    a.lattice = make_lattice(LatticeKind::line, ComponentKind::scalar, ElementType::f32,
                             kDimensionless, 3);
    a.data = payload;
    a.data_bytes = sizeof(payload);
    a.capacity_bytes = sizeof(payload);
    a.flags.store(static_cast<std::uint32_t>(BufferFlags::valid));

    const FieldBuffer b = a;   // a second handle, pointing at the same data
    REQUIRE(b.data == a.data);
    REQUIRE(b.data_bytes == a.data_bytes);
    REQUIRE(is_readable(b));
    REQUIRE(data_as<float>(b)[2] == 3.0f);

    // A write through the copy does not affect the original (each holds its own seq snapshot)
    FieldBuffer c;
    c = a;
    REQUIRE(c.data == a.data);
    REQUIRE(c.magic == kFieldBufferMagic);
}

TEST_CASE("abi.field_buffer.magic_constant", "[abi]") {
    // The constant is 0x51504642: the characters 'Q' 'P' 'F' 'B' packed from
    // the most significant byte down.
    //
    // This assertion replaces one that read
    // `kFieldBufferMagic == 'Q' | ('P' << 8) | ...` with no parentheses. `==`
    // binds tighter than `|`, so that expression compared the constant against
    // 'B' alone (0x42 is the low byte of 0x51504642), OR-ed the three other
    // shifted characters into the 0 or 1 result, and was therefore true for
    // **every** value of the constant. It sat in this file looking like a
    // check. The parentheses below are the whole point of the line.
    STATIC_REQUIRE(kFieldBufferMagic == (('Q' << 24) | ('P' << 16) | ('F' << 8) | 'B'));

    FieldBuffer b;
    REQUIRE(b.magic == kFieldBufferMagic);
    REQUIRE(b.layout == kFieldBufferLayout);
    REQUIRE(b.abi_major == kAbiMajor);
    REQUIRE(b.writer_seq.load() == 0);
    REQUIRE(b.flags.load() == 0);
}

TEST_CASE("abi.field_buffer.validate_ok", "[abi]") {
    float payload[4] = {0, 0, 0, 0};
    FieldBuffer b;
    b.lattice = make_lattice(LatticeKind::line, ComponentKind::scalar, ElementType::f32,
                             kDimensionless, 4);
    b.data = payload;
    b.data_bytes = sizeof(payload);
    b.capacity_bytes = sizeof(payload);
    REQUIRE(validate(b));

    // Capacity larger than needed is fine (reserved space)
    b.capacity_bytes = 1024;
    REQUIRE(validate(b));
}

TEST_CASE("abi.field_buffer.validate_rejects_bad_magic", "[abi]") {
    float payload[1] = {0};
    FieldBuffer b;
    b.lattice = make_lattice(LatticeKind::point, ComponentKind::scalar, ElementType::f32,
                             kDimensionless);
    b.data = payload;
    b.data_bytes = sizeof(payload);
    b.capacity_bytes = sizeof(payload);
    REQUIRE(validate(b));

    b.magic = 0xDEADBEEF;
    REQUIRE_FALSE(validate(b));
}

TEST_CASE("abi.field_buffer.validate_rejects_layout_mismatch", "[abi]") {
    float payload[1] = {0};
    FieldBuffer b;
    b.lattice = make_lattice(LatticeKind::point, ComponentKind::scalar, ElementType::f32,
                             kDimensionless);
    b.data = payload;
    b.data_bytes = sizeof(payload);
    b.capacity_bytes = sizeof(payload);

    b.layout = 2;
    REQUIRE_FALSE(validate(b));
    b.layout = kFieldBufferLayout;

    b.abi_major = 2;
    REQUIRE_FALSE(validate(b));
    b.abi_major = kAbiMajor;

    b.data = nullptr;
    REQUIRE_FALSE(validate(b));
}

TEST_CASE("abi.field_buffer.validate_rejects_oversized_data", "[abi]") {
    float payload[4] = {0, 0, 0, 0};
    FieldBuffer b;
    b.lattice = make_lattice(LatticeKind::line, ComponentKind::scalar, ElementType::f32,
                             kDimensionless, 4);
    b.data = payload;
    b.data_bytes = sizeof(payload);
    b.capacity_bytes = sizeof(payload);
    REQUIRE(validate(b));

    // data_bytes exceeds capacity -> reject
    b.data_bytes = 4096;
    REQUIRE_FALSE(validate(b));

    // data_bytes below the lattice requirement -> reject (incomplete data)
    b.data_bytes = 8;   // 16 needed
    b.capacity_bytes = sizeof(payload);
    REQUIRE_FALSE(validate(b));
}

TEST_CASE("abi.field_buffer.seqlock_roundtrip", "[abi]") {
    float payload[2] = {1.0f, 2.0f};
    FieldBuffer b;
    b.lattice = make_lattice(LatticeKind::line, ComponentKind::scalar, ElementType::f32,
                             kDimensionless, 2);
    b.data = payload;
    b.data_bytes = sizeof(payload);
    b.capacity_bytes = sizeof(payload);

    // Initial: seq 0 (even), valid flag not set
    REQUIRE(read_begin(b) == 0);
    REQUIRE(read_end(b, 0));
    REQUIRE_FALSE(is_readable(b));

    // Writer protocol
    const std::uint32_t seq = begin_write(b);
    REQUIRE(seq == 1);                     // odd = write in progress
    REQUIRE_FALSE(read_end(b, seq));       // the reader should retry
    end_write(b, seq, static_cast<std::uint32_t>(BufferFlags::valid));

    REQUIRE(b.writer_seq.load() == 2);     // even = stable
    REQUIRE(is_readable(b));
    REQUIRE(read_end(b, 2));

    // Second write
    const std::uint32_t seq2 = begin_write(b);
    REQUIRE(seq2 == 3);
    end_write(b, seq2, static_cast<std::uint32_t>(BufferFlags::valid) | static_cast<std::uint32_t>(BufferFlags::from_cache));
    REQUIRE(b.writer_seq.load() == 4);
    REQUIRE(has_flag(b.flags.load(), BufferFlags::from_cache));

    // A stale seq must be judged inconsistent
    REQUIRE_FALSE(read_end(b, 2));
    REQUIRE_FALSE(read_end(b, 1));
}
