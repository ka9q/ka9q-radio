
#include "iir.h"
#include <string.h>
#include <assert.h>

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

// Simple 2-pole real IIR notch filter, useful for suppressing FM PL tones

// Set notch frequency
// Note: does not clear filter state
void setIIRnotch(struct iir * const iir,float rel_freq){
  if(iir == NULL)
    return;

  // Sets positions of poles; closer to 1 increases sharpness. MUST be < 1 for stability
  // .999 gives 3 dB bandwidth of about 8 Hz (+/-4 Hz) at 100 Hz
  float const r = 0.999;
	
  iir->a[0] = 1;
  iir->a[1] = -2 * cos(2*M_PI*rel_freq); // Complex zeroes on unit circle
  iir->a[2] = 1;
  
  iir->b[0] = 1;
  iir->b[1] = iir->a[1] * r; // Complex poles just inside unit circle, same angles as zeroes
  iir->b[2] = r*r;
}

// https://eeweb.engineering.nyu.edu/iselesni/EL6113/matlab_examples/notch_filter_demo/html/notch_filter_demo.html
float applyIIRnotch(struct iir * const iir,float v){
  memmove(&iir->w[1],&iir->w[0],FILT_ORDER*sizeof(iir->w[0]));
  // Update w coefficients
  // Feedback part (poles)
  float w0 = v;
  for(int m=1;m <= FILT_ORDER; m++)
    w0 -= iir->b[m] * iir->w[m];
  iir->w[0] = w0;

  // Feedforward part (zeroes)
  float y = 0;
  for(int m = 0; m <= FILT_ORDER; m++)
    y += iir->a[m] * iir->w[m];

  return y;
}

