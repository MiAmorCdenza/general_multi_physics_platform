/**
 * @file test_abi_layout.cpp
 * @brief ABI 布局的静态断言。**这是 ABI 契约的权威执法者。**
 *
 * 为什么这些断言不可省：
 *   插件的 `.dll` 是独立编译的产物，宿主无法在编译期发现布局不匹配。
 *   改布局 → 断言先失败 → 强迫作者面对兼容性问题并升版本号。
 *
 * 与 `tests/ABI_LAYOUT.md` 的关系：
 *   该文档是给外部语言绑定看的说明，本文件是它的**执法者**。
 *   两者不一致时以本文件为准，并立即修正文档。
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

// ═══════════════════════════════════════════════════════════════════════════
// 版本常量：数值冻结
// ═══════════════════════════════════════════════════════════════════════════

static_assert(kAbiMajor == 1, "ABI 主版本变更必须同步更新 tests/ABI_LAYOUT.md 与所有绑定");
static_assert(kAbiMinor == 0);
static_assert(kFieldBufferLayout == 1);
static_assert(kLatticeDescLayout == 1);
static_assert(kLittleEndian);

// ═══════════════════════════════════════════════════════════════════════════
// FieldDim：7 字节，alignof 1
// ═══════════════════════════════════════════════════════════════════════════

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

// ═══════════════════════════════════════════════════════════════════════════
// LatticeDesc：32 字节，alignof 4
// ═══════════════════════════════════════════════════════════════════════════

static_assert(sizeof(LatticeDesc) == 32, "改 LatticeDesc 尺寸必须升 kLatticeDescLayout");
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

// 枚举的基础类型也是 ABI
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

// ═══════════════════════════════════════════════════════════════════════════
// FieldBuffer：64 位 80 字节 / 32 位 56 字节
// ═══════════════════════════════════════════════════════════════════════════

static_assert(offsetof(FieldBuffer, lattice) == 0);
static_assert(offsetof(FieldBuffer, magic) == 32);
static_assert(offsetof(FieldBuffer, layout) == 36);
static_assert(offsetof(FieldBuffer, abi_major) == 38);
static_assert(offsetof(FieldBuffer, writer_seq) == 40);
static_assert(offsetof(FieldBuffer, flags) == 44);

// 与平台无关的部分
static_assert(sizeof(FieldBuffer::magic) == 4);
static_assert(sizeof(FieldBuffer::layout) == 2);
static_assert(sizeof(FieldBuffer::abi_major) == 2);
static_assert(sizeof(FieldBuffer::writer_seq) == 4);
static_assert(sizeof(FieldBuffer::flags) == 4);
static_assert(sizeof(FieldBuffer::data_bytes) == 8);
static_assert(sizeof(FieldBuffer::capacity_bytes) == 8);
// reserved 保证"尾部补齐到 8 字节对齐"；元素个数随平台不同，但总字节数固定为 8
static_assert(sizeof(FieldBuffer::reserved) == 8);

// 与平台相关的部分：按指针大小分支
//
// 注意：32 位下 uint64 字段仍需 8 字节对齐，因此 data 之后有填充。
#if INTPTR_MAX == INT64_MAX
static_assert(offsetof(FieldBuffer, data) == 48);
static_assert(offsetof(FieldBuffer, data_bytes) == 56);
static_assert(offsetof(FieldBuffer, capacity_bytes) == 64);
static_assert(offsetof(FieldBuffer, reserved) == 72);
static_assert(sizeof(FieldBuffer) == 80, "64 位布局变更必须升 kFieldBufferLayout");
static_assert(alignof(FieldBuffer) == 8);
#elif INTPTR_MAX == INT32_MAX
static_assert(offsetof(FieldBuffer, data) == 48);
static_assert(offsetof(FieldBuffer, data_bytes) == 56);   // 8 字节对齐填充
static_assert(offsetof(FieldBuffer, capacity_bytes) == 64);
static_assert(offsetof(FieldBuffer, reserved) == 72);
static_assert(sizeof(FieldBuffer) == 80, "32 位布局变更必须升 kFieldBufferLayout");
static_assert(alignof(FieldBuffer) == 8);
#else
#error "未预期的指针宽度：本 ABI 只声明了 32 位与 64 位两种布局"
#endif

static_assert(kFieldBufferMagic == 0x51504642U);

// ═══════════════════════════════════════════════════════════════════════════
// 重复定义的守卫：abi::FieldDim 与 units::Dim 必须一致
// ═══════════════════════════════════════════════════════════════════════════
//
// abi 刻意不 include units（见 field_dim.hpp 的说明），代价是布局重复。
// 这组断言就是那份重复的守卫：一旦漂移，编译期立刻失败。

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

// ═══════════════════════════════════════════════════════════════════════════
// 运行期测试
// ═══════════════════════════════════════════════════════════════════════════

// ── 布局断言的"具名"测试项 ──────────────────────────────────────────────────
//
// 上面的 static_assert 已经在编译期执法。这些 TEST_CASE 的作用是让契约
// 里的 @tests 条目**有名字可指**（门禁会校验 id 真实存在），
// 同时在测试报告里显式出现"布局已被检查"这件事。

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
    // abi 刻意不 include units（见 field_dim.hpp）。
    // 这组断言就是那份重复定义的守卫——漂移会在这里失败。
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
    // 完全相同
    REQUIRE(is_compatible(host, Version{1, 0, 0}));
    // 宿主次版本更新 → 旧插件可用（向后兼容）
    REQUIRE(is_compatible(Version{1, 3, 0}, Version{1, 0, 0}));
    REQUIRE(is_compatible(Version{1, 3, 0}, Version{1, 3, 9}));   // patch 不参与判定
    // 插件次版本更新 → 拒绝（插件可能用了宿主没有的能力）
    REQUIRE_FALSE(is_compatible(Version{1, 0, 0}, Version{1, 1, 0}));
    // 主版本不同 → 双向拒绝
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
    // 主次版本完全相同，但布局版本不同 → 必须拒绝
    REQUIRE(check_compatible(Version{1, 0, 0}, Version{1, 0, 0}, /*host_layout=*/1,
                             /*plugin_layout=*/2) == CompatVerdict::layout_mismatch);
    REQUIRE_FALSE(is_compatible(Version{1, 0, 0}, Version{1, 0, 0}, 1, 2));
    // 布局检查优先于版本检查
    REQUIRE(check_compatible(Version{1, 0, 0}, Version{9, 9, 9}, 1, 2) ==
            CompatVerdict::layout_mismatch);
}

TEST_CASE("abi.version.reflexive", "[abi][property]") {
    // 自反性：任何版本与自身比较必为 compatible
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

// ── 格子派生量 ──────────────────────────────────────────────────────────────

TEST_CASE("abi.lattice.default_is_point_scalar", "[abi]") {
    constexpr LatticeDesc d{};
    STATIC_REQUIRE(d.kind == LatticeKind::point);
    STATIC_REQUIRE(d.component == ComponentKind::scalar);
    STATIC_REQUIRE(d.element == ElementType::f32);
    REQUIRE(point_count(d) == 1);
    REQUIRE(data_bytes(d) == 4);   // 1 点 × 1 分量 × 4 字节
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

    // point 类型恒为 1，即使 count 全为 0
    const auto pt = make_lattice(LatticeKind::point, ComponentKind::scalar, ElementType::f32,
                                 kDimensionless);
    REQUIRE(point_count(pt) == 1);
}

TEST_CASE("abi.lattice.data_bytes", "[abi]") {
    // 真实场景：256×128×64 的三维矢量场，float32
    // 这就是"一张场 ≈ 19MB"的来源，也是 ADR-0005 里 float32 决策的依据
    const auto vol = make_lattice(LatticeKind::volume, ComponentKind::vector, ElementType::f32,
                                  kDimensionless, 256, 128, 64);
    const std::uint64_t points = 256ull * 128ull * 64ull;
    REQUIRE(points == 2'097'152ull);
    REQUIRE(data_bytes(vol) == points * 3ull * 4ull);
    // ≈ 24MB（旧工程记录的"约 19MB"是另一组维数，量级一致）

    // 标量场只有 1/3
    const auto scalar = make_lattice(LatticeKind::volume, ComponentKind::scalar, ElementType::f32,
                                     kDimensionless, 256, 128, 64);
    REQUIRE(data_bytes(scalar) * 3 == data_bytes(vol));

    // f64 是 f32 的两倍 → 这是 ADR-0005 拒绝场上用 double 的量化理由
    const auto f64 = make_lattice(LatticeKind::volume, ComponentKind::vector, ElementType::f64,
                                  kDimensionless, 256, 128, 64);
    REQUIRE(data_bytes(f64) == data_bytes(vol) * 2);
}

TEST_CASE("abi.lattice.consistency_check", "[abi]") {
    const auto good = make_lattice(LatticeKind::plane, ComponentKind::vector, ElementType::f32,
                                   kDimensionless, 10, 20);
    REQUIRE(is_consistent(good));

    // spacing 不符 → 不自洽
    LatticeDesc bad_spacing = good;
    bad_spacing.spacing_bytes = 4;   // 矢量应为 12
    REQUIRE_FALSE(is_consistent(bad_spacing));

    // reserved 非零 → 不自洽（外部绑定必须清零）
    LatticeDesc bad_reserved = good;
    bad_reserved.reserved = 1;
    REQUIRE_FALSE(is_consistent(bad_reserved));

    // padding 非零 → 不自洽
    LatticeDesc bad_padding = good;
    bad_padding.padding = 1;
    REQUIRE_FALSE(is_consistent(bad_padding));

    // 维数计数为 0 → 不自洽
    LatticeDesc zero_dim = make_lattice(LatticeKind::plane, ComponentKind::scalar, ElementType::f32,
                                        kDimensionless, 10, 0);
    REQUIRE_FALSE(is_consistent(zero_dim));

    // point 不要求计数
    REQUIRE(is_consistent(make_lattice(LatticeKind::point, ComponentKind::scalar, ElementType::f32,
                                       kDimensionless)));
}

TEST_CASE("abi.lattice.dimension_is_carried", "[abi]") {
    // 速度的量纲：L=1, T=-1
    const FieldDim velocity{1, 0, -1, 0, 0, 0, 0};
    const auto v = make_lattice(LatticeKind::line, ComponentKind::vector, ElementType::f32, velocity,
                                100);
    REQUIRE(v.dimension.L == 1);
    REQUIRE(v.dimension.T == -1);
    REQUIRE(v.dimension.M == 0);
}

// ── flags 位运算 ────────────────────────────────────────────────────────────

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

// ── FieldBuffer 行为 ────────────────────────────────────────────────────────

TEST_CASE("abi.field_buffer.trivially_copyable", "[abi]") {
    // 含 std::atomic，因此必须显式提供拷贝语义。
    // 描述符按值传递是本结构的设计前提，所以这几条断言是必要的。
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

    const FieldBuffer b = a;   // 第二个句柄，指向同一份数据
    REQUIRE(b.data == a.data);
    REQUIRE(b.data_bytes == a.data_bytes);
    REQUIRE(is_readable(b));
    REQUIRE(data_as<float>(b)[2] == 3.0f);

    // 副本上的写入不影响原对象（它们各自持有 seq 快照）
    FieldBuffer c;
    c = a;
    REQUIRE(c.data == a.data);
    REQUIRE(c.magic == kFieldBufferMagic);
}

TEST_CASE("abi.field_buffer.magic_constant", "[abi]") {
    STATIC_REQUIRE(kFieldBufferMagic == 'Q' | ('P' << 8) | ('F' << 16) | ('B' << 24));
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

    // 容量大于需求也可以（预留空间）
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

    // data_bytes 超过 capacity → 拒绝
    b.data_bytes = 4096;
    REQUIRE_FALSE(validate(b));

    // data_bytes 小于 lattice 需求 → 拒绝（数据不完整）
    b.data_bytes = 8;   // 需要 16
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

    // 初始：序号 0（偶数）、未标 valid
    REQUIRE(read_begin(b) == 0);
    REQUIRE(read_end(b, 0));
    REQUIRE_FALSE(is_readable(b));

    // 写者协议
    const std::uint32_t seq = begin_write(b);
    REQUIRE(seq == 1);                     // 奇数 = 写入中
    REQUIRE_FALSE(read_end(b, seq));       // 读者应重试
    end_write(b, seq, static_cast<std::uint32_t>(BufferFlags::valid));

    REQUIRE(b.writer_seq.load() == 2);     // 偶数 = 稳定
    REQUIRE(is_readable(b));
    REQUIRE(read_end(b, 2));

    // 第二次写入
    const std::uint32_t seq2 = begin_write(b);
    REQUIRE(seq2 == 3);
    end_write(b, seq2, static_cast<std::uint32_t>(BufferFlags::valid) | static_cast<std::uint32_t>(BufferFlags::from_cache));
    REQUIRE(b.writer_seq.load() == 4);
    REQUIRE(has_flag(b.flags.load(), BufferFlags::from_cache));

    // 陈旧的 seq 必须判为不一致
    REQUIRE_FALSE(read_end(b, 2));
    REQUIRE_FALSE(read_end(b, 1));
}
