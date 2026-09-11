/**
 * @file qp/authoring/capability.hpp
 * @brief The single entry point of the capability module.
 *
 * What a plugin offers, and how the host finds a provider for a namespaced
 * capability id. Declaration and grant are separate state on purpose.
 *
 * @ownership   pure
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   Nothing in this module performs I/O
 * @errors      noexcept
 * @complexity  --
 * @nondet      none
 * @frozen      no
 * @tests       capability.registry.negotiate_grants_declared
 */
#pragma once

#include <qp/authoring/capability/capability.hpp>

namespace qp::authoring {

/// @brief ABI version of the capability module. Bump when Registry's observable
///        behaviour or the verdict set changes.
inline constexpr int kCapabilityAbiVersion = 1;

}  // namespace qp::authoring
