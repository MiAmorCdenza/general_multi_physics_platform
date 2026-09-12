/**
 * @file execution.cpp
 * @brief The run loop: bind an operator to a node, step it, record what came out.
 *
 * No physics here. The loop knows how to walk a state forward and how to write a trace; the
 * acceleration is the operator's business, and which operator is the plugin's.
 */
#include <qp/graph/execution/execution.hpp>

#include <qp/units/dimensions.hpp>

#include <cassert>
#include <string>
#include <utility>

namespace qp::graph::execution {
namespace {

namespace rt = qp::runtime;

/// Component index of position within one particle.
constexpr std::size_t kPosition = 0;
/// Component index of velocity within one particle.
constexpr std::size_t kVelocity = 1;
/// Component index of the angular frequency the operator integrates against.
constexpr std::size_t kOmega = 2;

}  // namespace

StateView StateView::zeroed(std::size_t n, std::size_t c) {
    StateView out;
    out.count = n;
    out.components_per_particle = c == 0 ? 1 : c;
    out.values.assign(n * out.components_per_particle, 0.0);
    return out;
}

double StateView::at(std::size_t i, std::size_t component) const noexcept {
    if (i >= count || component >= components_per_particle) return 0.0;
    return values[i * components_per_particle + component];
}

void StateView::set(std::size_t i, std::size_t component, double v) noexcept {
    if (i >= count || component >= components_per_particle) return;
    values[i * components_per_particle + component] = v;
}

bool StateView::is_consistent() const noexcept {
    return components_per_particle > 0 && values.size() == count * components_per_particle;
}

diag::Result<void> GraphRun::prepare(const Node& node,
                                     const std::vector<IOperatorBinder*>& binders,
                                     rt::RunId run, std::size_t particles) {
    if (particles == 0) return diag::ErrorCode::invalid_argument;

    // The state exists before any binder is consulted, because a binder is handed the layout it must
    // be able to work with. Asking it to build an operator and *then* checking the layout would let
    // it read the wrong component while deciding.
    state_ = StateView::zeroed(particles);

    operator_.reset();
    operator_name_.clear();

    for (IOperatorBinder* binder : binders) {
        if (binder == nullptr) continue;
        std::unique_ptr<IStateOperator> candidate = binder->bind(node.type_name, node, state_);
        if (candidate == nullptr) continue;  // not this binder's node type
        operator_name_ = std::string{candidate->name()};
        operator_ = std::move(candidate);
        break;
    }
    if (operator_ == nullptr) {
        // `not_implemented` rather than a generic failure: the caller can name the type it could not
        // run, which is the difference between "the run failed" and "nothing can run demo.foo yet".
        return diag::ErrorCode::not_implemented;
    }

    // A fresh trace per run so a second run does not append to the first one's samples, and its channels
    // are declared **here** rather than in `set_run`: a bound loop is one that can record, and
    // `Trace::append` refuses a sample whose width does not match the channel count. Leaving the channels
    // to `set_run` would make a bound-but-unfiled loop look ready and then fail on its first sample.
    //
    // The identity may still be unknown, which is the point of the parameter's contract: a caller that
    // binds before it opens a ledger entry has none yet, and `set_run` re-files the trace under the real
    // one once it does.
    trace_ = rt::Trace{run};
    return declare_channels();
}

diag::Result<void> GraphRun::declare_channels() {
    const auto x_channel = trace_.add_channel(rt::Channel{kPositionChannel, qp::units::dims::length});
    if (!x_channel.has_value()) return x_channel.error();
    const auto v_channel =
        trace_.add_channel(rt::Channel{kVelocityChannel, qp::units::dims::velocity});
    if (!v_channel.has_value()) return v_channel.error();
    return {};
}

void GraphRun::set_run(rt::RunId run) {
    if (!run.valid()) return;  // cannot erase a good run's trace with an id nobody issued
    // A fresh trace per run, so a second run does not append to the first one's samples. Reassigned
    // rather than cleared because the run identity is fixed at construction.
    trace_ = rt::Trace{run};
    const auto declared = declare_channels();
    // The only way this fails is a duplicate channel name, which the fixed channel set above cannot
    // produce; asserting the invariant here keeps `set_run` noexcept for its callers.
    assert(declared.has_value() && "the fixed channel set cannot collide with itself");
    (void)declared;
}

diag::Result<void> GraphRun::set_initial(std::size_t particle, double position, double velocity) {
    if (particle >= state_.count) return diag::ErrorCode::out_of_range;
    state_.set(particle, kPosition, position);
    state_.set(particle, kVelocity, velocity);
    return {};
}

void GraphRun::set_omega(double omega) noexcept {
    for (std::size_t i = 0; i < state_.count; ++i) {
        state_.set(i, kOmega, omega);
    }
}

RunOutcome GraphRun::run(std::size_t steps, double dt) {
    RunOutcome out;
    out.operator_name = operator_name_;

    if (operator_ == nullptr) {
        out.error = diag::ErrorCode::invalid_argument;
        return out;
    }
    if (!(dt > 0.0) || !state_.is_consistent()) {
        out.error = diag::ErrorCode::invalid_argument;
        return out;
    }

    // Records one sample of every particle. Only particle 0 is traced: the trace's shape is one
    // channel per quantity, so several particles would need several channels and the naming would
    // become the caller's business. A multi-particle run is still stepped in full -- what is
    // recorded is the first particle, which is the one the panels show.
    const auto sample = [this](double t) -> diag::Result<void> {
        return trace_.append(t, {rt::UncertainValue::measured(state_.at(0, kPosition), 0.0,
                                                              qp::units::dims::length),
                                 rt::UncertainValue::measured(state_.at(0, kVelocity), 0.0,
                                                              qp::units::dims::velocity)});
    };

    // The initial condition is recorded before the first step, so a trace of `steps` steps holds
    // `steps + 1` samples. Without it the initial state would be unrecoverable from the record, and
    // the confidence panel measures its energy drift against exactly that state.
    if (const auto first = sample(0.0); !first.has_value()) {
        out.error = first.error();
        return out;
    }

    for (std::size_t i = 0; i < steps; ++i) {
        const diag::Result<void> stepped = operator_->step(state_, dt);
        if (!stepped.has_value()) {
            // The samples already taken are kept. A run that diverged at step 900 is *more*
            // informative than an empty trace: the confidence panel can show where it went wrong,
            // and discarding the evidence would leave the user with nothing to look at.
            out.error = stepped.error();
            out.steps = i;
            return out;
        }
        const double t = dt * static_cast<double>(i + 1);
        if (const auto appended = sample(t); !appended.has_value()) {
            out.error = appended.error();
            out.steps = i + 1;
            return out;
        }
        out.steps = i + 1;
    }

    out.completed = true;
    return out;
}

}  // namespace qp::graph::execution
