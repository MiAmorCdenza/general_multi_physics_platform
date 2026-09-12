/**
 * @file hungry.cpp
 * @brief A plugin whose manifest declares a capability the test host does not grant.
 *
 * The gate this exists for is the one that has to happen **before** `register_into` runs. This fixture therefore
 * counts its own registrations into host-owned memory: the assertion is not only that the plugin was refused,
 * but that it was never entered. Refusal after the fact would have to undo work, and undoing a plugin's work is
 * a best-effort operation against code that is by definition not behaving as the host expects.
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

using qp::test::host_fixture::kHungryId;
using qp::test::host_fixture::Recording;

qp::diag::ErrorCode do_register(void* host_context) {
    // Reached only if the gate leaked. The count is the evidence.
    if (host_context != nullptr) static_cast<Recording*>(host_context)->note(kHungryId);
    return qp::diag::ErrorCode::ok;
}

void do_unregister(void* host_context) { (void)host_context; }

// Declares `file_io`, which the test host does not grant. The bit is declared as a **single** capability so
// that the refusal names exactly one reason: a manifest with two ungranted bits would leave a test unable to
// say which one the host reported.
const qp::plugin::PluginManifestC kManifest{kHungryId, "Host Fixture Hungry", 1, 0, 0,
                                            qp::abi::kFieldBufferLayout,
                                            static_cast<std::uint32_t>(qp::plugin::Capability::file_io),
                                            0, nullptr};

const qp::plugin::PluginExports kExports{qp::plugin::kExportsTag, qp::plugin::kExportsVersion, 0,
                                         &kManifest, do_register, do_unregister};

}  // namespace

QP_EXPORT const void* qp_plugin_entry() { return &kExports; }
