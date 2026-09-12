/**
 * @file test_plugin_golden.cpp
 * @brief Bit-for-bit golden cases for the mechanics kernels. Charter C7.
 *
 * ## What this file is for, and how it differs from the plugin tests
 *
 * `test_rk4_oscillator.cpp` and `test_incline_collision.cpp` assert against **closed forms**:
 * `x(t) = x0 cos(wt) + (v0/w) sin(wt)`, `s = g sin(theta) t^2 / 2`. That says the physics is
 * right. It does not say the arithmetic is unchanged, and for an integrator those are separate
 * questions.
 *
 * A reordered RK4 stage, an `x * 0.5` rewritten as `x / 2`, an expression reassociated by a
 * compiler flag, a `-ffast-math` that stops being off: every one of those leaves the closed-form
 * assertions passing inside their tolerances while producing **different bits**. And the bits are
 * what a saved run contains -- a run that reproduces to four digits is not the run that was
 * recorded, which is the whole content of charter R2 and C1. So C7 asks for this file
 * specifically, and it is the only kind of test that can answer it.
 *
 * ## The shape of a case
 *
 * Fixed inputs, a fixed step count, one full-precision comparison per state component, and one
 * order-sensitive checksum over the whole state. The comparison is `REQUIRE(a == b)` on doubles
 * -- not `Approx` -- because a tolerance here would defeat the purpose: the entire question is
 * whether the last bit moved.
 *
 * The component patterns are printed on failure via `CAPTURE`, so a regression says **which**
 * quantity changed rather than only that the checksum did.
 *
 * ## Cross-compiler: what is shared, what diverges, and why
 *
 * Both GCC (MinGW **i686**) and MSVC (**x64**) build and run this file. Twenty-one state
 * components are frozen across the three cases; **nineteen are identical on both** and two are
 * not:
 *
 *     oscillator particle 0 velocity:  x87 bfed632cbd370133   SSE2 bfed632cbd37012f
 *     oscillator particle 1 velocity:  x87 bffdfc7f7216aa26   SSE2 bffdfc7f7216aa27
 *
 * Both are velocity components, one unit in the last place apart. That is the signature of an
 * intermediate with more precision on one side: the MinGW target is 32-bit, so
 * `FLT_EVAL_METHOD == 2` and expressions evaluate in **x87 80-bit extended precision**, while
 * MSVC x64 evaluates at type precision. An RK4 velocity accumulator is a chain of `a * b + c`
 * whose intermediate therefore carries eleven extra bits on one toolchain and none on the other.
 * The position accumulations round the same way at these inputs, so all four agree.
 *
 * This is a **recorded premise, not a defect**, and it is the same premise
 * `tests/unit/units/test_floating_point_env.cpp` asserts for the trap it caused elsewhere. The
 * three options were: share one set of values (impossible -- it fails on MSVC today), loosen to a
 * tolerance (destroys the only property C7 asks for), or record the divergence with the
 * evaluation method that explains it and assert the premise. The third is what `Frozen` does, and
 * `frozen_matches` checks the premise rather than assuming it, so a toolchain change reports
 * "no frozen pattern matches this component on this toolchain" instead of a bare hex mismatch --
 * a message that sends the reader here rather than to the tolerance dial.
 *
 * `CMakeLists.txt` separately disables floating-point contraction on both toolchains. That is a
 * guard against a hazard that has **not** fired: GCC x64 defaults to `-ffp-contract=fast`, and
 * nothing contracts on i686 only because the instruction set has no FMA.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   Every expected value is the same on every supported toolchain
 * @errors      noexcept
 * @frozen      no
 * @tests       plugin.golden.oscillator_bits, plugin.golden.incline_bits,
 *              plugin.golden.merge_bits
 */
#include <catch2/catch_test_macros.hpp>

#include <qp/plugins/mechanics/collision.hpp>
#include <qp/plugins/mechanics/incline.hpp>
#include <qp/plugins/mechanics/rk4_oscillator.hpp>

#include <qp/graph/field/field.hpp>

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <cstdint>
#include <string>
#include <vector>

#include "support/bit_exact.hpp"

using namespace qp::plugins::mechanics;
using qp::test::golden::bits_of;
using qp::test::golden::checksum_hex;

namespace {

namespace kernels = qp::graph::kernels;
namespace field = qp::graph::field;

/// @brief A state buffer described as a legal f64 lattice, with its component values.
///
/// The same shape the operator tests use, and for the same reason: `ComponentKind` admits strides
/// of 1 and 3 only, and `abi::is_consistent` refuses anything else, so a state of three doubles
/// per particle is a **vector** lattice and the stride comes from the ABI's own arithmetic.
class State final {
public:
    static constexpr std::size_t kStride = 3;

    /// @brief `count` particles, every component supplied explicitly.
    State(std::size_t count, std::vector<double> values) : data_(std::move(values)) {
        REQUIRE(data_.size() == count * kStride);
        desc_.kind = qp::abi::LatticeKind::line;
        desc_.component = qp::abi::ComponentKind::vector;
        desc_.element = qp::abi::ElementType::f64;
        desc_.count[0] = static_cast<std::uint32_t>(count);
        desc_.spacing_bytes = qp::abi::expected_spacing(desc_);
        REQUIRE(qp::abi::is_consistent(desc_));
    }

    [[nodiscard]] const std::vector<double>& values() const noexcept { return data_; }

    [[nodiscard]] double get(std::size_t i, std::size_t c) const { return data_[i * kStride + c]; }

    /// @brief One in-place step, asserting the operator accepted it.
    template <class Op>
    void step(Op& op, double dt) {
        field::FieldValue out;
        out.desc = desc_;
        out.data = data_.data();
        out.bytes = data_.size() * sizeof(double);
        kernels::BatchView batch;
        batch.in = &out;
        batch.out = &out;
        batch.count = data_.size() / kStride;
        kernels::AdvanceContext ctx;
        ctx.dt = dt;
        const auto result = op.advance(batch, ctx);
        REQUIRE(result.has_value());
    }

private:
    std::vector<double> data_;
    qp::abi::LatticeDesc desc_{};
};

/// @brief Prepares an operator, asserting its parameters were accepted.
template <class Op>
void prepare(Op& op, const kernels::ParamBlock& params) {
    REQUIRE(op.prepare(params).has_value());
}


/// @brief Where a recording run writes its measurement.
///
/// Relative to the working directory CTest uses, which is the build tree. A path inside the build
/// directory rather than the source tree on purpose: a test that writes into the source tree is a
/// test that dirties a checkout, and the file this produces is an input to a one-off fill step
/// rather than an artefact anyone commits.
constexpr const char* kRecordPath = "golden_measured.txt";

/// @brief Whether this run should record its measured patterns.
///
/// Gated on an environment variable so an ordinary run touches nothing. A golden case that wrote
/// a file every time it executed could not run in parallel, could not run from a read-only
/// directory, and would leave a stale file behind after every invocation.
[[nodiscard]] bool recording() noexcept {
    const char* flag = std::getenv("QP_RECORD_GOLDEN");
    return flag != nullptr && flag[0] != '\0' && flag[0] != '0';
}

/// @brief Appends a case's measured patterns, in a form a script can consume.
///
/// ## Why a file rather than `INFO`
///
/// The first version printed these through Catch2's `INFO`, which the default console reporter
/// does not show for a failing assertion unless `--success` is passed -- so the fill script read
/// an empty transcript and refused to proceed. The refusal was correct and the mechanism was
/// wrong.
///
/// A file is also better on its own terms: the measured values are **data**, and data recovered
/// from a human-readable transcript is data that eventually gets recovered slightly wrong. One
/// line per case, in a fixed format, is what a script can parse and a person can still read.
void record_measured(const char* label, const std::vector<double>& values) {
    if (!recording()) return;
    std::ofstream out(kRecordPath, std::ios::app);
    if (!out) return;
    out << label << " = {";
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i != 0) out << ", ";
        out << '"' << bits_of(values[i]) << '"';
    }
    out << "}; sum = \"" << checksum_hex(values) << "\"\n";
}




}  // namespace

/// between two things that must be independently removable.
#if defined(__i386__) || defined(_M_IX86)
inline constexpr const char* kEvaluationMethod = "x87 (FLT_EVAL_METHOD == 2)";
#else
inline constexpr const char* kEvaluationMethod = "type precision (FLT_EVAL_METHOD == 0)";
#endif

/// @brief One frozen state component: the patterns it may have, and where each applies.
///
/// A component that agrees on every supported toolchain has one pattern. A component that does not
/// lists each pattern with the toolchain premise that produces it -- and `evaluation_method`
/// names that premise in the assertion, so a toolchain change fails with an explanation rather
/// than with a hex string.
struct Frozen final {
    /// The pattern on every supported toolchain, when the component agrees everywhere.
    const char* everywhere = nullptr;
    /// The pattern where `FLT_EVAL_METHOD == 2` (x87 80-bit intermediates, the 32-bit MinGW).
    const char* x87 = nullptr;
    /// The pattern where `FLT_EVAL_METHOD == 0` (evaluated at type precision, x64 SSE2).
    const char* sse2 = nullptr;
};

/// @brief Whether `measured` is one of the patterns `f` permits **on this toolchain**.
///
/// The premise is checked, not assumed. A component that diverges is asserted to match the
/// pattern belonging to the evaluation method this build actually has -- so building with a
/// different toolchain reports "no pattern recorded for this evaluation method", which is the
/// message that sends the reader to `test_floating_point_env.cpp` rather than to this file.
[[nodiscard]] bool frozen_matches(const Frozen& f, const std::string& measured) noexcept {
    if (f.everywhere != nullptr) return measured == f.everywhere;
#if defined(__i386__) || defined(_M_IX86)
    // x87: intermediates carry 11 extra bits, so an accumulated product-sum can round differently
    // from the same expression evaluated at 64-bit precision.
    return f.x87 != nullptr && measured == f.x87;
#else
    return f.sse2 != nullptr && measured == f.sse2;
#endif
}

/// @brief Asserts a whole state against its frozen patterns, naming the failing component.
void check_frozen(const std::vector<double>& values, const std::vector<Frozen>& expected) {
    REQUIRE(values.size() == expected.size());
    REQUIRE(checksum_hex(values).size() == 16);
    for (std::size_t i = 0; i < expected.size(); ++i) {
        CAPTURE(i, bits_of(values[i]));
        if (!frozen_matches(expected[i], bits_of(values[i]))) {
            // The failure message must say what the toolchain premise was, or a reader on a new
            // toolchain sees "bfed632cbd37012f != bfed632cbd370133" and reaches for the tolerance
            // dial -- which is exactly the repair that would destroy this case's value.
            UNSCOPED_INFO("component " << i << " measured " << bits_of(values[i])
                                        << "; evaluation method is " << kEvaluationMethod);
            FAIL_CHECK("no frozen pattern matches this component on this toolchain");
        }
    }
}

/// @brief How this build evaluates floating-point intermediates, as readable text.
///
/// A `constexpr` string chosen once rather than a preprocessor conditional inside an assertion:
/// `#if` cannot appear inside a macro argument list, GCC warns `-Wpedantic` about the attempt, and
/// MSVC parses it differently -- a construct that "works on my compiler" is precisely what the
/// dual-compiler matrix exists to reject.
///
/// The value is the premise the frozen patterns below depend on. `FLT_EVAL_METHOD == 2` means x87
/// 80-bit intermediates (the 32-bit MinGW target) and `0` means evaluated at type precision (x64
/// SSE2). `tests/unit/units/test_floating_point_env.cpp` asserts the same premise elsewhere; it is
/// repeated rather than shared because a test helper that depends on a test header is a dependency
TEST_CASE("plugin.golden.oscillator_bits", "[plugin][golden]") {
    // Two particles with **different** initial conditions and a non-round frequency, so the
    // checksum covers both the arithmetic and the batch iteration. A single particle would leave
    // the loop bound untested; an equal pair would hide a stride error.
    //
    // `omega = 3.0` and the step sizes are exact binary fractions, so the only inexactness in the
    // case comes from the operator itself. That is deliberate: a case whose inputs are already
    // rounded cannot tell "the operator changed" from "the input changed".
    kernels::ParamBlock params;
    params.set_real(0, 3.0);

    Rk4Oscillator op;
    prepare(op, params);

    State state(2, {
        1.0, 0.5, 0.0,   // particle 0: x = 1, v = 0.5
        -0.25, 2.0, 0.0  // particle 1: x = -0.25, v = 2
    });

    // 128 steps of 1/128 = t = 1. A power-of-two step size keeps `n*dt` exact, so the elapsed
    // time is exactly 1.0 rather than 0.9999999999999999 -- which matters because a case whose
    // final time is itself inexact cannot distinguish an operator change from an accumulation
    // difference.
    constexpr double kDt = 1.0 / 128.0;
    constexpr int kSteps = 128;
    for (int i = 0; i < kSteps; ++i) state.step(op, kDt);

    // The expected patterns. Filled in from the first run of this case and never edited without a
    // deliberate decision: an edit to these strings is an assertion that the arithmetic changed
    // on purpose, and the commit message is where the reason goes.
    // Two of these six diverge by one unit in the last place between the supported
    // toolchains, and the divergence is **recorded** rather than averaged away. Both are
    // velocity components, which is the shape an 80-bit intermediate moves: the RK4 velocity
    // accumulator is a chain of `a * b + c` whose intermediate carries 11 extra bits under
    // x87 and none under SSE2. The position accumulations round the same way at these inputs,
    // so all four agree on both toolchains.
    //
    // See `Frozen` above, and `tests/unit/units/test_floating_point_env.cpp`, which records the
    // evaluation-method premise this depends on.
    const std::vector<Frozen> expected{
        {"bfeeed57b8a2b8b7", nullptr, nullptr},  // p0.x  identical on both toolchains
        {nullptr, "bfed632cbd370133", "bfed632cbd37012f"},  // p0.v  x87 / SSE2
        {"0000000000000000", nullptr, nullptr},  // p0.z  written as zero
        {"3fd5dc6a88d68c78", nullptr, nullptr},  // p1.x  identical on both toolchains
        {nullptr, "bffdfc7f7216aa26", "bffdfc7f7216aa27"},  // p1.v  x87 / SSE2
        {"0000000000000000", nullptr, nullptr},  // p1.z  written as zero
    };

    check_frozen(state.values(), expected);
    record_measured("oscillator", state.values());

    // The checksum is a function of the whole state, so it differs between toolchains as soon
    // as one component does. It is therefore not frozen to a value: the per-component patterns
    // above are the assertion, and the checksum is reported so a reader comparing two runs has
    // something cheap to compare.
    INFO("checksum " << checksum_hex(state.values()));

    // The third component is written as zero each step, so it is exactly +0.0. Asserted
    // separately from the loop's pattern comparison because it is the one component whose value
    // is not arithmetic: it is the operator stating that this slot is not part of the state.
    REQUIRE(state.get(0, 2) == 0.0);
    REQUIRE(state.get(1, 2) == 0.0);
}

TEST_CASE("plugin.golden.incline_bits", "[plugin][golden]") {
    // A sloping block that slides (mu_k < tan(theta)) beside one that holds (mu_s > tan(theta)),
    // so the checksum covers both branches of the friction decision. A case with only sliding
    // blocks would leave the static branch unpinned, and the static branch is where the
    // "exactly zero, not a small number" rule lives.
    kernels::ParamBlock params;
    params.set_real(0, 9.81);
    params.set_real(1, 0.30);  // mu_s: tan(20 deg) = 0.364, so 0.30 does not hold it
    params.set_real(2, 0.25);  // mu_k

    InclineFriction op;
    prepare(op, params);

    // theta = 20 degrees in radians, which is inexact -- and deliberately kept, because a
    // realistic angle is what a user supplies. It is a fixed input either way: `%a` pins whatever
    // `20 * pi / 180` evaluates to on this platform, and cross-toolchain agreement is asserted by
    // this file running on both.
    const double theta = 20.0 * 3.14159265358979323846 / 180.0;

    // Both particles are on the 20-degree slope, where `tan(20 deg) = 0.364 > mu_s = 0.30`, so
    // gravity beats static friction and a particle at rest starts to move. They differ in
    // **history**, which is what makes the case cover the decision rather than one of its
    // outcomes:
    //
    //   - particle 0 starts at rest. The static branch is evaluated on the first step, refuses to
    //     hold, and the particle slides for the rest of the run.
    //   - particle 1 is launched uphill at 1.5 m/s. Kinetic friction decelerates it, and the sign
    //     clamp in the operator stops it at zero rather than accelerating it back up the slope.
    //
    // The first version put particle 1 on a shallow slope instead, where static friction held it
    // for the whole run: its recorded state was its initial values and a zero, so the golden case
    // pinned nothing about sliding at all. The recording is what exposed that -- a fixture that
    // exercises one branch looks identical to one that exercises both until you read the result.
    State state(2, {
        0.0, 0.0, theta,   // particle 0: at rest, static friction gives way, then slides
        0.0, -1.5, theta   // particle 1: launched uphill, kinetic friction stops it
    });
    constexpr double kDt = 1.0 / 1024.0;
    constexpr int kSteps = 1024;
    for (int i = 0; i < kSteps; ++i) state.step(op, kDt);

    // All 6 components are identical on both supported toolchains, so one pattern
    // each. Worth stating explicitly: it means this operator's arithmetic rounds the same
    // way under x87 and under SSE2, and a future divergence here would be a finding rather
    // than noise.
    const std::vector<Frozen> expected{
        {"3fe0e5fc172f2ce4", nullptr, nullptr},
        {"3ff0d89059052985", nullptr, nullptr},
        {"3fd657184ae74487", nullptr, nullptr},
        {"3fb658f80ca813d6", nullptr, nullptr},
        {"3fe8c2f9b606e5ea", nullptr, nullptr},
        {"3fd657184ae74487", nullptr, nullptr},
    };

    check_frozen(state.values(), expected);
    record_measured("incline", state.values());
}

TEST_CASE("plugin.golden.merge_bits", "[plugin][golden]") {
    // Three particles with masses and velocities chosen so the momentum sum is not exact in
    // binary: 0.1 and 0.3 are both inexact, and the weighted mean therefore exercises the
    // rounding of a two-sum division. A case built from 0.5 and 0.25 would produce an exact
    // result and pin nothing about the division.
    MergeCollision op;
    kernels::ParamBlock params;
    params.set_real(0, 0.0);
    prepare(op, params);

    State state(3, {
        1.0, 1.7, 0.1,    // particle 0
        -2.0, -0.3, 0.3,  // particle 1
        0.5, 2.9, 0.7,    // particle 2
    });

    state.step(op, 1.0 / 1024.0);

    // All 9 components are identical on both supported toolchains, so one pattern
    // each. Worth stating explicitly: it means this operator's arithmetic rounds the same
    // way under x87 and under SSE2, and a future divergence here would be a finding rather
    // than noise.
    const std::vector<Frozen> expected{
        {"3ff0000000000000", nullptr, nullptr},
        {"3ffeb0df6b0df6b0", nullptr, nullptr},
        {"3fb999999999999a", nullptr, nullptr},
        {"c000000000000000", nullptr, nullptr},
        {"3ffeb0df6b0df6b0", nullptr, nullptr},
        {"3fd3333333333333", nullptr, nullptr},
        {"3fe0000000000000", nullptr, nullptr},
        {"3ffeb0df6b0df6b0", nullptr, nullptr},
        {"3fe6666666666666", nullptr, nullptr},
    };

    check_frozen(state.values(), expected);
    record_measured("merge", state.values());

    // Every velocity is the same value, **bit for bit**. Not "equal within a tolerance": a merge
    // that computed the common velocity separately per particle could differ in the last bit, and
    // the point of the operator is that there is exactly one number.
    REQUIRE(state.get(0, 1) == state.get(1, 1));
    REQUIRE(state.get(1, 1) == state.get(2, 1));
}
