/**
 * @file source_nodes.hpp
 * @brief The first **driver**: a node that publishes a number the rest of the graph reads.
 *
 * ## What a driver is, and why the kit needs one now
 *
 * Every node in this plugin so far produces either a field table or particle state. The reference implementation has a
 * third kind -- `kp_source`, `day_source`, `imf_source` -- whose whole job is to publish **one number** that several
 * consumers read, and it exists because a magnetospheric model is driven by the solar wind rather than by a user
 * turning dials per node. `kp_source`'s own comment in the reference says it plainly: a manual value today, "NOAA
 * fetched by the server through `set_param`" later.
 *
 * `field.magnetopause` is where that promise comes due. Its header says the standoff distance "is a **driver's** job:
 * when this kit has one, `Kp` becomes a socket and this parameter becomes the fallback", and the reason it is the
 * driver's job rather than the boundary's is a layering one: writing `pdyn = 2 + Kp/2` inside a boundary model puts
 * a solar-wind model inside a magnetopause model, and two consumers of `Kp` would then each carry their own copy of
 * the conversion.
 *
 * ## The port type is what makes this a small node rather than a new mechanism
 *
 * `ports::kScalarF64` already exists -- "the default for measurement chains, parameters, uncertainties" -- and
 * `ports::Value` already carries a number, so a driver publishes a value through the interface the evaluator already
 * has: one output port, one number, no table and no geometry. Nothing in `core/` changes.
 *
 * ## What a driver does **not** do: it does not reach into another node's parameters
 *
 * The reference's engine drove nodes through `set_param` from outside the graph, which is a mutation with no author
 * and no undo. Here the value travels along a **wire**, exactly like a field, and each consumer that wants it
 * declares a socket and says what it does when the socket is empty. That keeps every edit in the session, every
 * value in the evaluation, and the choice of "wire it or type it" visible on the canvas. It also answers the
 * question this kit had been deferring -- *can a parameter be wired?* -- with a shape rather than a rule: the
 * parameter stays typed and keeps its meaning alone, the socket is **optional**, and the wired value wins. The
 * pusher's `use_drag` socket-plus-knob pair is the same shape and predates this node.
 *
 * @ownership   pure (the conversions) / observes (the reader)
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   The Kp model's two formulas are the only place `Kp` becomes geometry
 * @errors      See each declaration
 * @frozen      no
 * @tests       magnetosphere.source.the_kp_index_moves_the_magnetopause
 */
#pragma once

#include <qp/graph/ir/descriptor.hpp>
#include <qp/graph/ir/ids.hpp>
#include <qp/graph/ir/node.hpp>
#include <qp/host/host.hpp>

#include <cstddef>
#include <vector>

namespace qp::plugins::magnetosphere {

/**
 * @brief The solar-wind drivers: nodes whose product is a number the rest of the graph reads.
 *
 * @ownership   owns (the descriptors it builds)
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   Every type here declares exactly one output, of a numeric port type
 * @errors      See each declaration
 * @frozen      no
 * @tests       magnetosphere.source.the_kp_index_moves_the_magnetopause
 */
class SourceNodes final {
public:
    /// @brief The planetary K index, as a node: a number between 0 and 9.
    ///
    /// One of the two things a magnetospheric model is actually driven by (the other is the solar wind's dynamic
    /// pressure, which the reference derives from this same index). It is a **manual** value: an automatic feed is a
    /// server-side concern in the reference and would be a `set_param` from outside the graph here, which is the
    /// thing this kit does not do.
    static constexpr const char* kKpType = "source.kp";

    /// @brief The parameter a user sets: the index itself.
    static constexpr qp::graph::PortNumber kPortKp = 1;
    /// @brief The value it publishes, on a wire.
    static constexpr qp::graph::PortNumber kPortKpOut = 1;

    /// @brief The default index: 2, a quiet-to-moderate day.
    static constexpr double kDefaultKp = 2.0;
    /// @brief The bottom of the index's range. Below zero is not a quiet day, it is a different scale.
    static constexpr double kMinKp = 0.0;
    /// @brief The top of the index's range: 9 is a severe storm.
    static constexpr double kMaxKp = 9.0;

    /**
     * @brief The standoff distance the index implies, in earth radii: `10 / (2 + Kp/2)^(1/3)`.
     *
     * The reference's own formula, kept with its units corrected: in that engine the length unit is an earth radius
     * and the pressure is in nanopascals, so the constant 10 is a distance in `R_E` and the cube root is the
     * pressure scaling of a standoff distance. The direction is the physics worth checking: **more activity, less
     * cavity** -- `Kp = 2` puts the nose at 6.93 earth radii and `Kp = 6` pulls it in to 5.85.
     *
     * @param kp The index. Clamped into `[kMinKp, kMaxKp]` by the reader, not here.
     *
     * @ownership   pure
     * @thread      any
     * @pre         none
     * @post        A positive distance in earth radii, finite for any finite input
     * @invariant   Monotonically decreasing in `kp`
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       magnetosphere.source.the_kp_index_moves_the_magnetopause
     */
    [[nodiscard]] static double standoff_re_for_kp(double kp) noexcept;

    /**
     * @brief The flaring exponent the index implies: `0.55 + 0.02 Kp`.
     *
     * The reference's second Kp formula, and the weaker of the two: over the whole index range it moves the flank
     * by about ten percent, where the standoff moves by forty. It is here because a driver that set one of a
     * model's two numbers would leave the other describing a different day.
     *
     * @param kp The index, with the same clamping rule as the standoff.
     *
     * @ownership   pure
     * @thread      any
     * @pre         none
     * @post        An exponent in `[0.55, 0.73]` for `kp` in range
     * @invariant   Monotonically increasing in `kp`
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       magnetosphere.source.the_kp_index_moves_the_magnetopause
     */
    [[nodiscard]] static double flaring_for_kp(double kp) noexcept;

    /**
     * @brief The tail's lobe field the index implies: `(30 + 5 Kp)` nanotesla, in tesla.
     *
     * The reference's third and fourth Kp formulas, from `tail.py`, and they are here for the reason the two above
     * are: `30 + 5 Kp` is a **solar-wind** relation that a tail model happens to use, not a property of a current
     * sheet. A sheet that carried its own copy would answer "how strong is the lobe at this activity" differently
     * from the node next to it, and the difference would show up as a picture that is subtly out of proportion
     * rather than as a refusal.
     *
     * The numbers bracket the observed tail: at `Kp = 0` the lobes carry thirty nanotesla and at `Kp = 9` seventy
     * -five, which is the range a magnetometer in the mid-tail actually reports. Note how much larger they are than
     * this kit's own default for the same parameter (five nanotesla, chosen before there was a driver for it): the
     * reference's index-driven value is the realistic one, and the parameter is what an experiment uses when it
     * wants to hold the tail still.
     *
     * @param kp The index, with the same clamping rule as the standoff.
     *
     * @ownership   pure
     * @thread      any
     * @pre         none
     * @post        A field in `[30, 75]` nanotesla for `kp` in range
     * @invariant   Monotonically increasing in `kp`
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       magnetosphere.source.the_kp_index_sets_the_tails_lobe_field
     */
    [[nodiscard]] static double lobe_field_t_for_kp(double kp) noexcept;

    /**
     * @brief The tail's northward component the index implies: `(1.5 + 0.3 Kp)` nanotesla, in tesla.
     *
     * The component that makes the reference's tail **not** a pure Harris sheet: a Harris sheet has no `B_z` at all,
     * and a real tail's field lines are partly closed across it. At `Kp = 0` it is 1.5 nanotesla and at `Kp = 9` it
     * is 4.2, so the closed fraction grows with activity -- which is the direction the physics goes, and the reason
     * this is a Kp formula rather than a constant.
     *
     * @param kp The index, with the same clamping rule as the standoff.
     *
     * @ownership   pure
     * @thread      any
     * @pre         none
     * @post        A field in `[1.5, 4.2]` nanotesla for `kp` in range
     * @invariant   Monotonically increasing in `kp`
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       magnetosphere.source.the_kp_index_sets_the_tails_lobe_field
     */
    [[nodiscard]] static double tail_bz_t_for_kp(double kp) noexcept;

    /**
     * @brief The index a node carries, clamped into the scale's range.
     *
     * Clamped rather than refused, and the reason is the same one the parameter readers give everywhere: a
     * half-filled node is a graph being edited, and refusing to evaluate it would make the picture vanish while the
     * user is still choosing. A **non-finite** value is not clamped but replaced by the default, because a NaN is
     * not a point on the scale.
     *
     * @param node The node. Borrowed.
     *
     * @ownership   pure
     * @thread      main
     * @pre         none
     * @post        A value in `[kMinKp, kMaxKp]`
     * @invariant   A node with no parameter gets `kDefaultKp`
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       magnetosphere.source.the_kp_index_moves_the_magnetopause
     */
    [[nodiscard]] static double read_kp(const qp::graph::Node& node) noexcept;

    /// @brief The date, as a node: the second driver, and the one that makes the dipole tilt seasonal.
    ///
    /// The reference's `day_source` publishes the **magnetic tilt** the date implies -- the Earth's rotation axis is
    /// inclined by 23.44 degrees and its magnetic axis is offset from that by about 11, so the angle between the
    /// dipole and the Sun's direction runs between roughly minus twelve and plus thirty-four degrees over a year.
    /// That is why a magnetosphere has seasons, and why a course that measures anything at a fixed tilt is measuring
    /// one day of it.
    ///
    /// ## Degrees, and the one place a unit could have been lost
    ///
    /// The reference computes in **radians** (`ps` is what its engine's hinge wants) and this kit's dipole port is
    /// `tilt_degrees`, unit `deg`. A driver publishing radians into a degrees socket would be wrong by a factor of
    /// fifty-seven and would still *look* like a tilt, so this node publishes degrees and says so on the port. When
    /// the tail's hinge arrives it can take degrees and convert inside its own model, which is where a unit
    /// conversion belongs.
    static constexpr const char* kDayType = "source.day";

    /// @brief The parameter: the day of the year, 1 January at zero.
    static constexpr qp::graph::PortNumber kPortDay = 1;
    /// @brief The tilt it publishes, on a wire.
    static constexpr qp::graph::PortNumber kPortDayOut = 1;

    /// @brief The day the reference implementation's formula is anchored at: the June solstice, 21 June.
    static constexpr double kDefaultDay = 172.0;
    /// @brief The bottom of the day range.
    static constexpr double kMinDay = 0.0;
    /// @brief The top of the day range.
    static constexpr double kMaxDay = 365.0;

    /// @brief The obliquity: how far the rotation axis leans, in degrees.
    static constexpr double kObliquityDegrees = 23.44;
    /// @brief The offset of the magnetic axis from the rotation axis, in degrees.
    ///
    /// The reference uses eleven and this kit's own dipole constant is 11.5; the two are the same measurement to the
    /// accuracy anyone has it. The reference's number is kept here so that a run reproducing its fields gets its
    /// fields, and the difference is stated rather than quietly reconciled.
    static constexpr double kDayTiltOffsetDegrees = 11.0;
    /// @brief The length of the year the formula uses, in days: the tropical year, as the reference has it.
    static constexpr double kDaysPerYear = 365.25;

    /// @brief The tail's lobe field at `Kp = 0`, in nanotesla: the reference's `30`.
    static constexpr double kReferenceTailLobeBaseNt = 30.0;
    /// @brief How much the lobe field grows per unit of the index: the reference's `5`.
    static constexpr double kReferenceTailLobePerKpNt = 5.0;
    /// @brief The tail's northward component at `Kp = 0`, in nanotesla: the reference's `1.5`.
    static constexpr double kReferenceTailBzBaseNt = 1.5;
    /// @brief How much that component grows per unit of the index: the reference's `0.3`.
    static constexpr double kReferenceTailBzPerKpNt = 0.3;

    /**
     * @brief The dipole tilt a date implies, in **degrees**: `offset + obliquity cos(2 pi (day - 172) / 365.25)`.
     *
     * Three days are exact and the case asserts all three: the June solstice (day 172) is the maximum,
     * `23.44 + 11 = 34.44`; half a year later it is the minimum, `-23.44 + 11 = -12.44`; and a quarter of a year
     * either side the cosine vanishes and the tilt is the offset alone, `11`. The range is what makes the node
     * worth wiring: a magnetosphere at +34 degrees and one at -12 are different experiments.
     *
     * @param day The day of the year, clamped into `[kMinDay, kMaxDay]` by the reader rather than here.
     *
     * @ownership   pure
     * @thread      any
     * @pre         none
     * @post        A finite angle in `[offset - obliquity, offset + obliquity]`
     * @invariant   Periodic in `kDaysPerYear` before clamping, and monotone between the solstices
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       magnetosphere.source.the_date_sets_the_dipole_tilt
     */
    [[nodiscard]] static double tilt_degrees_for_day(double day) noexcept;

    /**
     * @brief The day a node carries, clamped into the year.
     *
     * Clamped rather than wrapped, and that is a decision worth stating: a wrapped year would make day 400 equal
     * day 35, which is true of a calendar and false of the graph being edited -- a node showing 400 is a node whose
     * author meant something the formula cannot say. Clamping keeps the value visible and the tilt finite.
     *
     * @param node The node. Borrowed.
     *
     * @ownership   pure
     * @thread      main
     * @pre         none
     * @post        A value in `[kMinDay, kMaxDay]`
     * @invariant   A node with no parameter, or with a value that is not a number, gets `kDefaultDay`
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       magnetosphere.source.the_date_sets_the_dipole_tilt
     */
    [[nodiscard]] static double read_day(const qp::graph::Node& node) noexcept;

    /**
     * @brief The type table: the Kp source, as a node type.
     *
     * @ownership   owns
     * @thread      main
     * @pre         none
     * @post        Exactly one descriptor, with one numeric output
     * @invariant   The parameter is not connectable and the output is
     * @errors      May allocate: the table it returns owns its descriptors, so it claims nothing about throwing.
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       magnetosphere.source.the_kp_index_moves_the_magnetopause
     */
    [[nodiscard]] static std::vector<qp::graph::NodeDesc> node_types();

    /**
     * @brief Registers the driver with `host` as a built-in.
     *
     * @param host The host. Borrowed.
     *
     * @ownership   observes `host`
     * @thread      main
     * @pre         none
     * @post        The type is registered unless its name was taken
     * @invariant   A type already registered under this name is left alone rather than duplicated
     * @errors      noexcept
     * @complexity  O(types)
     * @nondet      none
     * @frozen      no
     * @tests       magnetosphere.source.the_kp_index_moves_the_magnetopause
     */
    static std::size_t mount(qp::host::PluginHost& host) noexcept;
};

}  // namespace qp::plugins::magnetosphere
