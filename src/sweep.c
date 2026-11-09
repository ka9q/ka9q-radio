// Sweep (chirp) time base and parameter checks for radiod channels
#include <stddef.h>
#include <math.h>
#include <stdint.h>
#include <stdbool.h>
#include <float.h>
#include "sweep.h"

char const *sweep_validate(struct sweep_params const *p,double const out_samprate,double const lo_hz,double const hi_hz,double const span_lo_hz,double const span_hi_hz){
  if(p->rate == 0)
    return NULL; // no sweep
  if(!isfinite(p->rate) || !isfinite(p->period) || !isfinite(p->duration) || !isfinite(p->start))
    return "sweep parameter not finite";
  if(p->period <= 0)
    return "sweep-period must be > 0";
  if(p->period > SWEEP_MAX_PERIOD)
    return "sweep-period too long";
  if(p->duration < 0 || sweep_duration(p) > p->period)
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

double sweep_duration(struct sweep_params const *p){
  return p->duration > 0 ? p->duration : p->period;
}

// Time is a double of seconds: at input index 2^40 and 129.6 MS/s (about 2.5 hours) its resolution is ~1e-9 s,
// a fraction of a sample, and it coarsens as the counter grows. A time within 1e-6 sample below the period snaps to 0
// so that sweep_time and sweep_epoch agree on the sample where a sweep starts.
double sweep_time(struct sweep_params const *p,uint64_t const input_index,double const in_samprate){
  double t = fmod(p->start + input_index / in_samprate,p->period);
  if(t < 0)
    t += p->period;
  if(t >= p->period || p->period - t < 1e-6 / in_samprate)
    t = 0; // a tiny negative rounded up to period, or a start landing within rounding of this sample
  return t;
}

bool sweep_active(struct sweep_params const *p,double const t){
  return t < sweep_duration(p);
}

// Work in long double and in samples so the 129.6 MS/s counter keeps its sub-sample fraction.
// This relies on long double being wider than double: x86-64 (64-bit mantissa) and aarch64 (113 bits) are fine;
// where LDBL_MANT_DIG == 53 (armv7) the sub-sample fraction is lost at large indices (beyond about 2^53/period_samples sweeps).
#if LDBL_MANT_DIG < 64
#warning long double is not wider than double: sweep_epoch loses sub-sample precision at large input indices
#endif
void sweep_epoch(struct sweep_params const *p,uint64_t const input_index,double const in_samprate,int64_t *epoch_sample,double *epoch_frac){
  *epoch_sample = 0;
  *epoch_frac = 0;
  long double const period_samples = (long double)p->period * in_samprate;
  long double const start_samples = (long double)p->start * in_samprate;
  if(!isfinite(period_samples) || !isfinite(start_samples) || !(period_samples > 0))
    return; // never reached after sweep_admit; defence for an unchecked caller
  // Sweep k starts where start + e/samprate = k*period, i.e. e = k*period_samples - start_samples
  long double const x = (long double)input_index + start_samples;
  long double k = floorl(x / period_samples);
  // The next sweep starts within 1e-6 sample ahead: count it as this sample's start (agrees with sweep_time)
  if((k + 1) * period_samples - x < 1e-6L)
    k += 1;
  long double const e = k * period_samples - start_samples;
  // Defence in depth: clamp before the int64 conversion (out of range is undefined behaviour)
  // Admitted parameters reach it only past input index 2^62, over 1000 years at 129.6 MS/s
  long double const lim = 0x1p62L;
  if(!isfinite(e) || e > lim || e < -lim)
    return;
  long double const whole = floorl(e);
  *epoch_sample = (int64_t)whole;
  *epoch_frac = (double)(e - whole);
  if(*epoch_frac >= 1){
    *epoch_sample += 1; // e - whole just under 1 rounded up to 1.0 in the double conversion
    *epoch_frac = 0;
  }
}

bool sweep_block(struct sweep_params const *p,double const t_block,double const blocktime,double const out_samprate,double *swp_hz,double *chirp_per_sample){
  (void)blocktime;
  *swp_hz = 0;
  *chirp_per_sample = 0;
  if(p->rate == 0 || !(p->period > 0) || !(out_samprate > 0) || !sweep_active(p,t_block))
    return false;
  double const samptime = 1.0 / out_samprate;
  *swp_hz = t_block * p->rate;
  *chirp_per_sample = p->rate * samptime * samptime; // turns/sample
  return true;
}

char const *sweep_admit(struct sweep_params *p,bool const linear,double const out_samprate){
  if(p->rate == 0)
    return NULL; // no sweep, nothing to say
  if(!linear){
    // downconvert() also serves fm, wfm and spectrum; a swept LO there is never wanted
    p->rate = p->period = p->duration = p->start = 0;
    return "sweep ignored on a non-linear demodulator";
  }
  // The span is checked in downconvert(), where the front end's coverage is known
  char const *why = sweep_validate(p,out_samprate,NAN,NAN,NAN,NAN);
  if(why != NULL){
    p->rate = 0;
    return why;
  }
  // Fold start into [0,period): same sweep, bounded value (a LAN command may send 1e30)
  double s = fmod(p->start,p->period);
  if(s < 0)
    s += p->period;
  if(!(s >= 0 && s < p->period))
    s = 0; // -tiny + period rounded up to period
  p->start = s;
  return NULL;
}

void sweep_begin_block(struct sweep_report *r,struct sweep_params const *commanded,uint64_t const block_index,double const in_samprate){
  r->now = *commanded;
  r->block_index = block_index;
  r->ran = true;
  r->on = r->now.rate != 0 && r->now.period > 0 && in_samprate > 0;
  if(r->on)
    sweep_epoch(&r->now,block_index,in_samprate,&r->epoch_sample,&r->epoch_frac);
  else {
    r->epoch_sample = 0;
    r->epoch_frac = 0;
  }
}
