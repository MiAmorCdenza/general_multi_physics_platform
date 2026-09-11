/**
 * @file registry.cpp
 * @brief 端口类型注册表的实现。
 */
#include <qp/ports/registry.hpp>

#include <algorithm>

namespace qp::ports {
namespace {

/// @brief 构造一个标量端口描述。
[[nodiscard]] PortTypeDesc scalar(std::string_view name, PortTypeId id, NumericKind numeric,
                                  qp::units::Dim dim) noexcept {
    PortTypeDesc d{};
    d.id = id;
    d.name = name;
    d.numeric = numeric;
    d.constraint = DimensionConstraint::exact;
    d.dimension = dim;
    return d;
}

/// @brief 构造一个非数值端口描述（文本、句柄等）。
[[nodiscard]] PortTypeDesc plain(std::string_view name, PortTypeId id) noexcept {
    PortTypeDesc d{};
    d.id = id;
    d.name = name;
    d.numeric = NumericKind::none;
    d.constraint = DimensionConstraint::any;
    return d;
}

/// @brief 构造一个场端口描述。
[[nodiscard]] PortTypeDesc field(std::string_view name, PortTypeId id,
                                 std::uint8_t components) noexcept {
    PortTypeDesc d{};
    d.id = id;
    d.name = name;
    d.numeric = NumericKind::none;
    d.constraint = DimensionConstraint::any;
    d.field_element = 0;   // f32（见 ADR-0005：场数据一律 f32）
    d.field_components = components;
    return d;
}

}  // namespace

std::vector<PortTypeDesc> make_builtin_types() {
    std::vector<PortTypeDesc> out;
    out.reserve(kBuiltinTypeCount + 12);

    // ── 标量 ────────────────────────────────────────────────────────────────
    // 量纲为 none 的标量：具体量纲由节点的端口声明覆盖；
    // 这里的内置描述是"无特定量纲的数值"，供泛型节点使用。
    out.push_back(scalar("scalar_f64", kScalarF64, NumericKind::f64, qp::units::Dim{}));
    out.push_back(scalar("scalar_f32", kScalarF32, NumericKind::f32, qp::units::Dim{}));
    out.push_back(scalar("bool", kBool, NumericKind::boolean, qp::units::Dim{}));
    out.push_back(scalar("int64", kInt64, NumericKind::i64, qp::units::Dim{}));
    out.push_back(plain("string", kString));
    out.push_back(plain("enum", kEnum));
    out.push_back(plain("dimension", kDimension));

    // ── 场 ──────────────────────────────────────────────────────────────────
    out.push_back(field("scalar_field", kScalarField, 1));
    out.push_back(field("vector_field", kVectorField, 3));
    out.push_back(plain("field_table", kFieldTable));

    // ── 几何与粒子 ──────────────────────────────────────────────────────────
    out.push_back(plain("particle_buffer", kParticleBuffer));
    out.push_back(plain("geometry", kGeometry));

    // ── 测量与数据 ──────────────────────────────────────────────────────────
    out.push_back(plain("dataset", kDataset));
    out.push_back(plain("fit_result", kFitResult));

    // ── 逃生舱 ──────────────────────────────────────────────────────────────
    {
        PortTypeDesc d{};
        d.id = kAny;
        d.name = "any";
        d.numeric = NumericKind::none;
        d.constraint = DimensionConstraint::any;
        d.is_any = true;
        out.push_back(d);
    }

    return out;
}

PortTypeRegistry::PortTypeRegistry() {
    for (const auto& d : make_builtin_types()) {
        // 内置类型构造不通过 register_type：它们本就是权威定义，
        // 走注册路径会把"宿主保留区间"的检查套在自己头上。
        types_.push_back(d);
    }
}

Result<void> PortTypeRegistry::register_type(const PortTypeDesc& desc) {
    using qp::diag::ErrorCode;

    if (desc.id == kInvalidType) {
        return Result<void>{ErrorCode::invalid_argument};
    }
    if (desc.name.empty()) {
        return Result<void>{ErrorCode::missing_field};
    }

    // 宿主保留区间：插件的 ID 必须 >= kUserTypeBase。
    // 注意 kAny(1000) 也在保留区间内，且它是内置的——
    // 因此这里查的是"是否已被内置占用"，而不是"数值是否小于阈值"。
    const bool is_reserved_id = desc.id < kUserTypeBase;
    const PortTypeDesc* existing = find(desc.id);

    if (existing != nullptr) {
        // 幂等：同一 ID 且描述完全相同 → 允许（插件被加载两次是正常的）
        if (existing->name == desc.name && existing->numeric == desc.numeric &&
            existing->constraint == desc.constraint &&
            existing->dimension == desc.dimension &&
            existing->field_element == desc.field_element &&
            existing->field_components == desc.field_components &&
            existing->is_any == desc.is_any) {
            return Result<void>{};   // 无变化
        }
        // 同 ID 不同描述 → 拒绝。静默覆盖会让两个插件互相踩踏。
        return Result<void>{ErrorCode::duplicate_connection};
    }

    if (is_reserved_id) {
        // 未占用但落在保留区间：插件不得使用宿主预留的 ID
        return Result<void>{ErrorCode::out_of_range};
    }

    types_.push_back(desc);
    return Result<void>{};
}

const PortTypeDesc* PortTypeRegistry::find(PortTypeId id) const noexcept {
    if (id == kInvalidType) return nullptr;
    const auto it = std::find_if(types_.begin(), types_.end(),
                                 [id](const PortTypeDesc& d) { return d.id == id; });
    return it == types_.end() ? nullptr : &*it;
}

const PortTypeDesc* PortTypeRegistry::find_by_name(std::string_view name) const noexcept {
    if (name.empty()) return nullptr;
    const auto it = std::find_if(types_.begin(), types_.end(),
                                 [name](const PortTypeDesc& d) { return d.name == name; });
    return it == types_.end() ? nullptr : &*it;
}

bool PortTypeRegistry::has_builtin(PortTypeId id) const noexcept {
    const PortTypeDesc* d = find(id);
    return d != nullptr && d->id < kUserTypeBase;
}

const PortTypeRegistry& builtin_registry() noexcept {
    // C++11 起局部静态初始化是线程安全的；且此后**不再修改**，
    // 因此这是一份不可变共享数据，不违反"无可变全局状态"。
    static const PortTypeRegistry instance{};
    return instance;
}

}  // namespace qp::ports
