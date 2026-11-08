// Unit tests for the TS-1 pilot edge detector (pilot.c)
// Each test synthesizes 150 s of 96 kHz baseband and feeds it in 1740-sample batches
// make pilot_test && ./pilot_test ; exits 0 when every test passes
#undef NDEBUG
#include <assert.h>
#include <complex.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pilot.h"

#define SR 96000
#define SECONDS 150
#define BATCH 1740

// Sine integral Si(x): power series near 0, continued fraction for E1(ix) beyond
// (Numerical Recipes cisi)
static double sine_integral(double x){
  double const ax = fabs(x);
  double s;
  if(ax < 1e-300)
    return 0;
  if(ax <= 2.0){
    double term = ax, sum = ax;
    for(int k=1; k < 100; k++){
      term *= -ax*ax / ((2*k) * (2*k+1));
      double const t = term / (2*k+1);
      sum += t;
      if(fabs(t) < 1e-17 * fabs(sum))
        break;
    }
    s = sum;
  } else {
    double complex b = 1 + I*ax;
    double complex c = 1e300;
    double complex d = 1/b;
    double complex h = d;
    for(int i=2; i < 10000; i++){
      double const a = -(double)(i-1)*(i-1);
      b += 2;
      d = 1/(a*d + b);
      c = b + a/c;
      double complex const del = c*d;
      h *= del;
      if(cabs(del - 1) < 1e-16)
        break;
    }
    h *= cos(ax) - I*sin(ax);
    s = M_PI/2 + cimag(h);
  }
  return x < 0 ? -s : s;
}

// Uniform (0,1) from xorshift64*, then Box-Muller
static uint64_t Rng = 88172645463325252ULL;
static double uniform(void){
  Rng ^= Rng >> 12; Rng ^= Rng << 25; Rng ^= Rng >> 27;
  return ((Rng * 2685821657736338717ULL) >> 11) * 0x1.0p-53 + 0x1.0p-54;
}
static double gauss(void){
  return sqrt(-2*log(uniform())) * cos(2*M_PI*uniform());
}

// One polarity flip per second at edge, base polarity alternating per second,
// constant carrier phase: hf-timestd tests' make_bpsk()
struct synth {
  double ramp[SR];       // ramp(pos) within the second
  double amp;            // 0 = no pilot
  double noise_rms;      // complex rms
  double complex rot;
};
static void synth_init(struct synth *s,double edge,double bandlimit_hz,double amp,double noise_rms){
  double const tau = (52e-6 * SR) / 2.197;
  for(int i=0; i < SR; i++){
    if(bandlimit_hz > 0)
      s->ramp[i] = (2/M_PI) * sine_integral(2*M_PI*bandlimit_hz * (i - edge) / SR);
    else
      s->ramp[i] = tanh((i - edge) / tau);
  }
  s->amp = amp;
  s->noise_rms = noise_rms;
  s->rot = cexp(I*0.3);
}
static void synth_fill(struct synth const *s,float complex *buf,int n,int64_t t0){
  for(int i=0; i < n; i++){
    int64_t const t = t0 + i;
    double const base = ((t / SR) & 1) ? -1 : 1;
    double complex v = s->amp * base * s->ramp[t % SR] * s->rot;
    if(s->noise_rms > 0)
      v += s->noise_rms * (gauss() + I*gauss()) / M_SQRT2;
    buf[i] = v;
  }
}

static struct pilot_config const Cfg = { .samprate = SR, .fold_seconds = 30, .search_window_ms = 6.0, .snr_floor_db = 20.0 };

#define MAXEST 16
// Feed [0, SECONDS) in batches; collect estimates
static int run(struct synth const *s,struct pilot_estimate *est){
  struct pilot_state *p = pilot_create(&Cfg);
  assert(p != NULL);
  static float complex buf[BATCH];
  int nest = 0;
  for(int64_t t=0; t < (int64_t)SR*SECONDS; t += BATCH){
    int const n = (t + BATCH <= (int64_t)SR*SECONDS) ? BATCH : (int)((int64_t)SR*SECONDS - t);
    synth_fill(s,buf,n,t);
    struct pilot_estimate e;
    int const r = pilot_process(p,buf,n,t,&e);
    assert(r >= 0);
    if(r == 1){
      assert(nest < MAXEST);
      est[nest++] = e;
    }
  }
  pilot_destroy(p);
  return nest;
}

// Edge position within the second from the absolute index
static double abs_phase(struct pilot_estimate const *e){
  return fmod((double)(e->edge_index % SR) + e->edge_frac, SR);
}

static void test_clean_edge(void){
  static struct synth s;
  synth_init(&s,41234.0,0,1.0,0);
  struct pilot_estimate est[MAXEST];
  int const n = run(&s,est);
  assert(n == SECONDS / Cfg.fold_seconds);
  for(int i=0; i < n; i++){
    printf("  clean %d: %s edge %.6f abs %.6f snr %.1f dB locked %d\n",i,est[i].mode,
           est[i].edge_offset_samples,abs_phase(&est[i]),est[i].snr_db,est[i].locked);
    assert(est[i].locked);
    assert(est[i].n_seconds == Cfg.fold_seconds);
    assert(est[i].edge_frac >= 0 && est[i].edge_frac < 1);
    if(i > 0){
      assert(fabs(est[i].edge_offset_samples - 41234.0) <= 0.001);
      assert(fabs(abs_phase(&est[i]) - 41234.0) <= 0.001);
    }
  }
  assert(strcmp(est[n-1].mode,"tracking") == 0);
  printf("test_clean_edge: pass\n");
}

static void test_fractional_edge(void){
  static struct synth s;
  synth_init(&s,41234.37,25000,1.0,0);
  struct pilot_estimate est[MAXEST];
  int const n = run(&s,est);
  assert(n == SECONDS / Cfg.fold_seconds);
  for(int i=0; i < n; i++){
    printf("  frac %d: %s edge %.6f err %+.6f\n",i,est[i].mode,est[i].edge_offset_samples,
           est[i].edge_offset_samples - 41234.37);
    assert(fabs(est[i].edge_offset_samples - 41234.37) <= 0.005);
    assert(fabs(abs_phase(&est[i]) - 41234.37) <= 0.005);
  }
  printf("test_fractional_edge: pass\n");
}

static void test_noise_only_never_locks(void){
  static struct synth s;
  synth_init(&s,41234.0,0,0.0,1.0);
  struct pilot_estimate est[MAXEST];
  int const n = run(&s,est);
  for(int i=0; i < n; i++){
    printf("  noise %d: edge %.3f snr %.1f dB locked %d\n",i,est[i].edge_offset_samples,est[i].snr_db,est[i].locked);
    assert(!est[i].locked);
  }
  printf("test_noise_only_never_locks: pass (%d estimates, none locked)\n",n);
}

// A first_index jump mid-fold: -1, and the next estimate folds only post-gap seconds
static void test_epoch_change_discards_fold(void){
  static struct synth s;
  synth_init(&s,41234.0,0,1.0,0);
  struct pilot_state *p = pilot_create(&Cfg);
  assert(p != NULL);
  static float complex buf[BATCH];
  int64_t const gap_at = 10LL*SR + 12345;   // stream index where samples stop
  int64_t const resume = gap_at + 54321;    // where they start again: not a whole second
  int64_t const fold_end = resume + (int64_t)Cfg.fold_seconds * SR; // first post-gap fold completes here
  int errors = 0, nest = 0;
  int64_t t = 0;
  while(t < (int64_t)SR*SECONDS){
    int n = BATCH;
    if(t < gap_at && t + n > gap_at)
      n = (int)(gap_at - t);           // stop exactly at the gap
    if(t + n > (int64_t)SR*SECONDS)
      n = (int)((int64_t)SR*SECONDS - t);
    synth_fill(&s,buf,n,t);
    struct pilot_estimate e;
    int const r = pilot_process(p,buf,n,t,&e);
    if(r == -1){
      errors++;
      assert(t == resume);
    }
    if(r == 1){
      if(nest == 0){
        // The fold that completes first holds only post-gap samples
        assert(t < fold_end && t + n >= fold_end);
      }
      nest++;
      printf("  epoch %d: edge %.6f abs %.6f n_seconds %d amp %.6f\n",nest,e.edge_offset_samples,abs_phase(&e),e.n_seconds,e.plateau_amplitude);
      assert(e.n_seconds == Cfg.fold_seconds);
      assert(fabs(abs_phase(&e) - 41234.0) <= 0.001);
      assert(fabs(e.plateau_amplitude - 1.0) <= 1e-3);
      assert(e.locked);
    }
    t += n;
    if(t == gap_at)
      t = resume;
  }
  assert(errors == 1);
  assert(nest >= 1);
  pilot_destroy(p);
  printf("test_epoch_change_discards_fold: pass\n");
}

int main(void){
  test_clean_edge();
  test_fractional_edge();
  test_noise_only_never_locks();
  test_epoch_change_discards_fold();
  printf("all pilot tests passed\n");
  return 0;
}
