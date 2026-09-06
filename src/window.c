// Various window functions for Fourier analysis, filter design, etc
// Copyright Jan 2026 Phil Karn, KA9Q
#include <unistd.h>
#include <math.h>
#include <stdbool.h>
#include <assert.h>
#include <fftw3.h>
#include "misc.h"
#include "filter.h"
#include "window.h"

// Hamming window
double hamming_window(int const n,int const N){
  assert(N > 1 && n >=0 && n < N);
  if(N <= 1)
    return 1.0;
  if(n < 0 || n >= N)
    return 0.0;

  const double alpha = 25./46.;
  const double beta = (1-alpha);

  return alpha - beta * cospi(2.0 * n/(N-1));
}

// Hann / "Hanning" window
double hann_window(int n,int N){
  assert(N > 1 && n >=0 && n < N);
  if(N <= 1)
    return 1.0;
  if(n < 0 || n >= N)
    return 0.0;

  return 0.5 - 0.5 * cospi(2.0 * n/(N-1));
}

// common blackman window
double blackman_window(int const n, int const N){
  assert(N > 1 && n >=0 && n < N);
  if(N <= 1)
    return 1.0;
  if(n < 0 || n >= N)
    return 0.0;
  double const a0 = 0.42;
  double const a1 = 0.5;
  double const a2 = 0.08;
  return a0 - a1*cospi(2.0 * n/(N-1)) + a2*cospi(4.0 * n/(N-1));
}
// Exact Blackman window
double exact_blackman_window(int n,int N){
  assert(N > 1 && n >=0 && n < N);
  if(N <= 1)
    return 1.0;
  if(n < 0 || n >= N)
    return 0.0;
  double const a0 = 7938./18608;
  double const a1 = 9240./18608;
  double const a2 = 1430./18608;
  return a0 - a1*cospi(2.0 * n/(N-1)) + a2*cospi(4.0 * n/(N-1));
}
// Blackman-Harris
double blackman_harris_window(int n, int N){
  assert(N > 1 && n >=0 && n < N);
  if(N <= 1)
    return 1.0;
  if(n < 0 || n >= N)
    return 0.0;
  double const a0 = 0.35875;
  double const a1 = 0.48829;
  double const a2 = 0.14128;
  double const a3 = 0.01168;

  return a0 - a1 * cospi(2.0 * n/(N-1)) + a2 * cospi(4.0 * n/(N-1)) - a3 * cospi(6.0 * n/(N-1));
}

// 5-term HP/Agilent flat-top window" by Heinzel et al
double hp5ft_window(int n, int N){
  assert(N > 1 && n >=0 && n < N);
  double const a0 = 1.0;
  double const a1 = 1.912510941;
  double const a2 = 1.079173272;
  double const a3 = 0.1832630879;
  double const a4 = 0.0066586847;
  return a0 - a1 * cospi(2.0 * n/(N-1))
    + a2 * cospi(4.0 * n/(N-1))
    - a3 * cospi(6.0 * n/(N-1))
    + a4 * cospi(8.0 * n/(N-1));
}

#if 0
// Used by gaussian_window
// https://en.wikipedia.org/wiki/Window_function (section Approximate confined Gaussian window)
static inline double G(double const x,int const N, double const s){
  assert(isfinite(s));
  int const L = N+1;
  assert(L != 0 && s != 0);
  double const tmp = (x - N/2) / (2 * L *s);
  return exp(-tmp*tmp);
}

double gaussian_window(int n, int N, double s){
  assert(N > 1 && n >=0 && n < N && isfinite(s));
  if(N <= 1)
    return 1.0;
  if(n < 0 || n >= N)
    return 0.0;

  if(!isfinite(s) || s < 1e-6){
    // Special case to avoid divide by zero -> exp(-infinity)
    if(N & 1) // odd?
      return (n == N/2 - 1) ? 1 : 0;
    else
      return n == (N/2 - 1) ? 0.5 : (n == N/2) ? 0.5 : 0;
  }
  int const L = N+1;
  return G(n,N,s) - ( G(-0.5,N,s) * (G(n+L,N,s) + G(n-L,N,s) ) / ( G(-0.5 + L,N,s) + G(-0.5 - L,N,s) ) );
}

# else
// chat gpt version
/*
 * Gaussian window using the common "alpha" parameterization:
 *
 *   c = (N-1)/2
 *   t = (n - c)/c   (so endpoints are at t = ±1)
 *   w[n] = exp( -0.5 * (alpha * t)^2 )
 *
 * Properties (with normalize_peak=1):
 *   max(w) = 1
 *   w[0] = w[N-1] = exp(-0.5 * alpha^2)
 *
 * Returns 0 on success, -1 on invalid args.
 */
int gaussian_window_alpha(float *w, size_t N, double alpha, bool normalize_peak){
    if (!w || N == 0) return -1;
    if (!(alpha > 0.0)) return -1;

    const double c = 0.5 * (double)(N - 1);

    // N=1: define as 1.0
    if (N == 1) {
        w[0] = 1.0;
        return 0;
    }

    double maxv = 0.0;

    for (size_t n = 0; n < N; n++) {
        // Normalized coordinate in [-1, +1]
        const double t = ((double)n - c) / c;
        const double x = alpha * t;
        const double v = exp(-0.5 * x * x);
        w[n] = v;
        if (v > maxv) maxv = v;
    }

    if (normalize_peak && maxv > 0.0) {
        const double inv = 1.0 / maxv;
        for (size_t n = 0; n < N; n++)
            w[n] *= inv;
    }

    return 0;
}
#endif




#if 0
// Jim Kaiser was in my Bellcore department in the 1980s. Really friendly guy.
// Superseded by make_kaiser() routine that more efficiently computes entire window at once
static double const kaiser(int const n,int const M, double const beta){
  static double old_beta = NAN;
  static double old_inv_denom;

  // Cache old value of beta, since it rarely changes
  // Not thread safe
  if(beta != old_beta){
    old_beta = beta;
    old_inv_denom = 1. / i0(beta);
  }
  double const p = 2.0*n/(M-1) - 1;
  return i0(beta*sqrt(1-p*p)) * old_inv_denom;
}
#endif
// Compute an entire normalized Kaiser window
// More efficient than repeatedly calling kaiser(n,M,beta)
int make_kaiser(double * const window,int const M,double const beta){
  assert(window != NULL);
  if(window == NULL || M < 2 || !isfinite(beta))
    return -1;

  // Precompute unchanging partial values
  double const inv_denom = 1. / i0(beta); // Inverse of denominator
  double const pc = 2.0 / (M-1);

  // The window is symmetrical, so compute only half of it and mirror
  // this won't compute the middle value in an odd-length sequence
  double window_gain = 0;
  for(int n = 0; n < M/2; n++){
    double const p = pc * n  - 1;
    window[M-1-n] = window[n] = i0(beta * sqrt(1-p*p)) * inv_denom;
    window_gain += 2 * window[n];
  }
  // If sequence length is odd, middle value is unity
  if(M & 1){
    window[(M-1)/2] = 1; // The -1 is actually unnecessary
    window_gain += window[(M-1)/2];
  }
  window_gain = M / window_gain;
  for(int i = 0; i < M; i++)
    window[i] *= window_gain;

  return 0;
}
// Compute an entire Kaiser window - float version
// More efficient than repeatedly calling kaiser(n,M,beta)
int make_kaiserf(float * const window,int const M,double const beta){
  assert(window != NULL);
  if(window == NULL || M < 2 || !isfinite(beta))
    return -1;

  // Precompute unchanging partial values
  double const inv_denom = 1. / i0(beta); // Inverse of denominator
  double const pc = 2.0 / (M-1);

  // The window is symmetrical, so compute only half of it and mirror
  // this won't compute the middle value in an odd-length sequence
  for(int n = 0; n < M/2; n++){
    double const p = pc * n  - 1;
    double const w = i0(beta * sqrt(1-p*p)) * inv_denom;
    window[M-1-n] = window[n] = (float)w;
  }
  // If sequence length is odd, middle value is unity
  if(M & 1){
    window[(M-1)/2] = 1; // The -1 is actually unnecessary
  }
  return 0;
}

// The x < -1 case is included for completeness, although the
// half-spectrum construction below ordinarily evaluates x >= 0.
static inline double chebyshev_t(int N, double x){
  if(fabs(x) <= 1)
    return cos(N * acos(x));

  double const y = cosh(N * acosh(fabs(x)));
  return x < 0 && (N & 1) ? -y : y;
}
// Dolph-chebyshev window
// gamma is log of the amplitude ratio
// gamma = 1 => -20 dB; 2 => -40 dB; 5 => -100 dB
int make_chebyshevf(float * const window, int const N, double const gamma){
  assert(window != NULL && N >= 2 && isfinite(gamma) && gamma >= 0.0);
  if(window == NULL || N < 2 || !isfinite(gamma) || gamma < 0.0)
    return -1;
  int const order = N - 1;
  int const bins = N / 2 + 1;
  float complex spectrum[bins];
  fftwf_plan plan = plan_c2r(N, spectrum, window);
  if(plan == NULL)
    return -1;

  // gamma is A/20, so ripple is 10^(A/20)
  // T_order(beta) equals ripple by construction, making ripple the normalization denominator.
  double const ripple = pow(10.,gamma);
  double const beta = cosh(acosh(ripple)/order);
  // Shift the zero-phase window to the center of the output array.
  // For odd N, shift is integral
  // For even N, shift is half-integral
  double const shift = 0.5 * order;
  for(int k = 0; k < N; k++){
    double const x = beta * cospi((double)k/N);
    double const amp = chebyshev_t(order,x) / ripple;
    // FFTW's c2r transform uses the backward-transform sign:
    // exp(+j 2 pi k n / N)
    // Therefore, a right shift in time requires the negative frequency-domain phase ramp below
    double const phase = -2.0 * (double)k * shift / (double)N; // half rotations
    spectrum[k] = amp * csincospi(phase);
  }
  // For even N, the Nyquist bin must be real for a c2r transform.
  // It is theoretically zero because:
  //   T_(N-1)(beta cos(pi/2)) = T_odd(0) = 0
  // Force it to exactly zero to eliminate roundoff residue.
  if((N & 1) == 0)
    spectrum[N/2] = 0;
  fftwf_execute(plan);
  destroy_plan(&plan);
  // FFTW does not normalize its inverse transform. Peak
  // normalization removes both that scale factor and any other
  // constant scale in the frequency-domain construction
  float peak = 0;
  for(int n = 0; n < N; n++)
    peak = fmaxf(peak, fabsf(window[n]));
  if(!isfinite(peak) || peak <= 0)
    return -1;
  float const scale = 1/peak;
  for(int n = 0; n < N; n++)
    window[n] *= scale;

  return 0;
}

int normalize_windowf(float * const window, int const M){
  assert(window != NULL && M != 0);
  if(window == NULL || M == 0)
    return -1;
  double window_gain = 0;
  for(int n = 0; n < M; n++)
    window_gain += window[n];
  if(window_gain == 0 || !isfinite(window_gain))
    return -1;
  window_gain = M / window_gain;
  for(int i = 0; i < M; i++)
    window[i] *= (float)window_gain;

  return 0;
}
