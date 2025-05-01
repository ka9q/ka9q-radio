# Robust Noise Floor Estimation from FFT Bin Powers

Given FFT bin powers `P_k ~ Exp(μ)`, the goal is to estimate the mean noise power `μ` despite signal contamination, without relying on long-term temporal averaging.

---

## 1. Quantile-Based Estimator

If `p_q` is the `q`-th quantile of the bin powers, then:

`μ = p_q / -ln(1 - q)`

---

## 2. Average of Lowest Fraction `q`

If you sort all bin powers and average the lowest `q·N` values:

`μ = mean_lowest_q * [1 / (1 - q + q·ln(1 - q))]`

---

## 3. Average of Bins Below a Threshold

Let `p_q` be the `q`-th quantile, and define a threshold `T * p_q`.

Let:

`z = T * (-ln(1 - q))`

Then:

`μ = mean_of_bins_below_threshold * [1 / (1 - (z * exp(-z)) / (1 - exp(-z)))]`

---

## FFT Power Normalization

To get correct power values per bin:

`P_k = |X_k|^2 / (N * sum(w[n]^2))`

Where:
- `N` is the FFT size
- `w[n]` is the window function applied before the FFT

---

## Notes

- **Quantiles and averages must be computed in the linear power domain**, not in dB.
- These formulas assume enough bins are unaffected by signal.
- Windowing is optional if bins are wide and the signal is sparse — rectangular windows are acceptable in this context.
