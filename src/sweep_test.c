// Unit tests for the sweep (chirp) time unit and parameter validation (sweep.c)
// make sweep_test && ./sweep_test ; exits 0 when every test passes
#undef NDEBUG
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include "sweep.h"

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

int main(void){
  validate_rejects_zero_period();
  validate_rejects_duration_over_period();
  validate_rejects_steep_ramp();
  validate_rejects_span_outside_coverage();
  validate_accepts_codar_4463();
  time_folds_negative_start();
  active_below_duration();
  epoch_lands_on_sweep_start();
  fprintf(stderr,"sweep_test: all tests passed\n");
  return 0;
}
