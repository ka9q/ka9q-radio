// Various simple IIR filters
// Copyright 2022-2024, Phil Karn, KA9Q

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
// https://eeweb.engineering.nyu.edu/iselesni/EL6113/matlab_examples/notch_filter_demo/html/notch_filter_demo.html
void setIIRnotch(struct iir * const iir,double rel_freq){
  if(iir == NULL)
    return;

  // Sets positions of poles; closer to 1 increases sharpness. MUST be < 1 for stability
  // .999 gives 3 dB bandwidth of about 8 Hz (+/-4 Hz) at 100 Hz
  // It blocks tones very well, but it's so narrow that it lets through a
  // short burst at the beginning of a transmission since it doesn't
  // block the sidebands created by the turn-on transient.

  // .997 gives a 3 dB bandwidth of +/-11.5 Hz @ 100 Hz and seems to be
  // a good compromise
  double const r = 0.997;
  iir->order = 2;

  iir->b[0] = 1;
  iir->b[1] = -2 * cos(2*M_PI*rel_freq); // Complex zeroes on unit circle
  iir->b[2] = 1;

  iir->a[0] = 1;             // not actually used
  iir->a[1] = iir->b[1] * r; // Complex poles just inside unit circle, same angles as zeroes
  iir->a[2] = r*r;
}
// Simple 4-stage lowpass
// Stevens, The Scientist and Engineer's Guide to Digital Signal Processing, p 326
// Note a[] and b[] are swapped in that reference. Signs on a[] are also flipped
void setIIRlp(struct iir * const iir,double f){
  double x = exp(-14.445 * f);

  iir->order = 4;
  iir->b[0] = pow(1-x,4.);
  iir->a[1] = -4 * x;
  iir->a[2] = 6 * x * x;
  iir->a[3] = -4 * x * x * x;
  iir->a[4] = pow(x,4.);
}


// IIR, direct form II
// https://schaumont.dyn.wpi.edu/ece4703b21/lecture3.html
// Use double precision to minimize instability, we can afford it
double applyIIR(struct iir * const iir,double input){
  memmove(&iir->w[1],&iir->w[0],iir->order*sizeof(iir->w[0]));
  // Feedback part (poles)
  iir->w[0] = input;
  for(int m=1;m <= iir->order; m++)
    iir->w[0] -= iir->a[m] * iir->w[m];

  // Feedforward part (zeroes)
  double output = 0;
  for(int m = 0; m <= iir->order; m++)
    output += iir->b[m] * iir->w[m];

  return output;
}
