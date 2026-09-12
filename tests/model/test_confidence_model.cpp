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
    REQUIRE(r.energy_drift.has_value());
    REQUIRE(r.span.has_value());
    REQUIRE(r.span.value() > 0.0);
    REQUIRE(r.energy_drift_rate.has_value());

    // Dissipative, so the sign is negative. This is the assertion that stops a student attributing
    // the loss to damping they never added -- C8's reason for existing.
    REQUIRE(r.energy_drift.value() < 0.0);
    // Small, because RK4 is accurate: a drift of a percent over eighty periods would mean the
    // operator is wrong rather than that the method is dissipative.
    REQUIRE(std::abs(r.energy_drift.value()) < 1.0e-2);
    // And not zero: a diagnostic that always reported "conserve" would satisfy the bound above.
    REQUIRE(std::abs(r.energy_drift.value()) > 0.0);
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
        REQUIRE_FALSE(r.energy_drift.has_value());
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
        REQUIRE_FALSE(model.report().energy_drift.has_value());
    }

    // A state at rest at the origin conserves trivially, and a **relative** change from zero energy
    // is undefined rather than infinite. The model reports no figure, which is the honest answer:
    // there is nothing that could have drifted.
    {
        TracedRun still;
        still.add(0.0, 0.0, 0.0);
        still.add(1.0, 0.0, 0.0);
        ConfidenceModel model(still.trace());
        model.set_omega(1.0);
        REQUIRE_FALSE(model.report().energy_drift.has_value());
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
    REQUIRE(r.energy_drift.has_value());
    REQUIRE(r.energy_drift.value() < 0.0);

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
