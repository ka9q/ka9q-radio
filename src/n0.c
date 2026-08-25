#include <complex.h>
#include <math.h>
#include <assert.h>
#include "radio.h"
#include "defaults.h"

/*
==============================================================================
 Real-Time Noise Floor Estimation Specification
==============================================================================

Overview:
---------
This method provides fast, robust, and mathematically sound estimation of
the background noise floor (N0) from FFT bin powers in real-time SDR
applications. It works on short timescales without long-term averaging,
yet remains unbiased and resilient to signal contamination.

Algorithm:
----------
1. Calculate power in FFT bins from rectangular windowed FFT.
2. Select quantile (q) of bin powers (e.g. 10% quantile).
3. Determine threshold (T) as multiplier of quantile (e.g. T = 1.5).
4. Select bins where power < T * quantile.
5. Compute the average power of selected bins.
6. Apply correction factor C to obtain unbiased N0 estimate:

    z = T * (-ln(1 - q))
    C = 1 / [1 - (z * exp(-z)) / (1 - exp(-z))]
    N0_estimate = mean(selected_bins) * C

7. Exponentially smooth the N0 estimate in linear power domain:

    N0_smoothed += alpha * (N0_estimate - N0_smoothed);

Recommended Parameters:
-----------------------
- Quantile (q):      0.10 (10%)
- Threshold (T):     1.5
- Smoothing alpha:   0.1 (at 50 Hz block update rate)

This provides:
- Low bias
- Low variance
- Fast response (approx 0.6–1 second adaptation)
- Robustness against signal contamination

SNR Calculation:
----------------
True S/N (excluding noise):

    SNR_linear = max(0, (S_measured - B * N0_smoothed) / (B * N0_smoothed))
    SNR_dB = 10 * log10(SNR_linear)

Notes:
- log(0) -> -inf, which is correct
- Negative S -> clamped to zero before log to avoid NaN

Advantages over Older Min-based Methods:
----------------------------------------
- No bias from minimum selection
- Exact correction factor for unbiased estimation
- Fast response without long-term smoothing
- Tunable tradeoffs between purity and smoothness

Recommended Application:
------------------------
- SDR receive channels
- AGC thresholding
- SNR reporting and squelch
- Fast-changing noise environments (HF, FT8, QRM)
*/

static void swap(double *a, double *b);
static int partition(double *arr, int left, int right, int pivot_index);
static double quickselect(double *arr, int left, int right, int k);
static double quantile(double *array, int n, double p);

// Complex Gaussian noise has a Rayleigh amplitude distribution. The square of the amplitudes,
// ie the energies, has an exponential distribution. The mean of an exponential distribution
// is the mean of the samples, and the standard deviation is equal to the mean.
// However, the distribution is skewed, so you have to compensate for this when computing means from partial averages
// ChatGPT helped me work out the math; its reasoning is summarized in docs/noise.md
// I'm using its method 3 (average of bins below a threshold)
double estimate_noise(chan_t const *chan,int shift){
  assert(chan != NULL);
  if(chan == NULL)
    return NAN;
  struct filter_out const *slave = &chan->filter.out;
  assert(slave != NULL);
  if(slave == NULL)
    return NAN;
  if(slave->bins <= 0)
    return 0;

  int nbins = slave->bins;
  if(nbins < Min_noise_bins)
    nbins = Min_noise_bins;

  double energies[nbins];
  struct filter_in const * const master = slave->master;
  // slave->next_jobnum already incremented by execute_filter_output
  float complex const * const fdomain = master->fdomain[(slave->next_jobnum - 1) % ND];

  if(master->in_type == REAL){
    // Only half as many bins as with complex input, all positive or all negative
    // if shift < 0, the spectrum is inverted and we'll look at it in reverse order but that's OK
    // Look between -Fs/2 and +Fs/2. If this bounces too much we *could* look wider to hopefully find more noise bins to average

    // New algorithm (thanks to ChatGPT) responds instantly to changes because it averages noise across frequency rather than time
    // but is a little more wobbly than the old minimum-of-time-smoothed-energies because it doesn't average as much
    // Higher sample rates will give more stable results because more noise-only bins will be averaged
    //
    int mbin = abs(shift) - nbins/2; // lower edge
    if(mbin < 0)
      mbin = 0; // Don't let the window go below DC
    else if (mbin + nbins > master->bins)
      mbin = master->bins - nbins; // or above nyquist

    for(int i=0; i < nbins; i++,mbin++)
      energies[i] = cnrmf(fdomain[mbin]);
  } else { // Complex input
    int mbin = shift - nbins/2; // Start at lower channel edge (upper for inverted real)
    // Complex input that often straddles DC
    if(mbin < 0)
      mbin += master->bins; // starting in negative frequencies
    else if(mbin >= master->bins)
      mbin -= master->bins;
    if(mbin < 0 || mbin >= master->bins)
      return 0; // wraparound gives me a headache. Just give up

    for(int i=0; i < nbins; i++){
      energies[i] = cnrmf(fdomain[mbin]);
      if(++mbin == master->bins)
	mbin = 0; // wrap around from neg freq to pos freq
      if(mbin == master->bins/2)
	break; // fallen off the right edge
    }
  }
  // Not sure if this could be numerically unstable, but use double anyway especially since it's only executed once
  static double correction = 0;
  if(correction == 0){
    // Compute correction only once
    double const z = N_cutoff * (-log(1-N0_NQ));
    correction = 1 / (1 - z*exp(-z)/(1-exp(-z)));
  }

  double const en = N_cutoff * quantile(energies,nbins,N0_NQ); // energy in the 10th quantile bin
  // average the noise-only bins, excluding signal bins above 1.5 * q
  double energy = 0;
  int noisebins = 0;
  for(int i=0; i < nbins; i++){
    if(energies[i] <= en){
      energy += energies[i];
      noisebins++;
    }
  }
  if(noisebins == 0)
    return 0; // No noise bins?

  energy /= noisebins;
  // Scale for distribution
  double const noise_bin_energy = energy * correction;

  // correct for FFT scaling and normalize to 1 Hz
  // With an unnormalized FFT, the noise energy in each bin scales proportionately with the number of points in the FFT
  return noise_bin_energy / ((double)master->bins * Frontend.samprate);
}

// Swap two doubles
static void swap(double *a, double *b) {
  double const tmp = *a;
  *a = *b;
  *b = tmp;
}

// Partition step for quickselect
static int partition(double *arr, int left, int right, int pivot_index) {
  double const pivot_value = arr[pivot_index];
  swap(&arr[pivot_index], &arr[right]); // Move pivot to end
  int store_index = left;

  for (int i = left; i < right; i++) {
    if (arr[i] < pivot_value) {
      swap(&arr[store_index], &arr[i]);
      store_index++;
    }
  }
  swap(&arr[right], &arr[store_index]); // Move pivot to final place
  return store_index;
}

// Quickselect: find the k-th smallest element (0-based index)
static double quickselect(double *arr, int left, int right, int k) {
  while (left < right) {
    int const pivot_index = left + (right - left) / 2;
    int const pivot_new = partition(arr, left, right, pivot_index);
    if (pivot_new == k)
      return arr[k];
    else if (k < pivot_new)
      right = pivot_new - 1;
    else
      left = pivot_new + 1;
  }
  return arr[left];
}

// Compute the p-quantile (0 <= p <= 1) of array[0..n-1]
static double quantile(double *array, int n, double p) {
  if (n == 0) return NAN;

  double const pos = p * (n - 1);
  int const i = (int)floor(pos);
  double const frac = pos - i;
  double const q1 = quickselect(array, 0, n - 1, i);

  if (frac == 0.0)
    return q1;
  else {
    double q2 = quickselect(array, 0, n - 1, i + 1);
    return q1 + frac * (q2 - q1);  // Linear interpolation
  }
}
