/**
 * @file value_key.cpp
 * @brief 确定性哈希与规范化文本的实现。
 */
#include <qp/graph/eval/value_key.hpp>

#include <qp/units/unit_symbol.hpp>

#include <array>
#include <cstdio>
#include <cstring>
#include <string>

namespace qp::graph {
namespace {

/// @brief 量纲的规范化文本（七个指数，逗号分隔）。
[[nodiscard]] std::string dim_text(qp::units::Dim d) {
    static constexpr std::array<const char*, 7> kNames{"L", "M", "T", "I", "Th", "N", "J"};
    const std::array<qp::units::DimExp, 7> exps{d.L, d.M, d.T, d.I, d.Th, d.N, d.J};
    std::string out;
    for (std::size_t i = 0; i < 7; ++i) {
        if (i != 0) out += ',';
        out += kNames[i];
        out += '=';
        out += std::to_string(static_cast<int>(exps[i]));
    }
    return out;
}

/// @brief double 的**精确**文本。
///
/// 用 `%a`（十六进制浮点）而不是 `%.17g`：
///
/// `%.17g` 只保证"能被解析回同一个值"，**不保证不同值给出不同文本**。
/// 实测：`0.1 + 0.2` 与字面量 `0.3` 是两个不同的 double
/// （…0444 与 …9988），但 `%.17g` 把两者都输出成 `"0.3"`。
/// 那会让缓存键的精确比较**把两个不同的计算当成同一次**——
/// 这是最危险的一类缓存缺陷：不崩溃，只是算出别人的结果。
///
/// `%a` 用足够的十六进制位精确表示该 double，因此
/// "文本不同 ⟺ 值不同"成立。它同时也不受 locale 小数分隔符影响。
///
/// **但 `%a` 的实现输出不一致**：GCC 输出最短形式 `0x1.8p+0`，
/// MSVC 输出补零形式 `0x1.8000000000000p+0`。
/// 不做归一化时，缓存在两个编译器之间**无法互换**，测试期望也会分平台。
/// 因此这里把尾数小数部分的尾随零去掉
/// （`0x1.8000…p+0` → `0x1.8p+0`，`0x1.0000…p+0` → `0x1p+0`）。
/// 归一化不改变所表示的值，文本仍然精确。
[[nodiscard]] std::string f64_text(double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%a", v);

    std::string s{buf};
    const std::size_t dot = s.find('.');
    if (dot == std::string::npos) return s;   // 无小数部分
    const std::size_t p = s.find('p', dot);
    const std::size_t end = (p == std::string::npos) ? s.size() : p;

    std::size_t last = end;
    while (last > dot + 1 && s[last - 1] == '0') --last;

    if (last == dot + 1) {
        s.erase(dot, end - dot);              // 小数全零 → 连点一起去掉
    } else if (last < end) {
        s.erase(last, end - last);            // 只去尾随零
    }
    return s;
}

}  // namespace

ValueHash mix_bytes(ValueHash seed, const void* data, std::size_t len) noexcept {
    const auto* p = static_cast<const unsigned char*>(data);
    ValueHash h = seed;
    for (std::size_t i = 0; i < len; ++i) {
        h ^= static_cast<ValueHash>(p[i]);
        h *= kFnvPrime;
    }
    return h;
}

ValueHash mix_u64(ValueHash seed, std::uint64_t v) noexcept {
    // 按小端序逐字节混入：保证大小端平台给出同一哈希
    unsigned char buf[8];
    for (int i = 0; i < 8; ++i) {
        buf[i] = static_cast<unsigned char>((v >> (8 * i)) & 0xFFU);
    }
    return mix_bytes(seed, buf, sizeof(buf));
}

ValueHash hash_value(ValueHash seed, const qp::ports::Value& v) noexcept {
    // 种类标签先行混入：不同种类的值绝不能给出同一哈希
    seed = mix_u64(seed, static_cast<std::uint64_t>(v.kind()));
    switch (v.kind()) {
        case qp::ports::ValueKind::invalid:
            return seed;
        case qp::ports::ValueKind::f64: {
            const double d = v.as_f64();
            return mix_bytes(seed, &d, sizeof(d));
        }
        case qp::ports::ValueKind::f32: {
            const float f = v.as_f32();
            return mix_bytes(seed, &f, sizeof(f));
        }
        case qp::ports::ValueKind::i64:
            return mix_u64(seed, static_cast<std::uint64_t>(v.as_i64()));
        case qp::ports::ValueKind::boolean:
            return mix_u64(seed, v.as_bool() ? 1U : 0U);
        case qp::ports::ValueKind::text: {
            const std::string& s = v.as_text();
            return mix_bytes(seed, s.data(), s.size());
        }
        case qp::ports::ValueKind::dimension: {
            const qp::units::Dim d = v.as_dimension();
            return mix_bytes(seed, &d, sizeof(d));
        }
        case qp::ports::ValueKind::field_handle: {
            // 大对象走标识：按格子描述符的字节混入，不逐字节哈希数据。
            // "同一格子 = 同一份数据"由 core/abi 的发布方保证。
            const qp::abi::LatticeDesc l = v.as_field();
            return mix_bytes(seed, &l, sizeof(l));
        }
    }
    return seed;
}

ValueHash hash_port_values(
    ValueHash seed,
    const std::vector<std::pair<PortNumber, qp::ports::Value>>& values) noexcept {
    seed = mix_u64(seed, static_cast<std::uint64_t>(values.size()));
    for (const auto& [port, value] : values) {
        seed = mix_u64(seed, static_cast<std::uint64_t>(port));
        seed = hash_value(seed, value);
    }
    return seed;
}

std::string canonical_text(const qp::ports::Value& v) noexcept {
    switch (v.kind()) {
        case qp::ports::ValueKind::invalid:
            return "invalid";
        case qp::ports::ValueKind::f64:
            return "f64:" + f64_text(v.as_f64());
        case qp::ports::ValueKind::f32:
            // 与 f64 共用同一个归一化文本函数（f32 升到 double 后仍精确）
            return "f32:" + f64_text(static_cast<double>(v.as_f32()));
        case qp::ports::ValueKind::i64:
            return "i64:" + std::to_string(v.as_i64());
        case qp::ports::ValueKind::boolean:
            return v.as_bool() ? "bool:1" : "bool:0";
        case qp::ports::ValueKind::text:
            return "text:" + v.as_text();
        case qp::ports::ValueKind::dimension:
            return "dim:" + dim_text(v.as_dimension());
        case qp::ports::ValueKind::field_handle: {
            const qp::abi::LatticeDesc l = v.as_field();
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                          "field:%d/%d/%d:%u,%u,%u:%u",
                          static_cast<int>(l.kind), static_cast<int>(l.component),
                          static_cast<int>(l.element), l.count[0], l.count[1], l.count[2],
                          l.spacing_bytes);
            return buf;
        }
    }
    return "unknown";
}

}  // namespace qp::graph
