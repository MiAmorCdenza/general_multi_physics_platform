/**
 * @file run.hpp
 * @brief The identity of one experiment run: everything needed to reproduce it.
 *
 * ## Why this module is the one the plan tree calls "missing"
 *
 * The previous project had a graph, a version number and slots. It had no
 * concept of **a run**: no way to say "these numbers came from that
 * configuration". Without it, a result cannot be reproduced, cannot be
 * explained months later, and cannot be defended in a lab report -- which is the
 * whole point of the platform (charter R2 and C1).
 *
 * ## What a run identity has to contain, and why each field is there
 *
 * Reproduction is an all-or-nothing property: if any input is unpinned, the run
 * is not reproducible and no amount of care elsewhere fixes it.
 *
 * | field | why it cannot be omitted |
 * |---|---|
 * | seed | two runs with different seeds are different experiments |
 * | graph version | the same file reopened after an edit is a different graph |
 * | plugin versions | a plugin update silently changes the physics |
 * | toolchain | different compilers round floating point differently |
 * | optimisation level | the same compiler at -O2 and -O0 may differ |
 * | parameters | a parameter changed by hand is the commonest irreproducibility |
 *
 * ## The honest part: gaps are reported, not papered over
 *
 * A `RunRecord` reports which of those it actually has. The reason is that a
 * record which *claims* to be reproducible while missing the toolchain is worse
 * than one that admits the gap: the first sends a student looking for a
 * difference in their physics, the second tells them to re-run on the same
 * machine.
 *
 * Two separate questions are therefore kept apart:
 *
 *   - `is_complete()` -- were all the inputs captured? A fact about the record.
 *   - `numerics_are_reproducible()` -- does this experiment claim bit-exactness
 *     across machines? A claim made by the author, recorded rather than assumed.
 *
 * A plugin written with `-ffast-math` or with parallel reductions is *not*
 * bit-reproducible across machines, and the platform cannot discover that by
 * itself. So it asks, records the answer, and never infers one.
 *
 * @ownership   owns
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   A field appears at most once in a record's parameter list
 * @errors      noexcept
 * @complexity  --
 * @nondet      only through the system clock, and only in now_unix_seconds()
 * @frozen      no
 * @tests       run.spec.completeness, run.spec.missing_fields,
 *              run.record.roundtrip, run.id.monotonic,
 *              run.spec.summary_is_stable_and_greppable,
 *              run.started_at_is_a_real_clock
 */
#pragma once

// The one qp header this module names, and it arrived with `RunLedger::restore`: adopting a record from a document
// is a call that can be refused, and a refusal has to be a value rather than an exception. `run` may depend on
// `diag` (see the layer table) -- what it may not do is depend on a *view* of the runs, which is why the ledger
// reports through `Result` and the window turns that into a sentence.
#include <qp/diag/result.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace qp::runtime {

/// @brief Identifies one run within a session. Monotonic, never reused.
struct RunId final {
    std::uint64_t value = 0;

    [[nodiscard]] constexpr bool valid() const noexcept { return value != 0; }
    [[nodiscard]] friend constexpr bool operator==(RunId a, RunId b) noexcept {
        return a.value == b.value;
    }
    [[nodiscard]] friend constexpr bool operator!=(RunId a, RunId b) noexcept {
        return !(a == b);
    }
};

/**
 * @brief Which reproducibility inputs a spec actually carries.
 *
 * A bit set means "captured". The absent bits are the useful part: they are what
 * turns "I cannot reproduce this" into "the toolchain was not recorded, re-run
 * with it recorded".
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   Distinct bits mean distinct inputs
 * @errors      noexcept
 * @frozen      yes (renumbering would change what a recorded run claims)
 * @tests       run.spec.missing_fields
 */
enum class ReproField : std::uint32_t {
    none = 0,
    seed = 1U << 0,
    graph_version = 1U << 1,
    toolchain = 1U << 2,
    optimisation = 1U << 3,
    plugin_versions = 1U << 4,
    parameters = 1U << 5,
};

[[nodiscard]] constexpr ReproField operator|(ReproField a, ReproField b) noexcept {
    return static_cast<ReproField>(static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));
}

/// @brief Whether `f` is set in `set`.
[[nodiscard]] constexpr bool has_field(ReproField set, ReproField f) noexcept {
    return (static_cast<std::uint32_t>(set) & static_cast<std::uint32_t>(f)) != 0;
}

/// @brief Every field a complete record must carry.
inline constexpr ReproField kAllReproFields =
    ReproField::seed | ReproField::graph_version | ReproField::toolchain |
    ReproField::optimisation | ReproField::plugin_versions | ReproField::parameters;

/// @brief Stable short name of a field, for the missing-fields report.
[[nodiscard]] constexpr const char* to_string(ReproField f) noexcept {
    switch (f) {
        case ReproField::none: return "none";
        case ReproField::seed: return "seed";
        case ReproField::graph_version: return "graph_version";
        case ReproField::toolchain: return "toolchain";
        case ReproField::optimisation: return "optimisation";
        case ReproField::plugin_versions: return "plugin_versions";
        case ReproField::parameters: return "parameters";
    }
    return "unknown";
}

/**
 * @brief One recorded parameter: a name and its exact textual value.
 *
 * Text, not a number, because a parameter can be a number, a choice, a unit
 * string or a file reference, and because the recorded form has to be **stable
 * enough to compare**: a float formatted two different ways would show a
 * spurious difference between two runs that were actually identical. The writer
 * therefore stores the same text the graph holds, not a re-rendering of it.
 *
 * @ownership   owns
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   `name` is non-empty for a recorded parameter
 * @errors      noexcept
 * @frozen      no
 * @tests       run.record.roundtrip
 */
struct RecordedParameter final {
    std::string name{};
    std::string value{};
    /// The unit symbol, when the parameter has one. Empty for a bare number.
    std::string unit{};

    [[nodiscard]] friend bool operator==(const RecordedParameter& a,
                                         const RecordedParameter& b) {
        return a.name == b.name && a.value == b.value && a.unit == b.unit;
    }
};

/// @brief One plugin and the version it was at. Both are needed: an id alone
///        cannot tell two builds apart, and a version alone cannot tell two
///        plugins apart.
struct PluginPin final {
    std::string id{};
    std::string version{};

    [[nodiscard]] friend bool operator==(const PluginPin& a, const PluginPin& b) {
        return a.id == b.id && a.version == b.version;
    }
};

/**
 * @brief The description of a run: its identity, and every input it pins.
 *
 * @ownership   owns
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   `fields` describes exactly which of the six inputs are non-empty
 * @errors      noexcept
 * @frozen      no
 * @tests       run.spec.completeness
 */
struct RunSpec final {
    /// The run seed. Charter R2: reproduction means *the same seed*, so a run
    /// without one is not an experiment, it is an anecdote.
    std::uint64_t seed = 0;
    /// The graph version the run started from.
    std::uint64_t graph_version = 0;
    /// Compiler and version that produced the binaries, e.g. "msvc-19.51".
    std::string toolchain{};
    /// Optimisation level as the compiler saw it, e.g. "RelWithDebInfo".
    std::string optimisation{};
    /// Every plugin that took part, with its version.
    std::vector<PluginPin> plugins{};
    /// Parameters in the order the caller recorded them.
    std::vector<RecordedParameter> parameters{};
    /// Which of the six inputs the caller actually supplied.
    ReproField fields = ReproField::none;
    /// Whether the author claims bit-exact reproduction across machines.
    bool bit_exact = false;
    /// Wall-clock seconds when the run started. 0 means "not recorded".
    std::int64_t started_at = 0;

    /// @brief Whether every reproducibility input is present.
    [[nodiscard]] bool is_complete() const noexcept {
        return (static_cast<std::uint32_t>(fields) &
                static_cast<std::uint32_t>(kAllReproFields)) ==
               static_cast<std::uint32_t>(kAllReproFields);
    }

    /**
     * @brief The inputs that are missing, as a bit set.
     *
     * This is the value that makes an incomplete record actionable: it names what
     * to record next time instead of leaving a student to guess.
     *
     * @ownership   pure
     * @thread      any
     * @pre         none
     * @post        Zero when is_complete()
     * @invariant   missing() | fields contains every field present in fields
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       run.spec.missing_fields
     */
    [[nodiscard]] ReproField missing() const noexcept;

    /// @brief Missing inputs spelled out, in a fixed order, for a log line.
    [[nodiscard]] std::vector<std::string> missing_names() const noexcept;

    /**
     * @brief A stable one-line summary, safe to write into a log.
     *
     * Contains the seed and the completeness verdict, never the parameters: a
     * log line that embedded a whole parameter set would be unreadable, and the
     * parameters are already recorded in full in the record itself.
     *
     * @ownership   pure
     * @thread      any
     * @pre         none
     * @post        Returns a non-empty ASCII string
     * @invariant   Same spec yields the same text
     * @errors      noexcept
     * @complexity  O(plugins + missing)
     * @nondet      none
     * @frozen      no
     * @tests       run.record.roundtrip
     */
    [[nodiscard]] std::string summary() const noexcept;

    /// @brief The recorded value of `name`, or an empty view when absent.
    [[nodiscard]] std::string_view parameter(std::string_view name) const noexcept;
};

/**
 * @brief A spec plus the identity of the run it describes.
 *
 * @ownership   owns
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   `id` is valid for a record produced by a RunLedger
 * @errors      noexcept
 * @frozen      no
 * @tests       run.record.roundtrip
 */
struct RunRecord final {
    RunId id{};
    RunSpec spec{};

    [[nodiscard]] bool is_complete() const noexcept { return spec.is_complete(); }
};

/**
 * @brief Issues run ids and keeps the records.
 *
 * Ids are issued by the ledger, never by the caller, for the same reason
 * `kernels::KernelId` is issued by its registry: a caller that picked its own id
 * would eventually collide, and the collision would look like "the wrong run was
 * reproduced" rather than like an id bug.
 *
 * @ownership   owns
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   Ids are strictly increasing and never reused
 * @errors      noexcept
 * @frozen      no
 * @tests       run.id.monotonic
 */
class RunLedger final {
public:
    RunLedger() = default;
    // A **copy** is what must not exist: two ledgers holding the same records would issue the same ids to two
    // different runs, which is the collision this class exists to prevent. A **move** is a hand-over and is
    // allowed, because a value that a parser built and a session adopts has to be able to travel -- and a
    // `DocumentSnapshot` that could not move the ledger it just read would have to hold it behind a pointer for no
    // reason. The moved-from ledger is empty, which is the ordinary post-move state.
    RunLedger(const RunLedger&) = delete;
    RunLedger& operator=(const RunLedger&) = delete;
    RunLedger(RunLedger&&) noexcept = default;
    RunLedger& operator=(RunLedger&&) noexcept = default;

    /**
     * @brief Starts a run: issues an id and stores the spec.
     *
     * @ownership   owns (copies the spec)
     * @thread      main
     * @pre         none
     * @post        The returned id is greater than every previously issued id
     * @invariant   A stored record is never modified afterwards
     * @errors      noexcept
     * @complexity  O(spec)
     * @nondet      none
     * @frozen      no
     * @tests       run.id.monotonic
     */
    RunId begin(RunSpec spec) noexcept;

    /**
     * @brief Adopts a record **with its own id**, for a ledger being rebuilt from a document.
     *
     * The ids a document holds are the ones a reading's trace and a report's provenance point back into, so
     * restoring records with fresh ids would be worse than dropping them: the numbers would resolve to the *wrong*
     * run. This is the only way a caller supplies an id, and the ledger's own promise is enforced rather than
     * relaxed -- "ids are strictly increasing and never reused" -- so a record whose id is not greater than
     * `last_id()` is **refused**, which is what stops a hand-edited or corrupted file from forging a ledger whose
     * order cannot be trusted.
     *
     * @param record The record, with the id it was written under.
     *
     * @ownership   owns (copies the record)
     * @thread      main
     * @pre         none
     * @post        On success `find(record.id)` is that record and `last_id()` is `record.id`
     * @invariant   The records stay ordered by id and no id is reused
     * @errors      noexcept; reports `invalid_argument` through the result for an invalid id or one not greater
     *              than `last_id()`, with no state change
     * @complexity  O(spec)
     * @nondet      none
     * @frozen      no
     * @tests       run.id.a_restored_record_keeps_its_id
     */
    [[nodiscard]] qp::diag::Result<void> restore(RunRecord record) noexcept;

    /// @brief The record for `id`, or null when the ledger never issued it.
    [[nodiscard]] const RunRecord* find(RunId id) const noexcept;

    /// @brief Every record, in issue order.
    [[nodiscard]] const std::vector<RunRecord>& records() const noexcept { return records_; }

    /// @brief Number of runs started.
    [[nodiscard]] std::size_t size() const noexcept { return records_.size(); }

    /// @brief The most recently issued id, or an invalid id when empty.
    [[nodiscard]] RunId last_id() const noexcept;

    /**
     * @brief How many stored runs are missing at least one reproducibility input.
     *
     * The number a lab supervisor wants: it counts the results in a session that
     * cannot be defended.
     *
     * @ownership   pure
     * @thread      main
     * @pre         none
     * @post        Zero when every stored record is complete
     * @invariant   Never exceeds size()
     * @errors      noexcept
     * @complexity  O(records)
     * @nondet      none
     * @frozen      no
     * @tests       run.ledger.incomplete_count
     */
    [[nodiscard]] std::size_t incomplete_count() const noexcept;

private:
    std::vector<RunRecord> records_;
    std::uint64_t next_id_ = 1;
};

/**
 * @brief Describes the binary that is running, from the compiler's own macros.
 *
 * Measured rather than configured, and that is deliberate: a hand-written
 * "-O2" string in a config file describes what someone intended, while the
 * macros describe what the compiler did. For a reproducibility record only the
 * second is worth anything.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        Returns a non-empty identifier such as "msvc-19.51" or "gcc-15.2"
 * @invariant   Constant for the lifetime of the process
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       run.toolchain.is_measured
 */
[[nodiscard]] const char* toolchain_id() noexcept;

/**
 * @brief The optimisation level the binary was built with, from `NDEBUG` and the
 *        compiler's own macros.
 *
 * Reports what it can actually tell. A build with `-O2` on GCC reports "release";
 * it does not invent "-O2", because `__OPTIMIZE__` says only that optimisation is
 * on, not which level -- and a record that guessed would be worse than one that
 * says less than it knows.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        Returns "debug", "release", or "unknown"; never empty
 * @invariant   Constant for the lifetime of the process
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       run.optimisation.is_derived_from_macros
 */
[[nodiscard]] const char* optimisation_id() noexcept;

/// @brief Current wall-clock seconds since the Unix epoch, UTC.
[[nodiscard]] std::int64_t now_unix_seconds() noexcept;

}  // namespace qp::runtime
