/**
 * @file no_manifest.cpp
 * @brief Correct tag and version, but the manifest pointer is null.
 *
 * Checked separately from the tag because it is a different mistake made by a
 * differently-broken plugin: a static-initialisation order bug, or a plugin whose
 * manifest is built at runtime and was not ready when the entry point was called.
 * A tag check cannot see it, and dereferencing it would fault inside the host.
 */
#include "../../support/plugin_fixture_abi.hpp"

#if defined(_WIN32)
#  define QP_EXPORT extern "C" __declspec(dllexport)
#else
#  define QP_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace {

using namespace qp::test::fixture;

std::int32_t do_register(void* /*ctx*/) { return 0; }
void do_unregister(void* /*ctx*/) {}

const FixtureExports kExports{kTag, kExportsVersion, 0, nullptr, do_register, do_unregister};

}  // namespace

QP_EXPORT const void* qp_plugin_entry() { return &kExports; }
