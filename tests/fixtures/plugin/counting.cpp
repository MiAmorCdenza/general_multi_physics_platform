/**
 * @file counting.cpp
 * @brief A plugin that records through host memory how often it was called.
 *
 * The fixture that makes the unload ordering observable. "Unregister before
 * releasing the library" is invisible in any test that only inspects the final
 * state, because both orders reach the same final state -- one of them just calls
 * `unregister` through a pointer into memory it already released. The only way to
 * tell them apart is to count the calls, and the only place to keep the count is
 * the host, so the plugin writes through `host_context` and owns nothing.
 */
#include "../../support/plugin_fixture_abi.hpp"

#if defined(_WIN32)
#  define QP_EXPORT extern "C" __declspec(dllexport)
#else
#  define QP_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace {

using namespace qp::test::fixture;

const FixtureManifest kManifest{"org.qp.fixture.counting", "Fixture Counting", 1, 0, 0, kLayout,
                                kCapNodeType, 0, nullptr};

std::int32_t do_register(void* ctx) {
    if (ctx == nullptr) return 1;
    auto* counter = static_cast<Counter*>(ctx);
    ++counter->registered;
    // A failed registration is requested by the host, so the cleanup path can be
    // observed on demand. The counter has already recorded the attempt, which is
    // what makes the partial-installation case distinguishable from "never called".
    if (counter->fail_registration != 0) {
        return 2;
    }
    return 0;
}

void do_unregister(void* ctx) {
    if (ctx == nullptr) return;
    ++static_cast<Counter*>(ctx)->unregistered;
}

const FixtureExports kExports{kTag, kExportsVersion, 0, &kManifest, do_register, do_unregister};

}  // namespace

QP_EXPORT const void* qp_plugin_entry() { return &kExports; }
