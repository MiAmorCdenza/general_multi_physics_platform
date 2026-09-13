/**
 * @file fit_session.cpp
 * @brief The three states of a reading, turned into points and named exclusions.
 *
 * The header carries the reasoning; this file is the walk over the samples. It is short on purpose, and the
 * shortness is the point: the decision this module makes is one `switch` over `UncertaintyKind` plus one over
 * finiteness, and every branch that could have been a default is written out so that adding a fourth state to
 * `UncertaintyKind` fails to compile here rather than silently becoming "no weight".
 */
#include <qp/views/model/fit_session.hpp>

#include <cmath>

namespace qp::views::model {

namespace {

namespace rt = qp::runtime;

}  // namespace

const char* to_string(FitExclusion reason) noexcept {
    switch (reason) {
        case FitExclusion::uncertainty_unknown: return "uncertainty_unknown";
        case FitExclusion::uncertainty_zero: return "uncertainty_zero";
        case FitExclusion::value_not_finite: return "value_not_finite";
    }
    return "unknown";
}

const char* to_string(FitRefusal reason) noexcept {
    switch (reason) {
        case FitRefusal::no_channel: return "no_channel";
        case FitRefusal::channel_not_found: return "channel_not_found";
        case FitRefusal::no_samples: return "no_samples";
        case FitRefusal::not_enough_points: return "not_enough_points";
    }
    return "unknown";
}

std::size_t FitReport::excluded_count() const noexcept {
    std::size_t total = 0;
    for (const std::pair<FitExclusion, std::size_t>& entry : excluded) total += entry.second;
    return total;
}

std::size_t FitReport::excluded_count(FitExclusion reason) const noexcept {
    for (const std::pair<FitExclusion, std::size_t>& entry : excluded) {
        if (entry.first == reason) return entry.second;
    }
    return 0;
}

std::size_t FitReport::degrees_of_freedom() const noexcept {
    const std::size_t parameters = request.degree + 1;
    return points.size() > parameters ? points.size() - parameters : 0;
}

void FitSession::choose(FitRequest request) noexcept {
    if (request.degree > kMaximumFitDegree) request.degree = kMaximumFitDegree;
    request_ = std::move(request);
}

std::optional<std::size_t> FitSession::channel_index(const std::string& name) const noexcept {
    if (name.empty()) return std::nullopt;
    const std::vector<rt::Channel>& channels = trace_->channels();
    for (std::size_t i = 0; i < channels.size(); ++i) {
        if (channels[i].name == name) return i;
    }
    return std::nullopt;
}

FitReport FitSession::report() const {
    FitReport out;
    out.request = request_;
    out.sample_count = trace_->size();

    // The refusals are decided in the order a reader would ask the questions, so the reason reported is the
    // first one that applies rather than whichever check happened to run last.
    if (request_.channel.empty()) {
        out.refusal = FitRefusal::no_channel;
        return out;
    }
    const std::optional<std::size_t> column = channel_index(request_.channel);
    if (!column.has_value()) {
        out.refusal = FitRefusal::channel_not_found;
        return out;
    }

    // The dimension of the fitted **ordinate**, taken from the channel rather than from the request: a channel's
    // dimension is a fact about the trace, and a caller-supplied one would be a claim about it.
    //
    // Set **before** the empty-trace check, which is a correction rather than a style: the first version read it
    // after, so an empty trace reported "no samples" with a dimensionless channel and a caller could not tell a
    // trace that had not run yet from one whose channel this build cannot describe. The case that caught it
    // asserted the dimension of a report with no samples, which is the one report a hurried implementation is
    // most likely to leave half-filled.
    out.dim = trace_->channels()[*column].dim;

    if (trace_->empty()) {
        out.refusal = FitRefusal::no_samples;
        return out;
    }

    // Counted rather than pushed per occurrence: a trace of ten thousand samples with no stated uncertainty would
    // otherwise build a ten-thousand-entry list of the same sentence, and the report only ever shows the count.
    std::size_t unknown = 0;
    std::size_t zero = 0;
    std::size_t non_finite = 0;

    for (const rt::Sample& sample : trace_->samples()) {
        if (*column >= sample.values.size()) {
            // A sample one value short is a malformed trace rather than a reading, and the trace's own
            // `is_consistent` refuses to call it consistent. Counted as unusable, because a fit point without a
            // number is not a point.
            ++non_finite;
            continue;
        }
        const rt::UncertainValue& reading = sample.values[*column];
        if (!std::isfinite(reading.value) || !std::isfinite(sample.t) || !std::isfinite(reading.u)) {
            ++non_finite;
            continue;
        }
        switch (reading.kind) {
            case rt::UncertaintyKind::standard:
                if (reading.u <= 0.0) {
                    // A quantified uncertainty that is zero: the same situation as `exact` below, reached
                    // through the other state, and it has to be refused for the same reason. `1/0^2` is not a
                    // large weight, it is an infinite one.
                    ++zero;
                    continue;
                }
                break;
            case rt::UncertaintyKind::unknown:
                ++unknown;
                continue;
            case rt::UncertaintyKind::exact:
                // A counted or defined quantity. Its uncertainty really is zero, and a fit cannot use it: an
                // infinite weight would make this one reading the whole answer. Refused here rather than
                // converted to a small number, because a small number would be a claim about the instrument.
                ++zero;
                continue;
        }

        FitPoint point;
        point.x = sample.t;
        point.y = reading.value;
        point.sigma = reading.u;
        out.points.push_back(point);
    }

    const std::pair<FitExclusion, std::size_t> counts[] = {
        {FitExclusion::uncertainty_unknown, unknown},
        {FitExclusion::uncertainty_zero, zero},
        {FitExclusion::value_not_finite, non_finite},
    };
    for (const std::pair<FitExclusion, std::size_t>& entry : counts) {
        if (entry.second != 0) out.excluded.push_back(entry);
    }

    if (!degree_is_fittable(out.points.size(), request_.degree)) {
        out.refusal = FitRefusal::not_enough_points;
    }
    return out;
}

std::string fit_coefficient_name(std::size_t index) {
    // The rule the panel used before this moved here, kept letter for letter: `a` for a line's intercept, `b` for its
    // slope, and past `z` the counted form rather than the character after it. A name that depended on how many
    // coefficients happen to exist would make two sessions' files disagree about what `b` is.
    if (index > 25) return "c" + std::to_string(index);
    return std::string(1, static_cast<char>('a' + index));
}

}  // namespace qp::views::model
