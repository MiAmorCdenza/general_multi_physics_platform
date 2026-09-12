/**
 * @file empty.cpp
 * @brief A plugin whose manifest declares no capability at all.
 *
 * `ManifestVerdict::no_capabilities` is the one judgement a plugin cannot recover from by changing its code:
 * the manifest says that loading it could have no effect, so the host refuses it before mapping what it would
 * have contributed. This fixture is what makes that verdict observable through the host rather than only
 * through `judge`, which is where it was first tested and where it never met the loader.
 */
#include "host_plugin_fixture.hpp"

#include <qp/abi/abi_version.hpp>
#include <qp/plugin/loader.hpp>

#include <cstdint>

#if defined(_WIN32)
#  define QP_EXPORT extern "C" __declspec(dllexport)
#else
#  define QP_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace {

using qp::test::host_fixture::kEmptyId;

qp::diag::ErrorCode do_register(void* /*host_context*/) { return qp::diag::ErrorCode::ok; }
void do_unregister(void* /*host_context*/) {}

const qp::plugin::PluginManifestC kManifest{kEmptyId, "Host Fixture Empty", 1, 0, 0,
                                            qp::abi::kFieldBufferLayout, 0, 0, nullptr};

const qp::plugin::PluginExports kExports{qp::plugin::kExportsTag, qp::plugin::kExportsVersion, 0,
                                         &kManifest, do_register, do_unregister};

}  // namespace

QP_EXPORT const void* qp_plugin_entry() { return &kExports; }
