/**
 * @file error.hpp
 * @brief 错误码与后果级别：全平台统一的失败分类。
 *
 * 设计意图：插件的失败必须能被**归因给用户**，而不是变成一条无主日志。
 * 因此错误不是自由文本，而是分层的枚举：
 *   - ErrorCode  说明"哪里错了"（用于程序分支与测试断言）
 *   - Consequence 说明"错了会怎样"（用于宿主决定降级还是中止）
 *   - 自由文本只作为补充，永远不是判定依据
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   每个错误码恰好归属一个错误域
 * @errors      noexcept
 * @complexity  —
 * @nondet      none
 * @frozen      是（枚举值一旦发布即不可重新编号）
 * @tests       diag.error_code_values_are_stable, diag.error_domain_mapping
 */
#pragma once

#include <cstdint>
#include <string_view>

namespace qp::diag {

/// @brief 错误码。数值不得重排——它是稳定标识，不是内部序号。
///
/// 分域编码：高 8 位是域，低 8 位是域内序号。这样新增域不影响既有数值。
enum class ErrorCode : std::uint16_t {
    ok = 0,

    // ── 1xxx 输入与格式 ──────────────────────────────────────────────────────
    invalid_argument = 0x0101,
    malformed_document = 0x0102,
    unsupported_version = 0x0103,
    missing_field = 0x0104,
    out_of_range = 0x0105,

    // ── 2xxx 类型与量纲 ──────────────────────────────────────────────────────
    unknown_port_type = 0x0201,
    type_mismatch = 0x0202,
    dimension_mismatch = 0x0203,
    unit_mismatch = 0x0204,

    // ── 3xxx 图结构 ──────────────────────────────────────────────────────────
    unknown_node = 0x0301,
    unknown_port = 0x0302,
    cycle_detected = 0x0303,
    duplicate_connection = 0x0304,
    not_connected = 0x0305,
    graph_busy = 0x0306,

    // ── 4xxx 插件 ────────────────────────────────────────────────────────────
    plugin_not_found = 0x0401,
    plugin_incompatible = 0x0402,
    plugin_load_failed = 0x0403,
    plugin_capability_missing = 0x0404,

    // ── 5xxx 运行与数据 ──────────────────────────────────────────────────────
    run_not_found = 0x0501,
    seed_required = 0x0502,
    dataset_empty = 0x0503,
    fit_failed = 0x0504,

    // ── 9xxx 内部 ────────────────────────────────────────────────────────────
    internal_error = 0x0901,
    not_implemented = 0x0902,
    cancelled = 0x0903,
};

/**
 * @brief 失败的后果级别。宿主据此决定降级、重试还是中止。
 *
 * 这是"错误处理"与"错误报告"的分界：
 *   ErrorCode 给程序看，Consequence 给**调度决策**看。
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   顺序即严重程度（可比较、可排序）
 * @errors      noexcept
 * @complexity  —
 * @nondet      none
 * @frozen      是
 * @tests       diag.consequence_ordering
 */
enum class Consequence : std::uint8_t {
    /// 可忽略：本次操作无效，但系统状态完好。
    recoverable = 0,
    /// 该节点/插件本次失效，图上其余部分仍可继续。
    degraded = 1,
    /// 本次运行无法继续，但进程与文档完好。
    run_aborted = 2,
    /// 文档或进程状态不可信，必须停止。
    fatal = 3,
};

/// @brief 错误域。用于把错误码分组展示给用户。
enum class ErrorDomain : std::uint8_t {
    input = 0,
    typing = 1,
    graph = 2,
    plugin = 3,
    runtime = 4,
    internal = 5,
};

/**
 * @brief 取错误码的域。
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        返回 code 高 8 位对应的域；ok 归入 internal（无意义，仅保证全覆盖）
 * @invariant   同一域内的所有错误码，其域编号相同
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      否
 * @tests       diag.error_domain_mapping
 */
[[nodiscard]] constexpr ErrorDomain domain_of(ErrorCode code) noexcept {
    const auto v = static_cast<std::uint16_t>(code);
    if (v == 0) return ErrorDomain::internal;
    switch (v >> 8) {
        case 0x01: return ErrorDomain::input;
        case 0x02: return ErrorDomain::typing;
        case 0x03: return ErrorDomain::graph;
        case 0x04: return ErrorDomain::plugin;
        case 0x05: return ErrorDomain::runtime;
        default:   return ErrorDomain::internal;
    }
}

/**
 * @brief 错误码的稳定短名。用于日志、测试断言与序列化。
 *
 * 自由文本不是判定依据，这个短名才是。
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        返回非空的稳定标识符（ASCII、下划线分隔）
 * @invariant   同一 code 永远返回同一字符串；不同 code 不返回同一字符串
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      是（改名即破坏兼容）
 * @tests       diag.error_code_values_are_stable
 */
[[nodiscard]] constexpr std::string_view to_string(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::ok: return "ok";
        case ErrorCode::invalid_argument: return "invalid_argument";
        case ErrorCode::malformed_document: return "malformed_document";
        case ErrorCode::unsupported_version: return "unsupported_version";
        case ErrorCode::missing_field: return "missing_field";
        case ErrorCode::out_of_range: return "out_of_range";
        case ErrorCode::unknown_port_type: return "unknown_port_type";
        case ErrorCode::type_mismatch: return "type_mismatch";
        case ErrorCode::dimension_mismatch: return "dimension_mismatch";
        case ErrorCode::unit_mismatch: return "unit_mismatch";
        case ErrorCode::unknown_node: return "unknown_node";
        case ErrorCode::unknown_port: return "unknown_port";
        case ErrorCode::cycle_detected: return "cycle_detected";
        case ErrorCode::duplicate_connection: return "duplicate_connection";
        case ErrorCode::not_connected: return "not_connected";
        case ErrorCode::graph_busy: return "graph_busy";
        case ErrorCode::plugin_not_found: return "plugin_not_found";
        case ErrorCode::plugin_incompatible: return "plugin_incompatible";
        case ErrorCode::plugin_load_failed: return "plugin_load_failed";
        case ErrorCode::plugin_capability_missing: return "plugin_capability_missing";
        case ErrorCode::run_not_found: return "run_not_found";
        case ErrorCode::seed_required: return "seed_required";
        case ErrorCode::dataset_empty: return "dataset_empty";
        case ErrorCode::fit_failed: return "fit_failed";
        case ErrorCode::internal_error: return "internal_error";
        case ErrorCode::not_implemented: return "not_implemented";
        case ErrorCode::cancelled: return "cancelled";
    }
    return "unknown";
}

/// @brief 每个错误码的默认后果级别。插件可在具体场合覆盖。
[[nodiscard]] constexpr Consequence default_consequence(ErrorCode code) noexcept {
    switch (domain_of(code)) {
        case ErrorDomain::input:
        case ErrorDomain::typing:
            return Consequence::recoverable;   // 用户改一下参数即可
        case ErrorDomain::graph:
            return Consequence::degraded;      // 该节点失效，其余可继续
        case ErrorDomain::plugin:
            return Consequence::degraded;      // 插件失效不该拖垮文档
        case ErrorDomain::runtime:
            return Consequence::run_aborted;
        case ErrorDomain::internal:
            return Consequence::fatal;
    }
    return Consequence::fatal;
}

}  // namespace qp::diag
