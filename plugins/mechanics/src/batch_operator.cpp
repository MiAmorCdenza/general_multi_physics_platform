/**
 * @file batch_operator.cpp
 * @brief The ABI bridge between a state buffer and a mechanics kernel.
 */
#include <qp/plugins/mechanics/batch_operator.hpp>

#include <qp/graph/field/field.hpp>

#include <cstdint>
#include <utility>

namespace qp::plugins::mechanics {
namespace {

namespace kernels = graph::kernels;
namespace field = graph::field;
namespace execution = graph::execution;

/// @brief Describes a state buffer as a lattice the ABI accepts.
///
/// The mechanics state is three doubles per particle, which is exactly `ComponentKind::vector`, so
/// the description is legal without conversion -- the kernel reads and writes the state's own
/// memory. Returned by value: it is 32 bytes of POD, and a reference would need a lifetime the
/// caller has no reason to manage.
[[nodiscard]] qp::abi::LatticeDesc describe(const execution::StateView& state) noexcept {
    qp::abi::LatticeDesc desc{};
    desc.kind = qp::abi::LatticeKind::line;
    desc.component = qp::abi::ComponentKind::vector;
    desc.element = qp::abi::ElementType::f64;
    desc.count[0] = static_cast<std::uint32_t>(state.count);
    // Taken from the ABI's own arithmetic rather than multiplied out here, so this cannot drift from
    // what `is_consistent` checks.
    desc.spacing_bytes = qp::abi::expected_spacing(desc);
    return desc;
}

}  // namespace

BatchOperator::BatchOperator(std::unique_ptr<kernels::IBatchAdvancer> advancer,
                             std::string name) noexcept
    : advancer_(std::move(advancer)), name_(std::move(name)) {}

std::unique_ptr<execution::IStateOperator> BatchOperator::make(
    std::unique_ptr<kernels::IBatchAdvancer> advancer) {
    if (advancer == nullptr) return nullptr;
    const std::string name{advancer->name()};
    return std::unique_ptr<execution::IStateOperator>{new BatchOperator{std::move(advancer), name}};
}

std::string_view BatchOperator::name() const noexcept { return name_; }

diag::Result<void> BatchOperator::step(execution::StateView& state, double dt) {
    if (!state.is_consistent()) return diag::ErrorCode::invalid_argument;
    // A layout this adapter cannot describe is refused rather than described wrongly. The ABI admits
    // component counts of 1 and 3, and building a description for anything else would produce one
    // `abi::is_consistent` rejects -- so the failure would surface later, in the kernel, as a
    // type mismatch that names the batch rather than the caller who chose the layout.
    if (state.components_per_particle != kSupportedComponents) return diag::ErrorCode::type_mismatch;
    if (!(dt > 0.0)) return diag::ErrorCode::invalid_argument;

    field::FieldValue view;
    view.desc = describe(state);
    // Writable through a const pointer, which is what `FieldValue` offers: a field is normally
    // something a reader samples, and the batch contract makes the output side writable. The cast is
    // confined to this one place so there is exactly one site to audit, and it is reached only after
    // the layout checks above.
    view.data = state.values.data();
    view.bytes = state.values.size() * sizeof(double);

    kernels::BatchView batch;
    batch.in = &view;
    batch.out = &view;  // aliased on purpose; see the header
    batch.count = state.count;

    kernels::AdvanceContext ctx;
    ctx.dt = dt;
    ctx.step = step_index_;

    const diag::Result<void> advanced = advancer_->advance(batch, ctx);
    if (!advanced.has_value()) return advanced.error();
    // The host advances the step counter, as the kernel contract requires: two operators in one plan
    // must agree on what step it is, and an operator that incremented the host's counter would make
    // the value depend on how many operators ran before it.
    ++step_index_;
    return {};
}

}  // namespace qp::plugins::mechanics
