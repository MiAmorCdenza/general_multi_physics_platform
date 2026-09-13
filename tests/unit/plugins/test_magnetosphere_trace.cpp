/**
 * @file test_magnetosphere_trace.cpp
 * @brief Field lines: straight in a uniform field, closed in a dipole, and honest about where they stop.
 *
 * ## What these cases are for, and what they are not
 *
 * Every numerical method in this repository is checked against a case whose answer is known independently, and a
 * field-line tracer is the hardest kind to check that way: the curve has no closed form for any field worth
 * tracing. So the cases are built around **four exact statements**:
 *
 *   - a uniform field's lines are straight lines, so position, length and the fit are arithmetic;
 *   - a circular field's lines are circles, so the length is `2*pi*r` to the accuracy of the integrator -- and it
 *     is the case that shows the radial-reversal detector cannot see a closure at all;
 *   - a dipole's line through the equator at `L` has its **maximum radius at the equator** and reaches the
 *     surface, so the closest approach is exactly the surface radius because the foot point is placed there;
 *   - leaving the sampled box ends a line, which is measurable as "a smaller table gives a strictly shorter
 *     line" -- the statement that the clamp in the sampler never becomes data.
 *
 * What is deliberately absent is a case that checks the tracer against itself at a tighter tolerance: two
 * answers that agree with each other say nothing, which is the mistake that let a factor of 2209 survive two
 * rounds in this kit's own charge-to-mass conversion.
 */
#include <catch2/catch_test_macros.hpp>

#include <qp/plugins/magnetosphere/baked_field.hpp>
#include <qp/plugins/magnetosphere/dipole.hpp>
#include <qp/plugins/magnetosphere/trace.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

using namespace qp::plugins::magnetosphere;

namespace {

/// @brief A `BakedField` covering `+/- half_extent_re`, filled by evaluating `fill` at every node in metres.
///
/// The fill takes **metres**, because that is what a `BakedField` holds: the table is SI and the tracer converts
/// on every read, so a test that filled it in earth radii would be testing a unit convention rather than a curve.
[[nodiscard]] BakedField make_table(double half_extent_re, std::uint32_t nodes,
                                    const std::function<Vec3(const Vec3&)>& fill) {
    const double half_m = half_extent_re * kEarthRadiusM;
    const double spacing_m = 2.0 * half_m / static_cast<double>(nodes - 1);
    BakedField table{Vec3{-half_m, -half_m, -half_m}, Vec3{spacing_m, spacing_m, spacing_m}, nodes, nodes, nodes};
    for (std::uint32_t i = 0; i < nodes; ++i) {
        for (std::uint32_t j = 0; j < nodes; ++j) {
            for (std::uint32_t k = 0; k < nodes; ++k) {
                table.set_node(i, j, k, fill(table.node_position(i, j, k)));
            }
        }
    }
    return table;
}

/// @brief A table of one constant vector, in tesla.
[[nodiscard]] BakedField uniform_table(const Vec3& value, double half_extent_re = 3.0,
                                       std::uint32_t nodes = 13) {
    return make_table(half_extent_re, nodes, [&value](const Vec3&) { return value; });
}

/// @brief The real dipole, on a table big enough for the L shells these cases use.
[[nodiscard]] BakedField dipole_table(double half_extent_re = 8.0, std::uint32_t nodes = 65) {
    const DipoleField model{0.0, kDipoleMomentAm2};
    return make_table(half_extent_re, nodes, [&model](const Vec3& point) { return model.at(point); });
}

/// @brief The distance between two points.
[[nodiscard]] double distance(const Vec3& a, const Vec3& b) noexcept { return norm(a - b); }

}  // namespace

TEST_CASE("magnetosphere.trace.a_uniform_field_gives_a_straight_line", "[magnetosphere][trace]") {
    // The one case with an answer that can be written down without an integrator. `B` points along `+z` in a box
    // of `+/-6` earth radii, and the seed is four radii out along `x`, so the line is the straight segment from
    // `(4, 0, -6)` to `(4, 0, 6)`: length twelve, closest approach four, and every point on it at `x = 4`.
    const BakedField table = uniform_table(Vec3{0.0, 0.0, 1.0e-5}, 6.0, 25);
    const FieldTracer tracer{table.view(), table.origin(), table.spacing()};
    REQUIRE(tracer.usable());

    const FieldLine line = tracer.trace(Vec3{4.0, 0.0, 0.0});
    REQUIRE(line.usable());
    REQUIRE(line.stop == TraceStop::left_table);

    // Straightness, as the largest sideways excursion over the whole curve. A tracer with a sign error in one
    // Merson stage still produces a smooth-looking curve; it does not stay on the line.
    double worst_sideways = 0.0;
    double min_z = 0.0;
    double max_z = 0.0;
    for (const Vec3& point : line.points_re) {
        worst_sideways = std::max(worst_sideways, std::hypot(point.x - 4.0, point.y));
        min_z = std::min(min_z, point.z);
        max_z = std::max(max_z, point.z);
    }
    REQUIRE(worst_sideways < 1.0e-9);
    // It stops **inside** the sampled box, not on its boundary: the walk ends at the last point the table
    // describes rather than at whichever point happened to fall outside it, so the final point is at most one
    // step short of the face. Both halves are one step short, hence "length within two steps of twelve".
    REQUIRE(min_z > -6.0);
    REQUIRE(max_z < 6.0);
    REQUIRE(line.length_re > 12.0 - 2.0 * tracer.spec().step_max_re);
    REQUIRE(line.length_re < 12.0);
    // The seed is on the equator, so the closest approach is exactly the seed's own radius.
    REQUIRE(line.min_radius_re == 4.0);
    // The seed appears once, in the middle: the forward half starts at it and the backward half's copy is
    // dropped, so a curve traced both ways is not a curve with a duplicated point in it.
    std::size_t at_seed = 0;
    for (const Vec3& point : line.points_re) {
        if (distance(point, Vec3{4.0, 0.0, 0.0}) < 1.0e-12) ++at_seed;
    }
    REQUIRE(at_seed == 1);

    // One direction only is half the curve, and the stop reason is the same. Offered because a ray is what an
    // open polar line is, and because a caller debugging a stop reason wants one direction's own answer.
    const FieldLine half = tracer.trace(Vec3{4.0, 0.0, 0.0}, false);
    REQUIRE(half.usable());
    REQUIRE(half.length_re > 6.0 - tracer.spec().step_max_re);
    REQUIRE(half.length_re < 6.0);
}

TEST_CASE("magnetosphere.trace.a_dipole_line_returns_to_its_seed", "[magnetosphere][trace]") {
    // The picture a magnetosphere is drawn with, and the case with the sharpest available assertion: on a dipole,
    // the field line through the equatorial point at radius `L` has its **maximum radius at that point** (the line
    // is `r = L sin^2(theta)`), and it reaches the surface at the latitude where `sin^2(theta) = 1/L`. Four
    // statements follow, and every one of them is exact rather than approximate:
    //
    //   1. the seed is the maximum radius, so the seed's own radius is the curve's maximum;
    //   2. the closest approach is exactly the surface radius, because the foot point is *placed* there by
    //      interpolation rather than left wherever a step happened to land;
    //   3. the two halves end in opposite hemispheres, because the line is symmetric about the equator;
    //   4. the stop reason is `hit_surface` on both halves.
    const BakedField table = dipole_table();
    const FieldTracer tracer{table.view(), table.origin(), table.spacing()};
    REQUIRE(tracer.usable());

    const FieldLine line = tracer.trace(Vec3{5.0, 0.0, 0.0});
    REQUIRE(line.usable());
    REQUIRE(line.stop == TraceStop::hit_surface);
    REQUIRE(line.min_radius_re == tracer.spec().surface_radius_re);

    // Both ends are on the surface: the foot-point interpolation puts them there exactly, so this is an equality
    // and not a tolerance. A tracer that stopped at the first point inside the surface would read 0.98 here.
    REQUIRE(norm(line.points_re.front()) == tracer.spec().surface_radius_re);
    REQUIRE(norm(line.points_re.back()) == tracer.spec().surface_radius_re);
    // Opposite hemispheres, which is what "the line is symmetric about the equator" means for the two halves.
    REQUIRE(line.points_re.front().z * line.points_re.back().z < 0.0);

    // The seed is the widest point. Measured over the whole curve rather than asserted about the seed alone, so a
    // tracer that bulged past its seed on the way out would fail here.
    double widest = 0.0;
    for (const Vec3& point : line.points_re) widest = std::max(widest, norm(point));
    REQUIRE(widest == 5.0);

    // The line stays in the meridional plane it started in: a dipole with no tilt is axisymmetric, so a line
    // seeded on the `+x` axis never leaves the `y = 0` plane. A sign error in the rotation, or in the cross
    // product the dipole is built from, shows up as a curve that drifts out of the plane.
    for (const Vec3& point : line.points_re) REQUIRE(std::abs(point.y) < 1.0e-9);

    // The two sets of foot-point coordinates are the closed form's own numbers: at `r = 1`, `r = L sin^2(theta)`
    // puts the perpendicular distance from the axis at `sin(theta) = 1/sqrt(L) = 0.447214` and the axial distance
    // at `cos(theta) = 0.894427`. This kit's measured foot points are `(0.44781, 0, +/-0.89413)` -- agreeing to
    // four digits, which is the interpolation of a 0.25 R_E table talking, not the integrator.
    //
    // The line is also **symmetric about the equator**, which is what makes the picture a magnetosphere rather
    // than a lopsided blob.
    const Vec3 north = line.points_re.front().z > 0.0 ? line.points_re.front() : line.points_re.back();
    const Vec3 south = line.points_re.front().z > 0.0 ? line.points_re.back() : line.points_re.front();
    REQUIRE(std::abs(north.x - south.x) < 1.0e-9);
    REQUIRE(std::abs(north.z + south.z) < 1.0e-9);
    REQUIRE(std::abs(std::hypot(north.x, north.y) - std::sqrt(1.0 / 5.0)) < 2.0e-3);
    REQUIRE(std::abs(north.z - std::sqrt(1.0 - 1.0 / 5.0)) < 2.0e-3);
}

TEST_CASE("magnetosphere.trace.a_closed_line_is_reported_as_looped", "[magnetosphere][trace]") {
    // A **circular** field: `B = z_hat x r`, so every line is a circle about the `z` axis and the answer is
    // `2*pi*r`. This is the case that shows the radial-reversal detector is not enough on its own: the radius is
    // constant along a circle, so there is no reversal to count, and `dr` is a difference of nearly equal numbers
    // whose sign is rounding noise -- a counter that read every sign change would call this curve closed after
    // five steps for the wrong reason, and one with a deadband but no closure test would run it to `max_points`
    // around a loop it had already finished.
    const BakedField table = make_table(
        3.0, 25, [](const Vec3& p) { return Vec3{-p.y, p.x, 0.0} * 1.0e-6; });
    const FieldTracer tracer{table.view(), table.origin(), table.spacing()};

    const FieldLine line = tracer.trace(Vec3{2.0, 0.0, 0.0}, false);
    REQUIRE(line.usable());
    REQUIRE(line.stop == TraceStop::looped);
    // The circumference, to the accuracy the closure test can give: the walk ends at the first point that comes
    // back within **half a step** of the seed, so the length is short of the perimeter by at most that -- 0.1 of
    // 12.57, under a percent. The bound is asserted rather than a bare tolerance, because "short by less than the
    // closure threshold" is the actual claim the implementation makes.
    const double circumference = 2.0 * 3.14159265358979323846 * 2.0;
    // Measured at 1.0022 of the exact perimeter, and the residual has two causes that this bound covers together:
    // the closure test stops the walk within half a step of the seed, which makes the length **short**, and the
    // integrator's orbit drifts outward -- a traced radius of 2.0045 after one lap accounts for the whole excess,
    // which is what a fifth-order step of 0.2 with a 1e-4 tolerance actually delivers on a circle. Asserting the
    // total rather than either part, because the two act in opposite directions and a bound on one would be a
    // bound on nothing.
    REQUIRE(std::abs(line.length_re - circumference) / circumference < 0.01);
    // A circle keeps its radius, which is the property that made the reversal counter useless here.
    REQUIRE(std::abs(line.min_radius_re - 2.0) < 1.0e-3);
    for (const Vec3& point : line.points_re) REQUIRE(std::abs(point.z) < 1.0e-6);
}

TEST_CASE("magnetosphere.trace.a_zero_field_stops_before_it_starts", "[magnetosphere][trace]") {
    // No field, no direction, no curve. The reference implementation's version of this case is recorded in
    // `trace.cpp`: a zero field makes every Merson stage zero, so the error estimate is zero, so the step grows to
    // its cap while the position never changes and the walk appends duplicate points until its budget runs out.
    // Here it is a stop reason, and the honest answer is a line with nothing in it.
    const BakedField table = uniform_table(Vec3{0.0, 0.0, 0.0});
    const FieldTracer tracer{table.view(), table.origin(), table.spacing()};
    REQUIRE(tracer.usable());

    const FieldLine line = tracer.trace(Vec3{2.0, 0.0, 0.0});
    REQUIRE(line.stop == TraceStop::no_field);
    REQUIRE(line.points_re.empty());
    REQUIRE_FALSE(line.usable());

    // A tracer with nothing to trace answers the same way rather than crashing: an unreadable `FieldValue` -- the
    // shape an absent bake has -- is not usable, and every seed produces an empty line.
    const qp::graph::field::FieldValue nothing{};
    const FieldTracer blind{nothing, Vec3{}, Vec3{1.0, 1.0, 1.0}};
    REQUIRE_FALSE(blind.usable());
    REQUIRE_FALSE(blind.trace(Vec3{2.0, 0.0, 0.0}).usable());

    // And a spec whose numbers cannot run is refused at construction, so no trace ever divides by a zero
    // tolerance or loops forever on a zero budget.
    TraceSpec bad;
    bad.tolerance = 0.0;
    REQUIRE_FALSE(bad.usable());
    const FieldTracer refuses{table.view(), table.origin(), table.spacing(), bad};
    REQUIRE_FALSE(refuses.usable());
}

TEST_CASE("magnetosphere.trace.the_step_cap_does_not_decide_the_shape", "[magnetosphere][trace]") {
    // The claim that makes `step_max_re` a drawing parameter rather than an accuracy one, and it is measurable:
    // the **same** curve traced with caps two orders of magnitude apart must land in the same place, because what
    // decides the shape is the tolerance and the method's own error control. What changes with the cap is the
    // cost, and the sample counter is what says so.
    const BakedField table = dipole_table();
    const Vec3 seed{5.0, 0.0, 0.0};

    std::vector<Vec3> feet;
    std::vector<std::uint64_t> costs;
    for (const double cap : {2.0, 0.4, 0.05}) {
        TraceSpec spec;
        spec.step_max_re = cap;
        const FieldTracer tracer{table.view(), table.origin(), table.spacing(), spec};
        const FieldLine line = tracer.trace(seed);
        REQUIRE(line.usable());
        REQUIRE(line.stop == TraceStop::hit_surface);
        feet.push_back(line.points_re.front());
        costs.push_back(tracer.samples());
        REQUIRE(tracer.samples() > 0);
        tracer.reset_samples();
        REQUIRE(tracer.samples() == 0);
    }

    // The foot points agree. A tenth of an earth radius is generous next to the tolerance of 1e-4, and the point
    // of the assertion is the order of magnitude: a cap that leaked into the answer would move the foot point by
    // a large fraction of the cap, which is 0.05 to 2 radii here.
    for (std::size_t i = 1; i < feet.size(); ++i) {
        REQUIRE(distance(feet[0], feet[i]) < 0.1);
    }
    // The cost does not agree: a finer cap means more field reads, which is the trade the caller is making. This
    // is asserted rather than assumed because a cap that had no effect on the number of reads would mean the
    // growth rule was ignoring it.
    REQUIRE(costs[1] > costs[0]);
    REQUIRE(costs[2] > costs[1]);
    // Five stages a step, so the reads are a few times the number of points -- the arithmetic a report quotes
    // when it says what a picture cost.
    REQUIRE(costs[2] > 100);
}

TEST_CASE("magnetosphere.trace.leaving_the_table_stops_the_line", "[magnetosphere][trace]") {
    // The lesson the reference implementation documents at length and the reason the stop reason exists: the
    // sampler **clamps** outside the grid (deliberately -- see `baked_field.hpp`), so a trace that kept going past
    // the last sample would integrate a constant field and draw a long straight line that looks like data. The
    // assertion is not "the line is shorter than the box": it is that **a smaller table gives a strictly shorter
    // line**, which is what distinguishes a trace that stopped from one that continued on clamped values.
    const Vec3 north{0.0, 0.0, 1.0e-5};
    const BakedField wide = uniform_table(north, 3.0, 13);
    const BakedField narrow = uniform_table(north, 1.0, 9);

    const FieldTracer wide_tracer{wide.view(), wide.origin(), wide.spacing()};
    const FieldTracer narrow_tracer{narrow.view(), narrow.origin(), narrow.spacing()};
    const FieldLine long_line = wide_tracer.trace(Vec3{0.0, 0.0, 0.0});
    const FieldLine short_line = narrow_tracer.trace(Vec3{0.0, 0.0, 0.0});

    REQUIRE(long_line.stop == TraceStop::left_table);
    REQUIRE(short_line.stop == TraceStop::left_table);
    REQUIRE(short_line.length_re < long_line.length_re);
    // Each is within one step of twice its box, which is the quantitative form of the same statement.
    REQUIRE(long_line.length_re > 2.0 * 3.0 - 2.0 * wide_tracer.spec().step_max_re);
    REQUIRE(short_line.length_re < 2.0 * 1.0);

    // Every point is inside the sampled box, which is the invariant that makes "leaving" a statement about the
    // samples rather than about a sphere a caller chose.
    for (const Vec3& point : long_line.points_re) {
        REQUIRE(point.z >= wide_tracer.box_min_re().z);
        REQUIRE(point.z <= wide_tracer.box_max_re().z);
    }
    REQUIRE(wide_tracer.box_min_re().z == -3.0);
    REQUIRE(wide_tracer.box_max_re().z == 3.0);
    REQUIRE(wide_tracer.box_min_re().x == -3.0);

    // A seed outside the samples is not traced at all: there is no field there to follow, and the answer is an
    // empty line rather than a curve that starts from a clamped value.
    const FieldLine outside = wide_tracer.trace(Vec3{9.0, 0.0, 0.0});
    REQUIRE(outside.stop == TraceStop::left_table);
    REQUIRE(outside.points_re.empty());

    // The stop reasons are named, and the names are total -- a report that printed an empty string for one of
    // them would be a report a reader cannot act on.
    REQUIRE(std::string{to_string(TraceStop::hit_surface)} == "hit_surface");
    REQUIRE(std::string{to_string(TraceStop::left_table)} == "left_table");
    REQUIRE(std::string{to_string(TraceStop::looped)} == "looped");
    REQUIRE(std::string{to_string(TraceStop::max_points)} == "max_points");
    REQUIRE(std::string{to_string(TraceStop::no_field)} == "no_field");
}
