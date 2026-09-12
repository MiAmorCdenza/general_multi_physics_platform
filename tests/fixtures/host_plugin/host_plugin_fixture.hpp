/**
 * @file host_plugin_fixture.hpp
 * @brief The export block and the shared content the host fixtures register.
 *
 * ## Why these fixtures link the host, when the loader's fixtures deliberately do not
 *
 * `tests/fixtures/plugin/` exists to exercise the *loader*, so its fixtures must be able to disagree with the
 * host about the ABI -- a fixture that included the real header would agree by construction and every check it
 * exists to exercise would be guaranteed to pass. They are hand-written bytes for that reason.
 *
 * These fixtures exercise the opposite half: what happens *after* a library judged cleanly. There is nothing
 * to test about a tag or a version here, and the content a plugin contributes is a `NodeDesc`/`KernelDesc`/
 * `IExporter`, which is host-side C++ by definition. So they link `qp::host` and are built by the same
 * toolchain as the tests -- which is exactly the in-process plugin model this project documents, and exactly
 * why the loader's own ABI checks (tag, exports version, layout) are the ones that matter.
 *
 * ## What the host cannot see, and therefore what is asserted by pointer
 *
 * An instrument must be a concrete type with virtual functions, so a fixture that registers one would need a
 * whole implementation to test a table lookup. This fixture registers node types, a kernel and an exporter --
 * the three contributions whose registration is a one-line call -- and the instrument path is exercised by
 * `host.loads_a_real_plugin_and_mounts_what_it_registers` through the exporter's sibling API.
 *
 * The node type's name is deliberately spelled out in one place and returned by a function, because the test
 * must assert against the same bytes the plugin registered rather than against a copy it typed itself.
 */
#pragma once

#include <qp/graph/ir/descriptor.hpp>
#include <qp/graph/kernels/kernel.hpp>

#include <qp/diag/error.hpp>

#include <cstdint>
#include <string_view>

namespace qp::test::host_fixture {

/// @brief The node type this fixture registers. Named here so the test and the plugin cannot disagree.
inline constexpr std::string_view kNodeType = "fixture.spinner";
/// @brief A second type, so a plugin that registers more than one is not a special case.
inline constexpr std::string_view kSecondNodeType = "fixture.meter";
/// @brief The kernel this fixture registers.
inline constexpr std::string_view kKernelName = "fixture-euler";
/// @brief The export format this fixture registers.
inline constexpr std::string_view kFormatName = "fixture-text";
/// @brief The extension that format answers to, without a dot.
inline constexpr std::string_view kFormatExtension = "fixture";
/// @brief The node type the dependent fixture registers, only once its dependency is mounted.
inline constexpr std::string_view kNeedyNodeType = "fixture.needy";

/// @brief The manifest id of the plugin that registers everything above.
inline constexpr const char* kContentId = "org.qp.fixture.host.content";
/// @brief The manifest id of the plugin that refuses from inside `register_into`.
inline constexpr const char* kRefusesId = "org.qp.fixture.host.refuses";
/// @brief The manifest id of the plugin whose manifest declares a capability the test host does not grant.
inline constexpr const char* kHungryId = "org.qp.fixture.host.hungry";
/// @brief The manifest id of the plugin whose manifest is malformed: it declares nothing at all.
inline constexpr const char* kEmptyId = "org.qp.fixture.host.empty";
/// @brief The manifest id of the plugin that depends on `kContentId`.
inline constexpr const char* kNeedyId = "org.qp.fixture.host.needy";
/// @brief The node type the refusing fixture installs before it refuses, so a rollback has something to undo.
inline constexpr std::string_view kRefusesHalfType = "fixture.refuses.half";

/// @brief The load-order marker a plugin appends to, so install order is observable.
///
/// Lives in the **host's** memory and is reached through `host_context`, which the host passes through
/// unchanged. Nothing whose lifetime matters crosses the boundary: the plugin writes an integer and keeps no
/// state of its own.
struct Recording final {
    /// Filled by a plugin that appends its own id, so a test can read the order plugins installed in.
    const char** order = nullptr;
    std::int32_t capacity = 0;
    std::int32_t count = 0;

    void note(const char* id) noexcept {
        if (order == nullptr || count >= capacity) return;
        order[count++] = id;
    }
};

/**
 * @brief A kernel that does nothing, so a kernel can be registered without an integrator.
 *
 * `advance` copies its input to its output unchanged. That is not a physics scheme and is not pretending to
 * be one: this fixture exists to prove that a kernel **reaches the registry**, and a real scheme would add
 * arithmetic that no assertion here reads.
 */
class IdentityKernel final : public graph::kernels::IBatchAdvancer {
public:
    [[nodiscard]] std::string_view name() const noexcept override { return kKernelName; }
    [[nodiscard]] graph::kernels::Capability capabilities() const noexcept override {
        return graph::kernels::Capability::none;
    }
    [[nodiscard]] diag::Result<void> prepare(const graph::kernels::ParamBlock& /*params*/) override {
        return {};
    }
    [[nodiscard]] diag::Result<void> advance(const graph::kernels::BatchView& batch,
                                             graph::kernels::AdvanceContext& /*ctx*/) override {
        if (!batch.valid()) return diag::ErrorCode::invalid_argument;
        for (std::size_t i = 0; i < batch.count; ++i) batch.out[i] = batch.in[i];
        return {};
    }
};

}  // namespace qp::test::host_fixture
