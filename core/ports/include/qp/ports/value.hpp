/**
 * @file value.hpp
 * @brief 端口上流动的值：可复制的、类型安全的、量纲中立的标签联合。
 *
 * ## 为什么不用 `std::any`
 *
 * `std::any` 要求 `typeid` 匹配才能取值，而 `typeid`：
 *   - 跨 DLL 边界不可靠（各 DLL 的 RTTI 不共享）；
 *   - 无法序列化（学生保存的实验要能跨机器打开）；
 *   - 对图形编辑器不可枚举（它要列出"这个端口能接哪些类型"）。
 *
 * 因此用显式的 `Kind` 标签。它同时也是一份**可枚举的类型清单**，
 * 供 UI 与校验器直接消费。
 *
 * ## 量纲为什么不进 `Value`
 *
 * 量纲是**端口的契约**，不是值的属性。同一个 `42.0` 接到长度端口就是
 * 42 米，接到时间端口就是 42 秒。把量纲塞进值里会导致：
 *   - 每个数字都要带一份量纲（缓存键里全是量纲，命中率下降）；
 *   - "同一个值接到不同量纲端口"变成类型错误，而它在物理上是合法的。
 *
 * 判定"这个值配得上这个端口"由 `check.hpp` 完成。
 *
 * ## 大对象一律走句柄
 *
 * 19MB 的场不允许出现在值里。`field_handle` 只是一个 POD 描述符，
 * 真正的数据生命周期由 `core/abi` 的发布方保证（见 `ABI_LAYOUT.md` §3.4）。
 *
 * @ownership   pure（值类型，自持）
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   取用 `as_*` 前必须确认 kind，否则返回兜底值而非 UB
 * @errors      noexcept（所有访问器都不抛）
 * @frozen      是（`ValueKind` 的数值冻结）
 * @tests       ports.value.default_is_invalid, ports.value.construction,
 *              ports.value.numeric_accessors, ports.value.kind_is_exhaustive,
 *              ports.value.copy_independence, ports.value.never_throws,
 *              ports.value.widening_is_lossless_for_f32
 */
#pragma once

#include <qp/abi/lattice.hpp>
#include <qp/units.hpp>

#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <variant>

namespace qp::ports {

/// @brief 值的种类。数值即稳定标签，不得重排。
enum class ValueKind : std::uint8_t {
    invalid = 0,
    f64 = 1,
    f32 = 2,
    i64 = 3,
    boolean = 4,
    text = 5,
    dimension = 6,
    /// 场句柄：只有描述符，不含数据。真正的数据由发布方保证存活。
    field_handle = 7,
};

/**
 * @brief 端口上流动的值。
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   同一对象反复读取访问器得到同一结果
 * @errors      noexcept（访问器不抛；构造文本可能分配，失败即 terminate）
 * @frozen      是
 */
class Value final {
public:
    /// @brief 默认构造：无效值（尚未计算）。
    Value() noexcept = default;

    /// @brief 从 f64 构造。
    explicit Value(double v) noexcept : kind_(ValueKind::f64), data_(v) {}
    /// @brief 从 f32 构造。显式区分精度（ADR-0005）。
    explicit Value(float v) noexcept : kind_(ValueKind::f32), data_(v) {}
    /// @brief 从整数构造。
    explicit Value(std::int64_t v) noexcept : kind_(ValueKind::i64), data_(v) {}
    /// @brief 从布尔构造。
    explicit Value(bool v) noexcept : kind_(ValueKind::boolean), data_(v) {}
    /// @brief 从量纲构造。
    explicit Value(qp::units::Dim d) noexcept : kind_(ValueKind::dimension), data_(d) {}
    /// @brief 从文本构造。
    explicit Value(std::string s) : kind_(ValueKind::text), data_(std::move(s)) {}

    /// @brief 从场描述符构造。**只复制描述符，不复制数据。**
    explicit Value(qp::abi::LatticeDesc lattice) noexcept
        : kind_(ValueKind::field_handle), data_(lattice) {}

    [[nodiscard]] ValueKind kind() const noexcept { return kind_; }
    [[nodiscard]] bool valid() const noexcept { return kind_ != ValueKind::invalid; }

    // ── 访问器 ──────────────────────────────────────────────────────────────
    //
    // 全部返回兜底值而不是 UB：插件写错 kind 检查时应当得到可诊断的
    // 错误结果，而不是崩溃或随机数。@pre 违反由上层 check_value 报告。

    /// @brief 取 f64。kind 不符返回 0.0。
    [[nodiscard]] double as_f64() const noexcept {
        return kind_ == ValueKind::f64 ? std::get<double>(data_) : 0.0;
    }
    /// @brief 取 f32。kind 不符返回 0.0f。
    [[nodiscard]] float as_f32() const noexcept {
        return kind_ == ValueKind::f32 ? std::get<float>(data_) : 0.0f;
    }
    /// @brief 取整数。kind 不符返回 0。
    [[nodiscard]] std::int64_t as_i64() const noexcept {
        return kind_ == ValueKind::i64 ? std::get<std::int64_t>(data_) : 0;
    }
    /// @brief 取布尔。kind 不符返回 false。
    [[nodiscard]] bool as_bool() const noexcept {
        return kind_ == ValueKind::boolean && std::get<bool>(data_);
    }
    /// @brief 取量纲。kind 不符返回无量纲。
    [[nodiscard]] qp::units::Dim as_dimension() const noexcept {
        return kind_ == ValueKind::dimension ? std::get<qp::units::Dim>(data_)
                                             : qp::units::Dim{};
    }
    /// @brief 取文本。kind 不符返回空串。
    [[nodiscard]] const std::string& as_text() const noexcept {
        static const std::string kEmpty{};
        return kind_ == ValueKind::text ? std::get<std::string>(data_) : kEmpty;
    }
    /// @brief 取场描述符。kind 不符返回默认格子。
    [[nodiscard]] qp::abi::LatticeDesc as_field() const noexcept {
        return kind_ == ValueKind::field_handle ? std::get<qp::abi::LatticeDesc>(data_)
                                                : qp::abi::LatticeDesc{};
    }

    // ── 查询 ────────────────────────────────────────────────────────────────

    /// @brief 是否为数值（f64 / f32 / i64 / boolean）。
    [[nodiscard]] bool is_numeric() const noexcept {
        switch (kind_) {
            case ValueKind::f64:
            case ValueKind::f32:
            case ValueKind::i64:
            case ValueKind::boolean:
                return true;
            default:
                return false;
        }
    }

    /**
     * @brief 统一按 double 取值。非数值返回 0.0。
     *
     * 这是 ADR-0005 所说的"端口边界唯一一次拓宽"的落点：
     * f32 与整型在这里升为 double 供图内计算。
     * f32 → double 是**无损**的（有性质测试断言）。
     *
     * @ownership   pure
     * @thread      any
     * @pre         none
     * @post        数值类返回其数值；非数值类返回 0.0
     * @invariant   f32 载荷往返无损：static_cast<float>(to_double()) == as_f32()
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      否
     * @tests       ports.value.numeric_accessors, ports.value.widening_is_lossless_for_f32
     */
    [[nodiscard]] double to_double() const noexcept {
        switch (kind_) {
            case ValueKind::f64: return std::get<double>(data_);
            case ValueKind::f32: return static_cast<double>(std::get<float>(data_));
            case ValueKind::i64: return static_cast<double>(std::get<std::int64_t>(data_));
            case ValueKind::boolean: return std::get<bool>(data_) ? 1.0 : 0.0;
            default: return 0.0;
        }
    }

    /// @brief 种类的稳定短名。用于诊断与 UI。
    [[nodiscard]] const char* kind_name() const noexcept {
        switch (kind_) {
            case ValueKind::invalid: return "invalid";
            case ValueKind::f64: return "f64";
            case ValueKind::f32: return "f32";
            case ValueKind::i64: return "i64";
            case ValueKind::boolean: return "boolean";
            case ValueKind::text: return "text";
            case ValueKind::dimension: return "dimension";
            case ValueKind::field_handle: return "field_handle";
        }
        return "unknown";
    }

    /**
     * @brief 相等比较：种类相同**且**载荷相同。
     *
     * 刻意手写而不用 `data_ == other.data_`：那会要求 variant 里每个替代类型
     * 都有 `operator==`，而 `abi::LatticeDesc` 是纯 POD、不提供比较运算符
     * （ABI 层只认字节，不认语义）。逐类比较把"哪些类型可比较"这个决定
     * 留在本文件里，而不是泄漏给 abi。
     *
     * @ownership   pure
     * @thread      any
     * @pre         none
     * @post        种类不同则必定不等
     * @invariant   自反、对称
     * @errors      noexcept
     * @complexity  O(len)（文本比较）
     * @nondet      none
     * @frozen      否
     * @tests       ports.value.construction, ports.value.field_handle_carries_no_data
     */
    [[nodiscard]] friend bool operator==(const Value& a, const Value& b) noexcept {
        if (a.kind_ != b.kind_) return false;
        switch (a.kind_) {
            case ValueKind::invalid:
                return true;
            case ValueKind::f64:
                return std::get<double>(a.data_) == std::get<double>(b.data_);
            case ValueKind::f32:
                return std::get<float>(a.data_) == std::get<float>(b.data_);
            case ValueKind::i64:
                return std::get<std::int64_t>(a.data_) == std::get<std::int64_t>(b.data_);
            case ValueKind::boolean:
                return std::get<bool>(a.data_) == std::get<bool>(b.data_);
            case ValueKind::text:
                return std::get<std::string>(a.data_) == std::get<std::string>(b.data_);
            case ValueKind::dimension:
                return std::get<qp::units::Dim>(a.data_) == std::get<qp::units::Dim>(b.data_);
            case ValueKind::field_handle: {
                // LatticeDesc 是 POD：按字节比较即语义比较
                const auto& la = std::get<qp::abi::LatticeDesc>(a.data_);
                const auto& lb = std::get<qp::abi::LatticeDesc>(b.data_);
                return std::memcmp(&la, &lb, sizeof(la)) == 0;
            }
        }
        return false;
    }
    [[nodiscard]] friend bool operator!=(const Value& a, const Value& b) noexcept {
        return !(a == b);
    }

private:
    ValueKind kind_ = ValueKind::invalid;
    std::variant<std::monostate, double, float, std::int64_t, bool, std::string,
                 qp::units::Dim, qp::abi::LatticeDesc>
        data_{};
};

}  // namespace qp::ports
