// Sweep (chirp) time base and parameter checks for radiod channels
// Sweep time on a channel is t = fmod(start + input_index/samprate, period),
// the same quantity downconvert() in radio.c uses to place the coarse LO
#ifndef _SWEEP_H
#define _SWEEP_H 1
#include <stdint.h>
#include <stdbool.h>

struct sweep_params {
  double rate;      // Hz/s, positive = up; 0 = no sweep
  double period;    // s, sweep repeats at this interval; must be > 0 when rate != 0
  double duration;  // s, sweep runs for this long after each start; <= period
  double start;     // s, added to the sweep time base
};

// Returns NULL when the sweep is usable, else a short reason
// A rate of 0 means no sweep and is always valid
// lo_hz..hi_hz is the frequency span the channel must see (sweep plus filter edges)
// span_lo_hz..span_hi_hz is what the front end covers; the span check is skipped when either is not finite
char const *sweep_validate(struct sweep_params const *p,double out_samprate,double lo_hz,double hi_hz,double span_lo_hz,double span_hi_hz);

// Sweep time in [0,period) at an input sample index
double sweep_time(struct sweep_params const *p,uint64_t input_index,double in_samprate);

// True while the sweep is running, false in the gap before the next start
bool sweep_active(struct sweep_params const *p,double t);

// Most recent sweep start at or before input_index, on the input sample counter:
// epoch_sample + epoch_frac, with 0 <= epoch_frac < 1 sample
void sweep_epoch(struct sweep_params const *p,uint64_t input_index,double in_samprate,int64_t *epoch_sample,double *epoch_frac);
#endif
