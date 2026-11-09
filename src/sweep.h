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
  double duration;  // s, sweep runs for this long after each start; <= period; 0 = the whole period (see sweep_duration)
  double start;     // s, added to the sweep time base
};

// Returns NULL when the sweep is usable, else a short reason
// A rate of 0 means no sweep and is always valid
// lo_hz..hi_hz is the frequency span the channel must see (sweep plus filter edges)
// span_lo_hz..span_hi_hz is what the front end covers; the span check is skipped when either is not finite
char const *sweep_validate(struct sweep_params const *p,double out_samprate,double lo_hz,double hi_hz,double span_lo_hz,double span_hi_hz);

// Effective duration: the stored duration when > 0, else the period
// NOTE: on ka9q/main Phil's downconvert() treats duration 0 as "no sweep". Here 0 means "unset, use the period"
// so that config and TLV paths agree and a preset/section split needs no stored default. Phil decides which meaning stays.
double sweep_duration(struct sweep_params const *p);

// Sweep time in [0,period) at an input sample index
double sweep_time(struct sweep_params const *p,uint64_t input_index,double in_samprate);

// True while the sweep is running, false in the gap before the next start
bool sweep_active(struct sweep_params const *p,double t);

// Most recent sweep start at or before input_index, on the input sample counter:
// epoch_sample + epoch_frac, with 0 <= epoch_frac < 1 sample
void sweep_epoch(struct sweep_params const *p,uint64_t input_index,double in_samprate,int64_t *epoch_sample,double *epoch_frac);

// One block's sweep state, t_block = sweep time (folded, in [0,period)) at the start of the block
// swp_hz = t_block*rate while active, else 0 (coarse LO offset)
// chirp_per_sample = rate/out_samprate^2 in turns/sample while active, else 0 (fine ramp; the caller negates it for the NCO)
// Returns active. No sweep (rate 0 or period <= 0) is inactive with both outputs 0
// blocktime is accepted for the block-granular contract and not used: a block is active or not by its start
bool sweep_block(struct sweep_params const *p,double t_block,double blocktime,double out_samprate,double *swp_hz,double *chirp_per_sample);

// Admit a sweep on a channel: the run-time check for config and command paths alike
// Returns NULL when the sweep is usable or there is none, else a short reason; the caller logs it once
// A sweep on a non-linear demod is ignored: all four fields are zeroed. An invalid sweep has its rate zeroed
// Either way the next call returns NULL, so a caller that logs every non-NULL return logs once
char const *sweep_admit(struct sweep_params *p,bool linear,double out_samprate);

// The sweep in force for one block. The command path writes the channel's tune.sweep_* (the commanded values);
// downconvert() copies them here at the start of each block, on the channel's thread, so a command
// takes effect at the next block start and no block sees half-old, half-new parameters
struct sweep_report {
  struct sweep_params now; // parameters in force for the block
  uint64_t block_index;    // input sample index of the block's first sample
  bool on;                 // a sweep is in force (rate != 0, period > 0); the epoch below is valid
  int64_t epoch_sample;    // most recent sweep start at or before block_index (sweep_epoch)
  double epoch_frac;
};
void sweep_begin_block(struct sweep_report *r,struct sweep_params const *commanded,uint64_t block_index,double in_samprate);

// Status TLVs (sweep_status.c). On a linear channel all four parameters go out, zeros included,
// then SWEEP_EPOCH_SAMPLE/FRAC while a sweep is in force. On other demods only nonzero parameters go out.
void encode_sweep_status(uint8_t **bp,struct sweep_params const *commanded,struct sweep_report const *r,bool linear);
// Client side: returns true when type is a sweep tag. SWEEP_RATE (always first in the group) clears r->on;
// SWEEP_EPOCH_SAMPLE sets it
bool decode_sweep_status(struct sweep_params *p,struct sweep_report *r,int type,uint8_t const *cp,int optlen);
// radiod command side: returns true when type is one of the four sweep tags and the value was stored
// NaN and infinite values are ignored, as before
bool decode_sweep_command(struct sweep_params *p,int type,uint8_t const *cp,int optlen);
#endif
