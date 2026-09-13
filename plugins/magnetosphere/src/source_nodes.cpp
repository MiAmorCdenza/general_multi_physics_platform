/**
 * @file source_nodes.cpp
 * @brief The driver's arithmetic and its type table: two formulas and one number.
 *
 * There is no evaluator here. A driver publishes a `ports::Value` and nothing else -- no table, no geometry, no
 * domain -- so the field evaluator's `evaluate` answers for it with one line, which is the whole of what a driver
 * costs this kit.
 */
#include <qp/plugins/magnetosphere/source_nodes.hpp>

#include <qp/ports/port_type.hpp>
#include <qp/ports/value.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

namespace qp::plugins::magnetosphere {

namespace {

namespace graph = qp::graph;

/// @brief The index a node carries, before clamping: the parameter, or the default when it has none.
[[nodiscard]] double raw_kp(const graph::Node& node) noexcept {
    const qp::ports::Value value = node.param(SourceNodes::kPortKp);
    if (!value.valid()) return SourceNodes::kDefaultKp;
    const double kp = value.to_double();
    return std::isfinite(kp) ? kp : SourceNodes::kDefaultKp;
}

/// @brief The day a node carries, before clamping: the parameter, or the default when it has none.
[[nodiscard]] double raw_day(const graph::Node& node) noexcept {
    const qp::ports::Value value = node.param(SourceNodes::kPortDay);
    if (!value.valid()) return SourceNodes::kDefaultDay;
    const double day = value.to_double();
    return std::isfinite(day) ? day : SourceNodes::kDefaultDay;
}

}  // namespace

double SourceNodes::tilt_degrees_for_day(double day) noexcept {
    const double clamped = std::clamp(day, kMinDay, kMaxDay);
    const double phase = 2.0 * 3.14159265358979323846 * (clamped - kDefaultDay) / kDaysPerYear;
    return kDayTiltOffsetDegrees + kObliquityDegrees * std::cos(phase);
}

double SourceNodes::read_day(const graph::Node& node) noexcept {
    return std::clamp(raw_day(node), kMinDay, kMaxDay);
}

double SourceNodes::standoff_re_for_kp(double kp) noexcept {
    const double clamped = std::clamp(kp, kMinKp, kMaxKp);
    const double pdyn = 2.0 + clamped * 0.5;
    // `pdyn` is at least 2 by construction, so the cube root is of a positive number and the result is finite.
    return 10.0 / std::cbrt(pdyn);
}

double SourceNodes::flaring_for_kp(double kp) noexcept {
    const double clamped = std::clamp(kp, kMinKp, kMaxKp);
    return 0.55 + clamped * 0.02;
}

double SourceNodes::lobe_field_t_for_kp(double kp) noexcept {
    const double clamped = std::clamp(kp, kMinKp, kMaxKp);
    // Nanotesla on the way in, tesla on the way out: the reference writes `30 + 5 Kp` and means nanotesla, and the
    // port the value eventually lands on says `T`, so the conversion happens once, here, where the relation is
    // written down. A consumer that converted again would be wrong by nine orders of magnitude and would still
    // produce a smooth tail.
    return (kReferenceTailLobeBaseNt + clamped * kReferenceTailLobePerKpNt) * 1.0e-9;
}

double SourceNodes::tail_bz_t_for_kp(double kp) noexcept {
    const double clamped = std::clamp(kp, kMinKp, kMaxKp);
    return (kReferenceTailBzBaseNt + clamped * kReferenceTailBzPerKpNt) * 1.0e-9;
}

double SourceNodes::read_kp(const graph::Node& node) noexcept {
    return std::clamp(raw_kp(node), kMinKp, kMaxKp);
}

std::vector<graph::NodeDesc> SourceNodes::node_types() {
    graph::NodeDesc kp;
    kp.type_name = kKpType;
    kp.label = "Kp index";
    kp.description = "The planetary K index, as a number on a wire. One driver feeding several models is the point: "
                     "wire it into a magnetopause and the standoff distance and the flaring both move together, "
                     "instead of each node being told the same day by hand.";
    kp.category = "source";
    kp.version = 1;
    // A field-domain node: it is evaluated with the field graph, and it declares no geometry because a number has
    // none. It is **not** a field table, which is why it publishes `kScalarF64` rather than `kScalarField`.
    kp.allow_in_field_domain = true;
    kp.allow_in_particle_domain = false;
    kp.has_compute = true;

    graph::PortDesc index;
    index.number = kPortKp;
    index.name = "kp";
    index.label = "Kp";
    index.description = "The index, 0 to 9. Two is a quiet-to-moderate day and nine is a severe storm; the value is "
                        "clamped into that range rather than refused, so a node being edited still evaluates.";
    index.type = qp::ports::kScalarF64;
    index.connectable = false;
    index.required = true;
    index.unit_symbol = "1";
    index.step = 0.333;
    kp.inputs.push_back(index);

    graph::PortDesc out;
    out.number = kPortKpOut;
    out.name = "kp";
    out.label = "Kp";
    out.description = "The index, on a wire. A consumer that wants it declares a socket and says what it does when "
                      "the socket is empty.";
    out.type = qp::ports::kScalarF64;
    out.connectable = true;
    out.required = false;
    out.unit_symbol = "1";
    kp.outputs.push_back(out);

    graph::NodeDesc day;
    day.type_name = kDayType;
    day.label = "Date";
    day.description = "The day of the year, as the dipole tilt it implies. The Earth's axis leans by 23.44 degrees "
                      "and the magnetic axis is offset from it, so a magnetosphere at the June solstice is not the "
                      "one at the December solstice -- wire this into a dipole and the picture has a season.";
    day.category = "source";
    day.version = 1;
    day.allow_in_field_domain = true;
    day.allow_in_particle_domain = false;
    day.has_compute = true;

    graph::PortDesc day_of_year;
    day_of_year.number = kPortDay;
    day_of_year.name = "day";
    day_of_year.label = "Day of year";
    day_of_year.description = "Zero is 1 January and 172 is the June solstice, which is where the formula's cosine "
                              "is anchored. The value is clamped into the year rather than wrapped, so a node "
                              "showing 400 keeps showing it.";
    day_of_year.type = qp::ports::kScalarF64;
    day_of_year.connectable = false;
    day_of_year.required = true;
    day_of_year.unit_symbol = "d";
    day_of_year.step = 1.0;
    day.inputs.push_back(day_of_year);

    graph::PortDesc tilt;
    tilt.number = kPortDayOut;
    tilt.name = "tilt";
    tilt.label = "Dipole tilt";
    tilt.description = "The tilt the date implies, in **degrees**: the unit this kit's dipole socket is written in. "
                       "The reference implementation computes radians, which is a factor of fifty-seven away and "
                       "would still look like a tilt.";
    tilt.type = qp::ports::kScalarF64;
    tilt.connectable = true;
    tilt.required = false;
    tilt.unit_symbol = "deg";
    day.outputs.push_back(tilt);

    return {std::move(kp), std::move(day)};
}

std::size_t SourceNodes::mount(qp::host::PluginHost& host) noexcept {
    std::size_t registered = 0;
    for (graph::NodeDesc& desc : node_types()) {
        if (host.add_builtin_node_type(std::move(desc)) == qp::diag::ErrorCode::ok) ++registered;
    }
    return registered;
}

}  // namespace qp::plugins::magnetosphere
