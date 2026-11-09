// Sweep (chirp) time base and parameter checks for radiod channels
#include <stddef.h>
#include <math.h>
#include <stdint.h>
#include <stdbool.h>
#include "sweep.h"

char const *sweep_validate(struct sweep_params const *p,double const out_samprate,double const lo_hz,double const hi_hz,double const span_lo_hz,double const span_hi_hz){
  if(p->rate == 0)
    return NULL; // no sweep
  if(!isfinite(p->rate) || !isfinite(p->period) || !isfinite(p->duration) || !isfinite(p->start))
    return "sweep parameter not finite";
  if(p->period <= 0)
    return "sweep-period must be > 0";
  if(p->duration < 0 || p->duration > p->period)
    return "sweep-duration must be between 0 and sweep-period";
  if(!(out_samprate > 0))
    return "no output sample rate";
  double const T = 1.0 / out_samprate;
  if(fabs(p->rate * T * T) >= 0.5)
    return "sweep rate too steep for the output sample rate";
  if(isfinite(span_lo_hz) && isfinite(span_hi_hz)){
    if(!isfinite(lo_hz) || !isfinite(hi_hz) || lo_hz < span_lo_hz || hi_hz > span_hi_hz)
      return "sweep span outside front end coverage";
  }
  return NULL;
}

double sweep_time(struct sweep_params const *p,uint64_t const input_index,double const in_samprate){
  double t = fmod(p->start + input_index / in_samprate,p->period);
  if(t < 0)
    t += p->period;
  if(t >= p->period)
    t = 0; // t was a tiny negative that rounded up to period
  return t;
}

bool sweep_active(struct sweep_params const *p,double const t){
  return t < p->duration;
}

// Work in long double and in samples so the 129.6 MS/s counter keeps its sub-sample fraction
void sweep_epoch(struct sweep_params const *p,uint64_t const input_index,double const in_samprate,int64_t *epoch_sample,double *epoch_frac){
  long double const period_samples = (long double)p->period * in_samprate;
  long double const start_samples = (long double)p->start * in_samprate;
  // Sweep k starts where start + e/samprate = k*period, i.e. e = k*period_samples - start_samples
  long double const k = floorl(((long double)input_index + start_samples) / period_samples);
  long double const e = k * period_samples - start_samples;
  long double const whole = floorl(e);
  *epoch_sample = (int64_t)whole;
  *epoch_frac = (double)(e - whole);
}
