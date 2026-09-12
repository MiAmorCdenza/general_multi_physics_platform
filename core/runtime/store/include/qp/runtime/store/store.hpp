/**
 * @file store.hpp
 * @brief The shape of measured data: a value, its uncertainty, and what a fit produced.
 *
 * ## Why the *structure* is foundation and every algorithm is a plugin
 *
 * This is the module the whole platform is arranged around. The differentiator is
 * the closed loop **measure -> record -> uncertainty -> report**, and the loop
 * breaks in one specific way: if every plugin invents its own way to carry an
 * uncertainty, then a regression written by one author cannot consume a series
 * produced by another, and the lab ends up back in a spreadsheet.
 *
 * So the foundation owns the shape -- `Measurement`, `Dataset`, `FitResult` -- and
 * **not one algorithm**. Mean, standard deviation, weighted mean, chi-squared and
 * every regression are content: `plugins/analysis/`. A platform that shipped its
 * own least-squares would have decided, for every course in every university,
 * what a fit means.
 *
 * ## Why an uncertainty is a variant and not a double
 *
 * A bare `double` cannot express the three states that actually occur:
 *
 *   - **unknown** -- nobody has quantified the error. Copying a `double` default
 *     of 0 here would claim a perfect measurement.
 *   - **exact** -- a definitional or counted quantity (a stopwatch reading of
 *     exactly 100 periods, a currency amount). Zero uncertainty is *true*.
 *   - **standard** -- a quantified standard uncertainty, `u`.
 *
 * Collapsing "unknown" into "exact" is the single most damaging simplification
 * available in this module: it turns "we did not measure the error" into "there
 * is no error", and every downstream propagation then produces a confidently
 * wrong result. So the three states are distinct in the type, and a propagation
 * that meets an unknown uncertainty must report that it cannot answer -- which is
 * what `UncertainValue::is_usable()` is for.
 *
 * ## Why the unit is a dimension and not a string
 *
 * `units::Dim` is checked: `m + s` is a compile-time error in this project, and a
 * dataset that stored "kg" as text would lose that. The *display* symbol is
 * derived from the dimension by `units::unit_symbol` at the moment something
 * draws it, not stored.
 *
 * ## Why outlier rejection is not here
 *
 * Deciding that a point is an outlier is a physics judgement, and a library that
 * made it silently would let a student delete the measurement that disagreed with
 * their hypothesis. The structure records; the analyst decides.
 *
 * @ownership   owns
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   An uncertain value's uncertainty is never negative
 * @errors      noexcept
 * @complexity  --
 * @nondet      none
 * @frozen      no
 * @tests       store.uncertainty.states, store.dataset.statistics,
 *              store.fit.result_shape
 */
#pragma once

#include <qp/diag/result.hpp>
#include <qp/units/dim.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace qp::runtime {

/**
 * @brief How well a quantity's uncertainty is known.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   `standard` implies `u >= 0` and a finite value
 * @errors      noexcept
 * @frozen      no
 * @tests       store.uncertainty.states
 */
enum class UncertaintyKind : std::uint8_t {
    /// Nobody has quantified the error. **Not** the same as zero.
    unknown = 0,
    /// Definitional or counted: the value is exact, `u == 0` is true.
    exact = 1,
    /// A quantified standard uncertainty `u`.
    standard = 2,
};

/// @brief Stable short name of an uncertainty kind, for a report or a log.
[[nodiscard]] constexpr const char* to_string(UncertaintyKind k) noexcept {
    switch (k) {
        case UncertaintyKind::unknown: return "unknown";
        case UncertaintyKind::exact: return "exact";
        case UncertaintyKind::standard: return "standard";
    }
    return "unknown";
}

/**
 * @brief A measured quantity: a value, how well its error is known, and its dimension.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   kind == standard implies u is finite and >= 0
 * @errors      noexcept
 * @frozen      no
 * @tests       store.uncertainty.states
 */
struct UncertainValue final {
    /// The measured value, in the dataset's unit.
    double value = 0.0;
    /// The standard uncertainty `u`. Meaningless unless kind == standard.
    double u = 0.0;
    UncertaintyKind kind = UncertaintyKind::unknown;
    /// The physical dimension. Checked arithmetic depends on this being right.
    units::Dim dim{};

    /// @brief An exact value: the uncertainty is genuinely zero.
    [[nodiscard]] static constexpr UncertainValue exact(double v, units::Dim d = {}) noexcept {
        return UncertainValue{v, 0.0, UncertaintyKind::exact, d};
    }

    /// @brief A value with a quantified standard uncertainty.
    [[nodiscard]] static constexpr UncertainValue measured(double v, double uncertainty,
                                                          units::Dim d = {}) noexcept {
        return UncertainValue{v, uncertainty < 0.0 ? 0.0 : uncertainty,
                              UncertaintyKind::standard, d};
    }

    /// @brief A value whose error nobody has quantified.
    [[nodiscard]] static constexpr UncertainValue unquantified(double v,
                                                              units::Dim d = {}) noexcept {
        return UncertainValue{v, 0.0, UncertaintyKind::unknown, d};
    }

    /// @brief Whether a propagation may use this value.
    ///
    /// False for an unknown uncertainty. This is the query that stops the
    /// platform from quietly reporting a confidently wrong uncertainty: a
    /// propagation that meets an unknown must refuse, and the refusal is the
    /// information the user needs.
    [[nodiscard]] constexpr bool is_usable() const noexcept {
        return kind != UncertaintyKind::unknown;
    }

    /// @brief Relative standard uncertainty `u / |value|`, or nothing when unusable
    ///        or when the value is zero (where the ratio is undefined).
    [[nodiscard]] std::optional<double> relative_uncertainty() const noexcept;
};

/**
 * @brief One reading in a series: a value, an optional time, and a validity flag.
 *
 * `valid` exists so that a rejected or missing reading stays visible in the
 * dataset instead of being deleted. A series that silently dropped its
 * inconvenient points would make the mean wrong and the record dishonest; the
 * flag keeps the decision inspectable.
 *
 * @ownership   owns
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   A timestamp is either absent or non-decreasing across a dataset
 * @errors      noexcept
 * @frozen      no
 * @tests       store.dataset.valid_flags
 */
struct Measurement final {
    UncertainValue reading{};
    /// Seconds since the start of the run. Absent for a non-temporal series.
    std::optional<double> t{};
    /// Whether the analyst still counts this reading. False keeps it in the record.
    bool valid = true;
    /// Which node produced this reading, as a `(index, generation)` pair, or an invalid one when nothing did.
    ///
    /// ## Why a reading says where it came from
    ///
    /// "Measure -> record -> uncertainty -> report" is the loop this platform exists for, and the report is the
    /// end of it -- but a number in a report that cannot be traced back to the device that produced it is the
    /// artefact the loop replaces. The trace's channels carry their originating node for the same reason; this
    /// carries it for a **reading**, which is the other thing a dataset holds.
    ///
    /// Flat integers rather than a `NodeId`, and for the same layering reason `trace::Channel` gives: `store` is
    /// L2 and a node identifier belongs to L1's IR. The pair is layout-compatible with a `NodeId` by construction
    /// and a caller converts.
    ///
    /// Invalid for a reading a user typed in by hand, which is a legitimate kind of reading in a lab session and
    /// has no node behind it.
    struct Source final {
        std::uint32_t index = 0;
        std::uint32_t generation = 0;

        [[nodiscard]] constexpr bool valid() const noexcept { return index != 0 && generation != 0; }
        [[nodiscard]] friend constexpr bool operator==(Source a, Source b) noexcept {
            return a.index == b.index && a.generation == b.generation;
        }
        [[nodiscard]] friend constexpr bool operator!=(Source a, Source b) noexcept {
            return !(a == b);
        }
    };
    Source source{};
};

/**
 * @brief A series of readings of one quantity.
 *
 * @ownership   owns
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   Every measurement shares the dataset's dimension
 * @errors      noexcept
 * @frozen      no
 * @tests       store.dataset.statistics
 */
class Dataset final {
public:
    Dataset() = default;
    explicit Dataset(std::string name, units::Dim dim = {})
        : name_(std::move(name)), dim_(dim) {}

    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] units::Dim dim() const noexcept { return dim_; }
    void set_name(std::string name) { name_ = std::move(name); }

    /**
     * @brief Appends a reading.
     *
     * @ownership   owns
     * @thread      main
     * @pre         none
     * @post        size() grows by one
     * @invariant   The reading's dimension is overwritten with the dataset's, so a
     *              mixed-dimension series cannot be constructed by accident
     * @errors      noexcept
     * @complexity  O(1) amortized
     * @nondet      none
     * @frozen      no
     * @tests       store.dataset.statistics
     */
    void add(UncertainValue reading) noexcept;

    /// @brief Appends a bounded value with a quantified uncertainty.
    void add(double value, double uncertainty) noexcept;

    /**
     * @brief Appends a whole record: the reading **and** where it came from.
     *
     * The two `add` overloads above drop everything but the value, which is right for a reading with no
     * provenance and wrong for one an instrument produced. A caller that has a source must use this, because the
     * alternative -- appending the value and then reaching back into `readings()` to stamp it -- would leave the
     * record briefly claiming the number came from nowhere.
     *
     * @param reading The record. Its reading's dimension is overwritten with the dataset's, as above.
     *
     * @ownership   value
     * @thread      main
     * @pre         none
     * @post        `readings().back() == reading` up to the dimension normalisation, and `size()` is one larger
     * @invariant   The record is appended once; nothing else in the series changes
     * @errors      noexcept
     * @complexity  O(1) amortized
     * @nondet      none
     * @frozen      no
     * @tests       store.dataset.add_keeps_the_source
     */
    void add(Measurement record) noexcept;

    /// @brief Every reading, valid or not. The record keeps the rejected ones.
    [[nodiscard]] const std::vector<Measurement>& readings() const noexcept { return items_; }

    /**
     * @brief Marks a reading as rejected, **keeping it in the series**.
     *
     * The reading stays, with `valid == false`. Deleting it would make the
     * decision invisible: a reader could not tell an outlier that was rejected
     * from one that was never taken, and the mean would look clean while hiding
     * why. Rejection is a judgement, and a judgement has to be inspectable.
     *
     * @ownership   owns
     * @thread      main
     * @pre         none
     * @post        On success readings()[index].valid is false
     * @invariant   size() is unchanged; other readings are untouched
     * @errors      noexcept; returns out_of_range when the index is past the end
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       store.dataset.valid_flags
     */
    [[nodiscard]] diag::Result<void> reject(std::size_t index) noexcept;

    /// @brief Un-rejects a reading, so a mistaken rejection is recoverable.
    [[nodiscard]] diag::Result<void> restore(std::size_t index) noexcept;

    /// @brief Number of readings, valid or not.
    [[nodiscard]] std::size_t size() const noexcept { return items_.size(); }
    [[nodiscard]] bool empty() const noexcept { return items_.empty(); }

    /// @brief Number of readings the analyst still counts.
    [[nodiscard]] std::size_t valid_count() const noexcept;

    /**
     * @brief The arithmetic mean of the valid readings.
     *
     * Nothing when there are none. **An empty mean is not zero**: a caller that
     * treated it as zero would report a measurement that was never taken.
     *
     * @ownership   pure
     * @thread      main
     * @pre         none
     * @post        Nothing when valid_count() == 0
     * @invariant   Unaffected by readings with valid == false
     * @errors      noexcept
     * @complexity  O(n)
     * @nondet      none
     * @frozen      no
     * @tests       store.dataset.statistics
     */
    [[nodiscard]] std::optional<double> mean() const noexcept;

    /**
     * @brief The sample standard deviation (n-1 denominator) of the valid readings.
     *
     * Nothing when fewer than two valid readings. The n-1 denominator, not n:
     * these readings are a sample of a distribution, not the whole population,
     * and using n would understate the spread by a factor that matters most
     * exactly when n is small -- which is every student lab.
     *
     * Computed by Welford's method rather than `sum(x^2) - n*mean^2`. The naive
     * formula loses catastrophic precision when the values are large and their
     * spread is small, which is the normal case for a lab reading (a set of
     * periods near 1.5 s, differing in the third decimal).
     *
     * @ownership   pure
     * @thread      main
     * @pre         none
     * @post        Nothing when valid_count() < 2
     * @invariant   Unaffected by invalid readings
     * @errors      noexcept
     * @complexity  O(n)
     * @nondet      none
     * @frozen      no
     * @tests       store.dataset.statistics, store.dataset.welford_precision
     */
    [[nodiscard]] std::optional<double> sample_stddev() const noexcept;

    /**
     * @brief The standard uncertainty of the **mean**: `s / sqrt(n)`.
     *
     * This is the quantity Type-A uncertainty is about (charter R3). The chart is
     * explicit that the platform must produce it, and it is deliberately not the
     * same as `sample_stddev`: reporting the spread of the readings where the
     * uncertainty of the mean was meant is the classic error this overload
     * exists to make hard.
     *
     * @ownership   pure
     * @thread      main
     * @pre         none
     * @post        Nothing when valid_count() < 2
     * @invariant   Equals sample_stddev() / sqrt(valid_count())
     * @errors      noexcept
     * @complexity  O(n)
     * @nondet      none
     * @frozen      no
     * @tests       store.dataset.statistics
     */
    [[nodiscard]] std::optional<double> standard_error() const noexcept;

    /// @brief Number of valid readings that carry a quantified standard uncertainty.
    [[nodiscard]] std::size_t quantified_count() const noexcept;

    /**
     * @brief The inverse-variance weighted mean of the quantified valid readings.
     *
     * Nothing when no reading carries a quantified uncertainty. Weights are
     * `1/u^2`, which is what makes a precise reading count for more than a coarse
     * one -- and why a dataset of unquantified readings cannot produce one.
     *
     * @ownership   pure
     * @thread      main
     * @pre         none
     * @post        Nothing when quantified_count() == 0
     * @invariant   Equals sum(x/u^2) / sum(1/u^2)
     * @errors      noexcept
     * @complexity  O(n)
     * @nondet      none
     * @frozen      no
     * @tests       store.dataset.weighted_mean
     */
    [[nodiscard]] std::optional<double> weighted_mean() const noexcept;

    /// @brief The uncertainty of the weighted mean: `1 / sqrt(sum(1/u^2))`.
    [[nodiscard]] std::optional<double> weighted_mean_uncertainty() const noexcept;

    /**
     * @brief Drops every reading, keeping the dataset's identity.
     *
     * For a session that has moved on to a different experiment: those readings were taken of another
     * system, and a panel showing them beside the new graph would attribute them to it -- the panel does
     * not say which graph a reading came from, so the only honest answer is that they are not part of this
     * record any more.
     *
     * The name and the dimension **survive**, because they are what the session is measuring rather than
     * what was measured: a caller that wants a different quantity constructs a different dataset, which is
     * why the constructor takes both and there is no setter for the dimension.
     *
     * @ownership   owns
     * @thread      main
     * @pre         none
     * @post        `readings().empty()`, every statistic is absent, and `name()`/`dim()` are unchanged
     * @invariant   The dimension of a dataset never changes
     * @errors      noexcept
     * @complexity  O(readings)
     * @nondet      none
     * @frozen      no
     * @tests       store.dataset.clear_removes_readings
     */
    void clear() noexcept { items_.clear(); }

private:
    std::string name_{};
    units::Dim dim_{};
    std::vector<Measurement> items_{};
};

/**
 * @brief What a regression produced.
 *
 * The shape only: coefficients, their covariance, the residuals, and the usual
 * summary statistics. Every number here is *reported*, never computed by this
 * module -- which is why the type has no methods beyond lookup.
 *
 * `covariance` is a full matrix and not just the diagonal because off-diagonal
 * terms are what let a caller report a *correlated* uncertainty, and a fit that
 * reported only variances would make the covariance of two derived quantities
 * unobtainable afterwards.
 *
 * @ownership   owns
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   covariance is empty, or square with side == coefficients.size()
 * @errors      noexcept
 * @frozen      no
 * @tests       store.fit.result_shape
 */
struct FitResult final {
    /// Stable name of the model that produced the fit, e.g. "linear".
    std::string model{};
    /// Fitted coefficients, in the model's own order.
    std::vector<double> coefficients{};
    /// Row-major covariance matrix, or empty when the fit did not provide one.
    std::vector<double> covariance{};
    /// Residuals, one per fitted point, in the input order.
    std::vector<double> residuals{};
    /// Sum of squared weighted residuals.
    double chi_squared = 0.0;
    /// Degrees of freedom: points minus parameters, or -1 when not applicable.
    std::int32_t degrees_of_freedom = -1;
    /// Coefficient of determination, or nothing when the fit cannot define one.
    std::optional<double> r_squared{};

    /// @brief Whether the covariance matrix is square and matches the coefficient count.
    [[nodiscard]] bool has_covariance() const noexcept;

    /// @brief Whether the fit reproduced the points exactly (chi-squared == 0).
    [[nodiscard]] bool is_exact() const noexcept { return chi_squared == 0.0; }

    /// @brief The standard uncertainty of coefficient `i`: `sqrt(covariance[i][i])`.
    ///
    /// Nothing when there is no covariance or the diagonal entry is negative
    /// (which a malformed fit can produce, and which must not become a NaN that
    /// spreads silently through a report).
    [[nodiscard]] std::optional<double> coefficient_uncertainty(std::size_t i) const noexcept;

    /// @brief The covariance of coefficients `i` and `j`, or nothing.
    [[nodiscard]] std::optional<double> covariance_of(std::size_t i,
                                                      std::size_t j) const noexcept;
};

}  // namespace qp::runtime
