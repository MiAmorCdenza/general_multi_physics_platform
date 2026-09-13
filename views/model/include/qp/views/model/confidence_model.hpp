/**
 * @file confidence_model.hpp
 * @brief Whether a run's numbers are physics or arithmetic. Charter C8.
 *
 * ## What C8 actually asks for
 *
 * > "Model confidence panel: energy drift and div-B drift must be visible -- numerical error must
 * > not be mistaken for physics."
 *
 * The clause names a panel, but the load-bearing half is the reason. A student who watches an
 * oscillator's amplitude decay over ten thousand periods is looking at a **property of RK4**, not at
 * damping. Nothing in the graph says so, and nothing in a plot of `x(t)` distinguishes the two. The
 * only way to tell them apart is to measure a quantity the physics **conserves** and watch it fail
 * to be conserved.
 *
 * So this file computes the diagnostics and `views/qt/confidence_panel` displays them. The split is
 * the same as everywhere else in this layer: the arithmetic that can be wrong is Qt-free and
 * asserted on both compilers, and the widget only renders what the model decided.
 *
 * ## The honest limit of an energy diagnostic
 *
 * Energy requires knowing the **potential**, and a trace carries only position and velocity. For a
 * harmonic oscillator the potential is `0.5 * w^2 * x^2` and the caller supplies `w`. For anything
 * else -- a pendulum, a Coulomb field, a graph someone built this morning -- the potential is a
 * different function and this model cannot know it.
 *
 * That limit is stated rather than papered over. The alternative considered and rejected was to
 * **infer** `w` from the trace, by fitting or by differencing the velocity channel. It works on a
 * clean sinusoid and is a projection rather than a measurement: a run whose amplitude is decaying
 * because RK4 dissipates would be fitted to a slightly different `w`, the fitted energy would come
 * out conserved, and the diagnostic would report that all is well **precisely when it is not**.
 * A diagnostic that fails in the direction of false confidence is worse than no diagnostic.
 *
 * So `w` is an input, its default makes the report say which potential it assumed, and a caller
 * whose Hamiltonian is not quadratic is told that the number does not apply to them.
 *
 * ## Absent is not zero
 *
 * `energy_drift()` is absent -- not zero -- when the trace does not carry both channels a mechanical
 * state needs, or when the span is not positive. Zero says "this run conserves energy"; absent says
 * "this run cannot be asked". A report showing 0.0 for a trace of temperatures would assert energy
 * conservation about a quantity it never measured, which is the failure the measurement model
 * already refuses for uncertainty.
 *
 * @ownership   observes (holds a reference; owns no data)
 * @thread      ui
 * @pre         The referenced trace outlives this model
 * @post        none
 * @invariant   No number reported here was computed anywhere but in this file
 * @errors      See each declaration
 * @complexity  --
 * @nondet      none
 * @frozen      no
 * @tests       confidence.energy_drift_is_measured, confidence.absent_is_not_zero,
 *              confidence.clamp_count_is_reported, confidence.notes_name_the_mechanism
 */
#pragma once

#include <qp/runtime/trace/trace.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace qp::views::model {

/**
 * @brief One conservation law, read off the endpoints of a trace.
 *
 * **The confidence panel used to have exactly one diagnostic -- the harmonic oscillator's energy -- and that was the
 * minimal shape while one kind of run existed.** A magnetosphere run records `speed`, `energy`, `mu` and `radius`,
 * and the panel said "energy drift cannot be measured: the trace is missing displacement and velocity" about it,
 * which is true and useless: that run has two conservation laws of its own, and they are not the same kind of
 * statement.
 *
 *   - **`measures_error` is true** when a drift in this quantity is the **integrator's**: a magnetic force does no
 *     work, so a particle's speed is conserved by the *motion* and any change in it came from the step. The
 *     oscillator's energy is the same statement about a quadratic potential.
 *   - **`measures_error` is false** when a drift is a **property of the configuration**: the first adiabatic
 *     invariant `m v_perp^2 / 2B` holds only while the field varies slowly over a gyro-orbit, so its drift says how
 *     adiabatic *this experiment* is. A panel that showed the two in one column would be telling a student their
 *     physics is wrong when their experiment is simply not adiabatic -- which is why the distinction is a field
 *     rather than a word in a label.
 *
 * @ownership   owns
 * @thread      ui
 * @pre         none
 * @post        none
 * @invariant   `relative_drift` is absent exactly when `first` gives no scale to divide by
 * @errors      noexcept
 * @frozen      no
 * @tests       confidence.a_magnetic_run_is_judged_by_speed_and_mu
 */
struct InvariantCheck final {
    /// Stable name of the quantity, as a report or a table quotes it: `energy`, `speed`, `mu`.
    const char* name = "";
    /// Whether a drift here is arithmetic or physics. See the type's comment.
    bool measures_error = true;
    /// The quantity at the first and the last sample.
    double first = 0.0;
    double last = 0.0;
    /// `(last - first) / |first|`. **Signed**, so dissipation and blow-up are different findings.
    ///
    /// Absent when the first value is zero or non-finite: a relative change needs a scale, and `(x - 0) / 0` is not
    /// a large drift but an undefined one. A state at rest conserves trivially and there is nothing to report.
    std::optional<double> relative_drift{};
    /// The same per unit time, so runs of different lengths can be compared. Present exactly when
    /// `relative_drift` is.
    std::optional<double> relative_rate{};
};

/**
 * @brief How much of one run is trustworthy, in the terms C8 names.
 *
 * @ownership   owns
 * @thread      ui
 * @pre         none
 * @post        none
 * @invariant   Every optional is absent exactly when the corresponding quantity is unknowable
 * @errors      noexcept
 * @frozen      no
 * @tests       confidence.energy_drift_is_measured, confidence.absent_is_not_zero,
 *              confidence.a_magnetic_run_is_judged_by_speed_and_mu
 */
struct ConfidenceReport final {
    /// Number of samples the trace holds.
    std::size_t samples = 0;
    /// Every conservation law this trace can be judged by, in the order the model looks for them.
    ///
    /// Empty when the trace carries no channel this model knows, and the notes say which channel was missing --
    /// the rule the single field had, generalised: **absent is not zero**, and a report with no checks is a
    /// statement about the trace rather than about the run.
    ///
    /// At most two entries today: the energy of a quadratic potential (when the trace carries a displacement and a
    /// velocity) or the speed of a charged particle in a magnetic field (when it carries a speed), plus the first
    /// adiabatic invariant whenever the trace carries `mu`.
    std::vector<InvariantCheck> checks{};
    /// The time span the drifts were measured over, in seconds. Present whenever any check is.
    std::optional<double> span{};
    /// How many times the operator's clamp fired. Zero is a real answer meaning "never"; there is no
    /// absent case, because the model is told the count rather than discovering it.
    std::uint64_t clamps_fired = 0;
};

/**
 * @brief Reads C8's diagnostics off one trace.
 *
 * @ownership   observes
 * @thread      ui
 * @pre         `trace` outlives this object
 * @post        none
 * @invariant   Channels are located by **name**, never by position
 * @errors      See each declaration
 * @frozen      no
 * @tests       confidence.energy_drift_is_measured
 */
class ConfidenceModel final {
public:
    /// @brief Channel name carrying position along the measured degree of freedom.
    static constexpr const char* kPositionChannel = "displacement";
    /// @brief Channel name carrying the corresponding velocity.
    static constexpr const char* kVelocityChannel = "velocity";
    /// @brief Channel name carrying a **speed** whose change is the integrator's: the Lorentz force does no work.
    static constexpr const char* kSpeedChannel = "speed";
    /// @brief Channel name carrying the first adiabatic invariant, whose change is the configuration's.
    static constexpr const char* kMuChannel = "mu";

    /// @brief Angular frequency assumed when the caller does not supply one.
    ///
    /// One radian per second, which is a real value rather than a sentinel -- a sentinel would make
    /// `omega()` unreadable and the report would have to carry a separate flag. The report says
    /// which value was used, so a caller who forgot to set it sees "assuming omega = 1" rather than
    /// a silently meaningless number.
    static constexpr double kDefaultOmega = 1.0;

    /**
     * @brief A model over one trace.
     *
     * @param trace The run's time axis, **borrowed**. Borrowed rather than copied because a
     *              confidence report about a copy of the data is a report about something else --
     *              the same argument that made the measurement session borrow the run ledger.
     *
     * @ownership   observes `trace`
     * @thread      ui
     * @pre         `trace` outlives this model
     * @post        `omega()` is `kDefaultOmega`
     * @invariant   The borrowed reference is never stored past this object's lifetime
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       confidence.energy_drift_is_measured
     */
    explicit ConfidenceModel(const qp::runtime::Trace& trace) noexcept : trace_(&trace) {}

    /**
     * @brief Declares the angular frequency of the potential this run conserves.
     *
     * Required for the energy diagnostic to mean anything, and the caller must know it: it is a
     * parameter of the graph, so the graph's own parameter block is where it comes from. See the
     * file comment for why this is not inferred from the trace.
     *
     * @ownership   observes (stores a number)
     * @thread      ui
     * @pre         `w` is finite and positive, or the call is ignored
     * @post        `omega() == w` for a usable `w`; the previous value otherwise
     * @invariant   A non-finite or non-positive value never becomes the declared frequency
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       confidence.notes_name_the_mechanism
     */
    void set_omega(double w) noexcept;

    /**
     * @brief The angular frequency the report will assume.
     *
     * @ownership   pure
     * @thread      ui
     * @pre         none
     * @post        none
     * @invariant   Positive and finite
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       confidence.notes_name_the_mechanism
     */
    [[nodiscard]] double omega() const noexcept { return omega_; }

    /**
     * @brief Whether `set_omega` was called with a usable value.
     *
     * The report uses this to say whether the frequency is the caller's or the default, which is
     * the difference between a diagnostic and a guess.
     *
     * @ownership   pure
     * @thread      ui
     * @pre         none
     * @post        none
     * @invariant   False until a usable `set_omega`
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       confidence.notes_name_the_mechanism
     */
    [[nodiscard]] bool omega_was_declared() const noexcept { return omega_declared_; }

    /**
     * @brief The trace this model reports on.
     *
     * @ownership   borrows
     * @thread      ui
     * @pre         none
     * @post        none
     * @invariant   The same object passed to the constructor
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       confidence.energy_drift_is_measured
     */
    [[nodiscard]] const qp::runtime::Trace& trace() const noexcept { return *trace_; }

    /**
     * @brief Tells the model how many times the operator's clamp fired.
     *
     * Pushed rather than pulled, because the count belongs to the **operator** and not to the trace:
     * a trace records what was written, and a clamp records what the operator wanted to write. The
     * panel calls this once a run finishes with whatever `clamps_fired()` reports.
     *
     * @ownership   observes (stores a number)
     * @thread      ui
     * @pre         none
     * @post        The report includes this count
     * @invariant   The model never invents a clamp count
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       confidence.clamp_count_is_reported
     */
    void note_clamps(std::uint64_t count) noexcept { clamps_ = count; }

    /**
     * @brief Summarises the run.
     *
     * Evaluates `E = 0.5 * v^2 + 0.5 * w^2 * x^2` at the first and last sample, reading `x` and `v`
     * from the channels named `kPositionChannel` and `kVelocityChannel`.
     *
     * The endpoints rather than a fit: the question C8 asks is whether the quantity **drifted over
     * the run**, and a least-squares slope would answer a different question (the average rate)
     * while hiding a run that is conserved on average and oscillating about it.
     *
     * @ownership   owns the result
     * @thread      ui
     * @pre         none
     * @post        `report.samples == trace().size()`
     * @invariant   `energy_drift` has a value only when both channels are present, their first
     *              samples are finite, the first energy is non-zero, and the span is positive
     * @errors      May allocate; allocation failure terminates, as elsewhere in this project
     * @complexity  O(channels)
     * @nondet      none
     * @frozen      no
     * @tests       confidence.energy_drift_is_measured, confidence.absent_is_not_zero
     */
    [[nodiscard]] ConfidenceReport report() const;

    /**
     * @brief What the report means, in sentences a student can act on.
     *
     * Empty when there is nothing to warn about. Wording is decided here rather than in the panel
     * for the same reason the measurement model decides its gaps here: two places deciding what a
     * warning says means the tested one is not the one on screen.
     *
     * Every note names a **mechanism**, not a generic caution -- "the energy fell by 0.4% over 800
     * periods, which is RK4's dissipation rather than damping in your model" rather than "energy is
     * not conserved". The first tells the reader what to do; the second tells them to worry.
     *
     * @ownership   owns the result
     * @thread      ui
     * @pre         none
     * @post        Empty exactly when no note applies
     * @invariant   Every entry names a concrete observation
     * @errors      May allocate; allocation failure terminates, as elsewhere in this project
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       confidence.notes_name_the_mechanism
     */
    [[nodiscard]] std::vector<std::string> notes() const;

private:
    /// @brief The index of the channel named `name`, or nullopt when absent.
    [[nodiscard]] std::optional<std::size_t> channel_index(const char* name) const;

    const qp::runtime::Trace* trace_;
    double omega_ = kDefaultOmega;
    bool omega_declared_ = false;
    std::uint64_t clamps_ = 0;
};

}  // namespace qp::views::model
