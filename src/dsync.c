// Receive filter, level control, frequency tracking and symbol timing
// for the digital mode decoders (see digital.h)
// Copyright 2026, Matthew Kaufman, KA6SQG
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "digital.h"
#include "nco.h"

// Theory of operation
//
// The chain. A decoder runs its complex baseband stream through these pieces in this order:
// level control, frequency correction, receive lowpass, symbol timing, and then a one-symbol
// differential detector whose output make_softdibit() turns into a dibit and a confidence. The
// envelope ripple measure watches the stream beside the chain and only advises which receive
// response to use. Nothing here knows about frames: the decoder closes the frequency loop
// (update_freqtrack()) and tells the timing loop when it is locked (lock_symsync()).
//
// Receive filter. A windowed raised cosine lowpass with real symmetric taps, so its phase is
// linear and its delay a constant (ntaps - 1) / 2 samples. Two responses of different lengths
// share one delay line, the shorter centred on it, so they have the same delay and switching
// between them moves nothing in time. init_rrcfilter() makes instead the root raised cosine
// matched to a transmitter that shapes its symbols with one (P25 Phase 2).
//
// Envelope ripple. A linear modulation's envelope dips between symbols, so |x|^2 has a spectral
// line at the symbol rate; a constant-envelope modulation has none. A one-pole average of
// |x|^2 e^(-j 2 pi symrate t) measures that line, and dividing by the average of |x|^2 makes the
// measure independent of level. Only the magnitude is kept: the phase depends on the timing.
//
// Level control. Divides by the square root of a slow one-pole average of |x|^2. The Gardner
// detector's output goes as the square of the amplitude, so the timing loop's gains, and its
// error limit, mean what they say only at a known level: unity RMS.
//
// Frequency. For a tone at f the lag-one product y[i] conj(y[i-1]) has angle 2 pi f / samprate,
// so the angle of its sum over a block estimates the block's mean frequency, each pair of samples
// weighted by the product of their amplitudes, unambiguously within +/- samprate / 2. It is the
// mean of whatever the block holds: signal, noise, a neighbour, and for a 4-level FM signal the
// mean of the data's deviation too (p25p1.c says what that costs and what it does about it).
// The loop is first order. A block's residual added to the correction it was measured against
// is an absolute measurement of the offset, so no integrator is needed for zero steady-state
// error: the estimate moves toward each measurement through a one-pole lowpass whose corner is
// the loop's bandwidth (update_freqtrack()). A drift of R Hz/s is followed about R / (2 pi corner)
// Hz behind.
//
// Symbol timing. A fractional resampler makes two outputs per symbol, the symbol and the midpoint
// between two symbols, each with an 8-tap interpolator chosen from a polyphase bank. The Gardner
// detector (the midpoint times the difference of the symbols either side of it) needs neither
// decisions nor carrier phase, so it works while frequency and frame sync are still unknown.
// Its error drives a proportional-plus-integral loop whose output is the resampling ratio; the
// integral is the loop's estimate of the symbol rate. The decoder runs the loop wide while it
// searches and narrows it once frame sync is held. Every output's position is logged with the
// input it came from, so a past output can be computed again at another timing (symsync_near()),
// which lets the decoder try other timings on a frame sync that nearly passed without touching
// the loop.
//
// Soft dibits. The decoders take z = y[k] conj(y[k-1]), the phase change over one symbol. For
// pi/4 DQPSK that is the differential detector. For C4FM, an FM signal whose four deviations
// (+-600 and +-1800 Hz at 4800 symbols/s) advance the phase by +-45 or +-135 degrees in a symbol,
// it is the integral of the frequency over the symbol: the integrate-and-dump receiver. Both land
// on the same four points, which is what lets one chain take both.

// ---------------- receive filter ----------------

// 'ntaps' taps of the response into taps[0..ntaps-1]: the raised cosine with zero spacing
// 1 / (2 cutoff), h = sinc(x) cos(pi rolloff x) / (1 - (2 rolloff x)^2), x = 2 cutoff t, times a
// Hamming window. Before windowing its spectrum is flat to cutoff (1 - rolloff), half amplitude
// (-6 dB) at cutoff and zero beyond cutoff (1 + rolloff). At |2 rolloff x| = 1 the second factor
// is 0/0; its limit is pi/4. The window trims the sidelobes left by truncation, and dividing by
// the sum of the taps makes the DC gain one. The raised cosine here shapes a lowpass response;
// it is a Nyquist pulse only if cutoff is half the symbol rate, which p25p1.c's is not
static void design_rxfilter(float *taps,double samprate,double cutoff,int ntaps,double rolloff){
  double const fc = cutoff / samprate; // cycles/sample
  double const mid = (ntaps - 1) / 2.0;
  double sum = 0;
  for(int i=0; i < ntaps; i++){
    double const t = i - mid;
    double const x = 2 * fc * t; // in units of the sinc's zero spacing
    double const sinc = fabs(t) < 1e-12 ? 1.0 : sin(M_PI * x) / (M_PI * x);
    double rc = 1;
    if(rolloff > 0){
      double const d = 1 - 4 * rolloff * rolloff * x * x;
      rc = fabs(d) < 1e-9 ? M_PI / 4 : cos(M_PI * rolloff * x) / d;
    }
    double const window = ntaps > 1 ? 0.54 - 0.46 * cos(2 * M_PI * i / (ntaps - 1)) : 1.0;
    taps[i] = (float)(sinc * rc * window);
    sum += taps[i];
  }
  for(int i=0; i < ntaps; i++)
    taps[i] = (float)(taps[i] / sum);
}
int init_rxfilter(struct rxfilter *f,double samprate,double cutoff,int ntaps,double rolloff){
  assert(f != NULL);
  if(f == NULL || ntaps < 1 || cutoff <= 0 || cutoff >= samprate / 2)
    return -1;
  memset(f,0,sizeof *f);
  f->responses[0] = calloc((size_t)ntaps,sizeof *f->taps);
  f->history = calloc(2 * (size_t)ntaps,sizeof *f->history);
  if(f->responses[0] == NULL || f->history == NULL){
    free_rxfilter(f);
    return -1;
  }
  f->ntaps = ntaps;
  design_rxfilter(f->responses[0],samprate,cutoff,ntaps,rolloff);
  f->counts[0] = ntaps;
  f->nresponses = 1;
  select_rxfilter(f,0);
  return 0;
}
int add_rxfilter_response(struct rxfilter *f,double samprate,double cutoff,int ntaps,double rolloff){
  assert(f != NULL && f->history != NULL);
  // Centred on the delay line only if the two lengths differ by an even number
  if(f->nresponses >= RXFILTER_RESPONSES || ntaps < 1 || ntaps > f->ntaps || (f->ntaps - ntaps) % 2 != 0
     || cutoff <= 0 || cutoff >= samprate / 2)
    return -1;
  int const r = f->nresponses;
  f->responses[r] = calloc((size_t)f->ntaps,sizeof *f->taps);
  if(f->responses[r] == NULL)
    return -1;
  f->firsts[r] = (f->ntaps - ntaps) / 2;
  f->counts[r] = ntaps;
  design_rxfilter(f->responses[r] + f->firsts[r],samprate,cutoff,ntaps,rolloff);
  f->nresponses++;
  return r;
}
// The root raised cosine: its square is a raised cosine with zeros at every symbol but the
// centre, so a transmitter and receiver that both use it have no intersymbol interference at
// the right sampling instants. h(x), x in symbols, is the textbook expression, with its two
// 0/0 points (x = 0 and |4 alpha x| = 1) replaced by their limits; truncated at 'span' symbols
// each side with no window, and scaled to unity DC gain
int init_rrcfilter(struct rxfilter *f,double samprate,double symrate,double alpha,double span){
  assert(f != NULL);
  if(f == NULL || samprate <= 0 || symrate <= 0 || alpha <= 0 || alpha > 1 || span <= 0)
    return -1;
  double const sps = samprate / symrate; // samples per symbol
  int const half = (int)lrint(span * sps);
  int const ntaps = 2 * half + 1;
  memset(f,0,sizeof *f);
  f->responses[0] = calloc((size_t)ntaps,sizeof *f->taps);
  f->history = calloc(2 * (size_t)ntaps,sizeof *f->history);
  if(f->responses[0] == NULL || f->history == NULL){
    free_rxfilter(f);
    return -1;
  }
  f->ntaps = ntaps;
  float * const taps = f->responses[0];
  double sum = 0;
  for(int i=0; i <= half; i++){
    double const x = i / sps; // symbols from the centre
    double const a4x = 4 * alpha * x;
    double h;
    if(x < 1e-9)
      h = 1 - alpha + 4 * alpha / M_PI;
    else if(fabs(1 - a4x * a4x) < 1e-7) // the limit at |4 alpha x| = 1
      h = alpha / M_SQRT2 * ((1 + 2 / M_PI) * sin(M_PI / (4 * alpha)) + (1 - 2 / M_PI) * cos(M_PI / (4 * alpha)));
    else
      h = (sin(M_PI * x * (1 - alpha)) + a4x * cos(M_PI * x * (1 + alpha))) / (M_PI * x * (1 - a4x * a4x));
    taps[half + i] = taps[half - i] = (float)h;
    sum += i == 0 ? h : 2 * h;
  }
  for(int i=0; i < ntaps; i++)
    taps[i] = (float)(taps[i] / sum);
  f->counts[0] = ntaps;
  f->nresponses = 1;
  select_rxfilter(f,0);
  return 0;
}
void select_rxfilter(struct rxfilter *f,int response){
  assert(f != NULL);
  if(response < 0 || response >= f->nresponses)
    return;
  f->response = response;
  f->taps = f->responses[response];
  f->first = f->firsts[response];
  f->count = f->counts[response];
}
void free_rxfilter(struct rxfilter *f){
  if(f == NULL)
    return;
  for(int r=0; r < RXFILTER_RESPONSES; r++)
    free(f->responses[r]);
  free(f->history);
  memset(f,0,sizeof *f);
}
void run_rxfilter(struct rxfilter *f,float complex *buffer,int count){
  assert(f != NULL && f->taps != NULL);
  int const n = f->ntaps;
  int const m = f->count;
  // Only the nonzero taps: a shorter response costs no more than it would on its own line
  float const * restrict const taps = f->taps + f->first;
  for(int k=0; k < count; k++){
    f->history[f->pos] = f->history[f->pos + n] = buffer[k];
    if(++f->pos == n)
      f->pos = 0;
    // Oldest first from history[pos]; the taps are symmetric so the order is moot
    float complex const * restrict const w = f->history + f->pos + f->first;
    float complex acc = 0;
    for(int i=0; i < m; i++)
      acc += taps[i] * w[i];
    buffer[k] = acc;
  }
}

// ---------------- envelope ripple ----------------

// Two one-pole averages with the same factor, alpha = 1 / (time_constant samprate): the power,
// and the power mixed down by the symbol rate. The rotor is stepped by multiplication, not
// computed from a phase accumulator, and so is renormalized now and then
void init_envelope(struct envelope *e,double samprate,double symrate,double time_constant){
  assert(e != NULL);
  memset(e,0,sizeof *e);
  double a = 1.0 / (time_constant * samprate);
  if(a > 1)
    a = 1;
  e->alpha = (float)a;
  e->rotor = 1;
  double const w = -2 * M_PI * symrate / samprate;
  e->step = (float)cos(w) + I * (float)sin(w);
}
void run_envelope(struct envelope *e,float complex const *buffer,int count){
  float const alpha = e->alpha;
  float complex rotor = e->rotor;
  float complex ripple = e->ripple;
  float power = e->power;
  for(int i=0; i < count; i++){
    float const p = cnrmf(buffer[i]);
    power += alpha * (p - power);
    ripple += alpha * (p * rotor - ripple);
    rotor *= e->step;
    // Its magnitude drifts by a rounding error per sample: put it back on the unit circle now and then
    if((i & 1023) == 1023)
      rotor /= cabsf(rotor);
  }
  e->rotor = rotor / cabsf(rotor);
  e->ripple = ripple;
  e->power = power;
  e->samples += (uint64_t)count;
}
float envelope_ripple(struct envelope const *e){
  return e->power > 1e-20f ? cabsf(e->ripple) / e->power : 0;
}

// ---------------- level control ----------------

// Each sample is divided by the RMS level averaged up to and including itself. The average is
// slow against the symbol rate, so the gain does not follow the modulation's own envelope.
// prime_level() starts the average at the first block's mean power instead of at 1
void init_level(struct level *l,double samprate,double time_constant){
  assert(l != NULL);
  double a = 1.0 / (time_constant * samprate);
  if(a > 1)
    a = 1;
  l->alpha = (float)a;
  l->power = 1;
}
void prime_level(struct level *l,float complex const *buffer,int count){
  double power = 0;
  for(int i=0; i < count; i++)
    power += cnrmf(buffer[i]);
  if(count > 0 && power > 0)
    l->power = (float)(power / count);
}
void run_level(struct level *l,float complex *buffer,int count){
  float power = l->power;
  float const alpha = l->alpha;
  for(int i=0; i < count; i++){
    power += alpha * (cnrmf(buffer[i]) - power);
    buffer[i] *= 1.0f / sqrtf(power > 1e-10f ? power : 1e-10f);
  }
  l->power = power;
}

// ---------------- frequency tracking ----------------

// A block whose lag-one coherence is at or below the floor is white-noise-like and carries
// no frequency; full weight from floor + span up. Coherence is |sum of y[i] conj(y[i-1])|
// divided by the sum of |y[i-1]|^2: 1 for a pure tone, and for white noise alone of the order
// of 1 / sqrt(samples in the block), since the products then add with random phases
// Measured: clean C4FM has a coherence of 0.87 and white noise 0.04; noise that has been through
// a channel filter about 0.5. The floor was picked from those figures, not from a sweep
static double const Coherence_floor = 0.15;
static double const Coherence_span = 0.25;
// Above this filtered residual the unfiltered one may stand in for it (a cold start far off
// centre, where the receive filter cuts into the signal and under-reads it)
static double const Wide_residual = 500.0; // Hz

// Lag-one products within a block (not the pair across the block boundary: it straddles
// two corrections)
static struct lagone lag_one(float complex const *y,int count){
  struct lagone s = {0};
  for(int i=1; i < count; i++){
    float complex const p = y[i] * conjf(y[i-1]);
    s.re += crealf(p);
    s.im += cimagf(p);
    s.power += cnrmf(y[i-1]);
  }
  return s;
}
static inline bool lagone_valid(struct lagone const *s){
  return (s->re != 0 || s->im != 0) && s->power > 0;
}
static inline double lagone_hz(struct lagone const *s,double samprate){
  return atan2(s->im,s->re) * samprate / (2 * M_PI);
}
void set_freqtrack_corner(struct freqtrack *ft,double corner){
  // Per-sample one-pole factor for this corner; a block of n samples applies it n times.
  // y += a (x - y) has |H(w)|^2 = a^2 / (1 - 2 (1 - a) cos w + (1 - a)^2); setting that to 1/2
  // (-3 dB) at w gives a^2 + 2 (1 - c) a - 2 (1 - c) = 0, c = cos w, whose positive root is this
  double const w = 2 * M_PI * corner / ft->samprate;
  double const c = cos(w);
  ft->sample_factor = c - 1 + sqrt(c * c - 4 * c + 3);
}
void init_freqtrack(struct freqtrack *ft,double samprate,double corner){
  assert(ft != NULL);
  memset(ft,0,sizeof *ft);
  ft->samprate = samprate;
  ft->max_offset = 6000; // Hz: the estimate and every seed or adjustment are clamped to +/- this
  set_freqtrack_corner(ft,corner);
}
// A known offset becomes the estimate. Like every correction it applies from the next block;
// the current block's residual, measured against the old correction, is not blended in
void seed_freqtrack(struct freqtrack *ft,float offset){
  ft->estimate = fminf(fmaxf(offset,-ft->max_offset),ft->max_offset);
  ft->seed_pending = true;
}
void correct_freqtrack(struct freqtrack *ft,float complex *buffer,int count){
  double f = -(double)ft->applied / ft->samprate;
  if(f < -0.5 || f >= 0.5)
    f = 0;
  uint64_t const increment = set_nco(f);
  uint64_t phase = ft->phase;
  for(int i=0; i < count; i++){
    buffer[i] *= nco(phase);
    phase += increment;
  }
  ft->phase = phase;
}
void shift_freqtrack(struct freqtrack *ft,float complex *buffer,int count,float offset){
  ft->applied = offset;
  correct_freqtrack(ft,buffer,count);
}
void adjust_freqtrack(struct freqtrack *ft,float delta){
  ft->estimate = fminf(fmaxf(ft->estimate + delta,-ft->max_offset),ft->max_offset);
}
void measure_freqtrack(struct freqtrack *ft,float complex const *buffer,int count){
  ft->unfiltered = lag_one(buffer,count);
  ft->unfiltered_pending = true;
}
// The filtered and unfiltered sums come from the same block after the same correction, so
// they differ only in what the receive filter removed: noise and neighbours away from the
// signal, which pull the unfiltered one toward the centre of the stream, and, far off centre,
// part of the signal itself, which pulls the filtered one toward zero
bool freqtrack_residual(struct freqtrack const *ft,float complex const *buffer,int count,float *residual){
  if(count <= 1)
    return false;
  struct lagone const filtered = lag_one(buffer,count);
  struct lagone const unfiltered = ft->unfiltered_pending ? ft->unfiltered : filtered;
  if(!lagone_valid(&filtered) || !lagone_valid(&unfiltered))
    return false;
  double r = lagone_hz(&filtered,ft->samprate);
  if(fabs(r) > Wide_residual){
    double const wide = lagone_hz(&unfiltered,ft->samprate);
    if(wide * r > 0 && fabs(wide) > fabs(r))
      r = wide;
  }
  *residual = (float)r;
  return true;
}
// The loop update. The block was rotated by 'applied', so applied + residual is the block's
// offset itself. With its input held at x for n samples, y += a (x - y) moves y the fraction
// 1 - (1 - a)^n of the way to x; the estimate moves that far toward this block's measurement,
// so block by block the loop is the per-sample lowpass with the chosen corner, whatever the
// block size. The step is scaled by the coherence weight (0 for a block like white noise) when
// measure_freqtrack() saw the block. The new estimate is the correction for the next block
void update_freqtrack(struct freqtrack *ft,bool valid,float residual,int count){
  if(count <= 0)
    return;
  bool const measured = ft->unfiltered_pending;
  struct lagone const unfiltered = ft->unfiltered;
  ft->unfiltered_pending = false;
  if(ft->seed_pending){
    ft->seed_pending = false; // this block was corrected with the pre-seed value
  } else if(valid){
    double const total = (double)ft->applied + residual;
    double weight = 1;
    if(measured && unfiltered.power > 0){
      ft->coherence = (float)(sqrt(unfiltered.re * unfiltered.re + unfiltered.im * unfiltered.im) / unfiltered.power);
      weight = (ft->coherence - Coherence_floor) / Coherence_span;
      weight = weight < 0 ? 0 : weight > 1 ? 1 : weight;
    }
    double const a = weight * (1 - pow(1 - ft->sample_factor,(double)count));
    double next = ft->estimate + a * (total - ft->estimate);
    if(next > ft->max_offset)
      next = ft->max_offset;
    else if(next < -ft->max_offset)
      next = -ft->max_offset;
    ft->estimate = (float)next;
  }
  ft->applied = ft->estimate;
}

// ---------------- symbol timing ----------------

// Solve the n x n system a x = b in place (Gaussian elimination, partial pivoting); n is small
static int solve(int n,double a[n][n],double b[n]){
  for(int col=0; col < n; col++){
    int pivot = col;
    for(int r=col+1; r < n; r++)
      if(fabs(a[r][col]) > fabs(a[pivot][col]))
	pivot = r;
    if(fabs(a[pivot][col]) < 1e-300)
      return -1;
    if(pivot != col){
      for(int c=0; c < n; c++){
	double const t = a[col][c];
	a[col][c] = a[pivot][c];
	a[pivot][c] = t;
      }
      double const t = b[col];
      b[col] = b[pivot];
      b[pivot] = t;
    }
    for(int r=col+1; r < n; r++){
      double const m = a[r][col] / a[col][col];
      for(int c=col; c < n; c++)
	a[r][c] -= m * a[col][c];
      b[r] -= m * b[col];
    }
  }
  for(int r=n-1; r >= 0; r--){
    for(int c=r+1; c < n; c++)
      b[r] -= a[r][c] * b[c];
    b[r] /= a[r][r];
  }
  return 0;
}
static double dsinc(double x){
  return fabs(x) < 1e-12 ? 1.0 : sin(M_PI * x) / (M_PI * x);
}
// Interpolator arm k: the least-squares (MMSE) 8-tap filter whose output is the signal k/nphases
// of a sample before window[4], for a signal band-limited to a quarter of the sample rate.
// Minimizing the integral over |f| < B of |sum h[i] e^(-j 2 pi f i) - e^(-j 2 pi f p)|^2 gives
// R h = r with R[i][j] = sinc(2B(i-j)) and r[i] = sinc(2B(i-p))
// window[0] is the oldest of the eight samples and window[7] the newest, so p = 4 - k/nphases
// lies between window[3] and window[4], in the middle of the span, where an interpolator of
// this length errs least; every output is therefore 3 + k/nphases samples behind the newest
// sample in. Arm 0 is window[4] itself: r is then column 4 of R, and h the unit vector.
// B = 1/4 suits a signal at four or more samples per symbol after the receive lowpass; energy
// beyond a quarter of the sample rate is interpolated less accurately. The error of a flat
// band-limited signal is what is minimized, with no weighting by the signal's own spectrum.
// Computed for 128 steps these taps agree with GNU Radio's MMSE interpolator table (129 x 8
// values) to its printed precision, worst difference 5e-7; they are computed here from the
// definition, not copied. The arms are the same design at whatever step nphases gives
static int design_interpolator(float *taps,int nphases){
  double const b2 = 2 * 0.25;
  for(int k=0; k < nphases; k++){
    double const p = SYMSYNC_TAPS / 2 - (double)k / nphases;
    double a[SYMSYNC_TAPS][SYMSYNC_TAPS];
    double h[SYMSYNC_TAPS];
    for(int i=0; i < SYMSYNC_TAPS; i++){
      for(int j=0; j < SYMSYNC_TAPS; j++)
	a[i][j] = dsinc(b2 * (i - j));
      h[i] = dsinc(b2 * (i - p));
    }
    if(solve(SYMSYNC_TAPS,a,h) != 0)
      return -1;
    for(int i=0; i < SYMSYNC_TAPS; i++)
      taps[k * SYMSYNC_TAPS + i] = (float)h[i];
  }
  return 0;
}
// Loop gains for a natural frequency 'bw' (radians per update) and damping, the usual
// second-order digital loop. The loop's output is the resampling ratio r, held for the next
// 2/r input samples, so a ratio step moves the next symbol by 2 dr / r^2 samples; scaling by
// r^2 / 2 makes 'bw' the bandwidth of the timing itself
// The loop updates once per symbol. Let tau be the timing in input samples and K = error_gain
// the detector's slope, error per input sample of timing offset. A late sample gives e > 0,
// which raises the ratio and brings the next outputs earlier. kp e lasts one symbol, so it moves
// the timing once; ki e stays in the integral, a rate. With P = K kp 2 / r^2 and I = K ki 2 / r^2
// the closed loop's characteristic polynomial is
//   z^2 - (2 - P - I) z + (1 - P)
// Matching its roots to z = e^(sT) at the poles of the continuous second-order loop, natural
// frequency wn and damping zeta, with theta = wn T = bw:
//   1 - P = e^(-2 zeta theta)
//     so P = 2 e^(-zeta theta) sinh(zeta theta)
//   2 - P - I = 2 e^(-zeta theta) cos(wd T)
//     so I = 2 (1 - e^(-zeta theta) (sinh(zeta theta) + cos(wd T)))
// where wd T = theta sqrt(1 - zeta^2), cos becoming cosh of theta sqrt(zeta^2 - 1) above zeta = 1
// and 1 at it. kp and ki below are these divided by K 2 / r^2. r is the nominal ratio, not the
// current one; the integral stays within the rate limit of it. Since K is per input sample, the
// same signal gives a K that falls as the sample rate rises (it is the slope per symbol over the
// samples per symbol), and an error_gain right at one sample rate is not right at another
// (p25p1.c scales its own with the sample rate).
// With zeta = 1 the timing settles with a time constant of about 1 / bw symbols
static void set_gains(struct symsync *s,float bw){
  float const zeta = s->damping;
  float const k0 = 2.0f / s->error_gain;
  float cos_wd = 1;
  if(zeta > 1)
    cos_wd = coshf(bw * sqrtf(zeta * zeta - 1));
  else if(zeta < 1)
    cos_wd = cosf(bw * sqrtf(1 - zeta * zeta));
  float const scale = s->base_ratio * s->base_ratio / 2;
  s->kp = scale * k0 * expf(-zeta * bw) * sinhf(zeta * bw);
  s->ki = scale * k0 * (1 - expf(-zeta * bw) * (sinhf(zeta * bw) + cos_wd));
}
int init_symsync(struct symsync *s,double samprate,double symrate,int nphases,
		 float acquire_bw,float track_bw,float damping,float error_gain,float rate_limit){
  assert(s != NULL);
  if(s == NULL || nphases < 1 || samprate <= 0 || symrate <= 0 || 2 * symrate > samprate)
    return -1;
  memset(s,0,sizeof *s);
  s->taps = calloc((size_t)nphases * SYMSYNC_TAPS,sizeof *s->taps);
  if(s->taps == NULL || design_interpolator(s->taps,nphases) != 0){
    free_symsync(s);
    return -1;
  }
  s->nphases = nphases;
  s->base_ratio = (float)(2 * symrate / samprate);
  s->ratio = s->base_ratio;
  s->integral = s->base_ratio;
  // The ratio itself, proportional term included, stays within 10% of nominal
  s->out_min = 0.9f * s->base_ratio;
  s->out_max = 1.1f * s->base_ratio;
  // The integral is the loop's estimate of the symbol rate error; without signal it random
  // walks, and a narrow loop cannot pull in a rate error of a few percent once a signal returns
  s->int_min = (1 - rate_limit) * s->base_ratio;
  s->int_max = (1 + rate_limit) * s->base_ratio;
  s->acquire_bw = acquire_bw;
  s->track_bw = track_bw;
  s->damping = damping;
  s->error_gain = error_gain;
  set_gains(s,acquire_bw);
  return 0;
}
void free_symsync(struct symsync *s){
  if(s == NULL)
    return;
  free(s->taps);
  s->taps = NULL;
}
void lock_symsync(struct symsync *s,bool locked){
  if(locked == s->locked)
    return;
  s->locked = locked;
  set_gains(s,locked ? s->track_bw : s->acquire_bw);
}
void set_symsync_bandwidth(struct symsync *s,float bw){
  set_gains(s,bw);
}
void reset_symsync_rate(struct symsync *s){
  s->ratio = s->base_ratio;
  s->integral = s->base_ratio;
}
// The accumulator counts outputs: each input sample adds the ratio (outputs per input sample),
// and each time it reaches 1 an output instant has passed since the previous sample. Outputs
// alternate midpoint and symbol; only their order says which is which (slip_symsync() swaps
// them). The loop runs at each symbol, from the error of that symbol, the midpoint before it
// and the symbol before that. Each output's newest input sample and arm are logged for
// symsync_near(), and the input itself is kept in 'history'
void run_symsync(struct symsync *s,float complex const *buffer,int count,symsync_output out,void *arg){
  assert(s != NULL && s->taps != NULL);
  for(int k=0; k < count; k++){
    float complex const x = buffer[k];
    size_t const at = (size_t)s->samples_in & (SYMSYNC_HISTORY - 1);
    s->history[at] = s->history[at + SYMSYNC_HISTORY] = x;
    s->samples_in++;
    s->window[s->wpos] = s->window[s->wpos + SYMSYNC_TAPS] = x;
    if(++s->wpos == SYMSYNC_TAPS)
      s->wpos = 0;
    // The accumulator crossed an integer (acc - 1) / ratio input samples before the sample
    // just taken; arm k delays by k / nphases samples, so that fraction picks the arm.
    // The loop may change the ratio below, so use the one this step added
    float const ratio = s->ratio;
    s->accumulator += ratio;
    while(s->accumulator >= 1){
      float const delay = (s->accumulator - 1) / ratio;
      int arm = (int)(delay * (float)s->nphases);
      if(arm > s->nphases - 1)
	arm = s->nphases - 1;
      if(arm < 0)
	arm = 0;
      float const * restrict const h = s->taps + arm * SYMSYNC_TAPS;
      float complex const * restrict const w = s->window + s->wpos;
      float complex y = 0;
      for(int i=0; i < SYMSYNC_TAPS; i++)
	y += h[i] * w[i];

      bool is_symbol;
      if(s->expect_midpoint){
	s->mid = y;
	s->expect_midpoint = false;
	is_symbol = false;
      } else {
	// Gardner: the midpoint times the difference of the symbols either side of it
	float e = crealf(s->mid) * (crealf(y) - crealf(s->prev)) + cimagf(s->mid) * (cimagf(y) - cimagf(s->prev));
	// Telemetry only: |e| over the energy, averaged over about 100 symbols
	float const energy = cnrmf(s->mid) + cnrmf(y);
	if(energy > 1e-20f)
	  s->error_ema += 0.01f * (fabsf(e) / energy - s->error_ema);
	s->prev = y;
	s->expect_midpoint = true;
	is_symbol = true;
	// An error straddling a nudge, from samples on both sides of it, is not used
	if(s->ignore_next_error)
	  e = 0;
	s->ignore_next_error = false;
	// Clipped, so one burst of noise or a symbol far off moves the timing only so far; far from
	// lock the loop then moves at a bounded rate rather than in proportion to the error
	e = e > 0.15f ? 0.15f : e < -0.15f ? -0.15f : e;
	s->integral += s->ki * e;
	s->integral = s->integral > s->int_max ? s->int_max : s->integral < s->int_min ? s->int_min : s->integral;
	float r = s->kp * e + s->integral;
	r = r > s->out_max ? s->out_max : r < s->out_min ? s->out_min : r;
	s->ratio = r;
      }
      size_t const o = (size_t)s->outputs_out & (SYMSYNC_OUTPUTS - 1);
      s->outputs[o].newest = s->samples_in - 1;
      s->outputs[o].arm = (uint16_t)arm;
      s->outputs_out++;
      out(arg,y,is_symbol);
      s->accumulator -= 1;
    }
  }
}
// A past output at other timing, from the stored input: the same interpolator, moved by whole
// arms, carrying into the next or previous input sample as the arm wraps. Fails when the
// output or the samples it needs have left the history, or are not in yet
bool symsync_near(struct symsync const *s,uint64_t output,int arms,float complex *result){
  if(output >= s->outputs_out || s->outputs_out - output > SYMSYNC_OUTPUTS)
    return false;
  size_t const o = (size_t)output & (SYMSYNC_OUTPUTS - 1);
  // Arm a delays by a / nphases samples before the newest sample of the window: later = less delay
  int64_t newest = (int64_t)s->outputs[o].newest;
  int arm = (int)s->outputs[o].arm - arms;
  while(arm < 0){
    arm += s->nphases;
    newest++;
  }
  while(arm >= s->nphases){
    arm -= s->nphases;
    newest--;
  }
  int64_t const oldest = newest - SYMSYNC_TAPS + 1;
  if(newest >= (int64_t)s->samples_in || oldest < 0 || (int64_t)s->samples_in - oldest > SYMSYNC_HISTORY)
    return false;
  float const * restrict const h = s->taps + arm * SYMSYNC_TAPS;
  float complex const * restrict const w = s->history + ((size_t)oldest & (SYMSYNC_HISTORY - 1));
  float complex y = 0;
  for(int i=0; i < SYMSYNC_TAPS; i++)
    y += h[i] * w[i];
  *result = y;
  return true;
}
// Move the sampling instant through the resampler's phase only: the loop's ratio and integral
// are untouched. The resampler can only move an output within the current input sample
// The accumulator counts outputs, two per symbol; taking from it delays the next output, so a
// positive 'symbols' samples later
void nudge_symsync(struct symsync *s,float symbols){
  symbols = symbols > 0.49f ? 0.49f : symbols < -0.49f ? -0.49f : symbols;
  s->accumulator += -symbols * 2;
  s->ignore_next_error = true;
}
// The sampling instant moves half a symbol with no symbol added or lost: the next error is
// computed from samples one symbol apart with their own midpoint, as if the loop had always
// run on the other half
void slip_symsync(struct symsync *s){
  if(s->expect_midpoint){
    float complex const t = s->prev;
    s->prev = s->mid;
    s->mid = t;
    s->expect_midpoint = false;
  } else {
    s->prev = s->mid;
    s->expect_midpoint = true;
  }
}

// ---------------- soft symbols ----------------

// z is the phase change over one symbol (see the theory above). The dibit is its quadrant: the
// msb is 1 when the imaginary part is negative, the lsb 1 when the real part is negative. That
// is a Gray mapping: a step to a neighbouring point is one bit wrong. The confidence,
// sqrt(2) min(|cos|,|sin|) of the phase, is 1 at the four ideal points and 0 on an axis; it
// ignores |z|, so a weak symbol with the right phase is as confident as a strong one. It is
// the margin of the less certain of the two bits: near +-90 degrees the lsb, near 0 or 180 the msb
struct softdibit make_softdibit(float complex z){
  struct softdibit sd;
  float const re = crealf(z);
  float const im = cimagf(z);
  if(re >= 0)
    sd.dibit = im >= 0 ? 0 : 2;
  else
    sd.dibit = im >= 0 ? 1 : 3;
  sd.phase = atan2f(im,re);
  // Distance from the nearest decision boundary (the axes), normalized so an ideal point is 1
  float const mag = sqrtf(re * re + im * im);
  float c = 0;
  if(mag > 1e-10f){
    float const di = fabsf(re) / mag;
    float const dq = fabsf(im) / mag;
    c = (di < dq ? di : dq) * M_SQRT2f;
  }
  sd.confidence = c > 1 ? 1 : c;
  return sd;
}
