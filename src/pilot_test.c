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
#include "pilot_status.h"

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
  // Carrier-phase excursion: phi(t) = exc_rad sin^2(pi (t - exc_t0) / exc_len)
  // added on [exc_t0, exc_t0 + exc_len), a swing out and back; 0 = none
  int64_t exc_t0, exc_len;
  double exc_rad;
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
  s->exc_t0 = s->exc_len = 0;
  s->exc_rad = 0;
}
static void synth_fill(struct synth const *s,float complex *buf,int n,int64_t t0){
  for(int i=0; i < n; i++){
    int64_t const t = t0 + i;
    double const base = ((t / SR) & 1) ? -1 : 1;
    double complex v = s->amp * base * s->ramp[t % SR] * s->rot;
    if(s->exc_len > 0 && t >= s->exc_t0 && t < s->exc_t0 + s->exc_len){
      double const x = sin(M_PI * (double)(t - s->exc_t0) / s->exc_len);
      v *= cexp(I * s->exc_rad * x * x);
    }
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
    assert(est[i].locked == (i > 0)); // fold 0 holds the Costas gate's warm-up
    assert(est[i].costas_quiet == (i > 0));
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
  assert(n >= 1);           // the test proves nothing if no fold produced an estimate
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

// Carrier phase swings 5 rad over 12 s inside the third fold (60-90 s):
// the Costas quiet gate opens, so that fold neither claims quiet nor locks;
// the folds either side stay locked. Fold 0 holds the gate's warm-up
// (COSTAS_RELOCK_S of quiet before the first lock), so it is not quiet either
static void test_excursion_blocks_lock(void){
  static struct synth s;
  synth_init(&s,41234.0,0,1.0,0);
  s.exc_t0 = 63LL * SR;
  s.exc_len = 12LL * SR;
  s.exc_rad = 5.0;
  struct pilot_estimate est[MAXEST];
  int const n = run(&s,est);
  assert(n == SECONDS / Cfg.fold_seconds);
  for(int i=0; i < n; i++)
    printf("  excursion %d: edge %.6f snr %.1f dB costas_quiet %d locked %d\n",i,
           est[i].edge_offset_samples,est[i].snr_db,est[i].costas_quiet,est[i].locked);
  assert(!est[0].costas_quiet && !est[0].locked);
  assert(est[1].costas_quiet && est[1].locked);
  assert(!est[2].costas_quiet && !est[2].locked);
  assert(est[3].costas_quiet && est[3].locked);
  assert(est[4].costas_quiet && est[4].locked);
  printf("test_excursion_blocks_lock: pass\n");
}


// Brief: decim 675, block index 1 000 000, offset 10, frac 0.25 -> 1 006 918, frac 0.75
// (10.25 x 675 = 6918.75, exact in binary)
static void test_out_to_input(void){
  double frac = 0.25;
  int64_t const in = pilot_out_to_input(1000000,10,675,&frac);
  printf("  out_to_input: %lld frac %.17g\n",(long long)in,frac);
  assert(in == 1006918);
  assert(frac == 0.75);
  // Large output counter (past 2^31: 6 h at 96 kHz), frac 0
  frac = 0;
  assert(pilot_out_to_input(7,(int64_t)3000000000LL,1350,&frac) == 7 + 3000000000LL * 1350);
  assert(frac == 0);
  printf("test_out_to_input: pass\n");
}

// Edge host time S + 0.3 s names second S; the 0.3 s stays visible in host_time_ns
static void test_name_second_reports_host_gap(void){
  int64_t const B = 1000000000LL;
  int64_t const S = 1440000000LL;            // seconds since the GPS epoch, 2025-ish
  assert(pilot_name_second(S*B + 300000000LL) == S*B);
  assert(pilot_name_second(S*B + 600000000LL) == (S+1)*B);
  assert(pilot_name_second(S*B - 300000000LL) == S*B);
  assert(pilot_name_second(S*B) == S*B);

  // Through the report: edge at output index 1000 (frac 0) on a 64.8 MHz input,
  // decim 675 -> input 675000 + base. Host read one second of input later at S+1.3 s
  struct pilot_estimate e;
  memset(&e,0,sizeof(e));
  e.edge_index = 1000;
  e.edge_frac = 0;
  e.n_seconds = 30;
  e.split_half_delta = -0.96;  // sigma = 0.96/2/96000 = 5 us
  e.snr_db = 16.0;
  e.locked = true;
  int64_t const base = 5000;
  int64_t const edge_in = base + 675000;
  struct pilot_report r;
  pilot_make_report(&r,&e,4,base,675,96000,64.8e6,(S+1)*B + 300000000LL,edge_in + 64800000,1.25e-4);
  printf("  report: input %lld frac %g utc %lld host %lld sigma %g\n",(long long)r.edge_input_sample,
         r.edge_input_frac,(long long)r.edge_utc_ns,(long long)r.host_time_ns,r.sigma_s);
  assert(r.valid);
  assert(r.epoch == 4);
  assert(r.edge_input_sample == edge_in);
  assert(r.edge_input_frac == 0);
  assert(r.edge_utc_ns == S*B);
  assert(r.host_time_ns == S*B + 300000000LL);
  assert(fabs(r.sigma_s - 0.96/2/96000) < 1e-15);
  assert(r.snr_db == 16.0 && r.locked && r.fold_count == 30);
  assert(r.filter_delay_s == 1.25e-4);
  assert(r.est.edge_index == 1000);
  printf("test_name_second_reports_host_gap: pass\n");
}

// Report from epoch 3, then a block from epoch 4: the detector forgets
// everything (fold, Costas, own offset, bootstrap history), last.valid stays
// false until a new fold completes, and that fold bootstraps afresh
static void test_epoch_change_resets(void){
  static struct synth s;
  synth_init(&s,41234.0,0,1.0,0);
  struct pilot_state *p = pilot_create(&Cfg);
  assert(p != NULL);
  static float complex buf[BATCH];
  struct pilot_report last;
  memset(&last,0,sizeof(last));
  uint64_t epoch_seen = 0;
  assert(pilot_epoch_check(p,&last,&epoch_seen,3)); // first block: epoch 0 -> 3
  assert(epoch_seen == 3 && !last.valid);
  // 120 s in epoch 3: folds 0-2 bootstrap, fold 3 tracks
  int64_t t = 0;
  int nest = 0;
  struct pilot_estimate e;
  char mode[12] = "";
  int n = BATCH;
  for(; t < 120LL*SR; t += n){
    n = (t + BATCH <= 120LL*SR) ? BATCH : (int)(120LL*SR - t);
    assert(!pilot_epoch_check(p,&last,&epoch_seen,3));
    synth_fill(&s,buf,n,t);
    if(pilot_process(p,buf,n,t,&e) == 1){
      pilot_make_report(&last,&e,epoch_seen,0,675,SR,64.8e6,0,0,0);
      strcpy(mode,e.mode);
      nest++;
    }
  }
  assert(nest == 4 && last.valid && last.epoch == 3);
  assert(strcmp(mode,"tracking") == 0);
  // Half a fold more, then epoch 4 arrives: the counter rebases to 0
  int64_t const rebase = t + 15LL*SR;
  for(; t < rebase; t += BATCH){
    synth_fill(&s,buf,BATCH,t);
    assert(pilot_process(p,buf,BATCH,t,&e) == 0);
  }
  assert(pilot_epoch_check(p,&last,&epoch_seen,4));
  assert(epoch_seen == 4 && !last.valid);
  // Feed epoch 4 from output counter 0 (same synthetic edge phase)
  int64_t c = 0;
  nest = 0;
  char modes[4][12];
  for(; nest < 4; c += BATCH){
    assert(!pilot_epoch_check(p,&last,&epoch_seen,4));
    synth_fill(&s,buf,BATCH,c);
    int const r = pilot_process(p,buf,BATCH,c,&e);
    assert(r >= 0);   // a forget is not a gap: the next call starts a fresh fold
    if(r == 0 && nest == 0)
      assert(!last.valid); // nothing reported until the first post-forget fold
    if(r == 1){
      // The first fold holds exactly fold_seconds of epoch-4 samples, none from before
      if(nest == 0)
        assert(c < 30LL*SR && c + BATCH >= 30LL*SR);
      assert(fabs(abs_phase(&e) - 41234.0) <= 0.001);
      strcpy(modes[nest],e.mode);
      pilot_make_report(&last,&e,epoch_seen,0,675,SR,64.8e6,0,0,0);
      nest++;
    }
  }
  printf("  after forget: %s %s %s %s\n",modes[0],modes[1],modes[2],modes[3]);
  // Python's reset would keep tracking; the full forget re-bootstraps
  assert(strcmp(modes[0],"bootstrap") == 0);
  assert(strcmp(modes[1],"bootstrap") == 0);
  assert(strcmp(modes[2],"bootstrap") == 0);
  assert(strcmp(modes[3],"tracking") == 0);
  assert(last.valid && last.epoch == 4);
  pilot_destroy(p);
  printf("test_epoch_change_resets: pass\n");
}

// Bitwise field comparison (struct padding need not match; NaN equals NaN)
static bool same_double(double a,double b){
  return memcmp(&a,&b,sizeof(a)) == 0;
}
static bool same_estimate(struct pilot_estimate const *a,struct pilot_estimate const *b){
  return a->edge_index == b->edge_index && same_double(a->edge_frac,b->edge_frac)
    && same_double(a->edge_offset_samples,b->edge_offset_samples) && a->n_seconds == b->n_seconds
    && same_double(a->plateau_amplitude,b->plateau_amplitude) && same_double(a->fit_rms,b->fit_rms)
    && same_double(a->fold_retention,b->fold_retention) && same_double(a->split_half_delta,b->split_half_delta)
    && same_double(a->peak_prominence,b->peak_prominence) && same_double(a->transition_width,b->transition_width)
    && same_double(a->apex_distance,b->apex_distance) && same_double(a->snr_db,b->snr_db)
    && a->costas_quiet == b->costas_quiet && a->locked == b->locked && strcmp(a->mode,b->mode) == 0;
}

// radiod's split: pilot_accumulate on the demod thread, pilot_finish later on a
// worker. Finishing each fold several blocks after it completes must give the
// same estimates, bit for bit, as the synchronous pilot_process
static void test_async_matches_sync(void){
  static struct synth s;
  synth_init(&s,41234.37,25000,1.0,0.05);
  struct pilot_estimate sync_est[MAXEST], async_est[MAXEST];
  uint64_t const seed = Rng;
  int const nsync = run(&s,sync_est);
  Rng = seed;  // same noise
  struct pilot_state *p = pilot_create(&Cfg);
  assert(p != NULL);
  static float complex buf[BATCH];
  int nasync = 0, delay = -1;
  for(int64_t t=0; t < (int64_t)SR*SECONDS; t += BATCH){
    int const n = (t + BATCH <= (int64_t)SR*SECONDS) ? BATCH : (int)((int64_t)SR*SECONDS - t);
    synth_fill(&s,buf,n,t);
    int const f = pilot_accumulate(p,buf,n,t);
    assert(f >= 0 && !(f & PILOT_GAP));
    if(f & PILOT_FOLD){
      assert(pilot_pending(p));
      delay = 7;               // the worker gets to it 7 blocks later
    }
    if(delay >= 0 && delay-- == 0){
      struct pilot_estimate e;
      if(pilot_finish(p,&e) == 1)
        async_est[nasync++] = e;
      assert(!pilot_pending(p));
    }
  }
  if(delay >= 0){
    struct pilot_estimate e;
    if(pilot_finish(p,&e) == 1)
      async_est[nasync++] = e;
  }
  assert(nasync == nsync && nsync == SECONDS / Cfg.fold_seconds);
  for(int i=0; i < nsync; i++){
    printf("  async %d: %s edge %.9f\n",i,async_est[i].mode,async_est[i].edge_offset_samples);
    assert(same_estimate(&async_est[i],&sync_est[i]));
  }
  assert(pilot_dropped(p) == 0);
  pilot_destroy(p);
  printf("test_async_matches_sync: pass\n");
}

// A fold that completes while the previous one still waits for pilot_finish is
// dropped, never overwrites the pending one; nothing pending -> finish returns 0
static void test_unfinished_fold_drops_next(void){
  static struct synth s;
  synth_init(&s,41234.0,0,1.0,0);
  struct pilot_state *p = pilot_create(&Cfg);
  assert(p != NULL);
  struct pilot_estimate e;
  assert(pilot_finish(p,&e) == 0);
  static float complex buf[BATCH];
  int folds = 0;
  for(int64_t t=0; t < 65LL*SR; t += BATCH){
    synth_fill(&s,buf,BATCH,t);
    int const f = pilot_accumulate(p,buf,BATCH,t);
    if(f & PILOT_FOLD)
      folds++;
  }
  assert(folds == 1);           // the second completion found the slot busy
  assert(pilot_dropped(p) == 1);
  assert(pilot_finish(p,&e) == 1);
  // The pending fold was the first one: its last edge lies in [29 s, 30 s)
  assert(e.edge_index >= 29LL*SR && e.edge_index < 30LL*SR);
  assert(pilot_finish(p,&e) == 0);
  pilot_destroy(p);
  printf("test_unfinished_fold_drops_next: pass\n");
}

// A forget while a fold waits: the waiting fold belongs to the old counter and
// finishes as stale (0, no estimate)
static void test_forget_discards_pending(void){
  static struct synth s;
  synth_init(&s,41234.0,0,1.0,0);
  struct pilot_state *p = pilot_create(&Cfg);
  assert(p != NULL);
  static float complex buf[BATCH];
  int64_t t = 0;
  for(; !pilot_pending(p); t += BATCH){
    synth_fill(&s,buf,BATCH,t);
    assert(pilot_accumulate(p,buf,BATCH,t) >= 0);
  }
  pilot_forget(p);
  struct pilot_estimate e;
  assert(pilot_finish(p,&e) == 0);
  assert(!pilot_pending(p));
  pilot_destroy(p);
  printf("test_forget_discards_pending: pass\n");
}

// Fix round 1: where an epoch begins. A loss recorded at input sample L takes
// effect at the first block whose first sample is at or after L; a block that
// ends at or before L keeps the old epoch; a block with L strictly inside it
// keeps the old epoch (its first sample is pre-loss) but holds both sides of
// the loss, so it must not reach the detector
static void test_epoch_switch_at_loss_point(void){
  int64_t const len = 1296000;          // one 20 ms block at 64.8 MHz
  struct pilot_epoch_mark m[3] = {{ .epoch = 7, .start = 10*len + 5000 }};
  uint64_t e = 0;
  // Block entirely before the loss
  assert(pilot_epoch_for_block(m,1,6,9*len,len,&e) == 0 && e == 6);
  // Block ending exactly at the loss point: all pre-loss
  m[0].start = 10*len;
  assert(pilot_epoch_for_block(m,1,6,9*len,len,&e) == 0 && e == 6);
  // ...and the next block begins exactly at it: new epoch
  assert(pilot_epoch_for_block(m,1,6,10*len,len,&e) == 0 && e == 7);
  // Loss inside the block: old epoch, straddle
  m[0].start = 10*len + 5000;
  assert(pilot_epoch_for_block(m,1,6,10*len,len,&e) == 1 && e == 6);
  // First block after it: new epoch, clean
  assert(pilot_epoch_for_block(m,1,6,11*len,len,&e) == 0 && e == 7);
  // Marks at or below the current epoch are history
  assert(pilot_epoch_for_block(m,1,7,10*len,len,&e) == 0 && e == 7);
  // A late mark (clock-step monitor: start behind blocks already done) takes effect at once
  m[0].start = 2*len;
  assert(pilot_epoch_for_block(m,1,6,11*len,len,&e) == 0 && e == 7);
  // Two losses: passed one, the next still ahead inside this block
  m[0] = (struct pilot_epoch_mark){ .epoch = 7, .start = 10*len };
  m[1] = (struct pilot_epoch_mark){ .epoch = 8, .start = 11*len + 1 };
  assert(pilot_epoch_for_block(m,2,6,11*len,len,&e) == 1 && e == 7);
  assert(pilot_epoch_for_block(m,2,7,12*len,len,&e) == 0 && e == 8);
  // Both passed: the newest wins
  assert(pilot_epoch_for_block(m,2,6,13*len,len,&e) == 0 && e == 8);
  // No marks: nothing changes
  assert(pilot_epoch_for_block(m,0,6,13*len,len,&e) == 0 && e == 6);
  printf("test_epoch_switch_at_loss_point: pass\n");
}

// Fix round 1: a lapped-ring zero block leaves filter.out.sample_index where
// the previous block put it; such a block is not fresh (no anchor, no detector).
// A forward jump (jobs skipped) is a real block with its own true index
static void test_lapped_block_not_fresh(void){
  int64_t const len = 1296000;
  assert(pilot_block_fresh(false,0,0));            // first block
  assert(pilot_block_fresh(true,5*len,6*len));     // normal advance
  assert(!pilot_block_fresh(true,6*len,6*len));    // zero block: index did not move
  assert(pilot_block_fresh(true,6*len,8*len));     // after a zero block, or a skip ahead
  assert(!pilot_block_fresh(true,8*len,7*len));    // backwards: never a new block
  printf("test_lapped_block_not_fresh: pass\n");
}

// Task 6: every status TLV survives encode -> decode, using the same
// encode_pilot_status that radio_status.c calls
static void test_tlv_roundtrip(void){
  struct pilot_report r = {
    .valid = true, .epoch = 7, .edge_input_sample = 1234567890123LL, .edge_input_frac = 0.3125,
    .edge_utc_ns = 1446854418000000000LL, .host_time_ns = 1446854418012345678LL,
    .sigma_s = 2.5e-6, .snr_db = 31.5, .locked = true, .fold_count = 30, .filter_delay_s = 1.25e-4,
    .est = { .plateau_amplitude = 1.5, .fit_rms = 0.25, .fold_retention = 0.75, .split_half_delta = -0.5,
             .peak_prominence = 8.0, .transition_width = 3.5, .apex_distance = 0.125, .edge_offset_samples = 4321.5 },
  };
  uint8_t pkt[512], *bp = pkt;
  encode_pilot_status(&bp,7,0xfffffff0u,-98765432100LL,&r);
  *bp++ = EOL;
  struct pilot_status ps;
  memset(&ps,0,sizeof ps);
  int n = 0;
  uint8_t const *cp = pkt;
  while(*cp != EOL){
    enum status_type type = *cp++;
    int optlen = *cp++;
    assert(optlen < 128);
    assert(decode_pilot_status(&ps,type,cp,optlen));
    cp += optlen;
    n++;
  }
  assert(n == 13);
  assert(ps.epoch == 7 && ps.anchor_rtp == 0xfffffff0u && ps.anchor_input_sample == -98765432100LL);
  struct pilot_report const *g = &ps.report;
  assert(g->valid && g->edge_input_sample == r.edge_input_sample && g->edge_input_frac == r.edge_input_frac);
  assert(g->edge_utc_ns == r.edge_utc_ns && g->host_time_ns == r.host_time_ns);
  assert(g->sigma_s == r.sigma_s && g->snr_db == r.snr_db && g->locked && g->fold_count == 30);
  assert(g->filter_delay_s == r.filter_delay_s);
  assert(g->est.plateau_amplitude == 1.5f && g->est.fit_rms == 0.25f && g->est.fold_retention == 0.75f);
  assert(g->est.split_half_delta == -0.5f && g->est.peak_prominence == 8.0f && g->est.transition_width == 3.5f);
  assert(g->est.apex_distance == 0.125f && g->est.edge_offset_samples == 4321.5f);

  // A report from another epoch, or an invalid one, sends the anchor alone
  for(int k=0; k < 2; k++){
    struct pilot_report s = r;
    if(k == 0) s.epoch = 6; else s.valid = false;
    bp = pkt;
    encode_pilot_status(&bp,7,5,6,&s);
    assert(bp - pkt < 40);
    *bp++ = EOL;
    memset(&ps,0,sizeof ps);
    for(cp = pkt; *cp != EOL; ){
      enum status_type type = *cp++;
      int optlen = *cp++;
      assert(decode_pilot_status(&ps,type,cp,optlen));
      cp += optlen;
    }
    assert(ps.epoch == 7 && ps.anchor_rtp == 5 && ps.anchor_input_sample == 6 && !ps.report.valid);
  }
  printf("test_tlv_roundtrip: pass\n");
}

int main(void){
  test_clean_edge();
  test_fractional_edge();
  test_noise_only_never_locks();
  test_epoch_change_discards_fold();
  test_excursion_blocks_lock();
  test_out_to_input();
  test_name_second_reports_host_gap();
  test_epoch_change_resets();
  test_async_matches_sync();
  test_unfinished_fold_drops_next();
  test_forget_discards_pending();
  test_epoch_switch_at_loss_point();
  test_lapped_block_not_fresh();
  test_tlv_roundtrip();
  printf("all pilot tests passed\n");
  return 0;
}
