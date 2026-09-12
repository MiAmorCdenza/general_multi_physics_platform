/**
 * @file qp/plugin.hpp
 * @brief The single entry point of the plugin module.
 *
 * The plugin contract: what a plugin declares, how the host judges it, in what
 * order a set of plugins loads, and the one exported C function that turns a file
 * on disk into code running in this process.
 *
 * @ownership   mixed
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   Nothing in this module parses a file format
 * @errors      See each declaration
 * @complexity  --
 * @nondet      none
 * @frozen      no
 * @tests       plugin.manifest.valid
 */
#pragma once

#include <qp/plugin/loader.hpp>
#include <qp/plugin/manifest.hpp>

namespace qp::plugin {

/// @brief ABI version of the plugin module. Bump when the Manifest schema changes.
inline constexpr int kPluginAbiVersion = 1;

}  // namespace qp::plugin
