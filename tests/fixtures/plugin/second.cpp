/**
 * @file second.cpp
 * @brief A second well-formed plugin, so ordering can be tested with two subjects.
 *
 * Identical to `good.cpp` except for its identity. A set of one cannot show that
 * load order follows dependencies rather than input position.
 */
#include "../../support/plugin_fixture_abi.hpp"

#if defined(_WIN32)
#  define QP_EXPORT extern "C" __declspec(dllexport)
#else
#  define QP_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace {

using namespace qp::test::fixture;

/// Depends on `good`, which is what makes the order observable: this plugin must be
/// installed after the one it names, whatever order the caller lists the files in.
const char* const kDependencies[]{"org.qp.fixture.good"};

const FixtureManifest kManifest{"org.qp.fixture.second", "Fixture Second", 1, 0, 0, kLayout,
                                kCapNodeType, 1, kDependencies};

std::int32_t do_register(void* /*ctx*/) { return 0; }
void do_unregister(void* /*ctx*/) {}

const FixtureExports kExports{kTag, kExportsVersion, 0, &kManifest, do_register, do_unregister};

}  // namespace

QP_EXPORT const void* qp_plugin_entry() { return &kExports; }
