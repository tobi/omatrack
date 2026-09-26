//! Monotonic interpolation helpers (port of `src/core/MonotonicSeries.h`).
//! Pure functions over non-decreasing arrays; no allocation.

/// `std::lower_bound`: first index whose value is not less than `value`.
#[inline]
pub fn lower_bound(values: &[f64], value: f64) -> usize {
    values.partition_point(|v| *v < value)
}

/// `std::upper_bound`: first index whose value is greater than `value`.
#[inline]
#[expect(
    clippy::neg_cmp_op_on_partial_ord,
    reason = "Negated ordered comparisons deliberately include unordered (NaN) values; preserve that behavior."
)]
pub fn upper_bound(values: &[f64], value: f64) -> usize {
    values.partition_point(|v| !(value < *v))
}

/// Non-owning view over a pair of monotonically non-decreasing arrays.
#[derive(Debug, Clone, Copy)]
pub struct MonotonicView<'a> {
    pub x: &'a [f64],
    pub y: &'a [f64],
}

impl<'a> MonotonicView<'a> {
    pub fn new(x: &'a [f64], y: &'a [f64]) -> Self {
        let n = x.len().min(y.len());
        Self {
            x: &x[..n],
            y: &y[..n],
        }
    }

    fn n(&self) -> usize {
        self.x.len()
    }

    /// Linear interpolation of y at `xq`; clamps to the endpoints.
    pub fn at(&self, xq: f64) -> f64 {
        let n = self.n();
        if n == 0 {
            return 0.0;
        }
        if n == 1 {
            return self.y[0];
        }
        if xq <= self.x[0] {
            return self.y[0];
        }
        if xq >= self.x[n - 1] {
            return self.y[n - 1];
        }
        let hi = self.lower_index(xq);
        if hi == 0 {
            return self.y[0];
        }
        let lo = hi - 1;
        let span = self.x[hi] - self.x[lo];
        let local = if span > 0.0 {
            (xq - self.x[lo]) / span
        } else {
            0.0
        };
        self.y[lo] + (self.y[hi] - self.y[lo]) * local
    }

    /// Inverse: x at a y value (y also non-decreasing); clamps.
    pub fn invert(&self, yq: f64) -> f64 {
        let n = self.n();
        if n == 0 {
            return 0.0;
        }
        if n == 1 {
            return self.x[0];
        }
        if yq <= self.y[0] {
            return self.x[0];
        }
        if yq >= self.y[n - 1] {
            return self.x[n - 1];
        }
        let upper = lower_bound(self.y, yq);
        if upper == 0 {
            return self.x[0];
        }
        if upper == n {
            return self.x[n - 1];
        }
        let hi = upper;
        let lo = hi - 1;
        let span = self.y[hi] - self.y[lo];
        let local = if span > 0.0 {
            (yq - self.y[lo]) / span
        } else {
            0.0
        };
        self.x[lo] + (self.x[hi] - self.x[lo]) * local
    }

    /// Index of the first element whose x is >= `xq`, clamped to [0, n-1].
    pub fn lower_index(&self, xq: f64) -> usize {
        let n = self.n();
        if n <= 1 {
            return 0;
        }
        lower_bound(self.x, xq).min(n - 1)
    }
}

/// Linear interpolation of y at `xq` over explicit (x, y); clamps.
pub fn interpolate(x: &[f64], y: &[f64], xq: f64) -> f64 {
    if x.len() != y.len() || x.len() < 2 {
        return y.first().copied().unwrap_or(0.0);
    }
    MonotonicView::new(x, y).at(xq)
}

/// Interpolate a uniformly spaced array at fraction [0, 1]; clamps.
#[expect(
    clippy::cast_possible_truncation,
    clippy::cast_precision_loss,
    clippy::cast_sign_loss,
    reason = "Preserve the C++ port's sample-index widths and rounding at this numerical boundary; verified by parity."
)]
pub fn interpolate_fraction(y: &[f64], fraction: f64) -> f64 {
    let n = y.len();
    if n == 0 {
        return 0.0;
    }
    if n == 1 {
        return y[0];
    }
    let fraction = crate::num::clamp(fraction, 0.0, 1.0);
    let position = fraction * (n - 1) as f64;
    let lo = position.floor() as usize;
    let hi = (lo + 1).min(n - 1);
    y[lo] + (y[hi] - y[lo]) * (position - lo as f64)
}

/// Inverse of [`interpolate_fraction`] for a non-decreasing array.
#[expect(
    clippy::cast_precision_loss,
    reason = "Preserve the C++ port's sample-index widths and rounding at this numerical boundary; verified by parity."
)]
pub fn invert_fraction(y: &[f64], yq: f64) -> f64 {
    if y.len() < 2 {
        return 0.0;
    }
    if yq <= y[0] {
        return 0.0;
    }
    if yq >= y[y.len() - 1] {
        return 1.0;
    }
    let upper = lower_bound(y, yq);
    if upper == 0 {
        return 0.0;
    }
    if upper == y.len() {
        return 1.0;
    }
    let hi = upper;
    let lo = hi - 1;
    let span = y[hi] - y[lo];
    let local = if span > 0.0 { (yq - y[lo]) / span } else { 0.0 };
    (lo as f64 + local) / (y.len() - 1) as f64
}
