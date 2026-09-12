/**
 * @file content.cpp
 * @brief A plugin that really installs content into the host's registries.
 *
 * Everything the loader's fixtures deliberately avoid: it calls back into the host, registers two node types, a
 * kernel and an export format, and withdraws all four from `unregister`. The host's job is to record what
 * arrived and to be able to take it back, and neither property can be observed from a plugin that registers
 * nothing.
 *
 * `unregister` removes **nothing**, which is the strongest available form of the same statement: the host
 * records every successful `add_*` and withdraws the record itself, so a catalog still holding this plugin's
 * node types after the library is released would be the evidence that the record is what makes unloading exact.
 * A fixture that cleaned up perfectly would hide the ledger, and the ledger is the thing under test.
 */
#include "host_plugin_fixture.hpp"

#include <qp/host/host.hpp>

#include <qp/abi/abi_version.hpp>
#include <qp/graph/ir/ids.hpp>
#include <qp/plugin/loader.hpp>
#include <qp/ports/port_type.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#  define QP_EXPORT extern "C" __declspec(dllexport)
#else
#  define QP_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace {

using qp::test::host_fixture::IdentityKernel;
using qp::test::host_fixture::kContentId;
using qp::test::host_fixture::kFormatExtension;
using qp::test::host_fixture::kFormatName;
using qp::test::host_fixture::kKernelName;
using qp::test::host_fixture::kNodeType;
using qp::test::host_fixture::kSecondNodeType;

/// @brief The exporter this fixture contributes. It writes nothing, which is the point: registration and
/// withdrawal are what is under test, and a writer would add code no assertion reads.
class TextExporter final : public qp::runtime::IExporter {
public:
    [[nodiscard]] const qp::runtime::FormatDesc& format() const noexcept override { return desc_; }

    [[nodiscard]] qp::runtime::ExportRefusal write(
        const qp::runtime::ExportRequest& /*request*/) noexcept override {
        return qp::runtime::ExportRefusal::ok;
    }

    [[nodiscard]] bool is_available() const noexcept override { return true; }

private:
    qp::runtime::FormatDesc desc_{std::string{kFormatName}, "Fixture text",
                                  std::vector<std::string>{std::string{kFormatExtension}}, {}};
};

/// @brief The `source` field a plugin is supposed to fill in for its own node types.
const char* const kSource = kContentId;

/// @brief One node type with an input and an output, so it is a plausible description rather than a stub.
[[nodiscard]] qp::graph::NodeDesc node_type(std::string_view name, std::string_view label) {
    qp::graph::NodeDesc desc;
    desc.type_name = std::string{name};
    desc.label = std::string{label};
    desc.description = "Registered by a host fixture.";
    desc.category = "fixture";
    desc.source = kSource;

    qp::graph::PortDesc in;
    in.number = 1;
    in.name = "value";
    in.connectable = true;
    in.required = true;
    in.type = qp::ports::kScalarF64;
    desc.inputs.push_back(in);

    qp::graph::PortDesc out;
    out.number = 1;
    out.name = "value";
    out.connectable = true;
    out.type = qp::ports::kScalarF64;
    desc.outputs.push_back(out);
    return desc;
}

IdentityKernel g_kernel{};
TextExporter g_exporter{};

qp::diag::ErrorCode do_register(void* host_context) {
    if (host_context == nullptr) return qp::diag::ErrorCode::plugin_load_failed;
    auto& host = *static_cast<qp::host::IPluginHost*>(host_context);

    // The id comes from the host rather than from a literal here: a plugin that guessed its own id would put a
    // wrong attribution in every node's `source`, and nothing downstream could tell.
    const std::string_view mine = host.mounting_plugin();
    if (mine.empty()) return qp::diag::ErrorCode::plugin_load_failed;

    if (host.add_node_type(node_type(kNodeType, "Fixture Spinner")) != qp::diag::ErrorCode::ok) {
        return host.refuse("its first node type was refused");
    }
    if (host.add_node_type(node_type(kSecondNodeType, "Fixture Meter")) != qp::diag::ErrorCode::ok) {
        return host.refuse("its second node type was refused");
    }

    qp::graph::kernels::KernelDesc kernel;
    kernel.name = kKernelName;
    kernel.summary = "Copies the batch unchanged; a host fixture, not a scheme.";
    kernel.impl = &g_kernel;
    if (host.add_kernel(kernel) != qp::diag::ErrorCode::ok) {
        return host.refuse("its kernel was refused");
    }

    if (host.add_exporter(&g_exporter) != qp::diag::ErrorCode::ok) {
        return host.refuse("its export format was refused");
    }
    return qp::diag::ErrorCode::ok;
}

void do_unregister(void* host_context) {
    // Deliberately empty, and the argument is checked so the parameter is not unused: the host's own record is
    // supposed to remove everything this plugin registered.
    (void)host_context;
}

const qp::plugin::PluginManifestC kManifest{kContentId, "Host Fixture Content", 1, 0, 0,
                                            qp::abi::kFieldBufferLayout,
                                            static_cast<std::uint32_t>(qp::plugin::Capability::node_types) |
                                                static_cast<std::uint32_t>(qp::plugin::Capability::kernels) |
                                                static_cast<std::uint32_t>(qp::plugin::Capability::file_io),
                                            0, nullptr};

const qp::plugin::PluginExports kExports{qp::plugin::kExportsTag, qp::plugin::kExportsVersion, 0,
                                         &kManifest, do_register, do_unregister};

}  // namespace

QP_EXPORT const void* qp_plugin_entry() { return &kExports; }
