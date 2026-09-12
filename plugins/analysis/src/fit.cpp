/**
 * @file fit.cpp
 * @brief Weighted least squares by the normal equations, with a Cholesky solve.
 *
 * ## Why Cholesky and not an explicit inverse
 *
 * The normal matrix `A^T W A` is symmetric positive definite whenever the columns are linearly independent, so
 * it has a Cholesky factorisation `L L^T` and the solve is two triangular passes. Computing `(A^T W A)^-1` by
 * Gauss-Jordan and multiplying would give the same answer on paper and a worse one in double precision, because
 * the inverse of a nearly singular matrix is where the digits go. The covariance **is** an inverse mathematically
 * -- `(A^T W A)^-1` -- so it is computed by inverting the factor, one column at a time, which keeps the
 * triangular structure instead of forming an inverse of an inverse.
 *
 * The singularity test is on the Cholesky pivot rather than on a determinant: a determinant is a product of
 * `n` numbers and underflows long before the matrix is actually singular, so a determinant threshold refuses
 * well-conditioned systems with a large dynamic range. A pivot that is not positive means the matrix is not
 * positive definite, which means the columns are dependent, which is the fact worth reporting.
 */
#include <qp/plugins/analysis/fit.hpp>

#include <qp/diag/error.hpp>

#include <cmath>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace qp::plugins::analysis {
namespace {

namespace rt = qp::runtime;

/// @brief The largest degree this file will fit.
///
/// A Vandermonde column is `x^k`, so `x = 100` at degree 12 is `1e24` -- and `A^T W A` then has entries that
/// differ by fifty orders of magnitude, which no double precision solve survives. The limit is not a claim that
/// degree 12 is useful; it is the degree past which the column values themselves stop being representable for
/// any abscissa a lab measures.
constexpr std::size_t kMaximumDegree = 12;

/// @brief Whether every entry of `values` is finite.
[[nodiscard]] bool all_finite(const std::vector<double>& values) noexcept {
    for (const double v : values) {
        if (!std::isfinite(v)) return false;
    }
    return true;
}

/**
 * @brief Solves `m x = b` for a symmetric positive definite `m`, and returns `m^-1`.
 *
 * One factorisation serves both answers, which is the reason they are produced together: the covariance is an
 * inverse of the same matrix the coefficients come from, and factorising twice would be two chances to disagree
 * about whether the system is singular.
 *
 * @param matrix  Row-major `n x n`, symmetric. Read, not modified.
 * @param rhs     Right-hand side, `n` entries.
 * @param inverse Filled with the row-major inverse on success.
 * @param solution Filled with the solution on success.
 *
 * @return Whether the matrix factored -- false when it is not positive definite, i.e. the fit is singular.
 */
[[nodiscard]] bool cholSolve(const std::vector<double>& matrix, const std::vector<double>& rhs,
                             std::vector<double>& inverse, std::vector<double>& solution) {
    const std::size_t n = rhs.size();
    std::vector<double> lower(n * n, 0.0);

    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = 0; j <= i; ++j) {
            double sum = matrix[i * n + j];
            for (std::size_t k = 0; k < j; ++k) sum -= lower[i * n + k] * lower[j * n + k];
            if (i == j) {
                // Not `<= 0`: a pivot of exactly zero is singular, and a pivot that is negative is not positive
                // definite. Both mean the same thing to a caller -- this system does not determine its
                // coefficients -- and the refusal is the same one.
                if (!(sum > 0.0) || !std::isfinite(sum)) return false;
                lower[i * n + i] = std::sqrt(sum);
            } else {
                lower[i * n + j] = sum / lower[j * n + j];
            }
        }
    }

    // Forward substitution then back substitution, for the solution.
    solution.assign(n, 0.0);
    for (std::size_t i = 0; i < n; ++i) {
        double sum = rhs[i];
        for (std::size_t k = 0; k < i; ++k) sum -= lower[i * n + k] * solution[k];
        solution[i] = sum / lower[i * n + i];
    }
    for (std::size_t i = n; i-- > 0;) {
        double sum = solution[i];
        for (std::size_t k = i + 1; k < n; ++k) sum -= lower[k * n + i] * solution[k];
        solution[i] = sum / lower[i * n + i];
    }

    // The inverse, one unit vector at a time. `inverse` is built column by column and written row-major, so the
    // transpose is taken at the end rather than by indexing backwards here.
    inverse.assign(n * n, 0.0);
    std::vector<double> unit(n, 0.0);
    std::vector<double> column(n, 0.0);
    for (std::size_t col = 0; col < n; ++col) {
        unit.assign(n, 0.0);
        unit[col] = 1.0;
        for (std::size_t i = 0; i < n; ++i) {
            double sum = unit[i];
            for (std::size_t k = 0; k < i; ++k) sum -= lower[i * n + k] * column[k];
            column[i] = sum / lower[i * n + i];
        }
        for (std::size_t i = n; i-- > 0;) {
            double sum = column[i];
            for (std::size_t k = i + 1; k < n; ++k) sum -= lower[k * n + i] * column[k];
            column[i] = sum / lower[i * n + i];
        }
        for (std::size_t row = 0; row < n; ++row) inverse[row * n + col] = column[row];
    }

    for (const double v : inverse) {
        if (!std::isfinite(v)) return false;
    }
    for (const double v : solution) {
        if (!std::isfinite(v)) return false;
    }
    return true;
}

}  // namespace

diag::Result<rt::FitResult> fit_linear_model(const std::vector<std::vector<double>>& design_rows,
                                             const std::vector<Point>& points, std::string model,
                                             bool scale_covariance) {
    if (design_rows.empty() || design_rows.size() != points.size()) {
        return diag::ErrorCode::invalid_argument;
    }
    const std::size_t parameters = design_rows.front().size();
    if (parameters == 0) return diag::ErrorCode::invalid_argument;
    for (const std::vector<double>& row : design_rows) {
        if (row.size() != parameters) return diag::ErrorCode::invalid_argument;
        if (!all_finite(row)) return diag::ErrorCode::invalid_argument;
    }
    for (const Point& p : points) {
        // `sigma > 0` and not `>= 0`. A point with an unknown uncertainty must not enter a weighted fit at all:
        // a zero sigma is an infinite weight, which makes one reading the entire answer. Refusing is the same
        // rule the rest of the platform holds -- an unknown uncertainty is not a zero one -- expressed where it
        // can still be acted on.
        if (!std::isfinite(p.x) || !std::isfinite(p.y)) return diag::ErrorCode::invalid_argument;
        if (!(p.sigma > 0.0) || !std::isfinite(p.sigma)) return diag::ErrorCode::invalid_argument;
    }
    // A system with no more points than parameters has no degrees of freedom: it can always be satisfied
    // exactly, so it says nothing about whether the model is right. Refused rather than returned, because a
    // zero-residual answer would be read as a perfect fit.
    if (points.size() <= parameters) return diag::ErrorCode::invalid_argument;

    const std::size_t n = points.size();
    std::vector<double> normal(parameters * parameters, 0.0);
    std::vector<double> rhs(parameters, 0.0);
    for (std::size_t i = 0; i < n; ++i) {
        const double w = 1.0 / (points[i].sigma * points[i].sigma);
        for (std::size_t a = 0; a < parameters; ++a) {
            const double wa = w * design_rows[i][a];
            rhs[a] += wa * points[i].y;
            for (std::size_t b = 0; b <= a; ++b) {
                normal[a * parameters + b] += wa * design_rows[i][b];
            }
        }
    }
    // The lower triangle was accumulated; mirror it, so the factorisation can read either.
    for (std::size_t a = 0; a < parameters; ++a) {
        for (std::size_t b = a + 1; b < parameters; ++b) {
            normal[a * parameters + b] = normal[b * parameters + a];
        }
    }

    std::vector<double> covariance;
    std::vector<double> coefficients;
    // `fit_failed`, and it was the cause `diag` was missing until this module needed it: the input is
    // well-formed -- the points are finite, the sigmas are positive, there are enough of them -- and the system
    // still does not determine its coefficients. `invalid_argument` would name the data as the problem, which
    // sends a caller looking at a table of readings that is not wrong; a fit that will not converge is a
    // different sentence.
    if (!cholSolve(normal, rhs, covariance, coefficients)) return diag::ErrorCode::fit_failed;

    rt::FitResult out;
    out.model = std::move(model);
    out.coefficients = coefficients;

    // Residuals, then chi-squared from them. Computed from the **original** rows and weights rather than from
    // the normal equations, so a mistake in the accumulation above shows up here as a non-zero chi-squared for
    // data a line passes through exactly -- which is exactly what the exact-fit case checks.
    out.residuals.resize(n, 0.0);
    double chi = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        double predicted = 0.0;
        for (std::size_t a = 0; a < parameters; ++a) predicted += design_rows[i][a] * coefficients[a];
        const double residual = points[i].y - predicted;
        out.residuals[i] = residual;
        const double w = 1.0 / (points[i].sigma * points[i].sigma);
        chi += w * residual * residual;
    }
    out.chi_squared = chi;
    out.degrees_of_freedom = static_cast<std::int32_t>(n - parameters);

    // The covariance. `(A^T W A)^-1` by convention, or the scaled estimate when the caller says the stated
    // uncertainties are not to be trusted -- two questions, two answers, and the name at the call site says
    // which one was asked.
    double factor = 1.0;
    if (scale_covariance) {
        const double dof = static_cast<double>(n - parameters);
        factor = chi / dof;
    }
    for (double& entry : covariance) entry *= factor;
    out.covariance = std::move(covariance);

    // R-squared, and its absence. The total sum of squares is about the **weighted** mean, because that is the
    // quantity the model's own intercept is compared against for a fit that has one. With no scatter at all
    // there is nothing for the model to explain, and the ratio is 0/0: absent, not 1.0, and not 0.0.
    const double total = [&points] {
        double sum_w = 0.0;
        double sum_wy = 0.0;
        for (const Point& p : points) {
            const double w = 1.0 / (p.sigma * p.sigma);
            sum_w += w;
            sum_wy += w * p.y;
        }
        const double mean = sum_wy / sum_w;
        double out_total = 0.0;
        for (const Point& p : points) {
            const double w = 1.0 / (p.sigma * p.sigma);
            out_total += w * (p.y - mean) * (p.y - mean);
        }
        return out_total;
    }();
    if (total > 0.0) {
        out.r_squared = 1.0 - chi / total;
    }

    return out;
}

diag::Result<rt::FitResult> fit_polynomial(const std::vector<Point>& points, std::size_t degree,
                                           bool scale_covariance) {
    if (degree > kMaximumDegree) return diag::ErrorCode::invalid_argument;
    const std::size_t parameters = degree + 1;

    std::vector<std::vector<double>> rows;
    rows.reserve(points.size());
    for (const Point& p : points) {
        if (!std::isfinite(p.x)) return diag::ErrorCode::invalid_argument;
        std::vector<double> row(parameters, 1.0);
        // Built by repeated multiplication rather than by `std::pow`, which is both faster and -- more to the
        // point -- gives the same value for `x*x` as a caller who wrote it out, so a test that checks a
        // coefficient against hand arithmetic is checking the fit and not the power function.
        for (std::size_t k = 1; k < parameters; ++k) row[k] = row[k - 1] * p.x;
        // A column that has overflowed to infinity would produce a normal matrix of infinities and a
        // `singular` refusal that names the wrong cause.
        if (!all_finite(row)) return diag::ErrorCode::invalid_argument;
        rows.push_back(std::move(row));
    }

    const std::string name = degree == 0 ? "mean" : (degree == 1 ? "linear" : "polynomial");
    return fit_linear_model(rows, points, name, scale_covariance);
}

std::optional<double> reduced_chi_squared(const rt::FitResult& fit) noexcept {
    if (fit.degrees_of_freedom <= 0) return std::nullopt;
    if (!std::isfinite(fit.chi_squared)) return std::nullopt;
    return fit.chi_squared / static_cast<double>(fit.degrees_of_freedom);
}

}  // namespace qp::plugins::analysis
