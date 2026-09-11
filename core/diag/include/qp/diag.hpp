/**
 * @file qp/diag.hpp
 * @brief diag 模块的唯一入口。
 *
 * diag 是 L0 地基模块，**不依赖 core 内任何其他模块**。
 * 它是全平台的错误通道：任何模块、任何插件报告失败都走这里。
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   本模块不引入任何可变全局状态（诊断出口是接口，不是单例）
 * @errors      noexcept
 * @complexity  —
 * @nondet      none
 * @frozen      是（错误码数值与 Result 形状）
 * @tests       diag.error_code_values_are_stable, diag.error_code_names_are_unique,
 *              diag.error_domain_mapping, diag.consequence_ordering,
 *              diag.result.ok_value, diag.result.err_only, diag.result.layout,
 *              diag.result.value_or_fallback, diag.result.monadic_chaining,
 *              diag.result.void_specialization, diag.result.void_ok_is_truthy,
 *              diag.result.no_throw_on_access, diag.result.move_only_payload,
 *              diag.diagnostic.construction, diag.diagnostic.stable_text,
 *              diag.diagnostic.no_live_references,
 *              diag.sink.null_sink, diag.sink.collecting_sink,
 *              diag.sink.collecting_sink_thread_safe
 */
#pragma once

#include <qp/diag/contract.hpp>
#include <qp/diag/diagnostic.hpp>
#include <qp/diag/error.hpp>
#include <qp/diag/result.hpp>
#include <qp/diag/sink.hpp>

namespace qp::diag {

/// @brief diag 模块的 ABI 版本。错误码数值或 Result 形状变更时必须递增。
inline constexpr int kDiagAbiVersion = 1;

}  // namespace qp::diag
