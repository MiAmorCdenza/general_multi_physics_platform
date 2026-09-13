/**
 * @file execution.cpp
 * @brief The run loop: bind an operator to a node, step it, record what came out.
 *
 * No physics here. The loop knows how to walk a state forward and how to write a trace; the
 * acceleration is the operator's business, and which operator is the plugin's.
 */
#include <qp/graph/execution/execution.hpp>

// For `IGraphRun`'s default `fields()`. `execution.hpp` deliberately does not include `run_provider.hpp` -- the
// two are different subjects (driving one operator / driving a whole graph) and a translation unit that wants
// both says so.
#include <qp/graph/execution/run_provider.hpp>

#include <qp/plugin/guard.hpp>
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
                                     const StateView& layout, rt::RunId run) {
    // The layout is the caller's, in both dimensions. A layout with no particles is a caller mistake rather
    // than a default to be filled in: `StateView{}` is an empty state, and "run this over nothing" is not a
    // meaningful request. There used to be a `particles` argument beside this one, and it was unreachable --
    // whenever the layout named a count, that count won -- so it is gone rather than documented as ignored.
    //
    // `is_consistent` and not just the two counts: a layout whose `values` do not match its own claimed shape
    // would be allocated **from** the counts here, so the ragged buffer a caller passed would be discarded and
    // the run would quietly succeed at a shape nobody asked for. Refusing is what makes the layout a contract
    // rather than a hint -- and this is a case the ragged-layout assertion in
    // `execution.loop.rejects_unusable_arguments` caught, because the first version of this check only looked at
    // the counts and the run prepared happily.
    if (layout.count == 0 || layout.components_per_particle == 0 || !layout.is_consistent()) {
        return diag::ErrorCode::invalid_argument;
    }

    // The state exists before any binder is consulted, because a binder is handed the layout it must
    // be able to work with. Asking it to build an operator and *then* checking the layout would let
    // it read the wrong component while deciding.
    //
    // **The component count comes from the caller now.** It used to be `StateView::zeroed(particles)`, whose
    // default is the mechanics family's three, so a model with a two- or four-component state could never be
    // prepared: every binder was asked about a shape it does not accept and declined. Latent since the loop was
    // written, because the only operator in the tree was the three-component oscillator; `plugins/models`'
    // pendulum is two and its projectile is four, which is what surfaced it.
    state_ = StateView::zeroed(layout.count, layout.components_per_particle);

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
    // Recorded from the node that was actually bound, so the trace says which node produced its channels.
    // Taken **after** a binder claimed the node: a run that refused to bind has no source to name, and
    // naming the node anyway would attribute a trace that does not exist to a node that did nothing.
    source_node_ = rt::Channel::NodeRef{node.id.index, node.id.generation};

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
    // Both channels come from the same node, and the source travels with each: a reader looking at one column
    // of a report asks "which node made this number", and an answer that required the other column to be present
    // would be an answer that disappears when a run records only one quantity.
    const auto x_channel =
        trace_.add_channel(rt::Channel{kPositionChannel, qp::units::dims::length, source_node_});
    if (!x_channel.has_value()) return x_channel.error();
    const auto v_channel =
        trace_.add_channel(rt::Channel{kVelocityChannel, qp::units::dims::velocity, source_node_});
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

RunReadiness check_run(const qp::graph::Graph& graph, const ResolveContext& ctx,
                       const std::vector<IOperatorBinder*>& binders, const StateView& layout,
                       std::size_t particles) {
    RunReadiness out;

    if (graph.node_count() == 0) {
        // Different from "nothing here has an operator": one is a document nobody has started, the other is a
        // document this build cannot run, and the sentences a user needs are different.
        out.refusal = RunRefusal::empty_graph;
        out.detail = "nothing to run: the graph is empty";
        return out;
    }

    // Validation runs alongside the search rather than gating it: see `RunReadiness` for why a dimension
    // mismatch is reported instead of refused. A caller that wants to refuse can read the count.
    const Report report =
        validate_graph(graph, ctx);
    for (const Issue& issue : report.issues()) {
        if (!issue.is_error()) continue;
        ++out.validation_errors;
        if (out.first_problem.empty()) out.first_problem = issue.to_text();
    }

    // The search, by `can_bind` rather than by node order alone: a graph holding a type this build does not
    // have is still runnable as long as something in it is. The probe carries the **caller's** component count,
    // because that is the answer a binder gives: a probe of the wrong shape makes every binder decline and the
    // graph is reported as having nothing to run.
    const StateView probe = layout.components_per_particle > 0
                                ? StateView::zeroed(layout.count > 0 ? layout.count : particles,
                                                    layout.components_per_particle)
                                : StateView::zeroed(particles);
    const Node* candidate = nullptr;
    for (const qp::graph::NodeSlot& slot : graph.slots()) {
        if (!slot.occupied) continue;
        for (IOperatorBinder* binder : binders) {
            if (binder == nullptr) continue;
            if (binder->can_bind(slot.node.type_name, probe)) {
                candidate = &slot.node;
                break;
            }
        }
        if (candidate != nullptr) break;
    }

    if (candidate == nullptr) {
        out.refusal = RunRefusal::no_operator;
        // **A node of another domain is worth naming.** "No node in this graph has an operator yet" is true, and
        // about a graph holding a particle kit's nodes it is also unhelpful: those types *are* registered, the
        // wires *are* drawn, and the reason nothing runs is that the nodes belong to a domain whose state is a
        // particle batch driven by its own loop rather than by this one's `StateView`. The sentence says which
        // node and which domain, so the reader's next question is "what drives that" rather than "which plugin
        // is missing" -- the same distinction `IOperatorBinder::can_bind`'s own documentation was written for.
        //
        // The two domains are named separately because what they need is different: a field node has to be
        // **baked** before anything can run, and a particle node has to be **launched** first. A single "another
        // domain" message would leave the reader to guess which.
        for (const qp::graph::NodeSlot& slot : graph.slots()) {
            if (!slot.occupied) continue;
            const qp::graph::NodeDesc* desc =
                ctx.catalog == nullptr ? nullptr : ctx.catalog->find(slot.node.type_name);
            if (desc == nullptr || !desc->has_compute) continue;
            if (desc->allow_in_particle_domain && !desc->allow_in_field_domain) {
                out.detail = "nothing to run: '" + slot.node.type_name +
                             "' belongs to the particle domain, whose state is a particle batch rather than "
                             "this loop's state";
                return out;
            }
            if (desc->allow_in_field_domain && !desc->allow_in_particle_domain) {
                out.detail = "nothing to run: '" + slot.node.type_name +
                             "' belongs to the field domain, which is baked rather than run";
                return out;
            }
        }
        out.detail = "nothing to run: no node in this graph has an operator yet";
        return out;
    }
    out.node = candidate->id;
    out.type_name = candidate->type_name;

    // Asked, and the answer is discarded: this is a pre-flight, not a preparation. A binder's contract already
    // requires equivalent operators for the same input, so binding again for the real run is sound.
    for (IOperatorBinder* binder : binders) {
        if (binder == nullptr) continue;
        std::unique_ptr<IStateOperator> candidate_operator = binder->bind(candidate->type_name, *candidate, probe);
        if (candidate_operator == nullptr) continue;
        out.operator_name = std::string{candidate_operator->name()};
        break;
    }
    if (out.operator_name.empty()) {
        // The type is known -- `can_bind` said so -- and this instance cannot be honoured. Named as the node
        // type rather than as a code, because the useful thing to know is which node and which settings.
        out.refusal = RunRefusal::node_cannot_be_honoured;
        out.detail = "cannot run " + candidate->type_name +
                     ": no operator can honour this node as configured (check its parameters, damping and "
                     "integrator settings)";
        return out;
    }

    out.detail = "ready: " + out.operator_name + " would run " + out.type_name;
    if (out.validation_errors > 0) {
        // Said out loud rather than hidden: the run will produce numbers, and these are the reasons to distrust
        // them. Refusing would be stricter than the engine and would refuse the built-in demonstration first.
        out.detail += " (" + std::to_string(out.validation_errors) + " validation problem";
        out.detail += out.validation_errors == 1 ? "" : "s";
        out.detail += ": " + out.first_problem + ")";
    }
    return out;
}

const qp::graph::field::FieldSet& IGraphRun::fields() const noexcept {
    // One shared empty set rather than a member, because the answer is a property of *this interface's default*
    // and not of any run: a run that baked nothing has no fields, and giving every such run its own empty vector
    // would make the default cost an allocation. `const` and never written through, so sharing it is safe --
    // `FieldSet` has no mutable state a const caller can reach.
    static const qp::graph::field::FieldSet kNoFields{};
    return kNoFields;
}

void IGraphRun::set_run(qp::runtime::RunId /*run*/) noexcept {
    // The default is to ignore it, and that is the right default: a run that records nothing has no use for an
    // identity, and one that does record overrides this. Not a pure virtual, for the reason `fields()` is not.
}

const qp::runtime::Trace& IGraphRun::trace() const noexcept {
    // A shared empty trace, for the same reason as the shared empty set above. Its `RunId` is invalid, which is
    // what "this run was never given one" means -- `Trace` exposes no way to change it afterwards, deliberately,
    // so the id it carries is always the id some ledger issued.
    static const qp::runtime::Trace kNoTrace{qp::runtime::RunId{}};
    return kNoTrace;
}

std::optional<RunCadence> IGraphRun::preferred_cadence() const noexcept {
    // No opinion, which leaves the caller's own step count and size in place: the operator loop's oscillator was
    // measured against those two numbers, and a default that overrode them would change a run nobody asked to
    // change. See the declaration for why a run that *does* have an opinion must state one.
    return std::nullopt;
}

RunOutcome GraphRun::run(std::size_t steps, double dt) {
    RunOutcome out;
    out.operator_name = operator_name_;

    if (operator_ == nullptr) {
        out.error = diag::ErrorCode::invalid_argument;
        return out;
    }
    // A run goes **forward**: `dt > 0`, where the operator contract is more permissive and admits any non-zero
    // finite value. The narrowing is deliberate and it belongs here rather than in the operator, because a trace
    // is a time series and its samples are in non-decreasing time order -- a backward run would append samples
    // whose times go the wrong way. An operator is free to accept a negative step so that a round trip can
    // check its reversibility; a *run* does not offer one.
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
        // The plugin boundary, once per step. A `try` costs nothing on the path where nothing throws --
        // table-based unwinding puts the price on the throw, not on the entry -- and what the hot-path rule
        // forbids is a `throw` here, which would happen only if a plugin misbehaved. The label is the
        // operator's name, because that is what the run report and the trace's provenance name.
        const diag::Result<void> stepped = qp::plugin::call_guarded(
            operator_name_, &faults_, [this, dt] { return operator_->step(state_, dt); });
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
