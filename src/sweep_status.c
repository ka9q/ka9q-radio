// Status and command TLVs for the sweep (chirp) parameters: encode and decode in one place
// so sweep_test exercises exactly what radiod sends and accepts
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include "status.h"
#include "sweep.h"

void encode_sweep_status(uint8_t **bp,struct sweep_params const *commanded,struct sweep_report const *r,bool const linear){
  // In force for the reported block once one has run; the commanded values before that (a new or idle channel)
  struct sweep_params const *c = r->ran ? &r->now : commanded;
  if(linear){
    // All four, zeros included, so a client can tell "no sweep" from "no sweep support"
    // SWEEP_RATE first: decode_sweep_status() relies on it to clear a stale epoch
    encode_double(bp,SWEEP_RATE,c->rate);         // Hz/s
    encode_double(bp,SWEEP_DURATION,c->duration); // s
    encode_double(bp,SWEEP_PERIOD,c->period);     // s
    encode_double(bp,SWEEP_START,c->start);       // s
    if(r->on){
      encode_int64(bp,SWEEP_EPOCH_SAMPLE,(uint64_t)r->epoch_sample);
      encode_double(bp,SWEEP_EPOCH_FRAC,r->epoch_frac);
    }
    return;
  }
  // Other demods: as before, only what is set
  if(c->rate != 0)
    encode_double(bp,SWEEP_RATE,c->rate);
  if(c->duration != 0)
    encode_double(bp,SWEEP_DURATION,c->duration);
  if(c->period != 0)
    encode_double(bp,SWEEP_PERIOD,c->period);
  if(c->start != 0)
    encode_double(bp,SWEEP_START,c->start);
}

bool decode_sweep_status(struct sweep_params *p,struct sweep_report *r,int const type,uint8_t const *cp,int const optlen){
  switch(type){
  case SWEEP_RATE:
    p->rate = decode_double(cp,optlen);
    r->on = false; // the pair follows when a sweep is in force
    break;
  case SWEEP_DURATION:
    p->duration = decode_double(cp,optlen);
    break;
  case SWEEP_PERIOD:
    p->period = decode_double(cp,optlen);
    break;
  case SWEEP_START:
    p->start = decode_double(cp,optlen);
    break;
  case SWEEP_EPOCH_SAMPLE:
    r->epoch_sample = (int64_t)decode_int64(cp,optlen);
    r->on = true;
    break;
  case SWEEP_EPOCH_FRAC:
    r->epoch_frac = decode_double(cp,optlen);
    break;
  default:
    return false;
  }
  return true;
}

bool decode_sweep_command(struct sweep_params *p,int const type,uint8_t const *cp,int const optlen){
  double *dst;
  switch(type){
  case SWEEP_RATE:     dst = &p->rate; break;     // Hz/s
  case SWEEP_DURATION: dst = &p->duration; break; // s
  case SWEEP_PERIOD:   dst = &p->period; break;   // s
  case SWEEP_START:    dst = &p->start; break;    // s
  default:
    return false;
  }
  double const f = decode_double(cp,optlen);
  if(!isfinite(f))
    return false;
  *dst = f;
  return true;
}
