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


# Statistical Notes: Why Minimum-of-Averages is Biased and Quantile Methods Are Not

## Old Method: Minimum of Long-Term Averages

- Each bin had a long-term averaged power estimate (low variance due to smoothing).
- The noise power per bin followed a Gamma distribution (not exponential anymore).
- The minimum of these bins was selected as the noise estimate.

### Why This is Biased

- The minimum of random variables is always biased low.
- The more bins you have, the lower the expected minimum.
- Smoothing reduces variance but does not remove bias.

#### Result
- Stable but biased.
- Slow to respond to real noise changes.
- Statistically unpredictable bias.

---

## New Method: Quantile + Correction Factor

- Use q-th quantile of bin powers, or average of bins below threshold.
- Correct the estimate using known formulas from the exponential CDF.

### Why This is Unbiased

- Exponential distribution has exact relationship between quantile and mean:

```
mu = p_q / -ln(1 - q)
```

- Averaging multiple low-end bins is correctable using a mathematically defined correction factor.

#### Result
- Statistically unbiased (after correction).
- Low variance when averaging multiple bins.
- Fast response and mathematically sound.

---

## Summary Table

| Method                   | Bias               | Variance         | Speed     | Mathematical Foundation |
|--------------------------|--------------------|------------------|-----------|-------------------------|
| Min of smoothed bins     | Biased low          | Low, unpredictable | Very slow | Poor |
| Quantile (single bin)    | Unbiased (corrected)| Higher           | Fast      | Exact |
| Quantile + average of low bins | Unbiased (corrected) | Low | Fast | Exact |

---

## Final Conclusion

- Minimum of long-term averages looks smooth but hides systematic bias and reacts slowly.
- Quantile-based estimators, properly corrected, are unbiased, tunable, fast, and mathematically sound.
- Ideal for SDR, AGC, and real-time adaptive systems.
