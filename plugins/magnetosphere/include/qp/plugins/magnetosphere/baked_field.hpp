/**
 * @file baked_field.hpp
 * @brief A field model evaluated once onto a lattice, and read back by trilinear interpolation.
 *
 * ## Why a table rather than a formula in the pusher
 *
 * The reference implementation this kit is ported from makes exactly this call and states the reason in its own
 * comments: "B / E / drag all come from a baked table, sampled once per sub-step". The reason is that a pusher's
 * inner loop must not know what a field model *is*. A dipole is twelve lines of arithmetic and could be called
 * directly; a Tsyganenko expansion is a Fortran routine with a global state block, and a table is the only shape
 * both can present to a step. Choosing the table now means the Tsyganenko family arrives as a different **baker**
 * rather than as a second pusher.
 *
 * ## What "baked" means here, and what it does not
 *
 * Baked means **evaluated once, onto a fixed grid, and then only interpolated**. It does not mean approximate:
 * the grid is a description of where the model was asked, and the interpolation between samples is trilinear --
 * the same scheme the reference uses, which is second-order accurate in the spacing and exact at the nodes.
 *
 * Two consequences worth stating because they bound what a run can claim:
 *
 *   - **the table is a sampling of the model, so the run's field is the table's.** A course that measures the
 *     field's gradient between two nodes of a coarse grid is measuring the interpolation. `spacing()` is
 *     published so a caller can compare it against the gradient scale it cares about, and the reopening
 *     condition is a finer grid rather than a different interpolant;
 *   - **out of range is clamped, not extrapolated.** The reference clamps, and it is the right choice here: a
 *     linear extrapolation of a `1/r^3` field beyond the grid grows without bound and would hand a particle an
 *     enormous force for a reason no one could see. Clamping keeps the run finite and is reported by
 *     `clamped_samples()`, so "the particle left the modelled region" is a number rather than a guess.
 *
 * ## Storage, and why it is `f64` and a plain vector
 *
 * `abi::ElementType::f64` and a `std::vector<double>`: the platform's owner took double precision for the
 * particle path because the conservation checks are differences between numbers of order one, and a field read
 * is what feeds them. The data is laid out **exactly as `abi::LatticeDesc` describes** -- point index
 * `(i * ny + j) * nz + k`, then component -- so `field::get_component` reads the same memory, which is what
 * makes the table consumable by a kernel that only knows the field vocabulary.
 *
 * @ownership   owns (the samples)
 * @thread      main (bake) / eval (sample)
 * @pre         none
 * @post        none
 * @invariant   `view()` describes exactly `data().size()` doubles
 * @errors      See each declaration
 * @frozen      no
 * @tests       magnetosphere.baked_field.the_view_describes_the_storage,
 *              magnetosphere.baked_field.a_node_sample_is_exact,
 *              magnetosphere.baked_field.a_midpoint_is_the_average,
 *              magnetosphere.baked_field.out_of_range_is_clamped_and_counted
 */
#pragma once

#include <qp/abi/lattice.hpp>
#include <qp/graph/field/field.hpp>
#include <qp/plugins/magnetosphere/geometry.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace qp::plugins::magnetosphere {

/**
 * @brief A vector field sampled on a uniform three-dimensional grid.
 *
 * The axes are **uniform**, unlike the reference implementation's `Table3D`, which carries per-axis node arrays
 * so a bake can cluster samples near the magnetopause. That is a real technique and it is deliberately not here
 * yet: a uniform grid is what a first course's dipole needs, and a non-uniform one changes the interpolation's
 * index arithmetic from a division to a binary search -- a change worth making when a model's features are
 * concentrated, and not before. The reopening condition is written down rather than implied.
 *
 * @ownership   owns
 * @thread      main (bake) / eval (sample)
 * @pre         `nx`, `ny`, `nz` are at least 2 for a table that can be sampled
 * @post        none
 * @invariant   `data().size()` equals `nx * ny * nz * 3`
 * @errors      noexcept
 * @frozen      no
 * @tests       magnetosphere.baked_field.the_view_describes_the_storage
 */
class BakedField final {
public:
    /// @brief An empty table. `view()` describes nothing and every sample is zero.
    ///
    /// @ownership   owns
    /// @thread      main
    /// @pre         none
    /// @post        `empty()` is true
    /// @invariant   No allocation
    /// @errors      noexcept
    /// @complexity  O(1)
    /// @nondet      none
    /// @frozen      no
    /// @tests       magnetosphere.baked_field.the_view_describes_the_storage
    BakedField() = default;

    /**
     * @brief Allocates a uniform grid and zeroes it.
     *
     * @param origin  The `(x, y, z)` of node `(0, 0, 0)`, in metres.
     * @param spacing The distance between neighbouring nodes along each axis, in metres. Each must be positive.
     * @param nx      Nodes along `x`. At least 2.
     * @param ny      Nodes along `y`. At least 2.
     * @param nz      Nodes along `z`. At least 2.
     *
     * @ownership   owns
     * @thread      main
     * @pre         Every spacing is positive and every count is at least 2
     * @post        `width()/height()/depth()` are the counts and every sample is zero
     * @invariant   `data().size() == nx * ny * nz * 3`
     * @errors      May allocate; allocation failure terminates, as elsewhere in this project
     * @complexity  O(nx * ny * nz)
     * @nondet      none
     * @frozen      no
     * @tests       magnetosphere.baked_field.the_view_describes_the_storage
     */
    BakedField(Vec3 origin, Vec3 spacing, std::uint32_t nx, std::uint32_t ny, std::uint32_t nz);

    /// @brief Nodes along `x`.
    ///
    /// @ownership   pure
    /// @thread      any
    /// @pre         none
    /// @post        Zero for an empty table
    /// @invariant   Constant
    /// @errors      noexcept
    /// @complexity  O(1)
    /// @nondet      none
    /// @frozen      no
    /// @tests       magnetosphere.baked_field.the_view_describes_the_storage
    [[nodiscard]] std::uint32_t width() const noexcept { return nx_; }

    /// @brief Nodes along `y`.
    [[nodiscard]] std::uint32_t height() const noexcept { return ny_; }

    /// @brief Nodes along `z`.
    [[nodiscard]] std::uint32_t depth() const noexcept { return nz_; }

    /// @brief Whether the table holds no samples.
    [[nodiscard]] bool empty() const noexcept { return data_.empty(); }

    /// @brief The grid's node spacing, in metres.
    [[nodiscard]] const Vec3& spacing() const noexcept { return spacing_; }

    /// @brief The position of node `(0, 0, 0)`, in metres.
    [[nodiscard]] const Vec3& origin() const noexcept { return origin_; }

    /// @brief The position of node `(i, j, k)`, in metres.
    ///
    /// @param i Index along `x`.
    /// @param j Index along `y`.
    /// @param k Index along `z`.
    ///
    /// @ownership   pure
    /// @thread      any
    /// @pre         none
    /// @post        `origin + (i * sx, j * sy, k * sz)`
    /// @invariant   Agrees with `sample` at every node
    /// @errors      noexcept
    /// @complexity  O(1)
    /// @nondet      none
    /// @frozen      no
    /// @tests       magnetosphere.baked_field.a_node_sample_is_exact
    [[nodiscard]] Vec3 node_position(std::uint32_t i, std::uint32_t j, std::uint32_t k) const noexcept;

    /**
     * @brief Writes one node's vector.
     *
     * @param i Index along `x`.
     * @param j Index along `y`.
     * @param k Index along `z`.
     * @param value The field there, in tesla.
     *
     * @ownership   owns
     * @thread      main
     * @pre         The indices are inside the grid
     * @post        `sample(node_position(i, j, k))` returns exactly `value`
     * @invariant   An out-of-range index changes nothing
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       magnetosphere.baked_field.a_node_sample_is_exact
     */
    void set_node(std::uint32_t i, std::uint32_t j, std::uint32_t k, const Vec3& value) noexcept;

    /// @brief The samples, in the layout `abi::LatticeDesc` describes.
    [[nodiscard]] const std::vector<double>& data() const noexcept { return data_; }

    /// @brief A writable view of the samples, for a baker that fills the table in place.
    [[nodiscard]] std::vector<double>& data() noexcept { return data_; }

    /**
     * @brief A `FieldValue` describing this table, for a kernel that knows only the field vocabulary.
     *
     * The view **borrows** this object's storage: it is valid until the next `set_node` if the vector did not
     * reallocate, and it is invalid the moment the table is destroyed. Nothing here resizes after construction,
     * which is what makes the borrow safe for the duration of a run.
     *
     * @ownership   borrows from this object
     * @thread      eval
     * @pre         none
     * @post        A readable view for a non-empty table, and an invalid one for an empty table
     * @invariant   `view().desc` matches `width()/height()/depth()` and `spacing()`
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       magnetosphere.baked_field.the_view_describes_the_storage
     */
    [[nodiscard]] qp::graph::field::FieldValue view() const noexcept;

    /**
     * @brief The same view, describing **another physical quantity**.
     *
     * A table is a table: the samples are `f64` triples on a lattice, and what they *mean* is a `FieldDim`. The
     * magnetic view is the default because every model this kit shipped until now was magnetic; an electric field
     * is the case that made the dimension a parameter rather than a constant.
     *
     * A description that lied about the dimension would be worse than none at all: `field::is_valid_field` is
     * entitled to be asked what a buffer holds and to get a true answer, and a report that labelled volts per
     * metre as tesla would be wrong in a way nobody could see.
     *
     * @param dimension The exponents the samples carry.
     *
     * @ownership   borrows from this object
     * @thread      eval
     * @pre         none
     * @post        A readable view for a non-empty table, and an invalid one for an empty table
     * @invariant   `view()` equals `view(tesla_dimension())`
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       magnetosphere.baked_field.the_view_describes_the_storage
     */
    [[nodiscard]] qp::graph::field::FieldValue view(qp::abi::FieldDim dimension) const noexcept;

    /**
     * @brief The field at `point`, by trilinear interpolation, clamping outside the grid.
     *
     * @param point Where to evaluate, in **metres**.
     *
     * @ownership   pure
     * @thread      eval
     * @pre         none
     * @post        On a node, exactly the node's value; between nodes, the trilinear blend; outside the grid,
     *              the value at the nearest boundary node
     * @invariant   Never reads outside the storage, for any input including a non-finite one
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       magnetosphere.baked_field.a_node_sample_is_exact,
     *              magnetosphere.baked_field.a_midpoint_is_the_average,
     *              magnetosphere.baked_field.out_of_range_is_clamped_and_counted
     */
    [[nodiscard]] Vec3 sample(const Vec3& point) const noexcept;

    /// @brief How many samples were taken outside the grid, since construction or the last `reset_counts`.
    ///
    /// The evidence for a statement a report has to be able to make: a run whose particles left the modelled
    /// region is not a run about that region. Zero is a real answer meaning "every sample was inside".
    [[nodiscard]] std::uint64_t clamped_samples() const noexcept { return clamped_; }

    /// @brief Forgets the clamp count, so a second run is reported as its own.
    ///
    /// @ownership   owns
    /// @thread      main
    /// @pre         none
    /// @post        `clamped_samples()` is zero
    /// @invariant   The samples are untouched
    /// @errors      noexcept
    /// @complexity  O(1)
    /// @nondet      none
    /// @frozen      no
    /// @tests       magnetosphere.baked_field.out_of_range_is_clamped_and_counted
    void reset_counts() noexcept { clamped_ = 0; }

private:
    /// @brief The flat element offset of node `(i, j, k)`'s first component, or the data size when outside.
    [[nodiscard]] std::size_t offset_of(std::uint32_t i, std::uint32_t j, std::uint32_t k) const noexcept;

    Vec3 origin_{};
    Vec3 spacing_{};
    std::uint32_t nx_ = 0;
    std::uint32_t ny_ = 0;
    std::uint32_t nz_ = 0;
    std::vector<double> data_{};
    /// Mutable because sampling is a read: a `const` sample that could not report its own clamping would make
    /// the count a property of a non-const caller, and every caller here holds a `const` view.
    mutable std::uint64_t clamped_ = 0;
};

/**
 * @brief The dimension of a magnetic field: kg / (A s^2), which is M = 1, T = -2, I = -1.
 *
 * Beside the table rather than beside the models, because it is a property of **what a table can hold**: the same
 * `BakedField` publishes a magnetic field, an electric field, or anything else whose samples are three f64 per
 * node, and the dimension is the only thing that tells them apart.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        The exponents of a tesla
 * @invariant   Constant
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       magnetosphere.baked_field.the_view_describes_the_storage
 */
[[nodiscard]] inline qp::abi::FieldDim tesla_dimension() noexcept {
    qp::abi::FieldDim dim;
    dim.M = 1;
    dim.T = -2;
    dim.I = -1;
    return dim;
}

/**
 * @brief The dimension of an electric field: kg m / (A s^3) -- one length, one less inverse time than tesla.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        The exponents of a volt per metre
 * @invariant   Differs from `tesla_dimension()` in `L` and `T`
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       magnetosphere.baked_field.the_view_describes_the_storage
 */
[[nodiscard]] inline qp::abi::FieldDim volt_per_metre_dimension() noexcept {
    qp::abi::FieldDim dim;
    dim.M = 1;
    dim.L = 1;
    dim.T = -3;
    dim.I = -1;
    return dim;
}

/**
 * @brief The dimension of a **rate**: 1/s, which is `T = -1` and nothing else.
 *
 * The third one, beside the two fields, and it is here for the same reason they are: the dimension is a property of
 * what a table holds, and the tables this kit publishes now include a drag rate. It is also the smallest of the
 * three -- one exponent -- which is the point worth noticing: a scalar table whose samples are "per second" and one
 * whose samples are "dimensionless" have the same layout, so the dimension is the *only* thing that tells them
 * apart, and a consumer that ignored it would multiply a velocity by a weight and call it a drag.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        The exponents of a reciprocal second
 * @invariant   `T == -1`, every other exponent zero
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       magnetosphere.baked_field.the_view_describes_the_storage
 */
[[nodiscard]] inline qp::abi::FieldDim per_second_dimension() noexcept {
    qp::abi::FieldDim dim;
    dim.T = -1;
    return dim;
}

/**
 * @brief The trilinear read of a table somebody else owns, in **SI**.
 *
 * ## Why this is free rather than a member
 *
 * A `BakedField` is what a **baker** holds; a kernel and an emitter hold a `field::FieldValue` -- a descriptor and
 * a borrowed pointer -- because that is what crosses the plugin boundary and what the field domain publishes into
 * a `FieldSet`. Those two callers need the same arithmetic and must not each have their own copy of it: two
 * trilinear samplers in one kit is two answers to "what is the field between two nodes", and the disagreement
 * would show up as a particle that curves slightly differently from where an emitter launched it.
 *
 * The geometry travels beside the value because `abi::LatticeDesc` carries **counts** and not positions -- the
 * same reason `BorisAdvancer` documents six parameter slots for it. The two must describe the same table: a
 * mismatched origin is a box that is not where the samples are, and the result is a plausible field from the
 * wrong place.
 *
 * Out of range and non-finite points are **clamped to the boundary node**, matching `BakedField::sample`: a
 * linear extrapolation of a `1/r^3` field beyond the grid grows without bound and would hand a particle an
 * enormous force for a reason nobody could see.
 *
 * @param table     The samples. Must be a volume of vectors, `f64` or `f32`; anything else reads as zero.
 * @param origin_m  Where node `(0, 0, 0)` is, in metres.
 * @param spacing_m The distance between neighbouring nodes, in metres. Each must be positive.
 * @param point_m   Where to evaluate, in metres.
 *
 * @ownership   pure
 * @thread      eval
 * @pre         `table` is readable and describes a volume of at least 2 nodes an axis
 * @post        On a node, exactly that node's value; between nodes, the trilinear blend; outside, the nearest
 *              boundary node's value
 * @invariant   Never reads outside the described lattice, for any input including a non-finite point
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       magnetosphere.baked_field.a_kernel_and_an_emitter_read_the_same_table
 */
[[nodiscard]] Vec3 sample_baked(const qp::graph::field::FieldValue& table, const Vec3& origin_m,
                                const Vec3& spacing_m, const Vec3& point_m) noexcept;

/**
 * @brief The same read for a scalar table -- the drag coefficient's shape.
 *
 * Separate rather than shared through a template in the header, because the two differ in what a point holds
 * (three numbers or one) and a caller that had to instantiate a template to read a scalar would be paying for
 * the other case's code.
 *
 * @param table     The samples. Must be a volume of scalars; anything else reads as zero.
 * @param origin_m  Where node `(0, 0, 0)` is, in metres.
 * @param spacing_m The distance between neighbouring nodes, in metres.
 * @param point_m   Where to evaluate, in metres.
 *
 * @ownership   pure
 * @thread      eval
 * @pre         `table` is readable and describes a volume of at least 2 nodes an axis
 * @post        The trilinear blend, clamped to the boundary as `sample_baked` is
 * @invariant   Never reads outside the described lattice
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       magnetosphere.baked_field.a_kernel_and_an_emitter_read_the_same_table
 */
[[nodiscard]] double sample_baked_scalar(const qp::graph::field::FieldValue& table, const Vec3& origin_m,
                                         const Vec3& spacing_m, const Vec3& point_m) noexcept;

}  // namespace qp::plugins::magnetosphere
