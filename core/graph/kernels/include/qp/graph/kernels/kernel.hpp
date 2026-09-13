/**
 * @file kernel.hpp
 * @brief The native operator contract: what a time-stepping kernel must be, and how it is registered.
 *
 * ## The split this module exists to enforce
 *
 * The foundation owns the **contract**; the concrete integrators are **plugins**.
 * Boris, leapfrog, RK4 and Verlet are physics content: a classroom that wants a
 * different push does not want to rebuild the platform, and a platform that
 * hard-codes four integrators has to grow a fifth every semester.
 *
 * So this header defines what an operator *is* and how the host drives it, and
 * nothing else. There is no integrator in this file, and there must never be
 * one: the moment a specific scheme appears here, the plugin boundary is gone.
 *
 * ## Why the interface is a virtual call and not a step counter
 *
 * Requirement: 20,000 particles x 5 steps per frame x 60 fps must run with **no
 * allocation and no dispatch per particle**. That fixes the shape of the
 * interface:
 *
 *   - the host resolves one `IBatchAdvancer` per plan operation, **once**, at
 *     load time;
 *   - then it calls `advance` once per step, passing a `BatchView` that already
 *     describes every particle;
 *   - the implementation loops over particles internally.
 *
 * The virtual call therefore happens five times per frame per operator, not a
 * hundred thousand times. What must *not* happen is a call back into the host
 * per particle -- that is the shape that cannot be made fast, and this interface
 * does not offer it.
 *
 * ## Determinism is part of the contract, not a quality goal
 *
 * Charter R2 requires bit-for-bit reproduction from the same seed. A kernel that
 * reaches for a global random source, a wall clock, or an unordered container
 * makes that impossible, and the failure is invisible: the run finishes, the
 * numbers look plausible, and the replay differs. Hence:
 *
 *   - an operator must be a pure function of (state, parameters, step index);
 *   - randomness, if a scheme needs it, must come from `AdvanceContext::rng`,
 *     which the host seeds from the run seed plus the operator's slot index, so
 *     the same seed reproduces the same stream on every machine;
 *   - `advance` must not allocate, must not throw, and must not block.
 *
 * @ownership   pure (describes a contract; owns no state)
 * @thread      main (registration), eval (advance)
 * @pre         none
 * @post        none
 * @invariant   No integrator scheme is implemented in this module
 * @errors      noexcept
 * @complexity  --
 * @nondet      none
 * @frozen      no
 * @tests       kernel.registry.register_and_find, kernel.batch_view.span,
 *              kernel.param_block.slots, kernel.determinism.same_seed_same_stream
 */
#pragma once

#include <qp/abi/lattice.hpp>
#include <qp/diag/diagnostic.hpp>
#include <qp/diag/result.hpp>
#include <qp/graph/field.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

namespace qp::graph::kernels {

/**
 * @brief Stable identifier of a registered kernel.
 *
 * Allocated by the registry, never by the plugin. A plugin that picked its own
 * numeric id would eventually collide with another plugin, and the collision
 * would show up as "the wrong integrator ran", which is indistinguishable from
 * a physics bug.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   Two different registered kernels never share an id
 * @errors      noexcept
 * @frozen      yes (a published id must not be renumbered)
 * @tests       kernel.registry.register_and_find, kernel.registry.rejects_bad_input
 */
struct KernelId final {
    std::uint32_t index = 0;   ///< Registry slot; 0 is a sentinel meaning "none"

    [[nodiscard]] constexpr bool valid() const noexcept { return index != 0; }
    [[nodiscard]] friend constexpr bool operator==(KernelId a, KernelId b) noexcept {
        return a.index == b.index;
    }
    [[nodiscard]] friend constexpr bool operator!=(KernelId a, KernelId b) noexcept {
        return !(a == b);
    }
};

/// @brief The sentinel "no kernel" identifier.
inline constexpr KernelId kNoKernel{0};

/**
 * @brief What an operator does to its inputs, and what it needs from the host.
 *
 * The host reads these before running anything: `is_in_place` decides whether the
 * caller must hand over private state, `needs_scratch` decides whether a work
 * buffer has to be reserved once, and `is_stochastic` decides whether the
 * reproducibility ledger has to record an RNG stream.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   A non-stochastic operator must ignore AdvanceContext::rng
 * @errors      noexcept
 * @frozen      no
 * @tests       kernel.capabilities.flags
 */
enum class Capability : std::uint32_t {
    none = 0,
    /// Reads and writes the same buffers; the caller must not share them.
    is_in_place = 1U << 0,
    /// Needs a scratch buffer of at least ParticleBatch::scratch_bytes.
    needs_scratch = 1U << 1,
    /// Consumes AdvanceContext::rng and therefore affects reproducibility.
    is_stochastic = 1U << 2,
    /// Reads neighbouring particles (sorting, SPH, collision detection).
    is_neighbourhood = 1U << 3,
};

[[nodiscard]] constexpr Capability operator|(Capability a, Capability b) noexcept {
    return static_cast<Capability>(static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));
}

/// @brief Whether every bit of `f` is set in `caps`.
[[nodiscard]] constexpr bool has_capability(Capability caps, Capability f) noexcept {
    return (static_cast<std::uint32_t>(caps) & static_cast<std::uint32_t>(f)) != 0;
}

/**
 * @brief What an operator does when its state leaves the range it can represent.
 *
 * ## Why this is a declaration and not a private choice
 *
 * Charter C2 requires that a run never explodes numerically, and names "clamping before
 * accuracy" as the means. The requirement cannot be satisfied by each operator deciding
 * quietly, because the two available answers are **not** equivalent and the difference is
 * visible in the physics:
 *
 *   - A clamped run stays finite and is **wrong** past the clamp. The user sees a
 *     simulation that keeps running, which is what makes it dangerous.
 *   - An unclamped run produces a non-finite value, which the host can refuse to record
 *     and can report. The user sees a failure.
 *
 * Both are legitimate; which one is right depends on the experiment. A demonstration of
 * a pendulum at large amplitude wants the clamp, because the alternative is a blank
 * screen. A measurement wants the refusal, because a number past the clamp is
 * indistinguishable from a number before it. So the policy is **declared** so the host can
 * tell the user which one is in force, and so C8's confidence panel has something to
 * report when a clamp has actually fired.
 *
 * What the platform must never do is let an operator clamp silently. A silent clamp is a
 * wrong answer wearing a right answer's clothes, and `clamps_fired()` exists so that
 * "this run was clamped 41 000 times" can be shown rather than inferred.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   `limit` is finite and positive for every policy except `none`
 * @errors      noexcept
 * @frozen      no
 * @tests       kernel.clamp_policy.presets, kernel.clamp_policy.applies,
 *              kernel.clamp_policy.counts_what_it_changed
 */
struct ClampPolicy final {
    /// Whether to clamp at all. False means a non-finite state is passed on as-is.
    bool finite_only = true;
    /// Largest permitted absolute value of any state component a step writes.
    ///
    /// A magnitude rather than a per-component bound because "how big can this get before
    /// it is meaningless" is a question about the quantity, and answering it per component
    /// would need the operator to know which component means what -- which the host does,
    /// and it does not want to.
    double limit = 1.0e12;

    /**
     * @brief Clamp nothing. A non-finite value is the caller's problem.
     *
     * @ownership   pure
     * @thread      any
     * @pre         none
     * @post        `finite_only` is false
     * @invariant   The identity policy: applying it never changes a value
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       kernel.clamp_policy.presets
     */
    [[nodiscard]] static constexpr ClampPolicy none() noexcept {
        ClampPolicy p;
        p.finite_only = false;
        p.limit = 0.0;
        return p;
    }

    /**
     * @brief Refuse to write a non-finite value, with no magnitude bound.
     *
     * `limit` is set to 0 explicitly, and `apply` reads 0 as "no bound". Returning a
     * default-constructed policy instead would inherit the member initialiser `1.0e12` and
     * silently impose a magnitude bound on a policy documented as having none -- so a large
     * but perfectly finite value would come back altered while the caller had every reason
     * to expect it untouched. The default exists for callers who construct the struct
     * directly; it is not a decision either preset should inherit by accident.
     *
     * @ownership   pure
     * @thread      any
     * @pre         none
     * @post        `finite_only` is true and no finite value is altered
     * @invariant   Finite inputs pass through unchanged, bit for bit
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       kernel.clamp_policy.presets
     */
    [[nodiscard]] static constexpr ClampPolicy finite() noexcept {
        ClampPolicy p;
        p.finite_only = true;
        p.limit = 0.0;
        return p;
    }

    /**
     * @brief Refuse non-finite values and bound the magnitude.
     *
     * @ownership   pure
     * @thread      any
     * @pre         `max_abs` is finite and positive
     * @post        `limit == max_abs`
     * @invariant   A value within `[-max_abs, max_abs]` passes through unchanged
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       kernel.clamp_policy.presets
     */
    [[nodiscard]] static constexpr ClampPolicy bounded(double max_abs) noexcept {
        ClampPolicy p;
        p.finite_only = true;
        p.limit = max_abs;
        return p;
    }
};

/**
 * @brief Outcome of applying a `ClampPolicy` to one value.
 *
 * Returned rather than written through a pointer so the caller **cannot** ignore it
 * without saying so, and so a hot loop can accumulate the flag into an integer and inspect
 * it once per batch instead of once per sample.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   `changed` is true exactly when `value != original`
 * @errors      noexcept
 * @frozen      no
 * @tests       kernel.clamp_policy.applies, kernel.clamp_policy.counts_what_it_changed
 */
struct ClampResult final {
    /// The value to write.
    double value = 0.0;
    /// Whether the policy altered it. The evidence C8's panel reports.
    bool changed = false;
};

/**
 * @brief Applies `policy` to one value, reporting whether anything changed.
 *
 * Placed in the foundation rather than in each operator because C2 makes it a platform
 * guarantee, and three operators with three copies of this arithmetic is three chances for
 * one of them to differ on the NaN case -- which is the case that matters, because a NaN
 * compares false against every bound and a clamp written as
 * `if (v > limit) v = limit;` lets it through.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        For a `finite_only` policy, `value` is finite
 * @invariant   A value already inside the permitted range is returned unchanged, bit for bit
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       kernel.clamp_policy.applies, kernel.clamp_policy.counts_what_it_changed
 */
[[nodiscard]] constexpr ClampResult apply(const ClampPolicy& policy, double value) noexcept {
    // `finite_only` is false only for `ClampPolicy::none()`, whose whole purpose is to
    // report a non-finite state rather than hide it.
    if (!policy.finite_only) return ClampResult{value, false};

    // The finite test comes **first**, and that ordering is the point. A NaN compares false
    // against every bound, so a magnitude test written before this one would fall through,
    // write the NaN onward, and report that nothing had changed.
    //
    // `value != value` is the NaN test, written this way rather than as `std::isnan` because
    // this function is `constexpr` and `std::isnan` is not. The self-comparison is the
    // standard spelling the standard library itself uses for exactly this reason.
    constexpr double kInf = std::numeric_limits<double>::infinity();
    if (value != value || value > kInf || value < -kInf) {
        return ClampResult{0.0, true};
    }
    if (policy.limit > 0.0) {
        if (value > policy.limit) return ClampResult{policy.limit, true};
        if (value < -policy.limit) return ClampResult{-policy.limit, true};
    }
    return ClampResult{value, false};
}

/**
 * @brief Fixed-size parameter block handed to an operator.
 *
 * Why fixed-size instead of a variant or a map: this block lives inside a plan
 * operation, and a plan operation is resolved once and then executed thousands of
 * times per second. A `std::variant` of arbitrary plugin types would put a type
 * switch and a possible allocation on that path.
 *
 * The layout is private to the operator: it documented its own slots in its
 * manifest, and `prepare` is where it rejects a block it cannot use. The host
 * never interprets the contents.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   Unused slots stay 0
 * @errors      noexcept
 * @frozen      no
 * @tests       kernel.param_block.slots
 */
struct ParamBlock final {
    /// Number of double slots. Sized for the widest scheme the platform ships.
    ///
    /// **Twelve, and the scheme that forced the number is worth recording.** It was eight, which covers every
    /// operator written against a point-valued state: a step size, a bound, a coefficient or two. The first
    /// operator that did not fit was a relativistic Boris push reading a **baked volume field**, and the reason
    /// belongs to the field vocabulary rather than to the push -- `field::FieldValue` describes a lattice's
    /// **counts** and its element type and says nothing about **where the lattice sits**. An operator that
    /// interpolates between samples therefore has to be told the grid's origin and spacing, six numbers, and this
    /// block is where they go: the alternative is a new type in `core/graph/field` carrying physical geometry,
    /// and that module exists precisely because it has no opinion about physics.
    ///
    /// Four scalars and six grid numbers is ten; the block is twelve so the next operator to need one has
    /// somewhere to put it. The cost of the four extra slots is 32 bytes **per plan operation** -- this block is
    /// resolved once per plan and then executed thousands of times -- and an operator that had to launder geometry
    /// through a side channel to avoid those bytes would be paying for them where nobody could see it.
    static constexpr std::size_t kDoubles = 12;
    /// Number of integer slots (enum choices, flags, counts).
    static constexpr std::size_t kIntegers = 8;

    double reals[kDoubles] = {};        ///< Floating-point parameters
    std::int64_t integers[kIntegers] = {};  ///< Integer parameters

    [[nodiscard]] constexpr double real(std::size_t i) const noexcept {
        return i < kDoubles ? reals[i] : 0.0;
    }
    [[nodiscard]] constexpr std::int64_t integer(std::size_t i) const noexcept {
        return i < kIntegers ? integers[i] : 0;
    }
    constexpr void set_real(std::size_t i, double v) noexcept {
        if (i < kDoubles) reals[i] = v;
    }
    constexpr void set_integer(std::size_t i, std::int64_t v) noexcept {
        if (i < kIntegers) integers[i] = v;
    }
};

/**
 * @brief One batch of state an operator works on, in the field vocabulary.
 *
 * A batch is described, never owned: the host decides where particle state lives
 * (one arena, several buffers, a mapped device buffer) and the operator only sees
 * described spans. That is what allows the same operator to run on a CPU array in
 * a test and on a packed buffer in production without a second implementation.
 *
 * @ownership   observes
 * @thread      eval
 * @pre         none
 * @post        none
 * @invariant   `in` and `out` never alias when the operator is not in-place
 * @errors      noexcept
 * @frozen      no
 * @tests       kernel.batch_view.span
 */
struct BatchView final {
    field::FieldValue* in = nullptr;    ///< Input state, `count` entries
    field::FieldValue* out = nullptr;   ///< Output state, `count` entries (may equal `in`)
    std::size_t count = 0;              ///< Number of state entries in each array

    [[nodiscard]] constexpr bool valid() const noexcept {
        return in != nullptr && out != nullptr && count > 0;
    }
};

/**
 * @brief What the host tells an operator about the current step.
 *
 * @ownership   observes
 * @thread      eval
 * @pre         none
 * @post        none
 * @invariant   `step` increases by exactly one per host step and never wraps
 *              within a run
 * @errors      noexcept
 * @frozen      no
 * @tests       kernel.determinism.same_seed_same_stream
 */
struct AdvanceContext final {
    double dt = 0.0;            ///< Step size; a fixed value for a reproducible run
    std::uint64_t step = 0;     ///< Step index since the run started
    std::uint64_t seed = 0;     ///< Run seed, already mixed with the operator's slot
    std::uint64_t* rng = nullptr;  ///< Seeded state; may be null for a deterministic operator
    void* scratch = nullptr;    ///< Scratch area, non-null when needs_scratch is set
    std::size_t scratch_bytes = 0;

    /// @brief Advances the injected RNG and returns a value in [0, 1).
    ///
    /// A splitmix64 step: identical on every compiler and every platform, which
    /// is the whole point. `std::mt19937` would also be deterministic, but its
    /// distribution adaptors are not (libstdc++ and MSVC differ), so the contract
    /// hands out a raw uniform double and nothing else.
    [[nodiscard]] double next_unit() noexcept {
        if (rng == nullptr) return 0.0;
        *rng += 0x9E3779B97F4A7C15ULL;
        std::uint64_t z = *rng;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        z = z ^ (z >> 31);
        // 53 significant bits, the exact precision of a double's mantissa.
        return static_cast<double>(z >> 11) * (1.0 / 9007199254740992.0);
    }
};

/**
 * @brief The contract every time-stepping operator implements.
 *
 * Implementations are provided by plugins. The host resolves one per plan
 * operation and calls `advance` once per step.
 *
 * @ownership   observes (an implementation is owned by whoever registered it)
 * @thread      main (prepare), eval (advance)
 * @pre         none
 * @post        none
 * @invariant   `name` outlives the registration; `capabilities` never changes
 *              after registration
 * @errors      `prepare` returns a Result; `advance` must not throw
 * @frozen      no
 * @tests       kernel.registry.register_and_find, kernel.advance.rejects_bad_batch
 */
class IBatchAdvancer {
public:
    IBatchAdvancer() = default;
    virtual ~IBatchAdvancer() = default;
    IBatchAdvancer(const IBatchAdvancer&) = delete;
    IBatchAdvancer& operator=(const IBatchAdvancer&) = delete;

    /// @brief Stable, unique name. Used in diagnostics and in the run ledger.
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;

    /// @brief What this operator needs from the host.
    [[nodiscard]] virtual Capability capabilities() const noexcept = 0;

    /**
     * @brief Whether one `+dt` advance followed by one `-dt` advance returns to the starting state.
     *
     * ## Why a scheme has to say this
     *
     * Reversibility is the property that separates two integrators which are indistinguishable in a forward
     * run, and it is the one a physics course actually asks about: "run it forward, run it back, are you where
     * you started" is an experiment a student can perform, and the answer differs between methods that both put
     * an oscillator on an orbit.
     *
     * **Measured before it was written**, because the first guess was wrong. RK4 on a harmonic oscillator,
     * `x0 = 1`, `v0 = 0`, `omega = 2`, relative round-trip error `max|x-x0| / |x0|`:
     *
     *     1 step    at dt=1e-2 : 8.9e-13        100 steps at dt=1e-2 : 8.9e-11
     *     1 step    at dt=1e-3 : 6.7e-16        100 steps at dt=1e-3 : 8.9e-14
     *
     * It accumulates -- a thousand steps is worse than ten -- but it accumulates from rounding rather than from
     * truncation, so the answer is **yes** and the error stays around `1e-15` per step instead of growing with
     * time. The distinction is not academic and it is not guessable: an earlier draft of this comment asserted
     * the opposite and justified a `false` declaration with a measurement that had read the *state's* own
     * magnitudes instead of the error. The declaration is only worth having because the number behind it was
     * taken from a run.
     *
     * The default is `false`, and that matters as much as the field: a scheme that has not thought about this
     * says nothing, and a run built on it cannot be reported as reversible.
     *
     * ## What `true` obliges a scheme to do
     *
     * Accept a **negative** `dt`. A scheme that declares this and then refuses `dt < 0` has made a claim nothing
     * can check, which is exactly what `execution.model.a_time_reversible_operator_round_trips` exists to
     * refuse: it runs the round trip and fails if the scheme will not take it.
     *
     * @ownership   pure
     * @thread      any
     * @pre         none
     * @post        none
     * @invariant   Constant for the object's lifetime; false for an operator declaring `is_stochastic`
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       execution.model.a_time_reversible_operator_round_trips
     */
    [[nodiscard]] virtual bool is_time_reversible() const noexcept { return false; }

    /**
     * @brief Load-time validation and one-off setup.
     *
     * Called **before** any step runs, so that a parameter block the operator
     * cannot use is rejected while the user is still editing the graph rather
     * than in the middle of a run. Must not allocate per step; allocating here
     * is fine and expected.
     *
     * @ownership   observes
     * @thread      main
     * @pre         none
     * @post        On success the operator can service advance() with this block
     * @invariant   Calling prepare twice with equal arguments has the same effect as once
     * @errors      Returns an ErrorCode from the plugin domain on rejection
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       kernel.prepare.rejects_without_scratch, kernel.prepare.counts_calls
     */
    [[nodiscard]] virtual diag::Result<void> prepare(const ParamBlock& params) = 0;

    /**
     * @brief Advances the batch by one step.
     *
     * @param batch The state. `in` and `out` may alias when the operator is in-place.
     * @param ctx   The step context. `ctx.dt` is **non-zero and finite**; negative is legal, because a
     *              round trip is how `is_time_reversible` is checked and a claim nothing can falsify is a
     *              comment. A scheme that cannot honour a negative step must refuse it rather than
     *              substituting a magnitude.
     *
     * @ownership   observes
     * @thread      eval
     * @pre         batch.valid() and, when needs_scratch is set, ctx.scratch != nullptr
     * @post        On success `out` holds the state after one step
     * @invariant   No allocation, no throw, no blocking, and no host callback per particle
     * @errors      Returns an ErrorCode on rejection instead of throwing
     * @complexity  O(count) or better
     * @nondet      only through ctx.rng, and only when is_stochastic is set
     * @frozen      no
     * @tests       kernel.advance.rejects_bad_batch, kernel.determinism.same_seed_same_stream
     */
    [[nodiscard]] virtual diag::Result<void> advance(const BatchView& batch,
                                                     AdvanceContext& ctx) = 0;
};

/**
 * @brief Description of a kernel: who it is, what it does, and what it needs.
 *
 * A plain value so that a registry entry can be copied, logged and compared
 * without touching the implementation.
 *
 * @ownership   observes (`name` and `impl` outlive the entry)
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   `name` is non-empty and unique within a registry
 * @errors      noexcept
 * @frozen      no
 * @tests       kernel.registry.register_and_find
 */
struct KernelDesc final {
    std::string_view name{};        ///< Stable unique name, e.g. "boris"
    std::string_view summary{};     ///< One line for the editor; may be empty
    Capability capabilities = Capability::none;
    IBatchAdvancer* impl = nullptr; ///< Non-owning; the provider owns it

    [[nodiscard]] constexpr bool valid() const noexcept {
        return !name.empty() && impl != nullptr;
    }
};

}  // namespace qp::graph::kernels
