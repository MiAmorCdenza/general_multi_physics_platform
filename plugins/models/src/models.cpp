/**
 * @file models.cpp
 * @brief One RK4, three right-hand sides, and the claims each model makes about itself.
 */
#include <qp/plugins/models/models.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>

namespace qp::plugins::models {
namespace {

namespace ex = qp::graph::execution;

/// @brief The claims the three models share, with the two that differ left to each caller.
///
/// Dimensionally consistent and independent of other instances: both are properties of the equations written
/// below, and neither depends on how many of them are running. Purity and reversibility are arguments because
/// they are exactly where the models differ, and writing them out at each call site is what makes that visible.
[[nodiscard]] ex::SimModelDesc claims(bool is_pure, bool time_reversible) noexcept {
    ex::SimModelDesc desc;
    desc.is_pure = is_pure;
    desc.time_reversible = time_reversible;
    desc.is_dimensionally_consistent = true;
    desc.is_independent_of_other_instances = true;
    return desc;
}

}  // namespace

Rk4Model::Rk4Model(std::string_view name, std::size_t dimension, Derivative derivative,
                   ex::SimModelDesc desc) noexcept
    : name_(name),
      dimension_(dimension == 0 || dimension > kMaximumDimension ? kMaximumDimension : dimension),
      derivative_(std::move(derivative)),
      desc_(desc) {}

diag::Result<void> Rk4Model::step(ex::StateView& state, double dt) {
    // The step-size contract, and the two refusals are different mistakes. A zero step would append a sample that
    // duplicates the previous one and advance nothing, so a trace would grow while the physics stood still. A
    // non-finite step is not a step at all. Negative is **legal**, because a round trip is how the reversibility
    // claim above is checked and a claim nothing can falsify is a comment.
    if (!std::isfinite(dt)) return diag::ErrorCode::invalid_argument;
    if (dt == 0.0) return diag::ErrorCode::invalid_argument;
    if (!state.is_consistent()) return diag::ErrorCode::invalid_argument;

    // Fixed-size buffers, and that is a contract rather than a micro-optimisation: `step` says it allocates
    // nothing, and four heap vectors once per step is the allocation the contract forbids. `kMaximumDimension`
    // is the price, and the header states it.
    constexpr std::size_t kMax = kMaximumDimension;
    const std::size_t c = dimension_;
    std::array<double, kMax> y{};
    std::array<double, kMax> k1{};
    std::array<double, kMax> k2{};
    std::array<double, kMax> k3{};
    std::array<double, kMax> k4{};

    for (std::size_t i = 0; i < state.count; ++i) {
        for (std::size_t d = 0; d < c; ++d) y[d] = state.at(i, d);

        derivative_(0.0, y.data(), k1.data());
        for (std::size_t d = 0; d < c; ++d) y[d] = state.at(i, d) + 0.5 * dt * k1[d];
        derivative_(0.0, y.data(), k2.data());
        for (std::size_t d = 0; d < c; ++d) y[d] = state.at(i, d) + 0.5 * dt * k2[d];
        derivative_(0.0, y.data(), k3.data());
        for (std::size_t d = 0; d < c; ++d) y[d] = state.at(i, d) + dt * k3[d];
        derivative_(0.0, y.data(), k4.data());

        for (std::size_t d = 0; d < c; ++d) {
            const double next = state.at(i, d) + (dt / 6.0) * (k1[d] + 2.0 * k2[d] + 2.0 * k3[d] + k4[d]);
            if (!std::isfinite(next)) return diag::ErrorCode::invalid_argument;
            state.set(i, d, next);
        }
    }

    // The model's own constraint, if it has one -- a floor, for the projectile. Applied to the whole state after
    // every particle has been advanced, so a constraint that couples particles (a collision, say) can see them
    // all, and applied **inside** the step so the run's samples never show a state the model forbids.
    if (post_) post_(state);
    return {};
}

std::unique_ptr<ex::IStateOperator> make_damped_oscillator(double omega, double gamma) {
    // The derivative captures the parameters by value. A `const`-captured lambda would do; the point is that the
    // operator owns its numbers and reads nothing else, which is what makes two instances independent.
    const double w = omega;
    const double g = gamma;
    auto derivative = [w, g](double /*t*/, const double* y, double* dydt) {
        // y = {x, x'}. The frequency is a parameter rather than a state component here -- unlike the mechanics
        // kernel's layout, which carries it per particle -- because this model's frequency is fixed at binding
        // time and a state slot nothing writes is a state slot that will eventually be written by mistake.
        dydt[0] = y[1];
        dydt[1] = -(w * w) * y[0] - g * y[1];
    };
    // Pure and time-reversible: both terms are linear in the state, so no information is lost and running the
    // clock backwards retraces the trajectory.
    return std::make_unique<Rk4Model>("model.damped_oscillator.rk4", kOscillatorComponents,
                                      std::move(derivative), claims(/*is_pure=*/true,
                                                                    /*time_reversible=*/true));
}

std::unique_ptr<ex::IStateOperator> make_pendulum(double gravity, double length) {
    const double g = gravity;
    const double l = length;
    auto derivative = [g, l](double /*t*/, const double* y, double* dydt) {
        // y = {th, th'}. The **sine** and not the angle: see the header for why the difference is the lesson.
        dydt[0] = y[1];
        dydt[1] = -(g / l) * std::sin(y[0]);
    };
    return std::make_unique<Rk4Model>("model.pendulum.rk4", kPendulumComponents, std::move(derivative),
                                      claims(/*is_pure=*/true, /*time_reversible=*/true));
}

std::unique_ptr<ex::IStateOperator> make_projectile(double gravity, double drag) {
    const double g = gravity;
    const double k = drag;
    auto derivative = [g, k](double /*t*/, const double* y, double* dydt) {
        // y = {x, y, vx, vy}. Linear drag: `a = -k v`, plus gravity on the vertical component only.
        dydt[0] = y[2];
        dydt[1] = y[3];
        dydt[2] = -k * y[2];
        dydt[3] = -g - k * y[3];
    };
    auto model = std::make_unique<Rk4Model>("model.projectile.rk4", kProjectileComponents,
                                            std::move(derivative),
                                            claims(/*is_pure=*/true, /*time_reversible=*/false));
    // **Not time-reversible, and this is the interesting declaration of the three.** The equations themselves are
    // as linear as the oscillator's -- `dvy/dt = -g - k vy` has no preferred direction in time -- so a reading of
    // the equations alone would say "reversible". What is not reversible is the model's own definition, below:
    // it stops the projectile at the ground. Running the flight backwards from a projectile at rest on the floor
    // does not produce a launch. A declaration is about **the model as stated**, not about the differential
    // equation in isolation, and this is the sort of thing a plugin author gets wrong.
    //
    // **The floor.** Without it a projectile falls through the ground and keeps going, producing a trajectory that
    // looks like a plot of something real. The constraint is part of the model because "it lands" is part of what
    // the model says.
    model->set_post_step([](ex::StateView& state) {
        for (std::size_t i = 0; i < state.count; ++i) {
            if (state.at(i, 1) > 0.0) continue;
            // Position on the ground, velocity zero, in both components: a projectile that has landed does not
            // slide. The horizontal component is zeroed deliberately -- a bouncing or sliding projectile is a
            // different model, and leaving `vx` alone would make this one a projectile that lands and then keeps
            // travelling, which is neither.
            state.set(i, 1, 0.0);
            state.set(i, 2, 0.0);
            state.set(i, 3, 0.0);
        }
    });
    return model;
}

}  // namespace qp::plugins::models
