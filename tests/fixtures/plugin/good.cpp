/**
 * @file good.cpp
 * @brief A plugin that loads, registers, and unregisters cleanly.
 *
 * The baseline every failure fixture is a single change away from, so a test that
 * fails can be attributed to that change rather than to the fixture's shape.
 */
#include "../../support/plugin_fixture_abi.hpp"

#if defined(_WIN32)
#  define QP_EXPORT extern "C" __declspec(dllexport)
#else
#  define QP_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace {

using namespace qp::test::fixture;

const FixtureManifest kManifest{"org.qp.fixture.good", "Fixture Good", 1, 0, 0, kLayout,
                                kCapNodeType, 0, nullptr};

std::int32_t do_register(void* /*ctx*/) { return 0; }
void do_unregister(void* /*ctx*/) {}

const FixtureExports kExports{kTag, kExportsVersion, 0, &kManifest, do_register, do_unregister};

}  // namespace

QP_EXPORT const void* qp_plugin_entry() { return &kExports; }
