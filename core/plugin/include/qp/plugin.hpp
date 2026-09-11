/**
 * @file qp/plugin.hpp
 * @brief The single entry point of the plugin module.
 *
 * The plugin contract: what a plugin declares, how the host judges it, and in
 * what order a set of plugins loads. Parsing a file and mapping a shared library
 * are both adapters, not foundation.
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
 * @tests       plugin.manifest.valid
 */
#pragma once

#include <qp/plugin/manifest.hpp>

namespace qp::plugin {

/// @brief ABI version of the plugin module. Bump when the Manifest schema changes.
inline constexpr int kPluginAbiVersion = 1;

}  // namespace qp::plugin
