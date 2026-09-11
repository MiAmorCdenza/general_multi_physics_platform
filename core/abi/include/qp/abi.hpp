/**
 * @file qp/abi.hpp
 * @brief abi 模块的唯一入口。
 *
 * ## 这个模块的规矩与别处不同
 *
 * 其它 core 模块可以用模板、STL、C++ 惯用法。**abi 不可以。**
 *
 * 原因：abi 是给**外部语言绑定**（Python / MATLAB / 未来 Web）看的边界契约。
 * 那些绑定不编译 C++，只读一份逐字节的布局说明（见 `tests/ABI_LAYOUT.md`）。
 * 若 abi 头文件依赖模板库，"ABI 是什么"就被埋进了一份 C++ 实现里，
 * 外部无法独立消费——几十年来的 ABI 兼容噩梦都源于此。
 *
 * 因此 abi 的约束是：
 *   - 只用 C 语言子集：POD、定长整数、无模板（`data_as` 是唯一例外，见下）
 *   - **零 core 依赖**：连 `units` 都不 include（量纲自带 `FieldDim` 表示，
 *     一致性由 `tests/abi/` 的断言守住）
 *   - 每个结构体都有版本常量与布局断言
 *   - 布局变更必须升版本号，不兼容即拒绝加载
 *
 * `data_as<T>` 是唯一的模板：它只是 `static_cast` 的语法糖，不进入 ABI，
 * 外部绑定不需要它。
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   本模块不依赖 core 内任何其它模块
 * @errors      noexcept
 * @complexity  —
 * @nondet      none
 * @frozen      是
 * @tests       abi.version.values_are_frozen, abi.version.compatibility_matrix,
 *              abi.version.rejects_newer_major, abi.version.rejects_older_major,
 *              abi.version.accepts_newer_minor, abi.version.rejects_layout_mismatch,
 *              abi.version.reflexive, abi.version.verdict_names,
 *              abi.lattice.size_and_alignment, abi.lattice.field_offsets,
 *              abi.lattice.trivially_copyable,
 *              abi.lattice.default_is_point_scalar, abi.lattice.point_count,
 *              abi.lattice.data_bytes, abi.lattice.consistency_check,
 *              abi.lattice.dimension_is_carried,
 *              abi.field_buffer.size_and_alignment, abi.field_buffer.field_offsets,
 *              abi.field_buffer.flags_are_bitwise, abi.field_buffer.trivially_copyable,
 *              abi.field_buffer.copy_is_a_second_handle, abi.field_buffer.magic_constant,
 *              abi.field_buffer.validate_ok, abi.field_buffer.validate_rejects_bad_magic,
 *              abi.field_buffer.validate_rejects_layout_mismatch,
 *              abi.field_buffer.validate_rejects_oversized_data,
 *              abi.field_buffer.seqlock_roundtrip,
 *              abi.dim_matches_units_dim
 */
#pragma once

#include <qp/abi/abi_version.hpp>
#include <qp/abi/byte_order.hpp>
#include <qp/abi/field_buffer.hpp>
#include <qp/abi/field_dim.hpp>
#include <qp/abi/lattice.hpp>
