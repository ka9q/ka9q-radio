// TS-1 pilot edge detector: coherent fold + band-limited zero crossing
// Line-for-line port of hf-timestd BpskEdgeFineStage (core/bpsk_edge_fine_stage.py)
// and bootstrap_edge_index (core/bpsk_fold_bootstrap.py); Python names in comments.
//
// K seconds of complex baseband fold modulo samprate with the sign alternating
// every second (the TS-1 base polarity alternates), so the fold holds -A on
// [0,e) and +A on [e,p). Derotate by half the angle of mean(avg^2), find the
// edge globally (closed-form matched filter T(e)) until three folds agree
// within 1 ms, then search around our own offset ("tracking"). Localize to
// sub-sample by bisecting the fold's periodic band-limited interpolant.
//
// Differences from the Python, all forced by the C interface:
// - input must be contiguous: a first_index jump returns -1 and restarts the
//   fold there (the Python's batch-median registration and spread check exist
//   to survive mislabelled RTP; here the caller supplies an exact counter)
// - every fold index sees exactly fold_seconds samples, so the per-index
//   counts are constants, not arrays
// - the interpolant is evaluated in the time domain (periodic sinc), not by
//   rfft: same function, O(p) per point, no FFTW
// - no MF-seeded mode and no authority verdict feedback (radiod has neither)
// - snr_db is new (the Python reports none); it only gates `locked`
//
// Costas quiet gate: port of BpskPpsCalibratorMF._update_costas_lock
// (core/bpsk_pps_calibrator_mf.py) with its per-batch square-and-halve phase
// estimate and first-order loop. One pilot_process() call is one Python batch;
// dt comes from the first call's length, as Python pins it on its first batch.
// costas_quiet = the gate stayed locked after every call that fed the fold.
// The gate starts unlocked and needs COSTAS_RELOCK_S of quiet to lock, so the
// first fold after pilot_create/pilot_reset never claims quiet. A counter gap
// restarts the fold but not the gate: the loop tracks the carrier, not the
// counter. Loop bandwidth is hf-timestd's production value (the recorder and
// config template default, 1.0 Hz), not the class default of 3.0
//
// Accumulate and finish: radiod's demod thread must never stall a block, and
// finishing a fold costs ~0.1 s at 96 kHz. So the fold lives in two buffers.
// pilot_accumulate (demod thread) folds into one; at fold end it hands that
// buffer to the pending slot and continues into the other. pilot_finish
// (worker thread) finishes the pending buffer, clears it and frees the slot.
// The split is clean: accumulation touches only its own buffer, the Costas
// gate and the counter check; finishing reads only the pending buffer and
// owns the tracking state and the scratch arrays. The atomic `pending` flag
// passes the buffer between threads (release on hand-off, acquire on take).
// pilot_process runs the two inline, so the tests and pilot-replay exercise
// exactly the arithmetic radiod runs.
//
// pilot_forget (INPUT_EPOCH change) goes beyond Python's reset: radiod's
// output counter rebases, so an own offset carried across would land wrong.
// It bumps a generation number; each fold records the generation it began
// in, pilot_finish discards a fold from an older generation, and clears the
// tracking state before the first fold of a new one
// Copyright 2026, HamSCI
#include <complex.h>
#include <math.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include "pilot.h"

#define FIT_BAND_FRACTION 0.4
#define CROSSING_ITERATIONS 40
#define BOOTSTRAP_CONFIRM_BLOCKS 3
#define BOOTSTRAP_CONFIRM_TOLERANCE_MS 1.0
#define DEMOTE_AFTER_FAILED_BLOCKS 3

// hf-timestd COSTAS_* module constants
#define COSTAS_LOOP_BW_HZ 1.0
#define COSTAS_TAU_PHASE_EMA_S 10.0
#define COSTAS_TAU_DPHASE_EMA_S 0.5
#define COSTAS_DPHASE_MAX_RAD 0.050
#define COSTAS_PHASE_BAND_RAD 2.0
#define COSTAS_RELOCK_S 0.5

// One fold's accumulators (Python _acc, _acc_even, _acc_odd and their companions)
struct fold {
  double complex *acc, *acc_even, *acc_odd;
  double abs_sum;          // sum |x| over the block: fold retention denominator
  int64_t abs_n;
  int64_t cont;            // samples since fold start
  int64_t fold_start;      // output counter of the fold's first sample (Python registration)
  bool quiet;              // costas_locked after every batch that fed this fold
  uint64_t gen;            // pilot_forget generation the fold began in
};

struct pilot_state {
  struct pilot_config cfg;
  int p;                   // samples per second = fold length
  int W;                   // search half-width, samples
  int64_t block_len;       // fold_seconds * p

  // Accumulation side (pilot_accumulate's thread)
  struct fold fold[2];
  int cur;                 // fold[cur] is accumulating
  int pend;                // fold[pend] waits for pilot_finish while pending
  atomic_bool pending;
  atomic_uint_fast64_t dropped; // folds completed while the slot was busy
  atomic_uint_fast64_t gen;     // bumped by pilot_forget
  int64_t next_index;      // expected first_index of the next call
  bool have_next;

  // Finishing side (pilot_finish's thread) from here down, except the Costas gate
  uint64_t track_gen;      // generation the tracking state below belongs to

  // Self-acquisition (Python _own_offset_rtp, _bootstrap_history, _failed_blocks)
  bool tracking;
  double own_offset;       // output-counter phase mod p
  double history[BOOTSTRAP_CONFIRM_BLOCKS];
  int nhistory;
  int failed_blocks;
  bool verdict_pending;
  bool last_bootstrap;     // search mode of the block being finished

  // Costas quiet gate (Python _phase, _phase_ema, _dphase_ema, _costas_locked, ...)
  bool costas_coeffs;      // alpha etc. pinned from the first batch
  double costas_alpha, phase_ema_alpha, dphase_ema_alpha;
  int relock_batches;
  bool phase_initialized, phase_ema_set;
  double phase, phase_ema, dphase_ema;
  bool costas_locked;
  int relock_counter;

  // Scratch, allocated once
  double *in_phase, *tee, *sub, *seg, *outer;
};

struct edge_fit {          // Python _EdgeFit
  double edge_offset, amplitude, fit_rms, width;
};

// Python's float % : result takes the divisor's sign
static double pymod(double a,double m){
  double r = fmod(a,m);
  if(r < 0)
    r += m;
  return r;
}

struct pilot_state *pilot_create(struct pilot_config const *cfg){
  if(cfg == NULL || cfg->samprate < 8000 || cfg->fold_seconds < 1 || !(cfg->search_window_ms > 0))
    return NULL;
  struct pilot_state *p = calloc(1,sizeof(*p));
  if(p == NULL)
    return NULL;
  p->cfg = *cfg;
  p->p = cfg->samprate;
  p->W = (int)(cfg->search_window_ms * 1e-3 * p->p);
  if(p->W < 8)
    p->W = 8;
  if(2*p->W + 1 > p->p){
    free(p);
    return NULL;
  }
  p->block_len = (int64_t)cfg->fold_seconds * p->p;
  bool ok = true;
  for(int b=0; b < 2; b++){
    struct fold * const f = &p->fold[b];
    f->acc = calloc(p->p,sizeof(*f->acc));
    f->acc_even = calloc(p->p,sizeof(*f->acc_even));
    f->acc_odd = calloc(p->p,sizeof(*f->acc_odd));
    ok = ok && f->acc && f->acc_even && f->acc_odd;
  }
  atomic_init(&p->pending,false);
  atomic_init(&p->dropped,0);
  atomic_init(&p->gen,0);
  p->in_phase = calloc(p->p,sizeof(double));
  p->tee = calloc(p->p,sizeof(double));
  p->sub = calloc(p->p,sizeof(double));
  p->seg = calloc(2*p->W + 1,sizeof(double));
  p->outer = calloc(2*p->W + 1,sizeof(double));
  if(!ok || !p->in_phase || !p->tee || !p->sub || !p->seg || !p->outer){
    pilot_destroy(p);
    return NULL;
  }
  pilot_reset(p);
  return p;
}

void pilot_destroy(struct pilot_state *p){
  if(p == NULL)
    return;
  for(int b=0; b < 2; b++){
    free(p->fold[b].acc); free(p->fold[b].acc_even); free(p->fold[b].acc_odd);
  }
  free(p->in_phase); free(p->tee); free(p->sub); free(p->seg); free(p->outer);
  free(p);
}

// Python reset(): drop the fold. The self-acquired offset survives, as there
static void fold_reset(struct pilot_state const *p,struct fold *f){
  memset(f->acc,0,p->p * sizeof(*f->acc));
  memset(f->acc_even,0,p->p * sizeof(*f->acc_even));
  memset(f->acc_odd,0,p->p * sizeof(*f->acc_odd));
  f->abs_sum = 0;
  f->abs_n = 0;
  f->cont = 0;
  f->quiet = true;
}

// The tracking half of a full forget (finishing side)
static void tracking_reset(struct pilot_state *p){
  p->tracking = false;
  p->own_offset = 0;
  p->nhistory = 0;
  p->failed_blocks = 0;
  p->verdict_pending = false;
  p->last_bootstrap = false;
}

// Python reset()'s Costas part; the coefficients stay pinned, as there
static void costas_reset(struct pilot_state *p){
  p->phase = 0;
  p->phase_initialized = false;
  p->phase_ema_set = false;
  p->phase_ema = 0;
  p->dphase_ema = 0;
  p->costas_locked = false;
  p->relock_counter = 0;
}

// Python process_samples' Costas step, then _update_costas_lock
static void costas_batch(struct pilot_state *p,float complex const *s,int n){
  if(!p->costas_coeffs){
    double const dt = (double)n / p->p;
    p->costas_alpha = 1.0 - exp(-2.0 * M_PI * COSTAS_LOOP_BW_HZ * dt);
    p->phase_ema_alpha = 1.0 - exp(-dt / COSTAS_TAU_PHASE_EMA_S);
    p->dphase_ema_alpha = 1.0 - exp(-dt / COSTAS_TAU_DPHASE_EMA_S);
    p->relock_batches = (int)ceil(COSTAS_RELOCK_S / dt);
    if(p->relock_batches < 1)
      p->relock_batches = 1;
    p->costas_coeffs = true;
  }
  // Square and halve the angle: carrier phase modulo pi
  double complex sq = 0;
  for(int i=0; i < n; i++){
    double complex const x = s[i];
    sq += x * x;
  }
  double const phi_estimate = 0.5 * carg(sq / n);
  double phase_increment;
  if(!p->phase_initialized){
    p->phase = phi_estimate;
    p->phase_initialized = true;
    phase_increment = 0;
  } else {
    // Wrap to [-pi/2, pi/2): the squaring leaves a pi ambiguity
    double const delta = pymod((phi_estimate - p->phase) + M_PI/2,M_PI) - M_PI/2;
    phase_increment = p->costas_alpha * delta;
    p->phase += phase_increment;
  }
  // motion: |dphi| EMA below COSTAS_DPHASE_MAX_RAD
  p->dphase_ema += p->dphase_ema_alpha * (fabs(phase_increment) - p->dphase_ema);
  bool const motion_ok = p->dphase_ema <= COSTAS_DPHASE_MAX_RAD;
  // band: phi within COSTAS_PHASE_BAND_RAD of its slow EMA, frozen while in motion
  if(!p->phase_ema_set){
    p->phase_ema = p->phase;
    p->phase_ema_set = true;
  } else if(motion_ok){
    p->phase_ema += p->phase_ema_alpha * (p->phase - p->phase_ema);
  }
  bool const band_ok = fabs(p->phase - p->phase_ema) <= COSTAS_PHASE_BAND_RAD;
  bool const quiescent = motion_ok && band_ok;
  // Unlock at once; relock only after relock_batches quiet batches in a row
  if(p->costas_locked){
    if(!quiescent){
      p->costas_locked = false;
      p->relock_counter = 0;
    }
  } else if(quiescent){
    if(++p->relock_counter >= p->relock_batches)
      p->costas_locked = true;
  } else {
    p->relock_counter = 0;
  }
}

void pilot_reset(struct pilot_state *p){
  if(p == NULL)
    return;
  fold_reset(p,&p->fold[p->cur]);
  costas_reset(p);
  p->have_next = false; // next call starts a fold wherever it lands
}

void pilot_forget(struct pilot_state *p){
  if(p == NULL)
    return;
  pilot_reset(p);
  // The tracking state belongs to the finishing thread: pilot_finish clears
  // it when the first fold of the new generation arrives
  atomic_fetch_add_explicit(&p->gen,1,memory_order_relaxed);
}

// Python _closed_form_T: T(e) = C[p-1] - 2*C[e-1], C = cumsum(x)
static void closed_form_T(double const *x,int n,double *t){
  double c = 0;
  for(int i=0; i < n; i++){
    t[i] = c;             // C[i-1], 0 for i = 0
    c += x[i];
  }
  for(int i=0; i < n; i++)
    t[i] = c - 2.0 * t[i];
}

// argmax |t|, first on ties (np.argmax)
static int argmax_abs(double const *t,int n){
  int best = 0;
  double bv = fabs(t[0]);
  for(int i=1; i < n; i++){
    if(fabs(t[i]) > bv){
      bv = fabs(t[i]);
      best = i;
    }
  }
  return best;
}

// np.median of n values. Sorts in place by insertion: glibc qsort may malloc,
// and n here is only 2*(W/2) (576 at 96 kHz, 6 ms)
static double median(double *v,int n){
  for(int i=1; i < n; i++){
    double const x = v[i];
    int j = i - 1;
    for(; j >= 0 && v[j] > x; j--)
      v[j+1] = v[j];
    v[j+1] = x;
  }
  return (n & 1) ? v[n/2] : 0.5 * (v[n/2 - 1] + v[n/2]);
}

// Value at t = a + f (0 < f < 1) of x's periodic trigonometric interpolant,
// the function the Python builds from rfft with the Nyquist bin halved:
//   x(t) = sum_m x[m] D(t-m),  D(u) = sin(pi u) cot(pi u/n) / n  (n even)
//                                     sin(pi u) / (n sin(pi u/n)) (n odd)
// with sin(pi (t-m)) = (-1)^m sin(pi t), and sin(pi t) = (-1)^a sin(pi f)
static double interpolant(double const *x,int n,int64_t a,double f){
  double sum = 0;
  double const k = M_PI / n;
  for(int m=0; m < n; m++){
    double const u = f + (double)(a - m);
    double const g = (n & 1) ? 1 / sin(k * u) : 1 / tan(k * u);
    sum += (m & 1) ? -x[m] * g : x[m] * g;
  }
  double const s = (a & 1) ? -sin(M_PI * f) : sin(M_PI * f);
  return s * sum / n;
}

// Python _band_limited_crossing: zero of the interpolant between a and a+1
static double band_limited_crossing(double const *x,int n,int64_t a,double sign){
  double v_lo = sign * x[(int)pymod((double)a,n)];
  double const v_hi = sign * x[(int)pymod((double)(a+1),n)];
  if(v_lo == 0.0)
    return (double)a;
  if(v_hi == 0.0)
    return (double)(a+1);
  double lo = 0, hi = 1; // offsets from a
  for(int i=0; i < CROSSING_ITERATIONS; i++){
    double const mid = 0.5 * (lo + hi);
    double const v_mid = sign * interpolant(x,n,a,mid);
    if((v_mid > 0.0) == (v_lo > 0.0)){
      lo = mid;
      v_lo = v_mid;
    } else {
      hi = mid;
    }
  }
  return (double)a + 0.5 * (lo + hi);
}

static int sgn(double v){
  return (v > 0) - (v < 0);
}

// Python _fit_edge: localize the flip near centre. Returns false when the
// window holds no usable crossing
static bool fit_edge(struct pilot_state *p,double const *in_phase,double centre,struct edge_fit *fit){
  int const P = p->p, W = p->W, L = 2*W + 1;
  int const c = (int)pymod(nearbyint(centre),P);
  double *seg = p->seg;
  for(int i=0; i < L; i++)
    seg[i] = in_phase[(int)pymod((double)(c - W + i),P)];

  int const half = W / 2;
  for(int i=0; i < half; i++){
    p->outer[i] = fabs(seg[i]);
    p->outer[half + i] = fabs(seg[L - half + i]);
  }
  double const A = median(p->outer,2*half);
  if(!(A > 0.0))
    return false;

  // Sign change nearest the centre (local index W)
  int k = -1;
  for(int j=0; j < L-1; j++){
    if(sgn(seg[j+1]) != sgn(seg[j]) && (k < 0 || abs(j - W) < abs(k - W)))
      k = j;
  }
  if(k < 0)
    return false;
  bool const flipped = seg[k] > seg[k+1];
  if(flipped){
    for(int i=0; i < L; i++)
      seg[i] = -seg[i];
  }
  double const band = FIT_BAND_FRACTION * A;
  int lo = k, hi = k+1;
  while(lo > 0 && fabs(seg[lo-1]) < band)
    lo--;
  while(hi < L-1 && fabs(seg[hi+1]) < band)
    hi++;
  if(hi - lo < 1)
    return false;

  // Least-squares line through (lo..hi, seg): np.polyfit(xs, ys, 1)
  int const np = hi - lo + 1;
  double xm = 0, ym = 0;
  for(int i=lo; i <= hi; i++){
    xm += i;
    ym += seg[i];
  }
  xm /= np;
  ym /= np;
  double sxy = 0, sxx = 0;
  for(int i=lo; i <= hi; i++){
    sxy += (i - xm) * (seg[i] - ym);
    sxx += (i - xm) * (i - xm);
  }
  double const m = sxy / sxx;
  double const b = ym - m * xm;
  if(!(m > 0.0))
    return false;
  double ss = 0;
  for(int i=lo; i <= hi; i++){
    double const r = seg[i] - (m * i + b);
    ss += r * r;
  }
  fit->fit_rms = sqrt(ss / np) / A;
  // The line names the bracket and the fit quality; the edge itself is the
  // zero of the fold's band-limited interpolant inside that bracket
  double const sign = flipped ? -1.0 : 1.0;
  double const x0 = band_limited_crossing(in_phase,P,(int64_t)(c - W + k),sign) - (c - W);
  fit->edge_offset = pymod(c - W + x0,P);
  fit->amplitude = A;
  fit->width = hi - lo;
  return true;
}

// Python _triangle_fidelity: RMS of signed T from the tent
// (0,T[0]) -> (apex,T[apex]) -> (n-1,T[n-1]), over |T[apex]|; NaN = no evidence
static double triangle_fidelity(double const *t,int n,int apex){
  if(n < 2 || apex < 0 || apex >= n)
    return NAN;
  double const apex_t = t[apex];
  double const peak = fabs(apex_t);
  if(!isfinite(peak) || peak <= 0.0)
    return NAN;
  double ss = 0;
  // np.linspace: start + i*step, last point exactly stop
  double const step1 = apex > 0 ? (apex_t - t[0]) / apex : 0;
  for(int i=0; i < apex; i++){
    double const ideal = t[0] + i * step1;
    ss += (t[i] - ideal) * (t[i] - ideal);
  }
  int const n2 = n - apex;
  double const step2 = n2 > 1 ? (t[n-1] - apex_t) / (n2 - 1) : 0;
  for(int i=0; i < n2; i++){
    double const ideal = (i == n2 - 1 && n2 > 1) ? t[n-1] : apex_t + i * step2;
    ss += (t[apex + i] - ideal) * (t[apex + i] - ideal);
  }
  double const r = sqrt(ss / n) / peak;
  return isfinite(r) ? r : NAN;
}

// Python _split_half_delta: even-second sub-fold edge minus odd, wrapped
// to (-p/2, p/2]. Each sub-fold located globally (argmax |T|), then refined
static double split_half_delta(struct pilot_state *p,struct fold const *f,double phi){
  int const P = p->p;
  int const counts[2] = { (p->cfg.fold_seconds + 1) / 2, p->cfg.fold_seconds / 2 };
  double complex const *accs[2] = { f->acc_even, f->acc_odd };
  double complex const rot = cexp(-I * phi);
  double out[2];
  for(int h=0; h < 2; h++){
    if(counts[h] == 0)
      return NAN;
    for(int i=0; i < P; i++)
      p->sub[i] = creal(accs[h][i] / counts[h] * rot);
    closed_form_T(p->sub,P,p->tee);
    double const apex = argmax_abs(p->tee,P);
    struct edge_fit fit;
    out[h] = fit_edge(p,p->sub,apex,&fit) ? fit.edge_offset : apex;
  }
  double const d = out[0] - out[1];
  return pymod(d + P/2.0,P) - P/2.0;
}

// Python _note_block_failed
static void note_block_failed(struct pilot_state *p){
  p->verdict_pending = false;
  p->failed_blocks++;
  if(p->failed_blocks >= DEMOTE_AFTER_FAILED_BLOCKS){
    p->tracking = false;
    p->nhistory = 0;
    p->failed_blocks = 0;
  }
}

// Python _note_block_estimate: maintain the self-seed
static void note_block_estimate(struct pilot_state *p,double edge_phase){
  if(p->verdict_pending)
    p->failed_blocks = 0;
  p->verdict_pending = true;
  if(!p->last_bootstrap){
    p->own_offset = edge_phase;
    return;
  }
  if(p->nhistory == BOOTSTRAP_CONFIRM_BLOCKS){
    memmove(p->history,p->history + 1,(BOOTSTRAP_CONFIRM_BLOCKS - 1) * sizeof(double));
    p->nhistory--;
  }
  p->history[p->nhistory++] = edge_phase;
  if(p->nhistory < BOOTSTRAP_CONFIRM_BLOCKS)
    return;
  double const P = p->p;
  double const first = p->history[0];
  double const tol = BOOTSTRAP_CONFIRM_TOLERANCE_MS * 1e-3 * P;
  double spread = 0;
  for(int i=0; i < p->nhistory; i++){
    double const s = fabs(pymod(p->history[i] - first + P/2,P) - P/2);
    if(s > spread)
      spread = s;
  }
  if(spread <= tol){
    p->own_offset = first;
    p->tracking = true;
  }
}

// RMS of the derotated in-phase fold about its +/-A plateaus, more than W
// samples from the edge; snr_db = 20 log10(A / that)
static double fold_snr_db(struct pilot_state const *p,double A,double edge){
  int const P = p->p, W = p->W;
  int const e = (int)ceil(edge); // first sample of the [e,p) plateau
  double side = 0;
  int64_t n = 0;
  for(int i=0; i < P; i++){
    double const d = fabs(pymod(i - edge + P/2.0,P) - P/2.0);
    if(d <= W)
      continue;
    side += (i >= e) ? p->in_phase[i] : -p->in_phase[i];
  }
  double const s_hi = side >= 0 ? 1 : -1; // polarity of the [e,p) plateau
  double ss = 0;
  for(int i=0; i < P; i++){
    double const d = fabs(pymod(i - edge + P/2.0,P) - P/2.0);
    if(d <= W)
      continue;
    double const r = p->in_phase[i] - ((i >= e) ? s_hi : -s_hi) * A;
    ss += r * r;
    n++;
  }
  if(n == 0 || !(A > 0))
    return NAN;
  double rms = sqrt(ss / n);
  if(rms < 1e-12 * A)
    rms = 1e-12 * A;       // noise-free fold: cap at 240 dB, stay finite
  return 20 * log10(A / rms);
}

// Python _finish_block + _compute_estimate. Returns 1 with *out filled, 0 if no fit
static int finish_block(struct pilot_state *p,struct fold const *f,struct pilot_estimate *out){
  int const P = p->p;
  int const cnt = p->cfg.fold_seconds;
  double complex sq = 0;
  for(int i=0; i < P; i++){
    double complex const avg = f->acc[i] / cnt;
    sq += avg * avg;
  }
  // Derotate: squaring removes the BPSK sign, leaving twice the carrier phase
  double const phi = 0.5 * carg(sq / P);
  double complex const rot = cexp(-I * phi);
  double sum_abs_avg = 0;
  for(int i=0; i < P; i++){
    double complex const avg = f->acc[i] / cnt;
    p->in_phase[i] = creal(avg * rot);
    sum_abs_avg += cabs(avg);
  }
  // T(e) over the whole derotated fold: bootstrap locator and battery input
  closed_form_T(p->in_phase,P,p->tee);
  int const apex_idx = argmax_abs(p->tee,P);

  // Python _search_centre (no MF-seeded mode here)
  double centre;
  if(p->tracking){
    p->last_bootstrap = false;
    centre = pymod(p->own_offset - (double)pymod((double)f->fold_start,P),P);
  } else {
    p->last_bootstrap = true;
    centre = apex_idx;
  }
  struct edge_fit fit;
  if(!fit_edge(p,p->in_phase,centre,&fit)){
    note_block_failed(p);
    return 0;
  }
  double const edge_offset = fit.edge_offset;
  // Last edge inside this block, on the output counter
  int64_t const k_last = f->cont / P - 1;
  double const fl = floor(edge_offset);
  int64_t const edge_index = f->fold_start + k_last * P + (int64_t)fl;
  double const edge_frac = edge_offset - fl;
  // Python: edge_rtp = round(registration + c_edge); phase = edge_rtp % p
  int64_t const edge_rtp = (int64_t)nearbyint((double)edge_index + edge_frac); // half-even, as Python round()
  note_block_estimate(p,(double)(((edge_rtp % P) + P) % P));

  double const mean_abs = f->abs_n ? f->abs_sum / f->abs_n : 0.0;
  memset(out,0,sizeof(*out));
  out->edge_index = edge_index;
  out->edge_frac = edge_frac;
  out->edge_offset_samples = edge_offset;
  out->n_seconds = p->cfg.fold_seconds;
  out->plateau_amplitude = fit.amplitude;
  out->fit_rms = fit.fit_rms;
  out->fold_retention = mean_abs > 0 ? (sum_abs_avg / P) / mean_abs : 0.0;
  out->peak_prominence = triangle_fidelity(p->tee,P,apex_idx);
  out->transition_width = fit.width;
  out->apex_distance = pymod(apex_idx - edge_offset + P/2.0,P) - P/2.0;
  out->snr_db = fold_snr_db(p,fit.amplitude,edge_offset);
  out->split_half_delta = split_half_delta(p,f,phi); // reuses tee: after triangle_fidelity
  out->costas_quiet = f->quiet;
  out->locked = out->costas_quiet && out->snr_db >= p->cfg.snr_floor_db;
  strcpy(out->mode,p->last_bootstrap ? "bootstrap" : "tracking");
  return 1;
}

// Hand fold[cur] to the pending slot and continue in the other buffer, which
// pilot_finish left cleared. If the slot is still busy, drop the completed
// fold instead (it never reaches pilot_finish) and start over in place
static int hand_off(struct pilot_state *p){
  if(atomic_load_explicit(&p->pending,memory_order_acquire)){
    atomic_fetch_add_explicit(&p->dropped,1,memory_order_relaxed);
    fold_reset(p,&p->fold[p->cur]);
    return 0;
  }
  p->pend = p->cur;
  p->cur ^= 1;
  atomic_store_explicit(&p->pending,true,memory_order_release);
  return PILOT_FOLD;
}

// pilot_accumulate's body; with sync_out set, pilot_finish runs at each fold
// end before accumulation goes on, as pilot_process always did
static int accumulate(struct pilot_state *p,float complex const *samples,int n,
                      int64_t first_index,struct pilot_estimate *sync_out,int *sync_got){
  if(p == NULL || (samples == NULL && n > 0) || n < 0)
    return -1;
  int flags = 0;
  if(p->have_next && first_index != p->next_index){
    // Gap or epoch change: the fold so far belongs to another counter run
    fold_reset(p,&p->fold[p->cur]);
    flags |= PILOT_GAP;
  }
  p->next_index = first_index + n;
  p->have_next = true;

  if(n > 0)
    costas_batch(p,samples,n);  // Python skips empty batches entirely

  int const P = p->p;
  int consumed = 0;
  while(consumed < n){
    struct fold * const f = &p->fold[p->cur];
    if(f->cont == 0){
      f->fold_start = first_index + consumed;
      f->quiet = true;
      f->gen = atomic_load_explicit(&p->gen,memory_order_relaxed);
    }
    int64_t take = p->block_len - f->cont;
    if(take > n - consumed)
      take = n - consumed;
    // Never cross a fold boundary in one step: cont is 0 at every block start
    int idx = (int)(f->cont % P);
    int64_t sec = f->cont / P;
    double sign = (sec & 1) ? -1.0 : 1.0;
    double complex *half = (sec & 1) ? f->acc_odd : f->acc_even;
    double complex * const acc = f->acc;
    for(int64_t j=0; j < take; j++){
      float complex const x = samples[consumed + j];
      double complex const v = sign * (double complex)x;
      acc[idx] += v;
      half[idx] += v;
      f->abs_sum += cabsf(x);
      if(++idx == P){
        idx = 0;
        sec++;
        sign = -sign;
        half = (sec & 1) ? f->acc_odd : f->acc_even;
      }
    }
    if(!p->costas_locked)
      f->quiet = false;    // this batch fed the fold while the gate was open
    f->abs_n += take;
    f->cont += take;
    consumed += take;
    if(f->cont >= p->block_len){
      int const r = hand_off(p);
      flags |= r;
      if(sync_out != NULL && r == PILOT_FOLD){
        struct pilot_estimate est;
        if(pilot_finish(p,&est) == 1){
          *sync_out = est;
          *sync_got = 1;
        }
      }
    }
  }
  return flags;
}

int pilot_accumulate(struct pilot_state *p,float complex const *samples,int n,int64_t first_index){
  return accumulate(p,samples,n,first_index,NULL,NULL);
}

int pilot_finish(struct pilot_state *p,struct pilot_estimate *out){
  if(p == NULL || out == NULL)
    return 0;
  if(!atomic_load_explicit(&p->pending,memory_order_acquire))
    return 0;
  struct fold * const f = &p->fold[p->pend];
  int r = 0;
  if(f->gen == atomic_load_explicit(&p->gen,memory_order_relaxed)){
    if(p->track_gen != f->gen){
      tracking_reset(p);      // first fold since a pilot_forget
      p->track_gen = f->gen;
    }
    r = finish_block(p,f,out);
  } // else stale: began before a pilot_forget, belongs to the old counter
  fold_reset(p,f);            // cleared here, so the hand-off costs the demod thread nothing
  atomic_store_explicit(&p->pending,false,memory_order_release);
  return r;
}

bool pilot_pending(struct pilot_state *p){
  return p != NULL && atomic_load_explicit(&p->pending,memory_order_acquire);
}

uint64_t pilot_dropped(struct pilot_state *p){
  return p == NULL ? 0 : atomic_load_explicit(&p->dropped,memory_order_relaxed);
}

int pilot_process(struct pilot_state *p,float complex const *samples,int n,
                  int64_t first_index,struct pilot_estimate *out){
  if(out == NULL)
    return -1;
  int got = 0;
  int const flags = accumulate(p,samples,n,first_index,out,&got);
  if(flags < 0 || (flags & PILOT_GAP))
    return -1;
  return got;
}

// Radiod glue arithmetic, pure so pilot_test links it without radiod

int64_t pilot_out_to_input(int64_t block_input_index,int64_t out_offset,int decim,double *frac_io){
  double const f = (frac_io != NULL ? *frac_io : 0.0) * decim;
  double const fl = floor(f);
  if(frac_io != NULL)
    *frac_io = f - fl;
  return block_input_index + out_offset * decim + (int64_t)fl;
}

int64_t pilot_name_second(int64_t edge_host_ns){
  int64_t const B = 1000000000LL;
  int64_t r = edge_host_ns % B;
  if(r < 0)
    r += B;
  return edge_host_ns - r + (r >= B/2 ? B : 0);
}

void pilot_make_report(struct pilot_report *r,struct pilot_estimate const *e,uint64_t epoch,
                       int64_t base_input,int decim,int out_samprate,double in_samprate,
                       int64_t now_ns,int64_t now_input,double filter_delay_s){
  memset(r,0,sizeof(*r));
  double frac = e->edge_frac;
  r->edge_input_sample = pilot_out_to_input(base_input,e->edge_index,decim,&frac);
  r->edge_input_frac = frac;
  // Input samples from the edge to the host reading, back to host time
  double const since = (double)(now_input - r->edge_input_sample) - frac;
  r->host_time_ns = now_ns - llrint(since * 1e9 / in_samprate);
  r->edge_utc_ns = pilot_name_second(r->host_time_ns);
  r->epoch = epoch;
  r->sigma_s = fabs(e->split_half_delta) / 2.0 / out_samprate;
  r->snr_db = e->snr_db;
  r->locked = e->locked;
  r->fold_count = e->n_seconds;
  r->filter_delay_s = filter_delay_s;
  r->est = *e;
  r->valid = true;
}

bool pilot_epoch_check(struct pilot_state *p,struct pilot_report *last,uint64_t *epoch_seen,uint64_t epoch){
  if(epoch == *epoch_seen)
    return false;
  pilot_forget(p);
  last->valid = false;
  *epoch_seen = epoch;
  return true;
}

int pilot_epoch_for_block(struct pilot_epoch_mark const *marks,int n,uint64_t current,
                          int64_t s,int64_t len,uint64_t *epoch_out){
  uint64_t e = current;
  int straddle = 0;
  for(int i=0; i < n; i++){
    if(marks[i].epoch <= current)
      continue;               // history
    if(marks[i].start <= s){
      if(marks[i].epoch > e)
        e = marks[i].epoch;   // loss before this block's first sample
    } else if(marks[i].start < s + len)
      straddle = 1;           // loss inside this block
    // else still ahead
  }
  *epoch_out = e;
  return straddle;
}

bool pilot_block_fresh(bool have_last,int64_t last_index,int64_t index){
  return !have_last || index > last_index;
}
