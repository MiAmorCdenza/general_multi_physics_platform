/**
 * @file test_confidence_model.cpp
 * @brief Tests for charter C8's diagnostics.
 *
 * ## What is worth testing here
 *
 * Not that a subtraction works. What is worth testing is the pair of properties that make the
 * diagnostic trustworthy rather than merely present:
 *
 *   - **The sign and the magnitude are real.** A run integrated with a dissipative method shows a
 *     negative drift of the size the method's error implies, and the check is against the method's
 *     own measured behaviour rather than against a constant someone observed.
 *   - **Unaskable is not zero.** A trace with no velocity channel reports *no* energy figure, not a
 *     zero one. A zero would assert conservation about a quantity that was never measured, which is
 *     the failure C8 exists to prevent arriving from the other direction.
 *
 * The frequency case is here because it is where the honest limit lives: the model can only compute
 * a quadratic potential's energy, and the report has to say which frequency it assumed.
 */
#include <catch2/catch_test_macros.hpp>

#include <qp/views/model/confidence_model.hpp>

#include <qp/plugins/mechanics/rk4_oscillator.hpp>

#include <qp/graph/field/field.hpp>
#include <qp/runtime/store/store.hpp>
#include <qp/units/dimensions.hpp>

#include <cmath>
#include <string>
#include <vector>

using namespace qp::views::model;

namespace {

namespace rt = qp::runtime;
namespace kernels = qp::graph::kernels;
namespace field = qp::graph::field;

constexpr double kPi = 3.14159265358979323846;

/// @brief A trace with a displacement and a velocity channel, and a run recorded into it.
///
/// The channels are named through the model's own constants, so a rename in the model breaks this
/// helper rather than silently making every case unaskable -- which is exactly how a test suite
/// loses its coverage without going red.
class TracedRun final {
public:
    TracedRun()
        : trace_(rt::RunId{1}) {
        REQUIRE(trace_.add_channel(rt::Channel{ConfidenceModel::kPositionChannel, qp::units::dims::length})
                    .has_value());
        REQUIRE(trace_.add_channel(rt::Channel{ConfidenceModel::kVelocityChannel,
                                               qp::units::dims::velocity})
                    .has_value());
    }

    [[nodiscard]] rt::Trace& trace() noexcept { return trace_; }

    /// @brief Appends one sample. `u` is the per-sample uncertainty, zero by default.
    void add(double t, double x, double v, double u = 0.0) {
        REQUIRE(trace_.append(t, {rt::UncertainValue::measured(x, u, qp::units::dims::length),
                                  rt::UncertainValue::measured(v, u, qp::units::dims::velocity)})
                    .has_value());
    }

private:
    rt::Trace trace_;
};

/// @brief Integrates a unit oscillator and records every step, so the diagnostics see a real run.
///
/// Uses the plugin's own RK4 operator rather than a hand-rolled loop: the point of the case is that
/// the diagnostic reads a run the platform actually produces, and a bespoke integrator in the test
/// would be measuring the test.
void fill_with_rk4(TracedRun& run, double omega, double dt, int steps) {
    qp::plugins::mechanics::Rk4Oscillator op;
    kernels::ParamBlock params;
    params.set_real(0, omega);
    REQUIRE(op.prepare(params).has_value());

    std::vector<double> state{1.0, 0.0, 0.0};  // one particle: x = 1, v = 0, reserved
    qp::abi::LatticeDesc desc{};
    desc.kind = qp::abi::LatticeKind::line;
    desc.component = qp::abi::ComponentKind::vector;
    desc.element = qp::abi::ElementType::f64;
    desc.count[0] = 1;
    desc.spacing_bytes = qp::abi::expected_spacing(desc);

    for (int i = 0; i < steps; ++i) {
        run.add(static_cast<double>(i) * dt, state[0], state[1], 1.0e-9);
        field::FieldValue fv;
        fv.desc = desc;
        fv.data = state.data();
        fv.bytes = state.size() * sizeof(double);
        kernels::BatchView batch;
        batch.in = &fv;
        batch.out = &fv;
        batch.count = 1;
        kernels::AdvanceContext ctx;
        ctx.dt = dt;
        REQUIRE(op.advance(batch, ctx).has_value());
    }
    run.add(static_cast<double>(steps) * dt, state[0], state[1], 1.0e-9);
}

/// @brief The check named `name`, or null when the trace carries no such law.
[[nodiscard]] const qp::views::model::InvariantCheck* check_named(
    const qp::views::model::ConfidenceReport& report, const char* name) {
    for (const qp::views::model::InvariantCheck& check : report.checks) {
        if (std::string_view{check.name} == std::string_view{name}) return &check;
    }
    return nullptr;
}

}  // namespace

TEST_CASE("confidence.energy_drift_is_measured", "[confidence][model]") {
    // A run of the RK4 oscillator, whose dissipation this project has already characterised:
    // `plugin.mechanics.oscillator_loses_energy_slowly` measures the drift at dt = 5e-2 as roughly
    // 8.6e-4 over four million steps, and the rate goes like dt^6.
    //
    // This case does not re-derive that rate. It asserts the two things a confidence panel needs to
    // be right about: the drift is **negative** (the method dissipates), and it is **of the order**
    // the method's error implies rather than wildly wrong in either direction.
    TracedRun run;
    const double omega = 2.0 * kPi;  // one period per second
    const double dt = 5.0e-3;
    const int steps = 20'000;  // eighty periods

    fill_with_rk4(run, omega, dt, steps);

    ConfidenceModel model(run.trace());
    model.set_omega(omega);
    const ConfidenceReport r = model.report();

    REQUIRE(r.samples == static_cast<std::size_t>(steps) + 1);
    // **One check, and it is the energy**: a displacement with a velocity is what an oscillator is, and this trace
    // carries both, so the model has exactly one law to judge it by.
    REQUIRE(r.checks.size() == 1);
    const InvariantCheck& energy = r.checks.front();
    REQUIRE(std::string_view{energy.name} == std::string_view{"energy"});
    // ... and it is arithmetic, not physics: a drift here is the step's.
    REQUIRE(energy.measures_error);
    REQUIRE(energy.relative_drift.has_value());
    REQUIRE(energy.relative_rate.has_value());
    REQUIRE(r.span.has_value());
    REQUIRE(r.span.value() > 0.0);

    // Dissipative, so the sign is negative. This is the assertion that stops a student attributing
    // the loss to damping they never added -- C8's reason for existing.
    REQUIRE(energy.relative_drift.value() < 0.0);
    // Small, because RK4 is accurate: a drift of a percent over eighty periods would mean the
    // operator is wrong rather than that the method is dissipative.
    REQUIRE(std::abs(energy.relative_drift.value()) < 1.0e-2);
    // And not zero: a diagnostic that always reported "conserve" would satisfy the bound above.
    REQUIRE(std::abs(energy.relative_drift.value()) > 0.0);
    // The rate is the drift spread over the run's own span, not a second measurement.
    REQUIRE(energy.relative_rate.value() == energy.relative_drift.value() / r.span.value());
}


TEST_CASE("confidence.a_magnetic_run_is_judged_by_speed_and_mu", "[confidence][model]") {
    // **The gap this case closes was found in the window, not in a test.** After a magnetosphere run the confidence
    // panel said "energy drift cannot be measured: the trace is missing displacement and velocity (a quadratic
    // potential needs both)" -- true, and useless: that trace carries `speed`, which a magnetic field conserves
    // *exactly* (so its drift is the integrator's), and `mu`, the first adiabatic invariant, which is conserved
    // only while the field varies slowly over a gyro-orbit (so its drift is the configuration's). Two conservation
    // laws, two different kinds of statement, and the panel knew about neither.
    //
    // The trace is built here rather than taken from a magnetosphere run, because what is under test is the
    // *decision*: which channel makes a run an oscillator, which makes it a Lorentz run, and what each drift means.
    // The end-to-end half -- that a run of the kit's own provider produces such a trace -- is
    // `run.controller.a_content_graph_runs_through_the_provider`.
    qp::runtime::Trace trace{qp::runtime::RunId{9}};
    REQUIRE(trace.add_channel(qp::runtime::Channel{"speed", qp::units::dims::velocity}).has_value());
    REQUIRE(trace.add_channel(qp::runtime::Channel{"mu", qp::units::dims::energy / qp::units::dims::magnetic_flux_density}).has_value());
    // A speed that falls by a part in ten thousand, and an invariant that wanders by a percent: the first is the
    // step's error, the second is the experiment's adiabaticity, and the case asserts that the report says so.
    REQUIRE(trace.append(0.0, {rt::UncertainValue::exact(1.0e6), rt::UncertainValue::exact(2.0e-8)}).has_value());
    REQUIRE(trace.append(1.0, {rt::UncertainValue::exact(1.0e6), rt::UncertainValue::exact(2.0e-8)}).has_value());
    REQUIRE(trace.append(2.0, {rt::UncertainValue::exact(9.999e5), rt::UncertainValue::exact(2.02e-8)}).has_value());

    ConfidenceModel model(trace);
    const ConfidenceReport r = model.report();

    // Two checks, in the model's order: the error first, the physics second.
    REQUIRE(r.checks.size() == 2);
    const InvariantCheck* speed = check_named(r, "speed");
    const InvariantCheck* mu = check_named(r, "mu");
    REQUIRE(speed != nullptr);
    REQUIRE(mu != nullptr);
    // **The distinction the panel renders**: one of these is a bug if it drifts, the other is not.
    REQUIRE(speed->measures_error);
    REQUIRE_FALSE(mu->measures_error);

    // The speed's drift is the relative change of the endpoints, and it is negative (the step lost speed).
    REQUIRE(speed->relative_drift.has_value());
    REQUIRE(speed->relative_drift.value() < 0.0);
    REQUIRE(std::abs(speed->relative_drift.value() - (9.999e5 - 1.0e6) / 1.0e6) < 1.0e-12);
    REQUIRE(speed->relative_rate.has_value());
    REQUIRE(std::abs(speed->relative_rate.value() - speed->relative_drift.value() / 2.0) < 1.0e-12);
    REQUIRE(mu->relative_drift.has_value());
    REQUIRE(mu->relative_drift.value() > 0.0);

    // The notes say which is which, in words a student can act on. The energy sentence must **not** appear: this
    // run has no potential, and a note about omega would be noise about a quantity nobody computed.
    const std::vector<std::string> notes = model.notes();
    REQUIRE_FALSE(notes.empty());
    bool said_error = false;
    bool said_physics = false;
    bool said_omega = false;
    for (const std::string& note : notes) {
        if (note.find("speed changed by") != std::string::npos) {
            said_error = note.find("integrator") != std::string::npos;
        }
        if (note.find("mu changed by") != std::string::npos) {
            said_physics = note.find("adiabatic") != std::string::npos;
        }
        if (note.find("omega") != std::string::npos) said_omega = true;
    }
    REQUIRE(said_error);
    REQUIRE(said_physics);
    REQUIRE_FALSE(said_omega);

    // A trace with neither pair nor speed is still the "cannot be measured" case, and the sentence now names both
    // ways a run could have been measurable.
    qp::runtime::Trace bare{qp::runtime::RunId{10}};
    REQUIRE(bare.add_channel(qp::runtime::Channel{"mass", qp::units::dims::mass}).has_value());
    REQUIRE(bare.append(0.0, {rt::UncertainValue::exact(1.0)}).has_value());
    REQUIRE(bare.append(1.0, {rt::UncertainValue::exact(1.0)}).has_value());
    ConfidenceModel bare_model(bare);
    REQUIRE(bare_model.report().checks.empty());
    const std::vector<std::string> bare_notes = bare_model.notes();
    REQUIRE(bare_notes.size() == 1);
    REQUIRE(bare_notes.front().find("displacement") != std::string::npos);
    REQUIRE(bare_notes.front().find("speed") != std::string::npos);

    // An oscillator's trace keeps its energy check even when it also carries a mu -- the two are independent
    // questions, and a caller that recorded both gets both answers.
    qp::runtime::Trace both{qp::runtime::RunId{11}};
    REQUIRE(both.add_channel(qp::runtime::Channel{"displacement", qp::units::dims::length}).has_value());
    REQUIRE(both.add_channel(qp::runtime::Channel{"velocity", qp::units::dims::velocity}).has_value());
    REQUIRE(both.add_channel(qp::runtime::Channel{"mu", qp::units::dims::energy / qp::units::dims::magnetic_flux_density}).has_value());
    REQUIRE(both.append(0.0, {rt::UncertainValue::exact(1.0), rt::UncertainValue::exact(2.0),
                              rt::UncertainValue::exact(3.0)}).has_value());
    REQUIRE(both.append(1.0, {rt::UncertainValue::exact(1.0), rt::UncertainValue::exact(2.0),
                              rt::UncertainValue::exact(3.0)}).has_value());
    ConfidenceModel both_model(both);
    both_model.set_omega(1.0);
    const ConfidenceReport both_report = both_model.report();
    REQUIRE(both_report.checks.size() == 2);
    REQUIRE(check_named(both_report, "energy") != nullptr);
    REQUIRE(check_named(both_report, "mu") != nullptr);
    // Both conserved exactly here, so neither has a drift worth a note.
    for (const InvariantCheck& check : both_report.checks) {
        REQUIRE(check.relative_drift.has_value());
        REQUIRE(check.relative_drift.value() == 0.0);
    }
}

TEST_CASE("confidence.absent_is_not_zero", "[confidence][model]") {
    // The distinction the file header is about. Three traces that **cannot** answer the question,
    // and the assertion is that the model says so rather than reporting a number.
    //
    // A zero here would be the worst available outcome: a reader sees "energy drift 0.0" and
    // concludes the run is trustworthy, on the strength of a quantity that was never measured.
    {
        // Only a position channel: no energy is computable from it.
        rt::Trace positional(rt::RunId{1});
        REQUIRE(positional
                    .add_channel(rt::Channel{ConfidenceModel::kPositionChannel,
                                             qp::units::dims::length})
                    .has_value());
        REQUIRE(positional
                    .append(0.0, {rt::UncertainValue::measured(1.0, 0.0, qp::units::dims::length)})
                    .has_value());
        REQUIRE(positional
                    .append(1.0, {rt::UncertainValue::measured(0.5, 0.0, qp::units::dims::length)})
                    .has_value());

        ConfidenceModel model(positional);
        const ConfidenceReport r = model.report();
        REQUIRE(r.samples == 2);
        REQUIRE(r.checks.empty());
        REQUIRE_FALSE(r.span.has_value());

        // And the notes say which channel is missing rather than that something is wrong.
        const std::vector<std::string> notes = model.notes();
        REQUIRE_FALSE(notes.empty());
        bool names_velocity = false;
        for (const std::string& n : notes) {
            if (n.find(ConfidenceModel::kVelocityChannel) != std::string::npos) names_velocity = true;
        }
        REQUIRE(names_velocity);
    }

    // A single sample has no change to measure.
    {
        TracedRun one;
        one.add(0.0, 1.0, 0.0);
        ConfidenceModel model(one.trace());
        model.set_omega(1.0);
        REQUIRE(model.report().checks.empty());
    }

    // A state at rest at the origin conserves trivially, and a **relative** change from zero energy is undefined
    // rather than infinite. The honest answer is not "no figure" but a figure that says which of the two situations
    // this is: the law **was read** and has no scale to be a fraction of, which is a different statement from the
    // law not being in the trace at all. That distinction is what the checks table made room for, and this block is
    // where it is asserted -- the old shape could only say "absent".
    {
        TracedRun still;
        still.add(0.0, 0.0, 0.0);
        still.add(1.0, 0.0, 0.0);
        ConfidenceModel model(still.trace());
        model.set_omega(1.0);
        const ConfidenceReport r = model.report();
        REQUIRE(r.checks.size() == 1);
        const InvariantCheck& energy = r.checks.front();
        REQUIRE(std::string_view{energy.name} == std::string_view{"energy"});
        REQUIRE(energy.measures_error);
        // Its two values are there, and the drift is **absent rather than zero**: a zero would claim the energy was
        // measured and found not to change, which is a claim about a quantity that starts at zero.
        REQUIRE(energy.first == 0.0);
        REQUIRE(energy.last == 0.0);
        REQUIRE_FALSE(energy.relative_drift.has_value());
        REQUIRE_FALSE(energy.relative_rate.has_value());
        // ... and the note says so, in the terms a student can act on.
        const std::vector<std::string> notes = model.notes();
        REQUIRE_FALSE(notes.empty());
        bool explained = false;
        for (const std::string& note : notes) {
            if (note.find("energy") != std::string::npos && note.find("zero") != std::string::npos) {
                explained = true;
            }
        }
        REQUIRE(explained);
    }
}

TEST_CASE("confidence.clamp_count_is_reported", "[confidence][model]") {
    // The clamp count is the one diagnostic that is **never** absent, because it comes from the
    // operator rather than from the data. Zero means "the clamp never fired", which is a real
    // answer and does not produce a note; a non-zero count produces one that says the state is
    // bounded rather than correct.
    TracedRun run;
    fill_with_rk4(run, 1.0, 1.0e-2, 100);

    ConfidenceModel clean(run.trace());
    clean.set_omega(1.0);
    REQUIRE(clean.report().clamps_fired == 0);
    for (const std::string& n : clean.notes()) {
        REQUIRE(n.find("clamped") == std::string::npos);
    }

    ConfidenceModel clamped(run.trace());
    clamped.set_omega(1.0);
    clamped.note_clamps(41000);
    REQUIRE(clamped.report().clamps_fired == 41000);

    bool mentions_clamp = false;
    for (const std::string& n : clamped.notes()) {
        if (n.find("clamped") != std::string::npos) {
            mentions_clamp = true;
            // The note has to say what a clamped value **is**, not merely that clamping happened:
            // "bounded rather than correct" is the fact the reader needs.
            REQUIRE(n.find("bounded rather than") != std::string::npos);
        }
    }
    REQUIRE(mentions_clamp);
}

TEST_CASE("confidence.notes_name_the_mechanism", "[confidence][model]") {
    // C8's sentence is "numerical error must not be mistaken for physics", so the note for a
    // dissipative run has to name **the integrator** as the cause. A note reading "energy is not
    // conserved" tells the reader to worry; this one tells them what to do.
    TracedRun run;
    const double omega = 2.0 * kPi;
    fill_with_rk4(run, omega, 2.0e-2, 40'000);  // a coarse step, so the drift is unmistakable

    ConfidenceModel model(run.trace());
    model.set_omega(omega);
    const ConfidenceReport r = model.report();
    REQUIRE(check_named(r, "energy") != nullptr);
    REQUIRE(check_named(r, "energy")->relative_drift.value() < 0.0);

    bool names_the_method = false;
    for (const std::string& n : model.notes()) {
        if (n.find("property of the integrator") != std::string::npos) names_the_method = true;
    }
    REQUIRE(names_the_method);

    // A declared frequency produces no uncertainty note; the undeclared case does, and says which
    // value it assumed. That difference is the whole reason `omega_was_declared()` exists.
    REQUIRE(model.omega_was_declared());
    for (const std::string& n : model.notes()) {
        REQUIRE(n.find("assumes omega") == std::string::npos);
    }

    ConfidenceModel assumed(run.trace());
    REQUIRE_FALSE(assumed.omega_was_declared());
    REQUIRE(assumed.omega() == ConfidenceModel::kDefaultOmega);

    bool mentions_assumption = false;
    for (const std::string& n : assumed.notes()) {
        if (n.find("assumes omega") != std::string::npos) mentions_assumption = true;
    }
    REQUIRE(mentions_assumption);

    // A non-finite or non-positive frequency is refused rather than stored: storing it would make
    // every later energy a NaN, and the caller's mistake would surface as "your physics diverged".
    assumed.set_omega(std::nan(""));
    REQUIRE(assumed.omega() == ConfidenceModel::kDefaultOmega);
    assumed.set_omega(0.0);
    REQUIRE(assumed.omega() == ConfidenceModel::kDefaultOmega);
    assumed.set_omega(-1.0);
    REQUIRE(assumed.omega() == ConfidenceModel::kDefaultOmega);
    assumed.set_omega(3.5);
    REQUIRE(assumed.omega() == 3.5);
}
