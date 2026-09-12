/**
 * @file mechanics_binder.cpp
 * @brief Reads a spring-damper node's parameters and builds the RK4 operator for them.
 *
 * Three refusals live here, and each one is a case where running anyway would produce a plausible
 * wrong answer rather than a failure:
 *
 *   - **missing `k` or `m`** -- there is no frequency to integrate against. Substituting a default
 *     would integrate a different system than the graph describes.
 *   - **`c != 0`** -- the kernel has no damping term. See the header for why this is refused rather
 *     than ignored.
 *   - **an integrator other than `rk4`** -- the others have no implementation, and the user chose
 *     them for a reason.
 */
#include <qp/plugins/mechanics/mechanics_binder.hpp>

#include <qp/plugins/mechanics/batch_operator.hpp>
#include <qp/plugins/mechanics/rk4_oscillator.hpp>

#include <cmath>
#include <cstdint>
#include <memory>
#include <string>

namespace qp::plugins::mechanics {
namespace {

namespace execution = graph::execution;
namespace kernels = graph::kernels;

/// Port numbers of the spring-damper node's declarations, from `views/model/demo_library.cpp`.
///
/// Hard-coded here rather than looked up by name because these are the **node type's** published
/// numbers: `PortDesc::number` is documented as stable ("never rename later") and the YAML key, the
/// script name and this binder all address the same parameter through it. A name lookup would move
/// the failure from compile time to run time for no gain.
constexpr qp::graph::PortNumber kStiffness = 2;
constexpr qp::graph::PortNumber kDamping = 3;
constexpr qp::graph::PortNumber kMass = 4;
constexpr qp::graph::PortNumber kIntegrator = 5;

/// Index of the `rk4` choice in the node's integrator enum, from the same declaration.
constexpr std::int64_t kRk4Choice = 1;

}  // namespace

bool MechanicsBinder::can_bind(std::string_view type_name,
                               const execution::StateView& layout) const noexcept {
    return type_name == kSpringDamperType && layout.components_per_particle == kComponents;
}

std::unique_ptr<execution::IStateOperator> MechanicsBinder::bind(
    std::string_view type_name, const qp::graph::Node& node, const execution::StateView& layout) {
    // "Not mine" for a different type, and for a layout this family cannot describe. Both are
    // ordinary answers from a binder that was consulted about something else -- not errors.
    if (type_name != kSpringDamperType) return nullptr;
    if (layout.components_per_particle != kComponents) return nullptr;

    // `k` and `m` are required for a frequency. An unset parameter reads as an invalid `Value`, and
    // `to_double()` on an invalid value is 0.0 -- so the check is on the value being numeric rather
    // than on the result being non-zero, or a legitimate `k = 0` would be indistinguishable from
    // "unset". A stiffness of zero is a free particle, which the kernel refuses on its own terms.
    const qp::ports::Value stiffness = node.param(kStiffness);
    const qp::ports::Value mass = node.param(kMass);
    if (!stiffness.is_numeric() || !mass.is_numeric()) return nullptr;

    const double k = stiffness.to_double();
    const double m = mass.to_double();
    if (!(m > 0.0)) return nullptr;  // no frequency exists for a non-positive mass

    // Damping is refused rather than dropped. See the header.
    const qp::ports::Value damping = node.param(kDamping);
    if (damping.is_numeric() && damping.to_double() != 0.0) return nullptr;

    // Only the integrator with an implementation and a golden case behind it. The node offers three;
    // an unset choice reads as 0 (`euler`), which is also refused -- the user has not chosen yet, and
    // running the default would silently pick for them.
    const qp::ports::Value integrator = node.param(kIntegrator);
    if (!integrator.is_numeric() || integrator.as_i64() != kRk4Choice) return nullptr;

    const double omega = std::sqrt(k / m);
    // A frequency of zero or a non-finite one cannot be integrated, and the kernel refuses it -- but
    // refusing here returns "not mine", which would make the window report that nothing can run this
    // node type. Returning null for a node this binder *does* own would be the misleading answer, so
    // the checks above are the ones that may decline and this one may not: an unintegratable
    // frequency reaches the kernel, which reports its own code.
    kernels::ParamBlock params;
    params.set_real(0, omega);

    auto advancer = std::make_unique<Rk4Oscillator>();
    // Prepared here, before the adapter wraps it, so the plugin's own error code travels back to the
    // caller instead of being flattened into "no binder claimed this node".
    const diag::Result<void> prepared = advancer->prepare(params);
    if (!prepared.has_value()) return nullptr;
    return BatchOperator::make(std::move(advancer));
}

}  // namespace qp::plugins::mechanics
