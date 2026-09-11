/**
 * @file value_key.cpp
 * @brief Implementation of deterministic hashing and canonical text.
 */
#include <qp/graph/eval/value_key.hpp>

#include <qp/units/unit_symbol.hpp>

#include <array>
#include <cstdio>
#include <cstring>
#include <string>

namespace qp::graph {
namespace {

/// @brief Canonical text for a dimension (seven exponents, comma separated).
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

/// @brief The **exact** text for a double.
///
/// Use `%a` (hexadecimal float) rather than `%.17g`:
///
/// `%.17g` only guarantees "parses back to the same value"; it does **not guarantee that
/// different values produce different text**. Measured: `0.1 + 0.2` and the literal `0.3`
/// are two different doubles (...0444 and ...9988), yet `%.17g` prints both as `"0.3"`.
/// That would let the exact cache-key comparison treat **two different computations as one** --
/// the most dangerous class of cache defect: no crash, just someone else's result.
///
/// `%a` represents the double exactly with enough hex digits, so "different text <=>
/// different value" holds. It is also unaffected by the locale decimal separator.
///
/// **But `%a` output is not consistent across implementations**: GCC prints the shortest
/// form `0x1.8p+0`, MSVC the zero-padded form `0x1.8000000000000p+0`.
/// Without normalization the cache is **not interchangeable** between the two compilers,
/// and test expectations would differ per platform. So the trailing zeros of the
/// mantissa fraction are stripped here (`0x1.8000...p+0` -> `0x1.8p+0`,
/// `0x1.0000...p+0` -> `0x1p+0`). Normalization does not change the value; text stays exact.
[[nodiscard]] std::string f64_text(double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%a", v);

    std::string s{buf};
    const std::size_t dot = s.find('.');
    if (dot == std::string::npos) return s;   // no fractional part
    const std::size_t p = s.find('p', dot);
    const std::size_t end = (p == std::string::npos) ? s.size() : p;

    std::size_t last = end;
    while (last > dot + 1 && s[last - 1] == '0') --last;

    if (last == dot + 1) {
        s.erase(dot, end - dot);              // all-zero fraction -> drop the point too
    } else if (last < end) {
        s.erase(last, end - last);            // drop trailing zeros only
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
    // Mix in byte by byte, little-endian: little- and big-endian hosts hash identically
    unsigned char buf[8];
    for (int i = 0; i < 8; ++i) {
        buf[i] = static_cast<unsigned char>((v >> (8 * i)) & 0xFFU);
    }
    return mix_bytes(seed, buf, sizeof(buf));
}

ValueHash hash_value(ValueHash seed, const qp::ports::Value& v) noexcept {
    // Mix the kind tag in first: values of different kinds must never hash the same
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
            // Large objects hash by identity: mix the lattice descriptor bytes, not the data.
            // "Same lattice = same data" is guaranteed by the core/abi publisher.
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
            // Shares the f64 normalization function (f32 is exact once widened to double)
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
