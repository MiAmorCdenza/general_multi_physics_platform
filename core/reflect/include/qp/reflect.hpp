/**
 * @file qp/reflect.hpp
 * @brief The single entry point of the reflect module.
 *
 * The single source of a type's name. A document names types in text, and if C++,
 * YAML and a script each produced that text independently they would drift -- and
 * the drift appears as a document that loads on one build and not another, naming a
 * type that plainly exists.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   A name is written down once, next to the type it names
 * @errors      fails to compile when a type does not name itself
 * @complexity  --
 * @nondet      none
 * @frozen      no
 * @tests       reflect.type_name.is_compile_time
 */
#pragma once

#include <qp/reflect/reflect.hpp>

namespace qp::reflect {

/// @brief ABI version of the reflect module. Bump when a name convention changes.
inline constexpr int kReflectAbiVersion = 1;

}  // namespace qp::reflect
