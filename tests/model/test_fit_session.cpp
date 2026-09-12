/**
 * @file test_fit_session.cpp
 * @brief Tests for what a fit is run on: the points, and the readings it refuses to invent a weight for.
 *
 * Test case ids match the @tests fields in `views/model/fit_session.hpp` byte for byte.
 *
 * ## What is worth asserting here, given that the fit itself is elsewhere
 *
 * Not that a gradient comes out right -- `plugins/analysis` owns that, and `tests/unit/plugins/test_analysis.cpp`
 * measures it against closed-form answers. What is worth asserting here is the **selection**: a weighted fit is
 * only as honest as the weights it was handed, and the three states of `UncertaintyKind` have three different
 * answers that are easy to collapse into one:
 *
 *   - a reading with a quantified uncertainty becomes a point carrying **that** number;
 *   - a reading nobody quantified is excluded **by name**, because a default sigma would be a claim about the
 *     instrument that nothing measured;
 *   - a reading that claims to know itself exactly is excluded too, and for the opposite reason: its weight
 *     would be infinite, and one such point would be the whole answer.
 *
 * The last two are the cases a hurried implementation gets wrong in opposite directions, which is why they are
 * separate cases rather than one.
 */
#include <catch2/catch_test_macros.hpp>

#include <qp/views/model/fit_session.hpp>

#include <qp/runtime/run/run.hpp>
#include <qp/runtime/store/store.hpp>
#include <qp/runtime/trace/trace.hpp>
#include <qp/units/dimensions.hpp>

#include <cmath>
#include <cstddef>
#include <limits>
#include <string>
#include <utility>

using namespace qp::views::model;

namespace {

namespace rt = qp::runtime;

/// @brief A trace with one channel, ready for samples. Fails the case rather than returning a broken trace.
rt::Trace one_channel_trace(const char* name, qp::units::Dim dim) {
    rt::Trace trace{rt::RunId{}};
    const auto added = trace.add_channel(rt::Channel{name, dim, {}});
    REQUIRE(added.has_value());
    return trace;
}

/// @brief A reading that carries a quantified uncertainty.
rt::UncertainValue measured(double value, double u) {
    return rt::UncertainValue::measured(value, u);
}

/// @brief The names of the exclusions a report lists, in the order it lists them.
std::string exclusion_names(const FitReport& report) {
    std::string out;
    for (const std::pair<FitExclusion, std::size_t>& entry : report.excluded) {
        if (!out.empty()) out += ",";
        out += to_string(entry.first);
    }
    return out;
}

}  // namespace

TEST_CASE("fit.session.points_come_from_a_named_channel", "[fit]") {
    // The ordinary case: a channel sampled against time becomes (t, y, sigma) triples in trace order, and the
    // dimension reported is the **channel's** rather than anything the caller claimed.
    rt::Trace trace = one_channel_trace("displacement", qp::units::dims::length);
    REQUIRE(trace.append(0.0, measured(0.10, 0.01)).has_value());
    REQUIRE(trace.append(1.0, measured(0.20, 0.01)).has_value());
    REQUIRE(trace.append(2.0, measured(0.30, 0.01)).has_value());
    REQUIRE(trace.append(3.0, measured(0.40, 0.01)).has_value());

    FitSession session{trace};
    // Nothing is asked for until a caller asks: an empty request is a refusal by name rather than a default
    // channel, because a session that silently picked "the first channel" would fit something nobody chose.
    REQUIRE(session.request().channel.empty());
    REQUIRE(session.request().degree == 1);
    REQUIRE(&session.trace() == &trace);

    const FitReport unfitted = session.report();
    REQUIRE_FALSE(unfitted.fittable());
    REQUIRE(unfitted.refusal == FitRefusal::no_channel);
    REQUIRE(unfitted.points.empty());
    REQUIRE(unfitted.sample_count == 4);

    FitRequest request;
    request.channel = "displacement";
    request.degree = 1;
    session.choose(request);
    REQUIRE(session.request() == request);

    const FitReport report = session.report();
    REQUIRE(report.fittable());
    REQUIRE_FALSE(report.refusal.has_value());
    REQUIRE(report.sample_count == 4);
    REQUIRE(report.points.size() == 4);
    REQUIRE(report.excluded.empty());
    REQUIRE(report.excluded_count() == 0);
    REQUIRE(report.dim == qp::units::dims::length);
    REQUIRE(report.degrees_of_freedom() == 2);

    // Trace order, and the abscissa is the time column rather than the sample index. Those differ the moment a
    // run has an adaptive step, and a line fitted against indices would then have the wrong slope with nothing
    // on screen saying so.
    REQUIRE(report.points[0].x == 0.0);
    REQUIRE(report.points[1].x == 1.0);
    REQUIRE(report.points[3].x == 3.0);
    REQUIRE(report.points[3].y == 0.40);
    REQUIRE(report.points[3].sigma == 0.01);

    // A non-uniform sampling keeps its own abscissae, which is the assertion that would fail if the index were
    // used: the third sample's time is 5.0 and its index is 2.
    rt::Trace uneven{rt::RunId{}};
    REQUIRE(uneven.add_channel(rt::Channel{"x", qp::units::dims::length, {}}).has_value());
    REQUIRE(uneven.append(0.0, measured(0.0, 0.01)).has_value());
    REQUIRE(uneven.append(1.0, measured(0.1, 0.01)).has_value());
    REQUIRE(uneven.append(5.0, measured(0.5, 0.01)).has_value());
    REQUIRE(uneven.append(6.0, measured(0.6, 0.01)).has_value());
    // Its own request, naming **its** channel: `request` above names "displacement", and reusing it here was the
    // first version of this case. It failed with `points = 0`, which is what a channel the trace does not have
    // looks like -- and it is worth keeping the reason visible rather than fixing it silently, because the two
    // refusals (`channel_not_found` and `not_enough_points`) are exactly what a report must be able to tell apart.
    FitSession uneven_session{uneven};
    uneven_session.choose(FitRequest{"x", 1, false});
    const FitReport uneven_report = uneven_session.report();
    REQUIRE(uneven_report.fittable());
    REQUIRE(uneven_report.points.size() == 4);
    REQUIRE(uneven_report.points[0].x == 0.0);
    REQUIRE(uneven_report.points[2].x == 5.0);
    REQUIRE(uneven_report.points[2].x != 2.0);
}

TEST_CASE("fit.session.an_unknown_uncertainty_is_excluded", "[fit]") {
    // The rule the whole platform is built on, one layer down: a reading nobody quantified is **not** a reading
    // with a small uncertainty. Giving it a default sigma would put a number about the plotted axis's scale into
    // the weights -- and the fitted gradient would then depend on whether the trace was in metres or millimetres,
    // which is a claim about the data that no measurement supports.
    rt::Trace trace = one_channel_trace("voltage", qp::units::dims::voltage);
    REQUIRE(trace.append(0.0, measured(0.001, 1.0e-5)).has_value());
    REQUIRE(trace.append(1.0, rt::UncertainValue::unquantified(0.002)).has_value());
    REQUIRE(trace.append(2.0, measured(0.003, 1.0e-5)).has_value());
    REQUIRE(trace.append(3.0, rt::UncertainValue::unquantified(0.004)).has_value());
    REQUIRE(trace.append(4.0, measured(0.005, 1.0e-5)).has_value());

    FitSession session{trace};
    session.choose(FitRequest{"voltage", 1, false});
    const FitReport report = session.report();

    // Three points and a line's two parameters leaves one degree of freedom, so this **is** fittable -- and it
    // is, which is the half worth asserting: five samples of which two were never quantified still leave a line
    // worth drawing. The exclusions are surfaced, not fatal.
    REQUIRE(report.fittable());
    REQUIRE(report.points.size() == 3);
    REQUIRE(report.degrees_of_freedom() == 1);
    REQUIRE(report.excluded_count() == 2);
    REQUIRE(report.excluded_count(FitExclusion::uncertainty_unknown) == 2);
    REQUIRE(report.excluded_count(FitExclusion::uncertainty_zero) == 0);
    REQUIRE(report.excluded_count(FitExclusion::value_not_finite) == 0);
    REQUIRE(exclusion_names(report) == "uncertainty_unknown");
    // Every sample is accounted for: a point, or a named exclusion. Nothing is silently dropped.
    REQUIRE(report.points.size() + report.excluded_count() == report.sample_count);

    // And the reason is spelled out rather than left as a count, because the two are different instructions to
    // the reader: this one says "go and quantify your readings", the next case's says "this quantity is counted".
    REQUIRE(std::string{to_string(FitExclusion::uncertainty_unknown)} == "uncertainty_unknown");

    // A weighted mean of the three survivors needs one parameter and so has a degree of freedom: the same data
    // is fittable at degree 0, which is the assertion that shows the refusal came from the degrees of freedom
    // rather than from the exclusions.
    session.choose(FitRequest{"voltage", 0, false});
    const FitReport mean = session.report();
    REQUIRE(mean.fittable());
    REQUIRE(mean.degrees_of_freedom() == 2);
}

TEST_CASE("fit.session.an_exact_reading_has_no_weight", "[fit]") {
    // The other direction, and the one that catches a different mistake. A counted quantity -- twelve periods,
    // three trials, a defined constant -- really does have zero uncertainty, and `UncertaintyKind::exact` says
    // so truthfully. It still cannot be a fit point: `w = 1/sigma^2` is infinite, and one such reading would be
    // the entire answer while the fit reported a perfect chi-squared.
    //
    // Refusing it here rather than substituting a small sigma is the same decision as the previous case, reached
    // from the opposite side, and a reader who expects "exact means best" has to be told which way it goes.
    rt::Trace trace = one_channel_trace("period", qp::units::dims::time);
    REQUIRE(trace.append(0.0, rt::UncertainValue::exact(1.0)).has_value());
    REQUIRE(trace.append(1.0, measured(1.1, 0.01)).has_value());
    REQUIRE(trace.append(2.0, rt::UncertainValue::exact(1.2)).has_value());
    REQUIRE(trace.append(3.0, measured(1.3, 0.01)).has_value());
    REQUIRE(trace.append(4.0, measured(1.4, 0.01)).has_value());
    // A quantified uncertainty of zero is the same situation reached through `standard`: the state says the
    // error was measured and the number says it is nothing. `1/0^2` does not care which enumerator it came from.
    REQUIRE(trace.append(5.0, rt::UncertainValue::measured(1.5, 0.0)).has_value());

    FitSession session{trace};
    session.choose(FitRequest{"period", 1, false});
    const FitReport report = session.report();

    REQUIRE(report.fittable());
    REQUIRE(report.points.size() == 3);
    REQUIRE(report.degrees_of_freedom() == 1);
    // Both spellings of "no weight" are counted under one reason, which is why the reason names the situation
    // rather than the enumerator.
    REQUIRE(report.excluded_count(FitExclusion::uncertainty_zero) == 3);
    REQUIRE(report.excluded_count(FitExclusion::uncertainty_unknown) == 0);
    REQUIRE(exclusion_names(report) == "uncertainty_zero");
    // A point's sigma is positive by construction, so nothing downstream can be handed an infinite weight.
    for (const FitPoint& point : report.points) REQUIRE(point.sigma > 0.0);
}

TEST_CASE("fit.session.no_channel_is_refused_by_name", "[fit]") {
    // Three refusals a caller will meet in ordinary use, each with its own name because each has its own fix.
    // A single "cannot fit" would make a typo in a channel name indistinguishable from an empty trace.
    rt::Trace trace = one_channel_trace("displacement", qp::units::dims::length);
    FitSession session{trace};

    const FitReport nothing = session.report();
    REQUIRE(nothing.refusal == FitRefusal::no_channel);
    REQUIRE(std::string{to_string(FitRefusal::no_channel)} == "no_channel");

    session.choose(FitRequest{"velocity", 1, false});
    const FitReport wrong_name = session.report();
    REQUIRE(wrong_name.refusal == FitRefusal::channel_not_found);
    REQUIRE(std::string{to_string(FitRefusal::channel_not_found)} == "channel_not_found");
    REQUIRE(wrong_name.points.empty());

    session.choose(FitRequest{"displacement", 1, false});
    const FitReport no_samples = session.report();
    REQUIRE(no_samples.refusal == FitRefusal::no_samples);
    REQUIRE(std::string{to_string(FitRefusal::no_samples)} == "no_samples");
    // The channel was found, so its dimension is reported even though there is nothing to fit: the report says
    // what it was asked about, and "no samples yet" is a different statement from "no such channel".
    REQUIRE(no_samples.dim == qp::units::dims::length);
    REQUIRE(no_samples.sample_count == 0);

    // A sample whose number is not a number is not a point. An infinity in a fit is the classic way a report
    // acquires a plausible-looking coefficient computed from nothing.
    REQUIRE(trace.append(0.0, measured(0.0, 0.01)).has_value());
    REQUIRE(trace.append(1.0, measured(std::nan(""), 0.01)).has_value());
    REQUIRE(trace.append(2.0, measured(std::numeric_limits<double>::infinity(), 0.01)).has_value());
    REQUIRE(trace.append(3.0, measured(0.3, std::nan(""))).has_value());
    const FitReport broken = session.report();
    REQUIRE(broken.excluded_count(FitExclusion::value_not_finite) == 3);
    REQUIRE(broken.excluded_count() == 3);
    REQUIRE(broken.points.size() == 1);
    REQUIRE(broken.points.size() + broken.excluded_count() == broken.sample_count);
    // One surviving point is not a line, and the refusal says which of the two reasons applies.
    REQUIRE_FALSE(broken.fittable());
    REQUIRE(broken.refusal == FitRefusal::not_enough_points);
}

TEST_CASE("fit.session.degrees_of_freedom_decide_eligibility", "[fit]") {
    // A fit with as many parameters as points passes through every point and says nothing, so it is **not** a
    // fit. This is the boundary the whole "is a fit worth showing" question lives on, and it is asserted at the
    // predicate rather than only through a session: a caller asking "would a quadratic work here" has not built
    // one yet.
    REQUIRE_FALSE(degree_is_fittable(0, 1));
    REQUIRE_FALSE(degree_is_fittable(2, 1));
    REQUIRE(degree_is_fittable(3, 1));
    REQUIRE_FALSE(degree_is_fittable(3, 2));
    REQUIRE(degree_is_fittable(4, 2));
    // Degree 0 is a weighted mean, and it needs two points for the same reason: one parameter against one point
    // has no freedom left.
    REQUIRE_FALSE(degree_is_fittable(1, 0));
    REQUIRE(degree_is_fittable(2, 0));
    // The bound on the degree is the point where a Vandermonde column stops being representable, and it is a
    // refusal rather than a clamp here -- `choose` clamps, this asks.
    REQUIRE(degree_is_fittable(kMaximumFitDegree + 2, kMaximumFitDegree));
    REQUIRE_FALSE(degree_is_fittable(100, kMaximumFitDegree + 1));

    // `choose` clamps rather than refusing, and the report carries the degree that was **used**, so a caller
    // cannot be left guessing which of the two numbers describes the fit it is looking at.
    rt::Trace trace = one_channel_trace("displacement", qp::units::dims::length);
    for (int i = 0; i < 8; ++i) {
        REQUIRE(trace.append(static_cast<double>(i), measured(0.1 * i, 0.01)).has_value());
    }
    FitSession session{trace};
    session.choose(FitRequest{"displacement", kMaximumFitDegree + 5, false});
    REQUIRE(session.request().degree == kMaximumFitDegree);
    const FitReport report = session.report();
    REQUIRE(report.request.degree == kMaximumFitDegree);
    // Eight points against thirteen parameters: refused, and the count says why.
    REQUIRE_FALSE(report.fittable());
    REQUIRE(report.refusal == FitRefusal::not_enough_points);
    REQUIRE(report.degrees_of_freedom() == 0);
    REQUIRE(std::string{to_string(FitRefusal::not_enough_points)} == "not_enough_points");

    // A cubic through eight points has four degrees of freedom, and the count is the one a reduced chi-squared
    // will divide by -- so it is asserted here rather than left to the panel to recompute.
    session.choose(FitRequest{"displacement", 3, true});
    const FitReport cubic = session.report();
    REQUIRE(cubic.fittable());
    REQUIRE(cubic.degrees_of_freedom() == 4);
    // The covariance convention is carried, not chosen: this layer does not decide which question is being asked.
    REQUIRE(cubic.request.scale_covariance);
    REQUIRE(cubic.request == session.request());
}
