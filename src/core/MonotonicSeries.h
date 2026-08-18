// Qt-free monotonic interpolation helpers shared by the core analysis,
// comparison alignment, Track Atlas spatial mapping, and the trace store.
// All functions are pure: no globals, no I/O, no Qt types.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace omatrack {

/// Non-owning view over a pair of monotonically non-decreasing arrays.
/// Both pointers must outlive the view. n < 2 is handled with degenerate
/// clamping so callers do not need a separate size guard.
struct MonotonicView {
    const double* x = nullptr;
    const double* y = nullptr;
    size_t n = 0;

    /// Linear interpolation of y at query x. Clamps to the endpoints.
    double at(double xq) const {
        if (n == 0) return 0.0;
        if (n == 1) return y[0];
        if (xq <= x[0]) return y[0];
        if (xq >= x[n - 1]) return y[n - 1];
        const size_t hi = lowerIndex(xq);
        if (hi == 0) return y[0];
        const size_t lo = hi - 1;
        const double span = x[hi] - x[lo];
        const double local = span > 0.0 ? (xq - x[lo]) / span : 0.0;
        return y[lo] + (y[hi] - y[lo]) * local;
    }

    /// Inverse: given a y value, return the interpolated x. Assumes y is
    /// also monotonically non-decreasing. Clamps to the endpoints.
    double invert(double yq) const {
        if (n == 0) return 0.0;
        if (n == 1) return x[0];
        if (yq <= y[0]) return x[0];
        if (yq >= y[n - 1]) return x[n - 1];
        const double* upper = std::lower_bound(y, y + n, yq);
        if (upper == y) return x[0];
        if (upper == y + n) return x[n - 1];
        const size_t hi = size_t(upper - y);
        const size_t lo = hi - 1;
        const double span = y[hi] - y[lo];
        const double local = span > 0.0 ? (yq - y[lo]) / span : 0.0;
        return x[lo] + (x[hi] - x[lo]) * local;
    }

    /// Index of the first element whose x is >= xq, clamped to [0, n-1].
    size_t lowerIndex(double xq) const {
        if (n <= 1) return 0;
        const double* upper = std::lower_bound(x, x + n, xq);
        return std::min(size_t(upper - x), n - 1);
    }
};

/// Linear interpolation of y at xq over explicit (x, y) vectors.
/// Clamps to the endpoints. Returns y[0] (or 0) for degenerate inputs.
inline double interpolate(const std::vector<double>& x,
                          const std::vector<double>& y, double xq) {
    if (x.size() != y.size() || x.size() < 2)
        return y.empty() ? 0.0 : y[0];
    MonotonicView view{x.data(), y.data(), x.size()};
    return view.at(xq);
}

/// Interpolate a uniformly-spaced y vector at fractional position [0,1].
/// Equivalent to MonotonicView over x=[0,1,...,n-1], y=data,
/// query=fraction*(n-1). Clamps fraction to [0,1].
inline double interpolateFraction(const std::vector<double>& y,
                                  double fraction) {
    if (y.empty()) return 0.0;
    if (y.size() == 1) return y[0];
    fraction = std::clamp(fraction, 0.0, 1.0);
    const double position = fraction * double(y.size() - 1);
    const size_t lo = size_t(std::floor(position));
    const size_t hi = std::min(lo + 1, y.size() - 1);
    return y[lo] + (y[hi] - y[lo]) * (position - double(lo));
}

/// Inverse of interpolateFraction: given a y value, return the fraction
/// [0,1] that would produce it. Assumes y is monotonically non-decreasing.
inline double invertFraction(const std::vector<double>& y, double yq) {
    if (y.size() < 2) return 0.0;
    if (yq <= y.front()) return 0.0;
    if (yq >= y.back()) return 1.0;
    const auto upper = std::lower_bound(y.cbegin(), y.cend(), yq);
    if (upper == y.cbegin()) return 0.0;
    if (upper == y.cend()) return 1.0;
    const size_t hi = size_t(upper - y.cbegin());
    const size_t lo = hi - 1;
    const double span = y[hi] - y[lo];
    const double local = span > 0.0 ? (yq - y[lo]) / span : 0.0;
    return (double(lo) + local) / double(y.size() - 1);
}

}  // namespace omatrack
