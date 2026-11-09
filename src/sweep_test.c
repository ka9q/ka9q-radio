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
  double const t = sweep_time(&p,0,129.6e6);
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
  sweep_epoch(&p,3 * (uint64_t)sr + 17,sr,&es,&ef);
  assert(es == 3 * (int64_t)sr);
  assert(ef == 0.0);

  p.start = 0.3;
  uint64_t const idx = 100000000; // t = 0.3 + 0.7716 -> 0.0716 into the sweep
  sweep_epoch(&p,idx,sr,&es,&ef);
  assert(es <= (int64_t)idx);
  assert(ef >= 0 && ef < 1);
  assert(fabs((double)es + ef - 0.7 * sr) < 1e-3);
}

static void epoch_brief_exact_and_large_index(void){
  double const sr = 129.6e6;
  struct sweep_params p = { .rate = -25733.913, .period = 1.0, .duration = 1.0, .start = 0.3 };
  int64_t es; double ef;
  uint64_t const idx = 90720000ULL; // 0.7*129.6e6: the sample where the 0.3 s start lands a sweep
  sweep_epoch(&p,idx,sr,&es,&ef);
  assert(es == (int64_t)idx);
  assert(ef < 1e-3 || ef > 1 - 1e-3);
  assert(sweep_time(&p,idx,sr) < 1e-9); // sweep_time agrees: a sweep starts here
  // one sample earlier is the end of the previous sweep
  sweep_epoch(&p,idx - 1,sr,&es,&ef);
  assert(es == (int64_t)idx - 129600000);
  assert(sweep_time(&p,idx - 1,sr) > 0.99);

  p.start = 0;
  uint64_t const big = (1ULL << 40) + 12345;
  sweep_epoch(&p,big,sr,&es,&ef);
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
  assert(sweep_active(&p,sweep_time(&p,0,129.6e6)));
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
  sweep_begin_block(&r,&fm.p,123456,129.6e6);
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
  sweep_begin_block(&r,&cmd,5 * L,sr); // block 1 at 0.1 s
  assert(r.on && r.block_index == 5 * L && r.epoch_sample == 0 && r.epoch_frac == 0);

  // A client re-aligns between blocks: the command path writes the commanded start
  cmd.start = -0.05;
  // Block 1's report is unchanged, in parameters and in epoch
  assert(r.now.start == 0 && r.epoch_sample == 0);
  uint8_t pkt[256], *bp = pkt;
  encode_sweep_status(&bp,&cmd,&r,true);
  *bp++ = EOL;
  struct decoded d;
  memset(&d,0,sizeof d);
  walk(pkt,decode_one,&d);
  assert(d.p.start == -0.05);                 // the echo carries the commanded value
  assert(d.r.on && d.r.epoch_sample == 0);    // the epoch belongs to block 1

  // Block 2 at 0.12 s takes the new start: t = 0.07 s, so the sweep began at 0.05 s
  sweep_begin_block(&r,&cmd,6 * L,sr);
  assert(r.now.start == -0.05 && r.block_index == 6 * L);
  assert(r.epoch_sample == 6480000 && r.epoch_frac < 1e-6);
  // and the block's sweep time agrees with the epoch
  double const t = sweep_time(&r.now,r.block_index,sr);
  assert(fabs(t - (double)(6 * L - 6480000) / sr) < 1e-9);
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
  fprintf(stderr,"sweep_test: all tests passed\n");
  return 0;
}
