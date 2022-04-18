#include "iir.h"
// Experimental IIR complex notch filter
struct notchfilter *notch_create(double const f,float const bw){
  struct notchfilter *nf = calloc(1,sizeof(struct notchfilter));
  if(nf == NULL)
    return NULL;

  nf->osc_phase = 1;
  nf->osc_step = cispi(2*f);
  nf->dcstate = 0;
  nf->bw = bw;
  return nf;
}

complex float notch(struct notchfilter * const nf,complex float s){
  if(nf == NULL)
    return NAN;
  s = s * conj(nf->osc_phase) - nf->dcstate; // Spin down and remove DC
  nf->dcstate += nf->bw * s;   // Update smoothed estimate
  s *= nf->osc_phase;          // Spin back up
  nf->osc_phase *= nf->osc_step;
  return s;
}
