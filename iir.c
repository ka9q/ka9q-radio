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

// Goertzel filter
// Initialize goertzel state to fractional frequency f
void init_goertzel(struct goertzel *gp,float f){
  reset_goertzel(gp);
  float s,c;
  sincospif(2*f,&s,&c);
  gp->coeff = 2 * c;
  __real__ gp->cf = c; // exp(-j*2*pi*f/fs)
  __imag__ gp->cf = -s;
}

// Produce one sample of filter output
// The overall gain is such that N samples of an on-frequency sinusoid with peak amplitude 1 (2 units peak-to-peak)
// gives an output with a magnitude of N/2
complex float output_goertzel(struct goertzel *gp){
  update_goertzel(gp,0); // Nth sample must be zero
  return gp->s0 - gp->cf * gp->s1;
}
