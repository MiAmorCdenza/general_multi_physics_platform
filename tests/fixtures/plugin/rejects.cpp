/**
 * @file rejects.cpp
 * @brief A well-formed block declaring a manifest the judge must refuse.
 *
 * The block is structurally perfect -- every check in `inspect_exports` passes --
 * and the manifest has no id and no name. So the only check that can catch it is
 * `judge`, which is the point: the two layers of validation have to be shown to be
 * independent, or one of them is dead code that nobody notices is dead.
 *
 * A plugin with no identity cannot be attributed in a diagnostic, cannot be ordered
 * against its dependencies, and cannot be named in a report about why a session
 * refused to start.
 */
#include "../../support/plugin_fixture_abi.hpp"

#if defined(_WIN32)
#  define QP_EXPORT extern "C" __declspec(dllexport)
#else
#  define QP_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace {

using namespace qp::test::fixture;

const FixtureManifest kManifest{"", "", 1, 0, 0, kLayout, kCapNodeType, 0, nullptr};

std::int32_t do_register(void* /*ctx*/) { return 0; }
void do_unregister(void* /*ctx*/) {}

const FixtureExports kExports{kTag, kExportsVersion, 0, &kManifest, do_register, do_unregister};

}  // namespace

QP_EXPORT const void* qp_plugin_entry() { return &kExports; }
