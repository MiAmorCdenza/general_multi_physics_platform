/**
 * @file needy.cpp
 * @brief A plugin that depends on another one and refuses to install if the dependency is not mounted.
 *
 * A manifest declares dependency **ids**, and the loader orders a set by them -- but ordering is not presence.
 * A plugin whose dependency failed elsewhere is asked to install into a host that is missing it, and its own
 * registration would then produce a graph that runs and is wrong. So this fixture asks the host what is
 * mounted, which is the question only the host can answer, and that check is what
 * `host.mounts_a_dependent_after_its_dependency` asserts: it is green only if the dependency installed first.
 */
#include "host_plugin_fixture.hpp"

#include <qp/abi/abi_version.hpp>
#include <qp/host/host.hpp>
#include <qp/plugin/loader.hpp>
#include <qp/ports/port_type.hpp>

#include <algorithm>
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

using qp::test::host_fixture::kContentId;
using qp::test::host_fixture::kNeedyId;
using qp::test::host_fixture::kNeedyNodeType;

qp::diag::ErrorCode do_register(void* host_context) {
    if (host_context == nullptr) return qp::diag::ErrorCode::plugin_load_failed;
    auto& host = *static_cast<qp::host::IPluginHost*>(host_context);

    const std::vector<std::string_view> mounted = host.mounted_plugins();
    if (std::find(mounted.begin(), mounted.end(), std::string_view{kContentId}) == mounted.end()) {
        return host.refuse("its dependency was not mounted before it, so it has nothing to extend");
    }

    qp::graph::NodeDesc desc;
    desc.type_name = std::string{kNeedyNodeType};
    desc.label = "Needy Fixture";
    desc.category = "fixture";
    desc.source = std::string{host.mounting_plugin()};

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

    if (host.add_node_type(std::move(desc)) != qp::diag::ErrorCode::ok) {
        return host.refuse("its node type was refused");
    }
    return qp::diag::ErrorCode::ok;
}

void do_unregister(void* host_context) { (void)host_context; }

// One dependency, named. `load_order` sees this and installs `content` first whatever the file order is.
const char* const kDependencies[] = {kContentId};

// Declares `view_items`, which `content` does not. The host grants it, and the two plugins therefore offer
// **disjoint** capability ids -- which matters, because `authoring::Registry` refuses a second provider for an
// id it already serves. A dependent that declared the same bits as its dependency would be refused for a
// reason that has nothing to do with ordering, and this fixture exists to test ordering.
const qp::plugin::PluginManifestC kManifest{kNeedyId, "Host Fixture Needy", 1, 0, 0,
                                            qp::abi::kFieldBufferLayout,
                                            static_cast<std::uint32_t>(qp::plugin::Capability::view_items),
                                            1, kDependencies};

const qp::plugin::PluginExports kExports{qp::plugin::kExportsTag, qp::plugin::kExportsVersion, 0,
                                         &kManifest, do_register, do_unregister};

}  // namespace

QP_EXPORT const void* qp_plugin_entry() { return &kExports; }
