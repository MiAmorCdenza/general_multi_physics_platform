/**
 * @file qp/ports.hpp
 * @brief ports 模块的唯一入口。
 *
 * 端口类型注册表是"加一个域零核心改动"这个承诺的落点：
 * 新类型能被声明、能被校验、能被 UI 自动渲染，都不需要改宿主代码。
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   本模块不依赖 core 内除 units / diag / abi 之外的模块
 * @errors      noexcept
 * @complexity  —
 * @nondet      none
 * @frozen      是（已注册类型 ID 与名称）
 * @tests       ports.value.default_is_invalid, ports.value.construction,
 *              ports.value.numeric_accessors, ports.value.kind_is_exhaustive,
 *              ports.value.copy_independence, ports.value.never_throws,
 *              ports.value.widening_is_lossless_for_f32,
 *              ports.value.field_handle_carries_no_data, ports.value.size_is_bounded,
 *              ports.type_desc.basic_fields, ports.type_desc.dimension_constraint,
 *              ports.type_desc.numeric_kind_consistency,
 *              ports.registry.builtins_present, ports.registry.add_custom_type,
 *              ports.registry.rejects_duplicate_id, ports.registry.idempotent_reregister,
 *              ports.registry.rejects_reserved_range, ports.registry.rejects_invalid_id,
 *              ports.registry.count_and_lookup, ports.registry.deterministic_order,
 *              ports.registry.lookup_by_name, ports.registry.shared_builtins_is_readonly,
 *              ports.check.connect_same_type, ports.check.connect_rejects_direction,
 *              ports.check.connect_rejects_type_mismatch,
 *              ports.check.connect_rejects_dimension_mismatch,
 *              ports.check.connect_allows_numeric_widening,
 *              ports.check.connect_any_is_flagged, ports.check.connect_unknown_type,
 *              ports.check.connect_is_symmetric_for_mismatch,
 *              ports.check.dimension_compatible, ports.check.dimension_any_accepts_all,
 *              ports.check.dimension_same_as_input_is_deferred,
 *              ports.check.value_matches_port, ports.check.value_rejects_wrong_kind,
 *              ports.check.value_accepts_widening, ports.check.value_any_accepts_all,
 *              ports.check.value_rejects_invalid, ports.check.value_field_components_matter
 */
#pragma once

#include <qp/ports/check.hpp>
#include <qp/ports/port_type.hpp>
#include <qp/ports/registry.hpp>
#include <qp/ports/value.hpp>

namespace qp::ports {

/// @brief ports 模块的 ABI 版本。端口类型 ID 或 ValueKind 数值变更时必须递增。
inline constexpr int kPortsAbiVersion = 1;

}  // namespace qp::ports
