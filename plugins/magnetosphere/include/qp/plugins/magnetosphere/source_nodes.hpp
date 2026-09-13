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
