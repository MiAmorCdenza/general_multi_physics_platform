/**
 * @file test_kernel_clamp.cpp
 * @brief Tests for the clamping policy charter C2 requires of every kernel.
 *
 * The property under test is not "values get bounded" -- that is arithmetic anyone can
 * read. It is that the platform can **tell whether a clamp fired**, because a silent clamp
 * is a wrong answer wearing a right answer's clothes: the run stays finite, the plot keeps
 * drawing, and nothing distinguishes the numbers past the bound from the numbers before
 * it. Every case below therefore checks the `changed` flag as carefully as the value.
 *
 * The NaN cases are the ones that matter. A clamp written as `if (v > limit) v = limit;`
 * looks correct, passes a test that only feeds it large finite numbers, and lets a NaN
 * through -- because a NaN compares false against every bound. That is the defect this
 * file exists to prevent, and it is why `finite_only` is checked before the magnitude.
 */
#include <catch2/catch_test_macros.hpp>

#include <qp/graph/kernels.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

using namespace qp::graph::kernels;

namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

}  // namespace

TEST_CASE("kernel.clamp_policy.presets", "[kernel][clamp]") {
    // `none()` means exactly what it says: a non-finite state is passed on rather than
    // hidden, which is the answer a measurement wants -- a number past a bound cannot be
    // told apart from a number before it, so refusing is more useful than substituting.
    {
        const ClampPolicy p = ClampPolicy::none();
        REQUIRE_FALSE(p.finite_only);
        REQUIRE(p.limit == 0.0);
        // The identity: applying it never changes a value, including a non-finite one.
        REQUIRE_FALSE(apply(p, 1.0e300).changed);
        REQUIRE(apply(p, 1.0e300).value == 1.0e300);
        REQUIRE_FALSE(apply(p, kInf).changed);
        REQUIRE(std::isinf(apply(p, kInf).value));
        REQUIRE_FALSE(apply(p, kNaN).changed);
    }

    // `finite()` refuses a non-finite value but imposes no magnitude bound, so a large
    // finite value passes untouched.
    //
    // This case is why `finite()` cannot simply return a default-constructed policy: the
    // struct's member initialiser gives `limit = 1.0e12`, a value comfortably below `1e300`
    // and above almost everything else. Inheriting it would mean `finite()` silently bounded
    // the magnitude, and a run that reached 1e13 would be clamped by a policy whose own
    // documentation says it does not clamp. The test would have passed had it used 1.0e11.
    {
        const ClampPolicy p = ClampPolicy::finite();
        REQUIRE(p.finite_only);
        REQUIRE(p.limit == 0.0);
        REQUIRE_FALSE(apply(p, 1.0e300).changed);
        REQUIRE(apply(p, 1.0e300).value == 1.0e300);
        REQUIRE(apply(p, 1.0e13).value == 1.0e13);
    }

    // A directly constructed policy does carry the default bound, which is the difference
    // between "construct the struct" and "ask for a preset".
    {
        const ClampPolicy p;
        REQUIRE(p.finite_only);
        REQUIRE(p.limit == 1.0e12);
        REQUIRE(apply(p, 1.0e13).changed);
        REQUIRE(apply(p, 1.0e13).value == 1.0e12);
    }

    // `bounded()` does both.
    {
        const ClampPolicy p = ClampPolicy::bounded(100.0);
        REQUIRE(p.finite_only);
        REQUIRE(p.limit == 100.0);
        REQUIRE(apply(p, 150.0).changed);
        REQUIRE(apply(p, 150.0).value == 100.0);
    }
}

TEST_CASE("kernel.clamp_policy.applies", "[kernel][clamp]") {
    const ClampPolicy bound = ClampPolicy::bounded(10.0);

    // Inside the bound, a value is returned **bit for bit**. A clamp that routed values
    // through arithmetic would perturb every number in a run, and a golden regression test
    // would fail on a change that was supposed to be a no-op.
    REQUIRE(apply(bound, 0.0).value == 0.0);
    REQUIRE_FALSE(apply(bound, 0.0).changed);
    REQUIRE(apply(bound, 9.999).value == 9.999);
    REQUIRE_FALSE(apply(bound, 9.999).changed);
    REQUIRE(apply(bound, -9.999).value == -9.999);
    REQUIRE_FALSE(apply(bound, -9.999).changed);

    // Exactly at the bound is inside it. An implementation using `>=` rather than `>`
    // would clamp a value that is legal, and the `changed` flag would report a clamp that
    // did not need to happen -- which shows up in the confidence panel as numerical
    // trouble that is not there.
    REQUIRE(apply(bound, 10.0).value == 10.0);
    REQUIRE_FALSE(apply(bound, 10.0).changed);
    REQUIRE(apply(bound, -10.0).value == -10.0);
    REQUIRE_FALSE(apply(bound, -10.0).changed);

    // Outside it, the value is the bound and the flag is set.
    REQUIRE(apply(bound, 10.001).value == 10.0);
    REQUIRE(apply(bound, 10.001).changed);
    REQUIRE(apply(bound, -10.001).value == -10.0);
    REQUIRE(apply(bound, -10.001).changed);
    REQUIRE(apply(bound, 1.0e300).value == 10.0);
    REQUIRE(apply(bound, 1.0e300).changed);

    // The non-finite cases, which are the reason this function lives in the foundation
    // rather than in three copies inside three operators.
    //
    // Infinity and NaN need **different** answers, and conflating them is the easy mistake.
    //
    // `+inf` is greater than every bound, so it clamps to the bound like any other
    // oversized value; `-inf` clamps to the negative bound. An implementation that sent
    // both to zero would discard the sign, and a run that diverged toward minus infinity
    // would be reported as having diverged toward zero.
    REQUIRE(apply(bound, kInf).value == 10.0);
    REQUIRE(apply(bound, kInf).changed);
    REQUIRE(apply(bound, -kInf).value == -10.0);
    REQUIRE(apply(bound, -kInf).changed);

    // NaN is the case that needs `finite_only` checked **before** the magnitude. It
    // compares false against every bound, so the obvious spelling `if (v > limit) v =
    // limit;` falls through, writes the NaN onward, and reports that nothing changed.
    // A NaN carries no positional meaning to preserve, so it becomes the one finite double
    // that carries none either -- zero -- and the flag says a clamp fired.
    {
        const ClampResult r = apply(bound, kNaN);
        REQUIRE(r.changed);
        REQUIRE(std::isfinite(r.value));
        REQUIRE(r.value == 0.0);
    }

    // A policy with `finite_only` but no magnitude bound catches NaN the same way, which is
    // the whole difference between `ClampPolicy::finite()` and `ClampPolicy::none()`.
    {
        const ClampResult r = apply(ClampPolicy::finite(), kNaN);
        REQUIRE(r.changed);
        REQUIRE(r.value == 0.0);
    }
}

TEST_CASE("kernel.clamp_policy.counts_what_it_changed", "[kernel][clamp]") {
    // The shape an operator actually uses: accumulate the flag over a batch and inspect it
    // once, rather than branching per sample on the step path.
    //
    // This is the mechanism behind charter C8 -- "numerical error must not be mistaken for
    // physics". A run whose energy drifts looks like a physical effect; a run that reports
    // "the clamp fired 3 times" does not.
    const ClampPolicy bound = ClampPolicy::bounded(5.0);
    const double samples[] = {1.0, 100.0, -2.0, kNaN, 5.0, -100.0, 0.0};

    int clamps = 0;
    double largest = 0.0;
    for (const double v : samples) {
        const ClampResult r = apply(bound, v);
        if (r.changed) {
            ++clamps;
            largest = std::max(largest, std::abs(r.value));
        }
    }

    // Three of the seven are out of range: 100, NaN and -100. Note what is **not** counted:
    // 5.0 is exactly at the bound and is therefore inside it, and -2.0 and 0.0 are interior.
    // An off-by-one in the comparison would make this four, and the extra one would be a
    // clamp reported for a value that was legal.
    REQUIRE(clamps == 3);
    // Every written value obeys the bound, which is the guarantee the policy makes.
    for (const double v : samples) {
        REQUIRE(std::abs(apply(bound, v).value) <= 5.0);
    }
    REQUIRE(largest <= 5.0);
}
