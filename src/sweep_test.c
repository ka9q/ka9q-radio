// Unit tests for the sweep (chirp) time unit and parameter validation (sweep.c)
// make sweep_test && ./sweep_test ; exits 0 when every test passes
#undef NDEBUG
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "sweep.h"
#include "status.h"
#include "radio.h"

#define FE_LO 0.0
#define FE_HI 64.8e6

static void validate_rejects_zero_period(void){
  struct sweep_params p = { .rate = -25733.913, .period = 0, .duration = 0, .start = 0 };
  assert(sweep_validate(&p,4000,4.4e6,4.463e6,FE_LO,FE_HI) != NULL);
}

static void validate_rejects_duration_over_period(void){
  struct sweep_params p = { .rate = -25733.913, .period = 1.0, .duration = 1.1, .start = 0 };
  assert(sweep_validate(&p,4000,4.4e6,4.463e6,FE_LO,FE_HI) != NULL);
}

static void validate_rejects_steep_ramp(void){
  // |rate T^2| = 1e9 / 4000^2 = 62.5 >= 0.5
  struct sweep_params p = { .rate = 1e9, .period = 1.0, .duration = 1.0, .start = 0 };
  assert(sweep_validate(&p,4000,4.4e6,4.463e6,FE_LO,FE_HI) != NULL);
}

static void validate_rejects_span_outside_coverage(void){
  struct sweep_params p = { .rate = -25733.913, .period = 1.0, .duration = 1.0, .start = 0 };
  assert(sweep_validate(&p,4000,70e6,71e6,FE_LO,FE_HI) != NULL);
  assert(sweep_validate(&p,4000,-1e3,4.463e6,FE_LO,FE_HI) != NULL);
}

static void validate_accepts_codar_4463(void){
  struct sweep_params p = { .rate = -25733.913, .period = 1.0, .duration = 1.0, .start = 0 };
  assert(sweep_validate(&p,4000,4.463e6 - 25733.913,4.463e6,FE_LO,FE_HI) == NULL);
  // no rate, no sweep: always valid, even with a zero period
  struct sweep_params q = { 0, 0, 0, 0 };
  assert(sweep_validate(&q,4000,0,0,FE_LO,FE_HI) == NULL);
  // unknown coverage (NAN) skips only the span check
  assert(sweep_validate(&p,4000,4.4e6,4.463e6,NAN,NAN) == NULL);
}

static void time_folds_negative_start(void){
  struct sweep_params p = { .rate = 1000, .period = 1.0, .duration = 1.0, .start = -0.25 };
  double const t = sweep_time(&p,0,0,129.6e6);
  assert(fabs(t - 0.75) < 1e-12);
  assert(t >= 0 && t < p.period);
}

static void active_below_duration(void){
  struct sweep_params p = { .rate = 1000, .period = 1.0, .duration = 0.4, .start = 0 };
  assert(sweep_active(&p,0.39));
  assert(!sweep_active(&p,0.4));
  assert(!sweep_active(&p,0.9));
}

static void epoch_lands_on_sweep_start(void){
  double const sr = 129.6e6;
  struct sweep_params p = { .rate = -25733.913, .period = 1.0, .duration = 1.0, .start = 0.0 };
  int64_t es; double ef;
  sweep_epoch(&p,3 * (uint64_t)sr + 17,0,sr,&es,&ef);
  assert(es == 3 * (int64_t)sr);
  assert(ef == 0.0);

  p.start = 0.3;
  uint64_t const idx = 100000000; // t = 0.3 + 0.7716 -> 0.0716 into the sweep
  sweep_epoch(&p,idx,0,sr,&es,&ef);
  assert(es <= (int64_t)idx);
  assert(ef >= 0 && ef < 1);
  assert(fabs((double)es + ef - 0.7 * sr) < 1e-3);
}

static void epoch_brief_exact_and_large_index(void){
  double const sr = 129.6e6;
  struct sweep_params p = { .rate = -25733.913, .period = 1.0, .duration = 1.0, .start = 0.3 };
  int64_t es; double ef;
  uint64_t const idx = 90720000ULL; // 0.7*129.6e6: the sample where the 0.3 s start lands a sweep
  sweep_epoch(&p,idx,0,sr,&es,&ef);
  assert(es == (int64_t)idx);
  assert(ef < 1e-3 || ef > 1 - 1e-3);
  assert(sweep_time(&p,idx,0,sr) < 1e-9); // sweep_time agrees: a sweep starts here
  // one sample earlier is the end of the previous sweep
  sweep_epoch(&p,idx - 1,0,sr,&es,&ef);
  assert(es == (int64_t)idx - 129600000);
  assert(sweep_time(&p,idx - 1,0,sr) > 0.99);

  p.start = 0;
  uint64_t const big = (1ULL << 40) + 12345;
  sweep_epoch(&p,big,0,sr,&es,&ef);
  assert(es <= (int64_t)big && (int64_t)big - es < 129600000);
  assert(es % 129600000 == 0);
  assert(ef == 0.0);
}

// A preset gives rate and period, then the section overrides the period only.
// Duration is never stored as a default, so it follows the new period.
static void effective_duration_follows_period(void){
  struct sweep_params p = { .rate = -25733.913, .period = 1.0, .duration = 0, .start = 0 };
  assert(sweep_validate(&p,4000,4.4e6,4.463e6,FE_LO,FE_HI) == NULL);
  assert(sweep_duration(&p) == 1.0);
  p.period = 0.5; // section override
  assert(sweep_validate(&p,4000,4.4e6,4.463e6,FE_LO,FE_HI) == NULL);
  assert(sweep_duration(&p) == 0.5);
  assert(sweep_active(&p,sweep_time(&p,0,0,129.6e6)));
  // an explicit duration still wins, and is still checked against the period
  p.duration = 0.4;
  assert(sweep_duration(&p) == 0.4);
  p.period = 0.3;
  assert(sweep_validate(&p,4000,4.4e6,4.463e6,FE_LO,FE_HI) != NULL);
}

// Phil's original per-block arithmetic (radio.c before this change)
static void phil_block(double rate,double t,double out_samprate,double *swp,double *slope){
  double const samptime = 1.0 / out_samprate;
  *slope = rate * samptime * samptime;
  *swp = t * rate;
}

static void gap_has_no_ramp(void){
  struct sweep_params p = { .rate = -25733.913, .period = 1.0, .duration = 0.6, .start = 0 };
  double swp = 99, cps = 99;
  assert(!sweep_block(&p,0.7,0.02,4000,&swp,&cps));
  assert(cps == 0 && swp == 0);
}

static void active_has_ramp(void){
  struct sweep_params p = { .rate = -25733.913, .period = 1.0, .duration = 0.6, .start = 0 };
  double swp = 99, cps = 99, es, ec;
  assert(sweep_block(&p,0.3,0.02,4000,&swp,&cps));
  phil_block(p.rate,0.3,4000,&es,&ec);
  assert(cps == ec && swp == es && swp == 0.3 * p.rate);
}

static void full_period_matches_phil(void){
  // duration = period (CODAR), and duration unset: every block, bit for bit
  double const rates[] = { -25733.913, -99259.4, 198520.0 };
  double const rs[] = { 4000, 16000, 32000 };
  for(int d = 0; d < 2; d++)
    for(int i = 0; i < 3; i++)
      for(int b = 0; b < 50; b++){
	struct sweep_params p = { .rate = rates[i], .period = i == 1 ? 0.5 : 1.0, .duration = 0, .start = 0 };
	if(d)
	  p.duration = p.period;
	double t = b / 50.0 * p.period, swp, cps, es, ec;
	assert(sweep_block(&p,t,0.02,rs[i],&swp,&cps));
	phil_block(p.rate,t,rs[i],&es,&ec);
	assert(memcmp(&cps,&ec,sizeof ec) == 0 && memcmp(&swp,&es,sizeof es) == 0);
      }
}

static void no_sweep_is_inactive(void){
  struct sweep_params p = { 0, 1.0, 0, 0 }, q = { 100, 0, 0, 0 };
  double swp = 9, cps = 9;
  assert(!sweep_block(&p,0.1,0.02,4000,&swp,&cps) && swp == 0 && cps == 0);
  assert(!sweep_block(&q,0.1,0.02,4000,&swp,&cps) && swp == 0 && cps == 0);
}

// Walk a status/command packet up to EOL; calls f on each TLV. Returns the count
typedef void (*tlv_fn)(int type,uint8_t const *cp,int optlen,void *arg);
static int walk(uint8_t const *pkt,tlv_fn f,void *arg){
  int n = 0;
  for(uint8_t const *cp = pkt; *cp != EOL; ){
    int const type = *cp++;
    int const optlen = *cp++;
    assert(optlen < 128);
    if(f)
      f(type,cp,optlen,arg);
    cp += optlen;
    n++;
  }
  return n;
}

struct decoded { struct sweep_params p; struct sweep_report r; int handled; bool saw_rate; double rate; };
static void decode_one(int type,uint8_t const *cp,int optlen,void *arg){
  struct decoded *d = arg;
  if(decode_sweep_status(&d->p,&d->r,type,cp,optlen))
    d->handled++;
  if(type == SWEEP_RATE){
    d->saw_rate = true;
    d->rate = decode_double(cp,optlen);
  }
}

static void status_round_trip_epoch(void){
  struct sweep_params const cmd = { .rate = -25733.913, .period = 1.0, .duration = 0.6, .start = 0.3 };
  struct sweep_report r = { .now = cmd, .block_index = 7, .on = true, .epoch_sample = 123456789012LL, .epoch_frac = 0.25 };
  uint8_t pkt[256], *bp = pkt;
  encode_sweep_status(&bp,&cmd,&r,true);
  *bp++ = EOL;
  struct decoded d;
  memset(&d,0,sizeof d);
  assert(walk(pkt,decode_one,&d) == 6);
  assert(d.handled == 6);
  assert(d.p.rate == cmd.rate && d.p.period == cmd.period && d.p.duration == cmd.duration && d.p.start == cmd.start);
  assert(d.r.on && d.r.epoch_sample == 123456789012LL && d.r.epoch_frac == 0.25);
  // A later packet without the pair (sweep off) clears on
  r.on = false;
  bp = pkt;
  encode_sweep_status(&bp,&cmd,&r,true);
  *bp++ = EOL;
  assert(walk(pkt,decode_one,&d) == 4);
  assert(!d.r.on);
}

static void status_sends_zero_sweep(void){
  struct sweep_params const cmd = { 0, 0, 0, 0 };
  struct sweep_report const r = { .on = false };
  uint8_t pkt[256], *bp = pkt;
  encode_sweep_status(&bp,&cmd,&r,true);
  *bp++ = EOL;
  struct decoded d;
  memset(&d,0,sizeof d);
  d.rate = 99;
  assert(walk(pkt,decode_one,&d) == 4); // all four, no epoch
  assert(d.saw_rate && d.rate == 0.0);
  assert(!d.r.on);
  // A non-linear channel keeps Phil's rule: zeros are not sent
  bp = pkt;
  encode_sweep_status(&bp,&cmd,&r,false);
  *bp++ = EOL;
  assert(walk(pkt,NULL,NULL) == 0);
}

struct cmdarg { struct sweep_params p; int stored; };
static void command_one(int type,uint8_t const *cp,int optlen,void *arg){
  struct cmdarg *c = arg;
  if(decode_sweep_command(&c->p,type,cp,optlen))
    c->stored++;
}

static void nonlinear_ignores_sweep(void){
  // The command a client sends: all four sweep tags in one packet
  uint8_t pkt[256], *bp = pkt;
  encode_double(&bp,SWEEP_RATE,-25733.913);
  encode_double(&bp,SWEEP_PERIOD,1.0);
  encode_double(&bp,SWEEP_DURATION,1.0);
  encode_double(&bp,SWEEP_START,0.25);
  encode_double(&bp,SWEEP_START,NAN); // ignored, as before
  *bp++ = EOL;

  struct cmdarg fm = { { 0, 0, 0, 0 }, 0 };
  walk(pkt,command_one,&fm);
  assert(fm.stored == 4 && fm.p.rate == -25733.913 && fm.p.start == 0.25);
  // FM channel: ignored, every tune field back to 0, and the ignore is reported once
  assert(sweep_admit(&fm.p,false,24000) != NULL);
  assert(fm.p.rate == 0 && fm.p.period == 0 && fm.p.duration == 0 && fm.p.start == 0);
  assert(sweep_admit(&fm.p,false,24000) == NULL); // a second check logs nothing
  // The status of that channel carries no sweep at all
  struct sweep_report r;
  memset(&r,0,sizeof r);
  sweep_begin_block(&r,&fm.p,123456,0,129.6e6);
  assert(!r.on);
  uint8_t st[256];
  bp = st;
  encode_sweep_status(&bp,&fm.p,&r,false);
  *bp++ = EOL;
  assert(walk(st,NULL,NULL) == 0);

  // The same packet on a linear channel is admitted unchanged
  struct cmdarg lin = { { 0, 0, 0, 0 }, 0 };
  walk(pkt,command_one,&lin);
  assert(sweep_admit(&lin.p,true,4000) == NULL);
  assert(lin.p.rate == -25733.913 && lin.p.period == 1.0 && lin.p.start == 0.25);
  // and an invalid one on a linear channel loses its rate, once
  lin.p.period = 0;
  assert(sweep_admit(&lin.p,true,4000) != NULL && lin.p.rate == 0);
  assert(sweep_admit(&lin.p,true,4000) == NULL);
}

static void start_change_moves_epoch_at_block(void){
  double const sr = 129.6e6;
  uint64_t const L = 2592000; // 20 ms blocks
  struct sweep_params cmd = { .rate = -25733.913, .period = 1.0, .duration = 0, .start = 0 };
  struct sweep_report r;
  memset(&r,0,sizeof r);
  sweep_begin_block(&r,&cmd,5 * L,0,sr); // block 1 at 0.1 s
  assert(r.on && r.block_index == 5 * L && r.epoch_sample == 0 && r.epoch_frac == 0);

  // A client re-aligns between blocks: the command path writes the commanded start
  cmd.start = -0.05;
  // Block 1's report is unchanged, in parameters and in epoch: a status sent now (before block 2)
  // reports the values in force for block 1, never the commanded start beside block 1's epoch.
  // In radiod the status after a command normally goes out after the next block (see radio_status.c)
  assert(r.now.start == 0 && r.epoch_sample == 0);
  uint8_t pkt[256], *bp = pkt;
  encode_sweep_status(&bp,&cmd,&r,true);
  *bp++ = EOL;
  struct decoded d;
  memset(&d,0,sizeof d);
  walk(pkt,decode_one,&d);
  assert(d.p.start == 0);                     // in force for block 1
  assert(d.r.on && d.r.epoch_sample == 0);    // the epoch belongs to block 1

  // Block 2 at 0.12 s takes the new start: t = 0.07 s, so the sweep began at 0.05 s
  sweep_begin_block(&r,&cmd,6 * L,0,sr);
  assert(r.now.start == -0.05 && r.block_index == 6 * L);
  assert(r.epoch_sample == 6480000 && r.epoch_frac < 1e-6);
  // and the block's sweep time agrees with the epoch
  double const t = sweep_time(&r.now,r.block_index,0,sr);
  assert(fabs(t - (double)(6 * L - 6480000) / sr) < 1e-9);
  // Its status carries the new start and the new epoch together
  bp = pkt;
  encode_sweep_status(&bp,&cmd,&r,true);
  *bp++ = EOL;
  memset(&d,0,sizeof d);
  walk(pkt,decode_one,&d);
  assert(d.p.start == -0.05 && d.r.on && d.r.epoch_sample == 6480000);
  // Before any block (new or idle channel) status reports the commanded values
  struct sweep_report fresh;
  memset(&fresh,0,sizeof fresh);
  bp = pkt;
  encode_sweep_status(&bp,&cmd,&fresh,true);
  *bp++ = EOL;
  memset(&d,0,sizeof d);
  walk(pkt,decode_one,&d);
  assert(d.p.start == -0.05 && !d.r.on);
}

// LAN commands with extreme values: refused or folded, never undefined behaviour
// (build once with -fsanitize=undefined,float-cast-overflow -fno-sanitize-recover to check)
static void extreme_commands_refused_or_folded(void){
  double const sr = 129.6e6;
  // start 1e30 on a sane period: admitted, start folded into [0, period)
  struct sweep_params p = { .rate = -25733.913, .period = 1.0, .duration = 0, .start = 1e30 };
  assert(sweep_admit(&p,true,4000) == NULL);
  assert(p.rate != 0 && p.start >= 0 && p.start < p.period);
  p.start = -0.25;
  assert(sweep_admit(&p,true,4000) == NULL && p.start == 0.75);
  // period 1e300 (or anything over 1e6 s): refused
  struct sweep_params q = { .rate = -25733.913, .period = 1e300, .duration = 0, .start = 1e30 };
  assert(sweep_admit(&q,true,4000) != NULL && q.rate == 0);
  q = (struct sweep_params){ .rate = 1, .period = 1e6 + 1, .duration = 0, .start = 0 };
  assert(sweep_admit(&q,true,4000) != NULL && q.rate == 0);
  // NaN and Inf in any field: refused
  double const bad[] = { NAN, INFINITY, -INFINITY };
  for(int i = 0; i < 3; i++)
    for(int f = 0; f < 4; f++){
      struct sweep_params b = { .rate = -25733.913, .period = 1.0, .duration = 0, .start = 0 };
      double *fld[] = { &b.rate, &b.period, &b.duration, &b.start };
      *fld[f] = bad[i];
      assert(sweep_admit(&b,true,4000) != NULL && b.rate == 0);
    }
  // NaN and Inf never get past the command decoder either
  uint8_t pkt[64], *bp = pkt;
  encode_double(&bp,SWEEP_START,INFINITY);
  struct sweep_params c = { 0, 0, 0, 0 };
  assert(!decode_sweep_command(&c,SWEEP_START,pkt + 2,8) && c.start == 0);
  // Defence in depth: sweep_epoch and sweep_begin_block on unadmitted extremes stay defined
  struct sweep_params x[] = {
    { .rate = 1, .period = 1e300, .duration = 0, .start = 1e30 },
    { .rate = 1, .period = 1.0, .duration = 0, .start = 1e30 },
    { .rate = 1, .period = 1e-300, .duration = 0, .start = -1e30 },
    { .rate = 1, .period = NAN, .duration = 0, .start = 0 },
    { .rate = 1, .period = 1.0, .duration = 0, .start = INFINITY },
  };
  for(size_t i = 0; i < sizeof x / sizeof x[0]; i++){
    int64_t es = 1; double ef = 2;
    sweep_epoch(&x[i],(1ULL << 40),0,sr,&es,&ef);
    assert(ef >= 0 && ef < 1);
    struct sweep_report r;
    memset(&r,0,sizeof r);
    sweep_begin_block(&r,&x[i],(1ULL << 40),0,sr);
    assert(r.epoch_frac >= 0 && r.epoch_frac < 1);
  }
}

// radio.c used t = fmod(start + J*Blocktime, period); sweep_time uses the input index J*L.
// They agree within 1e-12 s, modulo the period, over the first 1e5 blocks (2000 s)
static double circ_diff(double a,double b,double period){
  double d = fmod(a - b,period);
  if(d < 0)
    d += period;
  return d > period / 2 ? period - d : d;
}
static void sweep_time_matches_phil_jobnum(void){
  double const sr = 129.6e6;
  uint64_t const L = 2592000;
  double const Blocktime = L / sr;
  double const starts[] = { 0, 0.3, 0.123456789 };
  double const periods[] = { 1.0, 0.5, 0.7 };
  for(int k = 0; k < 3; k++)
    for(uint64_t J = 0; J < 100000; J += 7){
      struct sweep_params p = { .rate = 1000, .period = periods[k], .duration = 0, .start = starts[k] };
      double const t = sweep_time(&p,J * L,0,sr);
      double const phil = fmod(p.start + J * Blocktime,p.period);
      assert(t >= 0 && t < p.period);
      assert(circ_diff(t,phil,p.period) < 1e-12);
    }
  // Boundary: block 50 starts exactly on a sweep start (50 * 20 ms = 1 s). sweep_time says 0, a new sweep;
  // Phil's form may give 0 or a hair under the period. Same instant, modulo the period
  struct sweep_params b = { .rate = 1000, .period = 1.0, .duration = 0, .start = 0 };
  assert(sweep_time(&b,50 * L,0,sr) == 0);
  assert(circ_diff(sweep_time(&b,50 * L,0,sr),fmod(50 * Blocktime,1.0),1.0) < 1e-12);
  // Negative start: Phil's fmod goes negative (-0.03 at block 1); sweep_time folds it to 0.97
  struct sweep_params n = { .rate = 1000, .period = 1.0, .duration = 0, .start = -0.05 };
  double const phil_n = fmod(n.start + 1 * Blocktime,n.period);
  assert(phil_n < 0);
  assert(fabs(sweep_time(&n,L,0,sr) - 0.97) < 1e-12);
  assert(circ_diff(sweep_time(&n,L,0,sr),phil_n,1.0) < 1e-12);
}

// Fix 1: the sweep runs on the ADC index. A block's output holds signal that reached the ADC one channel-filter
// delay, lag = (M-1)/2 input samples, before the block's RTP index; the LO time and SWEEP_EPOCH follow the content
static void epoch_includes_filter_delay(void){
  double const sr = 129.6e6;
  double const lag = (648001 - 1) / 2; // 324000 samples = 2.500 ms on B1
  struct sweep_params p = { .rate = -25733.913, .period = 1.0, .duration = 0, .start = 0 };
  struct sweep_report r;
  memset(&r,0,sizeof r);
  // RTP index 100000 samples past the third sweep start: the content is 224000 samples before it, still in sweep 2
  uint64_t const idx = 3 * (uint64_t)sr + 100000;
  sweep_begin_block(&r,&p,idx,lag,sr);
  assert(r.on && r.lag == lag && r.block_index == idx);
  assert(r.epoch_sample == 2 * (int64_t)sr && r.epoch_frac == 0);
  assert(fabs(r.t - (1.0 - 224000 / sr)) < 1e-12);
  // epoch and t agree on the ADC counter: block_index - lag - epoch = t*samprate
  assert(fabs(((double)idx - lag - (double)r.epoch_sample - r.epoch_frac) - r.t * sr) < 1e-3);
  // Past the filter delay the third sweep has begun, on the ADC counter
  sweep_begin_block(&r,&p,3 * (uint64_t)sr + 324005,lag,sr);
  assert(r.epoch_sample == 3 * (int64_t)sr && r.epoch_frac == 0);
  assert(fabs(r.t - 5 / sr) < 1e-15);
  // The first block: its content predates input index 0. No unsigned wrap: the sweep before 0 started at -sr
  sweep_begin_block(&r,&p,0,lag,sr);
  assert(r.epoch_sample == -(int64_t)sr && r.epoch_frac == 0);
  assert(fabs(r.t - (1.0 - 324000 / sr)) < 1e-12);
  // A fractional start keeps its sub-sample fraction on the ADC counter
  p.start = 0.3;
  sweep_begin_block(&r,&p,100000000,lag,sr);
  assert(fabs((double)r.epoch_sample + r.epoch_frac - 0.7 * sr) < 1e-3); // the sweep began at 0.7 s of ADC time
  assert(fabs(r.t - (0.3 + (100000000 - lag) / sr - 1.0)) < 1e-12);
  // The status packet carries the ADC-counter epoch
  uint8_t pkt[256], *bp = pkt;
  encode_sweep_status(&bp,&p,&r,true);
  *bp++ = EOL;
  struct decoded d;
  memset(&d,0,sizeof d);
  walk(pkt,decode_one,&d);
  assert(d.r.on && d.r.epoch_sample == r.epoch_sample && d.r.epoch_frac == r.epoch_frac);
}

// Fix 2: phase at each sweep start. Model downconvert()'s LO block by block, as radio.c builds it:
// coarse LO swp held for the block (its phase kept continuous by the Renfors terms), fine ramp chirp_per_sample
// across the block, and the wrap step from sweep_wrap_nco() added to the fine NCO phase before the mixdown.
// The NCO is the conjugate of the LO, so an NCO step of +x turns moves the LO by -x.
// The start frequency adds the same whole period to the ideal and to the model, so the model leaves it out.
// Returns the LO phase, less the ideal sawtooth LO, at the first block of each of n new sweeps (turns, in (-0.5,0.5])
static long double wrap_half(long double x){
  x -= floorl(x);
  return x > 0.5L ? x - 1 : x;
}
static void wrap_residuals(double start,double lag,uint64_t J0,bool correct,int n,long double *res,double *tau){
  double const sr = 129.6e6, out_sr = 4000;
  uint64_t const L = 2592000; // 20 ms blocks
  double const bt = L / sr;
  struct sweep_params const p = { .rate = -25733.913, .period = 1.0, .duration = 0, .start = start };
  long double const ideal_period = 0.5L * p.rate * p.period * p.period; // sawtooth LO turns per period, start freq 0
  struct sweep_report r;
  memset(&r,0,sizeof r);
  long double lo = 0; // LO turns since block J0
  double t_prev = -1;
  int k = 0;
  for(uint64_t J = J0; k < n; J++){
    sweep_begin_block(&r,&p,J * L,lag,sr);
    assert(r.on);
    uint64_t const step = sweep_wrap_nco(&r);
    if(J > J0 && r.t < t_prev){
      // first block of a new sweep
      if(correct)
	lo -= (long double)step * 0x1p-64L;
      if(k == 0)
	*tau = r.t;
      res[k] = wrap_half(lo - k * ideal_period); // the ideal advances one whole period between these blocks
      // they all sit tau into their sweep (50 blocks per period exactly)
      assert(fabs(r.t - *tau) < 1e-12);
      k++;
    } else
      assert(step == 0); // no step inside a sweep
    double swp, chirp;
    assert(sweep_block(&r.now,r.t,bt,out_sr,&swp,&chirp));
    lo += (long double)swp * bt + 0.5L * chirp * out_sr * out_sr * bt * bt;
    sweep_end_block(&r);
    t_prev = r.t;
  }
}

static void wrap_phase_repeats_each_sweep(void){
  double const rate = -25733.913;
  long double res[6];
  double tau;
  // B1's start, off a block boundary: tau_o = 19.2182 ms without the filter delay
  // Uncorrected (radiod before the fix) the LO gains rate*period*tau_o = -494.561 turns per sweep, so the
  // dechirped output (signal times the NCO) steps by +494.561 = -0.4393 turns, -2.760 rad: the night1119 measurement
  wrap_residuals(0.5392182466,0,0,false,4,res,&tau);
  assert(fabs(tau - 0.0192182466) < 1e-9);
  for(int k = 1; k < 4; k++){
    double const dechirp_step = -2 * M_PI * (double)wrap_half(res[k] - res[k-1]);
    assert(fabs(dechirp_step - (-2.760)) < 1e-3);
    assert(fabs(dechirp_step - 2 * M_PI * (double)wrap_half(-(long double)rate * tau)) < 1e-6);
  }
  // Corrected: the same phase at every sweep start, within 1e-6 rad. With the filter delay, near 0, near 2^40
  // and near 2^50 (100 days at 129.6 MS/s, where a double sweep time would err by ~1e-9 s, 3e-4 rad)
  double const lags[] = { 0, 324000 };
  uint64_t const J0s[] = { 0, 3, (1ULL << 40) / 2592000 + 7, (1ULL << 50) / 2592000 + 11 };
  double const starts[] = { 0.5392182466, 0.2683514, 0.0123456789 };
  for(int a = 0; a < 2; a++)
    for(int b = 0; b < 4; b++)
      for(int c = 0; c < 3; c++){
	wrap_residuals(starts[c],lags[a],J0s[b],true,6,res,&tau);
	for(int k = 1; k < 6; k++)
	  assert(fabs(2 * M_PI * (double)wrap_half(res[k] - res[0])) < 1e-6);
      }
  // A start on a block boundary needs no step: tau_o = 0
  wrap_residuals(0,0,0,false,4,res,&tau);
  assert(tau == 0);
  for(int k = 1; k < 4; k++)
    assert(fabs(2 * M_PI * (double)wrap_half(res[k] - res[0])) < 1e-6);
}

// The wrap step applies only to a wrap inside one unchanged, full-period sweep
static void wrap_step_only_on_a_clean_wrap(void){
  double const sr = 129.6e6;
  uint64_t const L = 2592000;
  struct sweep_params p = { .rate = -25733.913, .period = 1.0, .duration = 0, .start = 0.5392182466 };
  struct sweep_report r;
  memset(&r,0,sizeof r);
  // find a wrap: block 23 starts at t = 0.5392 + 0.46 = 0.9992, block 24 at 0.0192
  sweep_begin_block(&r,&p,23 * L,0,sr);
  sweep_end_block(&r);
  sweep_begin_block(&r,&p,24 * L,0,sr);
  assert(r.t < 0.02 && sweep_wrap_nco(&r) != 0);
  // the first block ever: nothing mixed before it
  struct sweep_report f;
  memset(&f,0,sizeof f);
  sweep_begin_block(&f,&p,24 * L,0,sr);
  assert(sweep_wrap_nco(&f) == 0);
  // the start changed between the blocks (a client re-aligned): no step
  sweep_begin_block(&r,&p,23 * L,0,sr);
  sweep_end_block(&r);
  struct sweep_params q = p;
  q.start += 1e-4;
  sweep_begin_block(&r,&q,24 * L,0,sr);
  assert(r.t < 0.02 && sweep_wrap_nco(&r) == 0);
  // a sweep with a gap (duration < period) is not corrected here
  p.duration = 0.6;
  sweep_begin_block(&r,&p,23 * L,0,sr);
  sweep_end_block(&r);
  sweep_begin_block(&r,&p,24 * L,0,sr);
  assert(sweep_wrap_nco(&r) == 0);
}

// The channel-level path check_sweep() uses (admit_chan_sweep in radio.h): an admitted sweep is written back,
// so a start of 1e30 from a LAN command never stays in the channel
static chan_t Chan; // large; static and zeroed
static void channel_keeps_folded_start(void){
  memset(&Chan,0,sizeof Chan);
  Chan.demod_type = LINEAR_DEMOD;
  Chan.output.samprate = 4000;
  Chan.tune.sweep_rate = -25733.913;
  Chan.tune.sweep_period = 1.0;
  Chan.tune.sweep_start = 1e30;
  assert(admit_chan_sweep(&Chan) == NULL);
  assert(Chan.tune.sweep_rate == -25733.913 && Chan.tune.sweep_period == 1.0);
  assert(Chan.tune.sweep_start >= 0 && Chan.tune.sweep_start < 1.0);
  Chan.tune.sweep_start = -0.25;
  assert(admit_chan_sweep(&Chan) == NULL && Chan.tune.sweep_start == 0.75);
  // refused: rate zeroed in the channel; non-linear: all four zeroed
  Chan.tune.sweep_period = 1e300;
  assert(admit_chan_sweep(&Chan) != NULL && Chan.tune.sweep_rate == 0);
  assert(admit_chan_sweep(&Chan) == NULL); // logs once
  Chan.demod_type = FM_DEMOD;
  Chan.tune.sweep_rate = 100;
  Chan.tune.sweep_period = 1.0;
  Chan.tune.sweep_start = 1e30;
  assert(admit_chan_sweep(&Chan) != NULL);
  assert(Chan.tune.sweep_rate == 0 && Chan.tune.sweep_period == 0 && Chan.tune.sweep_start == 0);
}

int main(void){
  validate_rejects_zero_period();
  validate_rejects_duration_over_period();
  validate_rejects_steep_ramp();
  validate_rejects_span_outside_coverage();
  validate_accepts_codar_4463();
  time_folds_negative_start();
  active_below_duration();
  epoch_lands_on_sweep_start();
  epoch_brief_exact_and_large_index();
  effective_duration_follows_period();
  gap_has_no_ramp();
  active_has_ramp();
  full_period_matches_phil();
  no_sweep_is_inactive();
  status_round_trip_epoch();
  status_sends_zero_sweep();
  nonlinear_ignores_sweep();
  start_change_moves_epoch_at_block();
  extreme_commands_refused_or_folded();
  sweep_time_matches_phil_jobnum();
  channel_keeps_folded_start();
  epoch_includes_filter_delay();
  wrap_step_only_on_a_clean_wrap();
  wrap_phase_repeats_each_sweep();
  fprintf(stderr,"sweep_test: all tests passed\n");
  return 0;
}
