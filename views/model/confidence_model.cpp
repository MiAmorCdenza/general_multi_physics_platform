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

#include <cmath>
#include <string>
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

ConfidenceReport ConfidenceModel::report() const {
    ConfidenceReport out;
    out.samples = trace_->size();
    out.clamps_fired = clamps_;

    // Both channels are required. A trace with only a position has no energy, and reporting the
    // potential term alone would be a quantity that is conserved by neither physics nor arithmetic.
    const std::optional<std::size_t> x_at = channel_index(kPositionChannel);
    const std::optional<std::size_t> v_at = channel_index(kVelocityChannel);
    if (!x_at.has_value() || !v_at.has_value()) return out;

    // Two samples are the minimum for a change, and the endpoints are what is compared.
    const std::vector<rt::Sample>& samples = trace_->samples();
    if (samples.size() < 2) return out;

    const rt::Sample& first = samples.front();
    const rt::Sample& last = samples.back();
    if (first.values.size() <= *x_at || first.values.size() <= *v_at) return out;
    if (last.values.size() <= *x_at || last.values.size() <= *v_at) return out;

    const double x0 = first.values[*x_at].value;
    const double v0 = first.values[*v_at].value;
    const double x1 = last.values[*x_at].value;
    const double v1 = last.values[*v_at].value;
    if (!std::isfinite(x0) || !std::isfinite(v0) || !std::isfinite(x1) || !std::isfinite(v1)) {
        return out;
    }

    const double e0 = energy_of(x0, v0, omega_);
    const double e1 = energy_of(x1, v1, omega_);
    // A zero initial energy has no relative change: `(e1 - 0) / 0` is not a large drift, it is
    // undefined. A state at rest at the origin conserves trivially and there is nothing to report.
    if (!(e0 > 0.0) || !std::isfinite(e0) || !std::isfinite(e1)) return out;

    const double span = last.t - first.t;
    // Non-positive because the trace refuses out-of-order times: equal times mean every sample
    // carried the same `t`, which makes a rate meaningless rather than zero.
    if (!(span > 0.0) || !std::isfinite(span)) return out;

    out.span = span;
    out.energy_drift = (e1 - e0) / e0;
    out.energy_drift_rate = *out.energy_drift / span;
    return out;
}

std::vector<std::string> ConfidenceModel::notes() const {
    std::vector<std::string> out;
    const ConfidenceReport r = report();

    // The unaskable case first, and phrased as what is missing rather than as a failure. A trace
    // without velocity is not a broken run; it is a run this diagnostic does not apply to, and
    // saying so is more useful than saying nothing.
    if (!r.energy_drift.has_value()) {
        const bool has_x = channel_index(kPositionChannel).has_value();
        const bool has_v = channel_index(kVelocityChannel).has_value();
        if (!has_x || !has_v) {
            std::string message = "energy drift cannot be measured: the trace is missing ";
            message += !has_x ? kPositionChannel : "";
            if (!has_x && !has_v) message += " and ";
            message += !has_v ? kVelocityChannel : "";
            message += " (a quadratic potential needs both)";
            out.push_back(std::move(message));
        } else if (r.samples < 2) {
            out.emplace_back("energy drift cannot be measured: fewer than two samples");
        } else {
            out.emplace_back(
                "energy drift cannot be measured: the run has no positive time span, or its "
                "initial energy is zero and has no relative change to report");
        }
    } else {
        const double drift = *r.energy_drift;

        // A tolerance rather than an exact zero: the endpoints of a numerically integrated orbit
        // essentially never agree bit for bit, and a note printed for a 1e-16 wobble is a note that
        // trains the reader to ignore notes.
        constexpr double kNegligible = 1.0e-6;
        if (std::abs(drift) > kNegligible) {
            std::string message = "energy changed by " + as_percent(drift) + " over " +
                                  std::to_string(r.samples) + " samples";
            if (drift < 0.0) {
                // The mechanism, named. This is the note that stops a student attributing RK4's
                // dissipation to damping they did not add -- C8's whole reason for existing.
                message +=
                    ": the method loses energy, which is a property of the integrator and not of "
                    "the model unless the model has damping";
            } else {
                message +=
                    ": the method gains energy, which means the step is too large for this "
                    "frequency and the run is not trustworthy at this resolution";
            }
            out.push_back(std::move(message));
        }
    }

    // The frequency note. A report that assumed `omega = 1` and did not say so is a number the
    // reader cannot check, and the default is a real value rather than a sentinel precisely so this
    // note is the only thing standing between the reader and a meaningless figure.
    if (r.energy_drift.has_value() && !omega_declared_) {
        out.emplace_back(
            "the energy figure assumes omega = 1 rad/s because no frequency was declared; if the "
            "graph's potential is not quadratic with that frequency, the figure does not apply");
    }

    // Clamping is the one diagnostic that is never absent: the count came from the operator.
    if (r.clamps_fired > 0) {
        std::string message = "the operator clamped " + std::to_string(r.clamps_fired) +
                              " value(s); past the clamp the numbers are bounded rather than "
                              "correct, so this run's state is not the state the physics would give";
        out.push_back(std::move(message));
    }

    return out;
}

}  // namespace qp::views::model
