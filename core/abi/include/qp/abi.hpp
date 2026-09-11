/**
 * @file qp/abi.hpp
 * @brief The single entry point of the abi module.
 *
 * ## This module plays by different rules
 *
 * Other core modules may use templates, the STL, and C++ idioms. **abi may not.**
 *
 * Reason: abi is the boundary contract read by **external language bindings** (Python
 * / MATLAB / future Web). They do not compile C++; they read a byte-exact layout
 * spec (see `tests/ABI_LAYOUT.md`). Burying "what the ABI is" in template-dependent
 * C++ would leave outsiders unable to consume it -- the root of the ABI nightmare.
 *
 * So abi's constraints are:
 *   - C subset only: POD, fixed-width integers, no templates (`data_as` excepted, below)
 *   - **Zero core dependencies**: not even `units` is included (a dimension carries
 *     its own `FieldDim`; `tests/abi/` assertions guard that consistency)
 *   - Every struct has a version constant and layout assertions
 *   - A layout change must bump the version; an incompatible one is refused
 *
 * `data_as<T>` is the only template: sugar over `static_cast`, it never enters the
 * ABI, and external bindings do not need it.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   This module depends on no other module inside core
 * @errors      noexcept
 * @complexity  —
 * @nondet      none
 * @frozen      yes
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
