/**
 * @file confidence_model.cpp
 * @brief Implementation of the C8 diagnostics.
 *
 * Two channel lookups, two energy evaluations, and a subtraction. The judgement is in which cases
 * produce **no answer** rather than a number -- see the header for why absent and zero are different
 * claims, and why the oscillator's frequency is an input rather than something inferred from the
 * trace.
 */
#include <qp/views/model/confidence_model.hpp>

#include <qp/runtime/store/store.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>
#include <utility>

namespace qp::views::model {
namespace {

namespace rt = qp::runtime;

/// @brief Total energy of a harmonic state: `0.5 v^2 + 0.5 w^2 x^2`.
[[nodiscard]] double energy_of(double x, double v, double w) noexcept {
    return 0.5 * v * v + 0.5 * w * w * x * x;
}

/// @brief Formats a relative change the way a reader expects to see one: a percentage.
[[nodiscard]] std::string as_percent(double fraction) {
    const double percent = fraction * 100.0;
    char buffer[64] = {};
    // `%.4g`: enough digits to see a small drift, few enough that a confidence note reads as a
    // sentence. The full-precision figure lives in the report where a caller can format it.
    std::snprintf(buffer, sizeof(buffer), "%.4g%%", percent);
    return std::string{buffer};
}

}  // namespace

void ConfidenceModel::set_omega(double w) noexcept {
    // A non-finite or non-positive frequency is ignored rather than stored. Storing it would make
    // every later energy `NaN` and the report would then be about the input rather than about the
    // run -- and the caller's mistake would surface as "your physics diverged".
    if (!std::isfinite(w) || !(w > 0.0)) return;
    omega_ = w;
    omega_declared_ = true;
}

std::optional<std::size_t> ConfidenceModel::channel_index(const char* name) const {
    const std::vector<rt::Channel>& channels = trace_->channels();
    for (std::size_t i = 0; i < channels.size(); ++i) {
        if (channels[i].name == name) return i;
    }
    return std::nullopt;
}

namespace {

/// @brief One endpoint value of a channel, or nothing when the sample is short or the value is not a number.
[[nodiscard]] std::optional<double> channel_value(const rt::Sample& sample, std::size_t index) {
    if (sample.values.size() <= index) return std::nullopt;
    const double value = sample.values[index].value;
    if (!std::isfinite(value)) return std::nullopt;
    return value;
}

}  // namespace

ConfidenceReport ConfidenceModel::report() const {
    ConfidenceReport out;
    out.samples = trace_->size();
    out.clamps_fired = clamps_;

    // Two samples are the minimum for a change, and the endpoints are what is compared.
    const std::vector<rt::Sample>& samples = trace_->samples();
    if (samples.size() < 2) return out;
    const rt::Sample& first = samples.front();
    const rt::Sample& last = samples.back();

    // The time span first, because a rate needs it and a non-positive span means the trace carried
    // the same `t` throughout -- which makes a rate meaningless rather than zero.
    const double span = last.t - first.t;
    if (!(span > 0.0) || !std::isfinite(span)) return out;

    // **A conservation law at a time, and only the ones this trace can be judged by.** The order is the order of
    // the questions: is this an oscillator (a displacement and a velocity, with a quadratic potential)? If not, is
    // it a charged particle in a magnetic field (a speed, which the Lorentz force conserves *exactly*)? And
    // independently of either: does it carry the first adiabatic invariant, whose drift is the experiment's rather
    // than the integrator's?
    const auto add_check = [&out, span](const char* name, bool measures_error, double a, double b) {
        InvariantCheck check;
        check.name = name;
        check.measures_error = measures_error;
        check.first = a;
        check.last = b;
        // A zero first value has no relative change: `(b - 0) / 0` is not a large drift, it is undefined, and a
        // state at rest conserves trivially. The check is still recorded -- with its two values and no drift --
        // because "this quantity was read and cannot be scaled" is a different statement from "this quantity is
        // not in the trace".
        if (a != 0.0 && std::isfinite(a) && std::isfinite(b)) {
            check.relative_drift = (b - a) / std::abs(a);
            check.relative_rate = *check.relative_drift / span;
        }
        out.checks.push_back(check);
    };

    // The oscillator's energy: **both** channels are required, because a trace with only a position has no energy
    // and the potential term alone is conserved by neither physics nor arithmetic.
    const std::optional<std::size_t> x_at = channel_index(kPositionChannel);
    const std::optional<std::size_t> v_at = channel_index(kVelocityChannel);
    if (x_at.has_value() && v_at.has_value()) {
        const std::optional<double> x0 = channel_value(first, *x_at);
        const std::optional<double> v0 = channel_value(first, *v_at);
        const std::optional<double> x1 = channel_value(last, *x_at);
        const std::optional<double> v1 = channel_value(last, *v_at);
        if (x0.has_value() && v0.has_value() && x1.has_value() && v1.has_value()) {
            add_check("energy", true, energy_of(*x0, *v0, omega_), energy_of(*x1, *v1, omega_));
        }
    } else if (const std::optional<std::size_t> speed_at = channel_index(kSpeedChannel);
               speed_at.has_value()) {
        // A magnetic field does no work, so a particle's speed is conserved by the **motion**: any change is the
        // step's. This is the oscillator's energy for a Lorentz run, and it is why the kit records `speed` at all.
        const std::optional<double> s0 = channel_value(first, *speed_at);
        const std::optional<double> s1 = channel_value(last, *speed_at);
        if (s0.has_value() && s1.has_value()) add_check("speed", true, *s0, *s1);
    }

    // The first adiabatic invariant, whenever the trace carries it -- including beside an energy, because it is not
    // a second way of saying the same thing: it is conserved only while the field varies slowly over a gyro-orbit.
    if (const std::optional<std::size_t> mu_at = channel_index(kMuChannel); mu_at.has_value()) {
        const std::optional<double> m0 = channel_value(first, *mu_at);
        const std::optional<double> m1 = channel_value(last, *mu_at);
        if (m0.has_value() && m1.has_value()) add_check("mu", false, *m0, *m1);
    }

    // A span is part of the answer only when something was measured over it.
    if (!out.checks.empty()) out.span = span;
    return out;
}

std::vector<std::string> ConfidenceModel::notes() const {
    std::vector<std::string> out;
    const ConfidenceReport r = report();

    // **A sentence per check, and the kind of number it is.** The old version had one diagnostic and one paragraph;
    // the shape now is "for each law this trace carries, say what its drift means" -- and the meaning depends on
    // `measures_error`, which is the whole reason that field exists. A panel that said "the method loses energy"
    // about the adiabatic invariant would be reporting a numerical defect where the experiment is simply not
    // adiabatic, and a student would go looking for a bug in the integrator.
    //
    // A tolerance rather than an exact zero: the endpoints of a numerically integrated orbit essentially never
    // agree bit for bit, and a note printed for a 1e-16 wobble is a note that trains the reader to ignore notes.
    constexpr double kNegligible = 1.0e-6;
    for (const InvariantCheck& check : r.checks) {
        if (!check.relative_drift.has_value()) {
            std::string message = std::string{check.name} +
                                  " was read but has no relative change to report: its first value is zero, and "
                                  "a quantity that starts at zero has no scale to be a fraction of";
            out.push_back(std::move(message));
            continue;
        }
        const double drift = *check.relative_drift;
        if (std::abs(drift) <= kNegligible) continue;
        std::string message = std::string{check.name} + " changed by " + as_percent(drift) + " over " +
                              std::to_string(r.samples) + " samples";
        if (check.measures_error) {
            if (drift < 0.0) {
                // The mechanism, named. This is the note that stops a student attributing RK4's dissipation to
                // damping they did not add -- C8's whole reason for existing.
                message += ": the method loses " + std::string{check.name} +
                           ", which is a property of the integrator and not of the model unless the model has "
                           "damping";
            } else {
                message += ": the method gains " + std::string{check.name} +
                           ", which means the step is too large for this motion and the run is not trustworthy at "
                           "this resolution";
            }
        } else {
            // **Not an error, and the sentence has to say so.** The adiabatic invariant holds only while the field
            // varies slowly over a gyro-orbit; a drift here measures the configuration, and calling it a defect
            // would send the reader to the wrong place entirely.
            message += ": the invariant holds only while the field changes slowly over a gyro-orbit, so this is a "
                       "property of the configuration rather than an error -- the run says how adiabatic this "
                       "experiment is, not how accurate the step was";
        }
        out.push_back(std::move(message));
    }

    // The unaskable case, and phrased as what is missing rather than as a failure. A trace without any of the
    // channels this model knows is not a broken run; it is a run these diagnostics do not apply to, and saying which
    // channel was wanted is more useful than saying nothing.
    if (r.checks.empty()) {
        const bool has_x = channel_index(kPositionChannel).has_value();
        const bool has_v = channel_index(kVelocityChannel).has_value();
        if (has_x && has_v) {
            out.emplace_back(
                "no drift can be measured: the run has no positive time span, or its first sample is not a number");
        } else {
            std::string message = "no drift can be measured: the trace has neither a ";
            message += kPositionChannel;
            message += " with a ";
            message += kVelocityChannel;
            message += " (a quadratic potential needs both) nor a ";
            message += kSpeedChannel;
            message += " (which a magnetic field conserves exactly)";
            out.push_back(std::move(message));
        }
    }

    // The frequency note. A report that assumed `omega = 1` and did not say so is a number the reader cannot check,
    // and the default is a real value rather than a sentinel precisely so this note is the only thing standing
    // between the reader and a meaningless figure. **Only when an energy was actually computed**: a magnetic run
    // has no potential and the sentence would be noise.
    const bool has_energy = std::any_of(r.checks.begin(), r.checks.end(), [](const InvariantCheck& check) {
        return std::string_view{check.name} == std::string_view{"energy"};
    });
    if (has_energy && !omega_declared_) {
        out.emplace_back(
            "the energy figure assumes omega = 1 rad/s because no frequency was declared; if the graph's potential "
            "is not quadratic with that frequency, the figure does not apply");
    }

    // Clamping is the one diagnostic that is never absent: the count came from the operator.
    if (r.clamps_fired > 0) {
        std::string message = "the operator clamped " + std::to_string(r.clamps_fired) +
                              " value(s); past the clamp the numbers are bounded rather than correct, so this "
                              "run's state is not the state the physics would give";
        out.push_back(std::move(message));
    }

    return out;
}

}  // namespace qp::views::model
