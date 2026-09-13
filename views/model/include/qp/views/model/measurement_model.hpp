/**
 * @file measurement_model.hpp
 * @brief The closed loop as a **model**: readings in, uncertainty and a report out.
 *
 * ## Why this is not written in the QWidget
 *
 * The differentiation the whole platform rests on is "measure, record, quantify the
 * uncertainty, report it", and the part of that which can be **wrong** is arithmetic and
 * bookkeeping, not layout. A rule like "a series with no quantified uncertainty cannot
 * claim a standard error" is the kind of thing that is quietly wrong for a year if it lives
 * inside a Qt slot, because testing it costs a QApplication and an event loop and so nobody
 * does.
 *
 * So this file is ordinary C++ over `runtime/store`, `runtime/trace` and `runtime/run`, and
 * it is asserted by the ordinary Catch2 suite on **both** compilers -- including the GCC
 * build, which cannot link Qt at all. `views/qt/measurement_panel` is then thin: it renders
 * what this says and calls the methods below.
 *
 * ## The one rule this model enforces
 *
 * **Unknown uncertainty is not zero uncertainty.** `store::UncertaintyKind` has three
 * states because a real lab has three: `unknown` (nobody quantified the error), `exact` (a
 * counted or defined quantity, where u = 0 is a true statement), and `standard` (a
 * quantified standard uncertainty). Collapsing `unknown` into `exact` is the most damaging
 * simplification available: it turns "we did not measure the error" into "there is no
 * error", and every propagation afterwards produces a confidently wrong number.
 *
 * So a standard error is reported only when the readings actually carry uncertainty, and
 * when they do not, the model **says so** rather than printing a plausible figure. The
 * report's gaps section exists for the same reason: a report that claims reproducibility
 * while the run ledger is missing its toolchain sends the student looking for the
 * discrepancy in their own physics.
 *
 * @ownership   owns
 * @thread      ui (a view calls in; nothing here spawns a thread)
 * @pre         none
 * @post        none
 * @invariant   `dataset()` and `trace()` are the only sources of the reported numbers
 * @errors      See each declaration
 * @complexity  --
 * @nondet      none
 * @frozen      no
 * @tests       measurement.model.add_and_retake, measurement.model.unknown_is_not_zero,
 *              measurement.model.report_names_its_gaps
 */
#pragma once

#include <qp/runtime/io/io.hpp>
#include <qp/runtime/run/run.hpp>
#include <qp/runtime/store/store.hpp>
#include <qp/runtime/trace/trace.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace qp::views::model {

/**
 * @brief What the report says about one quantity, with the gaps left visible.
 *
 * A struct rather than a formatted string because the panel needs the parts: a table of
 * numbers and a list of warnings are different widgets, and a caller that had to parse a
 * sentence to find out whether the uncertainty was known would be a caller that eventually
 * guesses wrong.
 *
 * @ownership   owns
 * @thread      ui
 * @pre         none
 * @post        none
 * @invariant   A field is empty exactly when the corresponding quantity is unknown
 * @errors      noexcept
 * @frozen      no
 * @tests       measurement.model.unknown_is_not_zero, measurement.model.report_names_its_gaps
 */
struct ReportLine final {
    /// What was measured, as the dataset names it.
    std::string quantity{};
    /// Number of readings, rejected ones excluded.
    std::size_t count = 0;
    /// Number of readings whose uncertainty was rejected as unusable, if any.
    std::size_t rejected = 0;
    /// How many of the live readings carry a **quantified** uncertainty.
    std::size_t quantified = 0;
    /// The mean, absent when there are no live readings.
    std::optional<double> mean{};
    /// The sample standard deviation, absent when fewer than two readings are live.
    std::optional<double> sample_stddev{};
    /// The standard error of the mean, absent when fewer than two readings are live.
    std::optional<double> standard_error{};
    /// The combined standard uncertainty from the readings' own uncertainties, absent when
    /// **none** of them carry one. Its absence is the difference between "we did not
    /// measure the error" and "the error is zero".
    std::optional<double> combined_uncertainty{};
};

/**
 * @brief The measurement session: one dataset, one trace, one run ledger.
 *
 * @ownership   owns
 * @thread      ui
 * @pre         none
 * @post        none
 * @invariant   The dataset's dimension is fixed at construction and never changes
 * @errors      See each declaration
 * @frozen      no
 * @tests       measurement.model.add_and_retake
 */
class MeasurementModel final {
public:
    /**
     * @brief A session measuring one quantity.
     *
     * @param quantity Human-readable name, shown as the table's caption.
     * @param dim      The dimension every reading must have. Fixed here rather than per
     *                 reading so that a series cannot mix metres with seconds, which is the
     *                 mistake a hand-kept spreadsheet makes and no one notices until the
     *                 mean is meaningless.
     *
     * @param ledger   The session's run ledger, **borrowed**, not copied.
     *
     *                 An earlier version owned its own, and the window then had two: the status
     *                 line read one and this model's gap list the other, so the two disagreed
     *                 on screen about how many runs the session had. That is the
     *                 two-sources-of-truth failure the `authoring` layer exists to prevent,
     *                 one layer up, and borrowing is the fix -- the ledger is the window's and
     *                 this model is a view of it.
     * @param run      The run these readings belong to, when one has been started. A trace is
     *                 the record of *one* run, so it cannot exist without an identity: samples
     *                 with no run attached are the artefact the platform exists to replace,
     *                 because a number in a report cannot then be traced to the configuration
     *                 that produced it.
     *
     * @ownership   observes `ledger`
     * @thread      ui
     * @pre         `ledger` outlives this model
     * @post        `dataset().name() == quantity` and the dataset is empty
     * @invariant   The dimension never changes afterwards
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       measurement.model.add_and_retake
     */
    explicit MeasurementModel(qp::runtime::RunLedger& ledger, std::string quantity,
                              qp::units::Dim dim = {}, qp::runtime::RunId run = {});

    /**
     * @brief The readings, in the order they were taken.
     *
     * @ownership   borrows from this object
     * @thread      ui
     * @pre         none
     * @post        none
     * @invariant   Returned reference is stable until the next mutation
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       measurement.model.add_and_retake
     */
    [[nodiscard]] const qp::runtime::Dataset& dataset() const noexcept { return dataset_; }

    /**
     * @brief The time series recorded alongside the readings.
     *
     * @ownership   borrows from this object
     * @thread      ui
     * @pre         none
     * @post        none
     * @invariant   Returned reference is stable until the next mutation
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       measurement.model.trace_is_separate_from_readings
     */
    [[nodiscard]] const qp::runtime::Trace& trace() const noexcept { return trace_; }

    /**
     * @brief The runs recorded in this session, for the reproducibility line.
     *
     * @ownership   borrows from this object
     * @thread      ui
     * @pre         none
     * @post        none
     * @invariant   Returned reference is stable until the next mutation
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       measurement.model.report_names_its_gaps
     */
    [[nodiscard]] const qp::runtime::RunLedger& ledger() const noexcept { return ledger_; }

    /**
     * @brief Records one reading.
     *
     * @param value  The measured value, in the dataset's dimension.
     * @param kind   How its uncertainty is known. `unknown` is a first-class answer and is
     *               **not** the same as `exact`.
     * @param u      The standard uncertainty. Ignored unless `kind` is `standard`, because
     *               storing it otherwise would put a number in a field that means "we
     *               quantified this".
     * @param source Which node produced this reading, or a default-constructed value for a reading a user typed
     *               in. The default is the honest one for the manual case, which is a legitimate kind of reading
     *               in a lab session and has no node behind it.
     *
     * @ownership   value
     * @thread      ui
     * @pre         none
     * @post        The dataset grows by one live reading
     * @invariant   The reading's dimension is the dataset's, whatever the caller passed
     * @errors      noexcept
     * @complexity  O(1) amortized
     * @nondet      none
     * @frozen      no
     * @tests       measurement.model.add_and_retake, measurement.model.a_reading_names_its_node
     */
    void add_reading(double value,
                     qp::runtime::UncertaintyKind kind = qp::runtime::UncertaintyKind::unknown,
                     double u = 0.0,
                     qp::runtime::Measurement::Source source = {});

    /**
     * @brief Which node produced the reading at `index`, or nothing.
     *
     * The reverse lookup the panel needs in order to **point at** the thing a number came from: a report whose
     * numbers cannot be traced back to a device is the artefact this platform replaces. Nothing is returned for
     * an out-of-range index or for a reading with no source, which are different situations and the same answer.
     *
     * @param index Index into `dataset().readings()`.
     *
     * @ownership   pure
     * @thread      ui
     * @pre         none
     * @post        The source when the reading exists, has one, and is live; nothing otherwise
     * @invariant   A rejected reading still reports its source, because the record of where a discarded reading
     *              came from is part of why it was discarded
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       measurement.model.a_reading_names_its_node
     */
    [[nodiscard]] std::optional<qp::runtime::Measurement::Source> source_of(
        std::size_t index) const noexcept;

    /**
     * @brief Marks a reading as rejected, or reports that it could not be.
     *
     * Rejection is not deletion, and the distinction is the reason `Dataset` has the
     * operation at all: a reading that was taken and then discarded for a stated reason is
     * part of the record, and a student who can make one vanish has a way to reach the
     * answer they expected. The count stays visible in the report either way.
     *
     * @param index Index into `dataset().readings()`.
     *
     * @ownership   owns
     * @thread      ui
     * @pre         none
     * @post        On success the reading is excluded from every statistic
     * @invariant   The reading remains in the dataset, marked
     * @errors      Returns `out_of_range` for an index past the end
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       measurement.model.add_and_retake
     */
    [[nodiscard]] diag::Result<void> reject(std::size_t index);

    /**
     * @brief Puts a rejected reading back.
     *
     * @ownership   owns
     * @thread      ui
     * @pre         none
     * @post        On success the reading counts again
     * @invariant   Restoring a live reading succeeds and changes nothing
     * @errors      Returns `out_of_range` for an index past the end
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       measurement.model.add_and_retake
     */
    [[nodiscard]] diag::Result<void> restore(std::size_t index);

    /**
     * @brief Replaces the trace with an empty one belonging to `run`.
     *
     * Needed because a **run replaces the session**, it does not extend it. The seeded demonstration
     * describes a different experiment from the one a user just ran, and appending a real trace to it
     * would make the time axis go backwards at the seam -- which the trace refuses, so the failure would
     * appear as an append error rather than as "you started a new experiment".
     *
     * The run identity is supplied by the caller rather than minted here, because the ledger that issued
     * it is the authority on which run these numbers belong to. Two sources for that answer is the defect
     * this session already fixed once.
     *
     * @param run The run the new trace belongs to.
     *
     * @ownership   owns
     * @thread      ui
     * @pre         none
     * @post        `trace().size() == 0` and `trace().channel_count() == 0`
     * @invariant   The dataset is untouched: readings are a separate record from the time axis
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       measurement.model.reset_trace_starts_a_new_recording
     */
    void reset_trace(qp::runtime::RunId run) noexcept { trace_ = qp::runtime::Trace{run}; }

    /**
     * @brief Appends one sample to the trace.
     *
     * @param t      Simulation or wall time of the sample, non-decreasing.
     * @param values One **uncertain** value per registered channel.
     *
     * A sample carries `UncertainValue` rather than `double` because the time axis is a
     * measurement too: a sampled signal has an error bar, and a trace of bare doubles would
     * make the platform's central claim false for every number it plots. That the trace
     * module depends on `store` for the type is the same decision one layer up.
     *
     * @ownership   value
     * @thread      ui
     * @pre         `values.size()` equals the trace's channel count
     * @post        On success the trace grows by one sample
     * @invariant   An out-of-order time is refused rather than reordered, because
     *              reordering would renumber the samples already recorded
     * @errors      Propagates the trace's refusal codes; may allocate,
     *              and allocation failure terminates
     * @complexity  O(channels)
     * @nondet      none
     * @frozen      no
     * @tests       measurement.model.trace_is_separate_from_readings
     */
    [[nodiscard]] diag::Result<void> add_sample(double t,
                                                std::vector<qp::runtime::UncertainValue> values);

    /**
     * @brief Appends one sample whose channels are all plain measured values.
     *
     * A convenience for the common case -- a simulation step producing one number per
     * channel, with no per-channel uncertainty -- built on the overload above rather than
     * a second code path.
     *
     * @ownership   owns
     * @thread      ui
     * @pre         `values.size()` equals the trace's channel count
     * @post        Equivalent to `add_sample(t, ...)` with each value marked measured
     * @invariant   Identical refusal codes to the overload above
     * @errors      Propagates the trace's refusal codes
     * @complexity  O(channels)
     * @nondet      none
     * @frozen      no
     * @tests       measurement.model.trace_is_separate_from_readings
     */
    [[nodiscard]] diag::Result<void> add_sample(double t, const std::vector<double>& values,
                                                double uncertainty = 0.0);

    /**
     * @brief Declares a trace channel. Call before the first sample.
     *
     * @ownership   value
     * @thread      ui
     * @pre         none
     * @post        On success the channel exists and later samples must supply its value
     * @invariant   Names are unique within the trace
     * @errors      Propagates the trace's refusal codes; may allocate,
     *              and allocation failure terminates
     * @complexity  O(1) amortized
     * @nondet      none
     * @frozen      no
     * @tests       measurement.model.trace_is_separate_from_readings
     */
    [[nodiscard]] diag::Result<std::size_t> add_channel(std::string name, qp::units::Dim dim);

    /**
     * @brief Starts a run in the ledger and returns its id.
     *
     * Goes through `RunLedger::begin` rather than accepting a finished `RunRecord` for two
     * reasons. The ledger issues the id, and a caller that supplied its own could collide
     * with one already issued. And a run is **started** before it produces anything: the
     * samples that follow belong to it, and the model needs the id at that moment to give
     * its trace an identity.
     *
     * @ownership   owns
     * @thread      ui
     * @pre         none
     * @post        The returned id is greater than every previously issued one
     * @invariant   The record is stored as given; this model never edits a run's claims
     * @errors      Copies the spec, which may allocate; a failure propagates
     * @complexity  O(spec)
     * @nondet      none
     * @frozen      no
     * @tests       measurement.model.report_names_its_gaps
     */
    qp::runtime::RunId begin_run(qp::runtime::RunSpec spec);

    /**
     * @brief Summarises the current dataset.
     *
     * Total, and it never throws: a session with no readings produces a line whose optional
     * fields are all empty, which the panel renders as an empty row rather than as zeros.
     * A zero mean over no readings is a fabrication, and it is the one a rushed
     * implementation produces.
     *
     * @ownership   owns the result
     * @thread      ui
     * @pre         none
     * @post        `line.count == dataset().valid_count()`
     * @invariant   `line.combined_uncertainty` has a value only when `line.quantified > 0`
     * @errors      May allocate; allocation failure terminates, as elsewhere in this project
     * @complexity  O(n) in the reading count
     * @nondet      none
     * @frozen      no
     * @tests       measurement.model.unknown_is_not_zero
     */
    [[nodiscard]] ReportLine report_line() const;

    /**
     * @brief The gaps a reader must be told about, in the order they should be told.
     *
     * Empty means the session is complete enough to report as it stands. Anything in the
     * list is a statement about **what is missing**, phrased so the reader can act on it:
     * "no reading carries a quantified uncertainty" rather than "uncertainty: N/A".
     *
     * The distinction matters more than it looks. A report that is silent about a gap is
     * claiming there is no gap, and a student whose write-up is graded on uncertainty
     * analysis needs to know whether their error bars are real before they submit it.
     *
     * @ownership   owns the result
     * @thread      ui
     * @pre         none
     * @post        Empty exactly when no gap applies
     * @invariant   Every entry names a concrete absence
     * @errors      May allocate; allocation failure terminates, as elsewhere in this project
     * @complexity  O(n) in the reading count plus the ledger size
     * @nondet      none
     * @frozen      no
     * @tests       measurement.model.report_names_its_gaps
     */
    [[nodiscard]] std::vector<std::string> gaps() const;

    /**
     * @brief Whether a format can carry this session, and why not when it cannot.
     *
     * Delegates to the io module's pre-flight check rather than re-deriving the answer: the
     * refusal must be the **same** refusal the exporter would give, or the panel will offer
     * an export that then fails. This is the "the user can see the limitation" side of the
     * io module's contract, made visible one layer up.
     *
     * @param format The exporter to test. It is an `IExporter` rather than a `FormatDesc`
     *               because the io module's check takes the exporter -- the answer depends on
     *               what the implementation can actually write, and a description is a claim
     *               about that rather than a substitute for it.
     *
     * @ownership   pure
     * @thread      ui
     * @pre         none
     * @post        Indicates `ok` exactly when `check_export` would
     * @invariant   Agrees with `check_export` for every exporter and session
     * @errors      noexcept
     * @complexity  O(channels)
     * @nondet      none
     * @frozen      no
     * @tests       measurement.model.export_refusal_matches_the_exporter
     */
    [[nodiscard]] qp::runtime::ExportRefusal export_readiness(
        const qp::runtime::IExporter& format) const;

    /**
     * @brief Whether this session's **readings** may be written by `format`, asked before any dialog.
     *
     * Delegated to `check_export` for the reason the trace's readiness is: a panel that computed this itself could
     * pass a format that the exporter then refuses, and the user's experience would be a button that raises an error
     * dialog. The labels are left out because whether a format can write this table does not depend on what the rows
     * are called.
     *
     * @ownership   pure
     * @thread      ui
     * @pre         none
     * @post        The same answer `check_export` gives for `readings_export_request`
     * @invariant   Never writes and never touches the filesystem
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       measurement.model.a_readings_request_names_the_table
     */
    [[nodiscard]] qp::runtime::ExportRefusal readings_readiness(
        const qp::runtime::IExporter& format) const;

    /**
     * @brief The request an export of this session would run, ready for `check_export` or an exporter.
     *
     * The **policy** an export carries lives here, in one place, and the reason is the same one that made
     * `export_readiness` delegate: a caller that assembled its own request could require something the
     * panel did not, and the two answers would differ for the same session. The policy is:
     *
     *   - the trace is this session's, borrowed for the duration of the call and not retained;
     *   - the uncertainty is **required exactly when the session has quantified any**. A user who entered
     *     an uncertainty and then exported to a format that drops it would lose the distinction the panel
     *     exists to show; a user who quantified nothing has nothing to lose, and refusing every format for
     *     them would be a rule nobody could act on.
     *
     * @param path Where the export would go. Not read by this function; carried so the request is complete.
     *
     * @ownership   observes `path` for the call; the returned request borrows this model's trace
     * @thread      ui
     * @pre         none
     * @post        `trace` is this session's trace and `require_uncertainty` is the policy above
     * @invariant   `export_readiness(format)` equals `check_export(format, export_request(path))`
     * @errors      May allocate (the path is copied); allocation failure terminates
     * @complexity  O(path)
     * @nondet      none
     * @frozen      no
     * @tests       measurement.model.export_request_carries_the_policy
     */
    [[nodiscard]] qp::runtime::ExportRequest export_request(std::string path) const;

    /**
     * @brief The same policy, for the **readings** table.
     *
     * A second request rather than a flag on the first, because the two tables are different artifacts of the same
     * session and the subject is what says which one is being written: the trace is the run's samples, the readings
     * are the numbers the student wrote down. Everything that made `export_request` a member applies here too --
     * the dataset is this session's, borrowed for the call, and the uncertainty is required exactly when the session
     * has quantified any -- so the two functions cannot drift about the policy while differing about the table.
     *
     * @param path   Where the export would go. Not read here; carried so the request is complete.
     * @param labels One label per reading, naming the node each came from, or null. **Resolved by the caller**,
     *               because turning a reading's `(index, generation)` source into a name needs the graph and this
     *               layer is the one that has it. Borrowed for the call; the request does not outlive it.
     *
     * @ownership   observes `path` and `labels` for the call; the returned request borrows this model's dataset
     * @thread      ui
     * @pre         `labels` is null, or names every reading
     * @post        `subject` is `readings`, `readings` is this session's dataset, and `require_uncertainty` is the
     *              policy the trace request uses
     * @invariant   `readings_readiness(format)` equals `check_export(format, readings_export_request(path, labels))`
     * @errors      May allocate (the path is copied); allocation failure terminates
     * @complexity  O(path)
     * @nondet      none
     * @frozen      no
     * @tests       measurement.model.a_readings_request_names_the_table
     */
    [[nodiscard]] qp::runtime::ExportRequest readings_export_request(
        std::string path, const std::vector<std::string>* labels) const;

    /**
     * @brief Empties the session: no readings, no samples, and a trace belonging to `run`.
     *
     * Called when the document changes -- a new one, or one opened from a file. The readings were taken
     * while a **different** experiment was on screen, and this panel does not say which graph a reading came
     * from, so leaving them would attribute them to the document now being edited. The same reasoning as
     * `reset_trace`, one record further: a session's two records are emptied together or the panel shows
     * two experiments at once.
     *
     * The dataset's quantity and dimension survive, because they are what this session measures rather than
     * what it has measured.
     *
     * @param run The run the new, empty trace belongs to; a default-constructed id means "no run yet".
     *
     * @ownership   owns
     * @thread      ui
     * @pre         none
     * @post        `dataset().readings().empty()` and `trace().empty()`, with `trace().run() == run`
     * @invariant   The quantity and the dimension are unchanged
     * @errors      noexcept
     * @complexity  O(readings)
     * @nondet      none
     * @frozen      no
     * @tests       measurement.model.clear_session_empties_both_records
     */
    void clear_session(qp::runtime::RunId run) noexcept {
        dataset_.clear();
        trace_ = qp::runtime::Trace{run};
    }

private:
    qp::runtime::RunLedger& ledger_;
    qp::runtime::Dataset dataset_;
    qp::runtime::Trace trace_;
};

}  // namespace qp::views::model
