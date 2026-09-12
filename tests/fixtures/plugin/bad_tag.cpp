/**
 * @file bad_tag.cpp
 * @brief Correct shape, wrong magic.
 *
 * This is the fixture that justifies the tag existing at all. Everything after
 * offset 8 in an export block is a pointer, so a host that trusted the shape would
 * read `manifest` out of bytes that are not a manifest and then follow it. The tag
 * is what makes that a clean refusal instead.
 */
#include "../../support/plugin_fixture_abi.hpp"

#if defined(_WIN32)
#  define QP_EXPORT extern "C" __declspec(dllexport)
#else
#  define QP_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace {

using namespace qp::test::fixture;

const FixtureManifest kManifest{"org.qp.fixture.badtag", "Fixture Bad Tag", 1, 0, 0, kLayout,
                                kCapNodeType, 0, nullptr};

std::int32_t do_register(void* /*ctx*/) { return 0; }
void do_unregister(void* /*ctx*/) {}

// The manifest and both callbacks are perfectly valid. Only the four bytes at offset
// 0 are wrong, so a test that reports `wrong_tag` is reporting exactly one defect.
const FixtureExports kExports{0xDEADBEEFu, kExportsVersion, 0, &kManifest, do_register,
                              do_unregister};

}  // namespace

QP_EXPORT const void* qp_plugin_entry() { return &kExports; }
