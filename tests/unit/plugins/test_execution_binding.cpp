/**
 * @file test_execution_binding.cpp
 * @brief Which node becomes which operator, and what is refused.
 *
 * ## Why this half needs the plugin and the other half does not
 *
 * `tests/unit/graph/test_execution.cpp` drives the loop with a stub operator and asks whether it
 * records the initial condition, starts a second run clean, and refuses a bad step. None of that
 * depends on physics.
 *
 * These cases are the opposite: they ask whether a `demo.spring_damper` node with particular
 * parameters produces a runnable operator or an honest refusal, which is knowledge the plugin owns.
 * The two files were one until the contract gate pointed out that a plugin header's `@tests` cannot
 * reach a case filed under `unit/graph/`.
 *
 * ## What is actually being tested
 *
 * The refusals matter more than the acceptance. A graph with damping has no operator here, and a
 * graph choosing `verlet` has no operator here. Running either anyway would produce a
 * plausible-looking wrong answer -- a decaying curve with the wrong decay, a symplectic-looking
 * trajectory that is dissipative -- and the whole platform exists to make that outcome impossible
 * rather than merely unlikely.
 */
#include <catch2/catch_test_macros.hpp>

#include <qp/graph/execution/execution.hpp>

#include <qp/plugins/mechanics/mechanics_binder.hpp>

#include <qp/diag/result.hpp>
#include <qp/graph/ir.hpp>

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace {

namespace execution = qp::graph::execution;
namespace rt = qp::runtime;
using qp::graph::Node;

/// @brief A spring-damper node with the parameters a run needs.
///
/// Built through `Node::set_param` rather than by writing `params` directly, so the helper exercises
/// the same path a property-panel edit takes and cannot produce a node shape the real one never has.
[[nodiscard]] Node spring_damper(double k, double m, double c = 0.0, std::int64_t integrator = 1) {
    Node node;
    node.id = qp::graph::NodeId{1, 1};
    node.type_name = qp::plugins::mechanics::MechanicsBinder::kSpringDamperType;
    node.set_param(2, qp::ports::Value{k});           // stiffness
    node.set_param(3, qp::ports::Value{c});           // damping
    node.set_param(4, qp::ports::Value{m});           // mass
    node.set_param(5, qp::ports::Value{integrator});  // integrator choice
    return node;
}

/// @brief The binder list. A function rather than a global so no case can leave state behind.
[[nodiscard]] std::vector<execution::IOperatorBinder*> binders() {
    static qp::plugins::mechanics::MechanicsBinder binder;
    return {&binder};
}

}  // namespace

TEST_CASE("execution.binding.binder_comes_from_the_plugin", "[execution]") {
    // The binder ships with the operator, so the run loop has no idea which node types exist. Two
    // consequences are asserted: an unknown type is refused with `not_implemented` rather than a
    // generic failure, and the operator's name travels from the kernel to the caller so a report can
    // say which method produced the numbers.
    execution::GraphRun run;
    REQUIRE(run.prepare(spring_damper(12.0, 0.5), binders(), execution::StateView::zeroed(1), rt::RunId{1}).has_value());
    REQUIRE(run.is_ready());
    REQUIRE(run.operator_name() == "rk4_oscillator");

    {
        Node unknown;
        unknown.id = qp::graph::NodeId{2, 1};
        unknown.type_name = "demo.something_else";
        execution::GraphRun other;
        const auto prepared = other.prepare(unknown, binders(), execution::StateView::zeroed(1), rt::RunId{1});
        REQUIRE_FALSE(prepared.has_value());
        REQUIRE(prepared.error() == qp::diag::ErrorCode::not_implemented);
        REQUIRE_FALSE(other.is_ready());
    }

    // A layout this family cannot describe is declined, not mis-described. The binder is told the
    // layout precisely so it can say no instead of reading the wrong component.
    {
        Node odd;
        odd.id = qp::graph::NodeId{3, 1};
        odd.type_name = qp::plugins::mechanics::MechanicsBinder::kSpringDamperType;
        odd.set_param(2, qp::ports::Value{12.0});
        odd.set_param(4, qp::ports::Value{0.5});
        odd.set_param(5, qp::ports::Value{std::int64_t{1}});

        // Four components per particle is legal for `StateView` and not describable by this adapter.
        // A direct call to `bind` is the only way to reach it, because `GraphRun` builds the
        // three-component layout itself -- which is exactly why the binder checks rather than assumes.
        qp::plugins::mechanics::MechanicsBinder binder;
        const execution::StateView four = execution::StateView::zeroed(1, 4);
        REQUIRE(binder.bind(odd.type_name, odd, four) == nullptr);
    }
}

TEST_CASE("execution.binding.missing_parameter_is_refused", "[execution]") {
    // `k` and `m` are required for a frequency. An unset parameter reads as an invalid `Value`, and the
    // check is on the value being **numeric** rather than on the result being non-zero -- otherwise a
    // legitimate `k = 0` would be indistinguishable from "the user has not filled this in yet".
    {
        Node missing_k;
        missing_k.type_name = qp::plugins::mechanics::MechanicsBinder::kSpringDamperType;
        missing_k.set_param(4, qp::ports::Value{0.5});
        missing_k.set_param(5, qp::ports::Value{std::int64_t{1}});
        execution::GraphRun run;
        REQUIRE_FALSE(run.prepare(missing_k, binders(), execution::StateView::zeroed(1), rt::RunId{1}).has_value());
    }
    {
        Node missing_m;
        missing_m.type_name = qp::plugins::mechanics::MechanicsBinder::kSpringDamperType;
        missing_m.set_param(2, qp::ports::Value{12.0});
        missing_m.set_param(5, qp::ports::Value{std::int64_t{1}});
        execution::GraphRun run;
        REQUIRE_FALSE(run.prepare(missing_m, binders(), execution::StateView::zeroed(1), rt::RunId{1}).has_value());
    }

    // A non-positive mass has no frequency. Refused rather than producing a NaN frequency that would
    // surface later as a diverged run with no explanation of why.
    {
        execution::GraphRun zero;
        REQUIRE_FALSE(zero.prepare(spring_damper(12.0, 0.0), binders(), execution::StateView::zeroed(1), rt::RunId{1}).has_value());
        execution::GraphRun negative;
        REQUIRE_FALSE(negative.prepare(spring_damper(12.0, -1.0), binders(), execution::StateView::zeroed(1), rt::RunId{1})
                          .has_value());
    }

    // A stiffness of zero is a **free particle**, which the kernel refuses on its own terms rather than
    // the binder refusing it as "not mine". The distinction matters: the binder declining would make
    // the window report that nothing can run this node type, which is misleading about a node it owns.
    {
        execution::GraphRun free_particle;
        REQUIRE_FALSE(free_particle.prepare(spring_damper(0.0, 0.5), binders(), execution::StateView::zeroed(1), rt::RunId{1})
                          .has_value());
    }
}

TEST_CASE("execution.binding.damping_is_refused", "[execution]") {
    // The refusal that matters most. The kernel integrates `x'' = -omega^2 x` and has no damping term,
    // so a node saying `c = 0.5` cannot be honoured. Running it anyway would produce a decaying-looking
    // result with the wrong decay -- plausible, wrong, and indistinguishable from correct to anyone
    // reading a plot. That is the failure mode this platform exists to prevent, so the honest answer is
    // that no operator provides it yet.
    execution::GraphRun damped;
    const auto prepared = damped.prepare(spring_damper(12.0, 0.5, /*c=*/0.5), binders(), execution::StateView::zeroed(1), rt::RunId{1});
    REQUIRE_FALSE(prepared.has_value());
    REQUIRE(prepared.error() == qp::diag::ErrorCode::not_implemented);
    REQUIRE_FALSE(damped.is_ready());

    // Undamped is accepted, so the refusal is about damping and not about the fixture.
    execution::GraphRun undamped;
    REQUIRE(undamped.prepare(spring_damper(12.0, 0.5, 0.0), binders(), execution::StateView::zeroed(1), rt::RunId{1}).has_value());
}

TEST_CASE("execution.binding.other_integrators_are_refused", "[execution]") {
    // The node offers `euler`, `rk4`, `verlet`; only `rk4` has an implementation and a bit-exact golden
    // case. Running RK4 for a user who chose `verlet` would be the same silent substitution as ignoring
    // damping, one level up: a symplectic integrator is chosen precisely to get different behaviour,
    // and handing back a dissipative one contradicts the choice.
    for (const std::int64_t choice : {std::int64_t{0}, std::int64_t{2}}) {
        execution::GraphRun run;
        REQUIRE_FALSE(run.prepare(spring_damper(12.0, 0.5, 0.0, choice), binders(), execution::StateView::zeroed(1), rt::RunId{1})
                          .has_value());
    }

    // An unset choice reads as 0 (`euler`) and is refused for the same reason: the user has not chosen,
    // and picking a default would silently decide which method produced the numbers.
    {
        Node unchosen;
        unchosen.type_name = qp::plugins::mechanics::MechanicsBinder::kSpringDamperType;
        unchosen.set_param(2, qp::ports::Value{12.0});
        unchosen.set_param(4, qp::ports::Value{0.5});
        execution::GraphRun run;
        REQUIRE_FALSE(run.prepare(unchosen, binders(), execution::StateView::zeroed(1), rt::RunId{1}).has_value());
    }
}

TEST_CASE("execution.binding.end_to_end_against_a_closed_form", "[execution]") {
    // The integration of all of the above: a node's parameters become an operator, the operator is
    // stepped, and the recorded trajectory matches the closed form. Checked here rather than in the
    // loop's file because the frequency comes from `sqrt(k/m)` -- which is the binding's arithmetic,
    // not the loop's.
    const double k = 12.0;
    const double m = 0.5;
    const double omega = std::sqrt(k / m);

    execution::GraphRun run;
    REQUIRE(run.prepare(spring_damper(k, m), binders(), execution::StateView::zeroed(1), rt::RunId{7}).has_value());
    REQUIRE(run.set_initial(0, 1.0, 0.0).has_value());
    run.set_omega(omega);

    // One period in 4096 steps, so the final time is exact and the comparison is about the integrator
    // rather than about accumulated step-size error.
    const double pi = 3.14159265358979323846;
    const double period = 2.0 * pi / omega;
    const std::size_t steps = 4096;
    const double dt = period / static_cast<double>(steps);

    const execution::RunOutcome outcome = run.run(steps, dt);
    REQUIRE(outcome.ok());
    REQUIRE(run.trace().size() == steps + 1);

    // After one period the oscillator has returned, to RK4's accuracy.
    REQUIRE(std::abs(run.trace().samples().back().values[0].value - 1.0) < 1.0e-9);

    // And a quarter period in, it is at zero with the expected velocity -- so the phase is right and
    // not merely the amplitude. `x(t) = cos(omega t)`, `v(t) = -omega sin(omega t)`.
    execution::GraphRun quarter;
    REQUIRE(quarter.prepare(spring_damper(k, m), binders(), execution::StateView::zeroed(1), rt::RunId{8}).has_value());
    REQUIRE(quarter.set_initial(0, 1.0, 0.0).has_value());
    quarter.set_omega(omega);
    const std::size_t q_steps = 1024;
    const double q_dt = (period / 4.0) / static_cast<double>(q_steps);
    REQUIRE(quarter.run(q_steps, q_dt).ok());

    REQUIRE(std::abs(quarter.trace().samples().back().values[0].value) < 1.0e-9);
    REQUIRE(std::abs(quarter.trace().samples().back().values[1].value + omega) < 1.0e-6);
}
