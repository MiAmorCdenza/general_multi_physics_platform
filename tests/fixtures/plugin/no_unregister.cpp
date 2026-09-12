/**
 * @file no_unregister.cpp
 * @brief Correct in every way except that it offers no way to undo its registration.
 *
 * Refused on purpose, and this is the least obvious rule in the loader. The plugin
 * works: it would register, it would probably run. The problem is that a registry
 * holds **non-owning** pointers to objects this library owns -- `KernelDesc::impl`
 * is the documented case -- so a host that accepts it can never release the mapping.
 * The choice is between leaking the library for the life of the process and
 * releasing memory that registries still point into, and the second one reads
 * unmapped memory at a call site arbitrarily far from the unload that caused it.
 *
 * Refusing at load time turns that into a message the plugin author can act on,
 * while they still have the code in front of them.
 */
#include "../../support/plugin_fixture_abi.hpp"

#if defined(_WIN32)
#  define QP_EXPORT extern "C" __declspec(dllexport)
#else
#  define QP_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace {

using namespace qp::test::fixture;

const FixtureManifest kManifest{"org.qp.fixture.nounregister", "Fixture No Unregister", 1, 0, 0,
                                kLayout, kCapNodeType, 0, nullptr};

std::int32_t do_register(void* /*ctx*/) { return 0; }

// No `unregister`, deliberately.
const FixtureExports kExports{kTag, kExportsVersion, 0, &kManifest, do_register, nullptr};

}  // namespace

QP_EXPORT const void* qp_plugin_entry() { return &kExports; }
