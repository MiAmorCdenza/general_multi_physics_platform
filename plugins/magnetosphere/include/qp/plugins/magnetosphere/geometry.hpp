/**
 * @file geometry.hpp
 * @brief Three doubles and the operations a particle pusher needs, without a library.
 *
 * ## Why this is not `Eigen`
 *
 * The layer gate allows Eigen in `graph/eval` and `graph/kernels` only, and this directory is a plugin. That rule
 * is doing real work here rather than being an obstacle: a Boris push reads and writes three doubles per particle
 * per sub-step, and the arithmetic it needs is nine multiply-adds, one cross product and one norm. Pulling in a
 * linear-algebra library for that would put a template instantiation and a header of several thousand lines
 * between the reader and the equation.
 *
 * ## Why the operators are free functions rather than members
 *
 * Because `a * b` and `a + b` read like the equations they implement, and `Vec3::cross(other)` does not. The
 * physics in this kit is checked line by line against a textbook, so the source is written to be checkable that
 * way.
 *
 * ## What is deliberately absent
 *
 * No normalisation that silently guards against a zero vector, no `normalized()` returning a default, no
 * operator that hides a division. A pusher's arithmetic has to distinguish "the field is zero here" from "the
 * field is not a number", and a helper that turned one into the other would remove exactly the information the
 * caller needs to report which happened.
 *
 * @ownership   pure (a value type and pure functions)
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   Every operation is total: no input produces undefined behaviour
 * @errors      noexcept
 * @frozen      no
 * @tests       magnetosphere.geometry.norm_and_dot,
 *              magnetosphere.geometry.cross_is_anticommutative
 */
#pragma once

#include <cmath>

namespace qp::plugins::magnetosphere {

/**
 * @brief A vector in three dimensions, in whatever unit the caller is working in.
 *
 * A plain aggregate with public members, so `Vec3{x, y, z}` and `field.x` both read the way the equations do.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   No member is ever a signalling NaN produced by this type
 * @errors      noexcept
 * @frozen      no
 * @tests       magnetosphere.geometry.norm_and_dot
 */
struct Vec3 final {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

/**
 * @brief Component-wise sum.
 *
 * @param a The first vector.
 * @param b The second vector.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        `{a.x + b.x, a.y + b.y, a.z + b.z}`
 * @invariant   Commutative
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       magnetosphere.geometry.norm_and_dot
 */
[[nodiscard]] constexpr Vec3 operator+(const Vec3& a, const Vec3& b) noexcept {
    return Vec3{a.x + b.x, a.y + b.y, a.z + b.z};
}

/**
 * @brief Component-wise difference.
 *
 * @param a The vector to subtract from.
 * @param b The vector to subtract.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        `{a.x - b.x, a.y - b.y, a.z - b.z}`
 * @invariant   Anti-commutative
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       magnetosphere.geometry.norm_and_dot
 */
[[nodiscard]] constexpr Vec3 operator-(const Vec3& a, const Vec3& b) noexcept {
    return Vec3{a.x - b.x, a.y - b.y, a.z - b.z};
}

/**
 * @brief Uniform scaling.
 *
 * @param a The vector.
 * @param s The scalar.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        Every component multiplied by `s`
 * @invariant   `(a * s) * t == a * (s * t)` up to floating-point rounding
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       magnetosphere.geometry.norm_and_dot
 */
[[nodiscard]] constexpr Vec3 operator*(const Vec3& a, double s) noexcept {
    return Vec3{a.x * s, a.y * s, a.z * s};
}

/**
 * @brief Uniform scaling, scalar first, so `s * a` reads the way the equations write it.
 *
 * @param s The scalar.
 * @param a The vector.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        `a * s`
 * @invariant   Agrees with `operator*(const Vec3&, double)` bit for bit
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       magnetosphere.geometry.norm_and_dot
 */
[[nodiscard]] constexpr Vec3 operator*(double s, const Vec3& a) noexcept {
    return a * s;
}

/**
 * @brief Dot product.
 *
 * @param a The first vector.
 * @param b The second vector.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        The scalar projection sum
 * @invariant   Symmetric; zero for orthogonal vectors
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       magnetosphere.geometry.norm_and_dot
 */
[[nodiscard]] constexpr double dot(const Vec3& a, const Vec3& b) noexcept {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

/**
 * @brief Cross product, right-handed.
 *
 * The sign convention is the whole content of this function and it is worth stating: with `x` east, `y` north and
 * `z` up, `cross(x_hat, y_hat) == z_hat`. A left-handed cross product would reverse the direction a charged
 * particle gyrates in, and the symptom -- a proton drifting the wrong way round the Earth -- is far enough
 * downstream that nobody would trace it back here.
 *
 * @param a The first vector.
 * @param b The second vector.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        `{a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x}`
 * @invariant   `cross(a, b) == cross(b, a) * -1`
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       magnetosphere.geometry.cross_is_anticommutative
 */
[[nodiscard]] constexpr Vec3 cross(const Vec3& a, const Vec3& b) noexcept {
    return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

/**
 * @brief Squared length. The form to prefer when only a comparison is wanted: no square root, so no rounding.
 *
 * @param a The vector.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        `dot(a, a)`
 * @invariant   Never negative
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       magnetosphere.geometry.norm_and_dot
 */
[[nodiscard]] constexpr double norm2(const Vec3& a) noexcept { return dot(a, a); }

/**
 * @brief Length. Uses `std::sqrt`, so it is not `constexpr`.
 *
 * @param a The vector.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        The Euclidean length, or a NaN only when a component already is one
 * @invariant   `norm(a) * norm(a) == norm2(a)` up to one rounding
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       magnetosphere.geometry.norm_and_dot
 */
[[nodiscard]] inline double norm(const Vec3& a) noexcept {
    return std::sqrt(norm2(a));
}

/**
 * @brief Whether every component is a finite number.
 *
 * The check a pusher makes before it trusts a field sample, and it exists because the alternative -- letting a
 * NaN in and retiring the particle afterwards -- loses the information that the **field** was the source. A
 * particle whose state became non-finite is a numerical failure; a particle handed a non-finite field is a
 * configuration whose field model is broken, and those want different sentences in a report.
 *
 * @param a The vector.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        True exactly when no component is an infinity or a NaN
 * @invariant   False implies `norm2` is not a real number
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       magnetosphere.geometry.norm_and_dot
 */
[[nodiscard]] inline bool is_finite(const Vec3& a) noexcept {
    return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z);
}

}  // namespace qp::plugins::magnetosphere
