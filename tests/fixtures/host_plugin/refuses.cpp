/**
 * @file refuses.cpp
 * @brief A plugin whose `register_into` refuses, and says why in its own words.
 *
 * The host cannot invent this sentence. A plugin refuses for reasons only it knows, and a host that reported a
 * bare `plugin_fault` would leave the user reading a code that names the symptom and not the cause. This
 * fixture is the reason `IPluginHost::refuse` exists rather than a comment about error codes.
 *
 * It installs one node type **before** refusing, because the other half of the contract is that nothing is left
 * behind: a refusal with a partial installation is the state the ledger exists to undo.
 */
#include "host_plugin_fixture.hpp"

#include <qp/abi/abi_version.hpp>
#include <qp/host/host.hpp>
#include <qp/plugin/loader.hpp>
#include <qp/ports/port_type.hpp>

#include <cstdint>
#include <string>
#include <string_view>

#if defined(_WIN32)
#  define QP_EXPORT extern "C" __declspec(dllexport)
#else
#  define QP_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace {

using qp::test::host_fixture::kRefusesHalfType;
using qp::test::host_fixture::kRefusesId;

/// @brief The sentence the test asserts on. Spelled once, so the test and the plugin cannot disagree.
constexpr const char* kReason = "the fixture's bench supply is not connected";

qp::diag::ErrorCode do_register(void* host_context) {
    if (host_context == nullptr) return qp::diag::ErrorCode::plugin_load_failed;
    auto& host = *static_cast<qp::host::IPluginHost*>(host_context);

    qp::graph::NodeDesc desc;
    desc.type_name = std::string{kRefusesHalfType};
    desc.label = "Half Installed";
    desc.category = "fixture";
    desc.source = std::string{host.mounting_plugin()};

    qp::graph::PortDesc out;
    out.number = 1;
    out.name = "value";
    out.connectable = true;
    out.type = qp::ports::kScalarF64;
    desc.outputs.push_back(out);

    if (host.add_node_type(std::move(desc)) != qp::diag::ErrorCode::ok) {
        return host.refuse("its node type was refused before it could refuse");
    }
    return host.refuse(kReason);
}

void do_unregister(void* host_context) { (void)host_context; }

const qp::plugin::PluginManifestC kManifest{kRefusesId, "Host Fixture Refuses", 1, 0, 0,
                                            qp::abi::kFieldBufferLayout,
                                            static_cast<std::uint32_t>(qp::plugin::Capability::node_types),
                                            0, nullptr};

const qp::plugin::PluginExports kExports{qp::plugin::kExportsTag, qp::plugin::kExportsVersion, 0,
                                         &kManifest, do_register, do_unregister};

}  // namespace

QP_EXPORT const void* qp_plugin_entry() { return &kExports; }
