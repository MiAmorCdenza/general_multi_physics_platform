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
    static constexpr std::size_t kDoubles = 8;
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
