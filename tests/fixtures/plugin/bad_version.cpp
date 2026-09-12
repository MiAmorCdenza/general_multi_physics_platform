/**
 * @file bad_version.cpp
 * @brief Correct magic, an exports version this host cannot read.
 *
 * The case is a plugin built from a **newer** header than the host's, which is the
 * direction that actually happens: a student updates their plugin SDK, runs an
 * older host, and the export block has grown a field. The host must refuse rather
 * than read the fields it thinks it knows, because the ones it knows may have moved.
 */
#include "../../support/plugin_fixture_abi.hpp"

#if defined(_WIN32)
#  define QP_EXPORT extern "C" __declspec(dllexport)
#else
#  define QP_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace {

using namespace qp::test::fixture;

const FixtureManifest kManifest{"org.qp.fixture.badversion", "Fixture Bad Version", 1, 0, 0,
                                kLayout, kCapNodeType, 0, nullptr};

std::int32_t do_register(void* /*ctx*/) { return 0; }
void do_unregister(void* /*ctx*/) {}

// Version 99, tagged correctly, everything else well-formed.
const FixtureExports kExports{kTag, 99, 0, &kManifest, do_register, do_unregister};

}  // namespace

QP_EXPORT const void* qp_plugin_entry() { return &kExports; }
