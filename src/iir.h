// Various simple IIR filters
// Copyright 2022-2023, Phil Karn, KA9Q
#ifndef _IIR_H
#define _IIR_H 1
#include <complex.h>
#include <math.h>
#include <stdlib.h>
#include "misc.h"
#include "iir.h"

// Experimental complex notch filter
struct notchfilter {
  float complex osc_phase; // Phase of local complex mixer
  float complex osc_step;  // mixer phase increment (frequency)
  float complex dcstate;    // Average signal at mixer frequency
  float bw;                 // Relative bandwidth of notch
};


struct notchfilter *notch_create(float,float);
#define notch_delete(x) free(x)
float complex notch(struct notchfilter *,float complex);

// Goertzel state
struct goertzel {
  float coeff; // 2 * cos(2*pi*f/fs) = 2 * creal(cf)
  float complex cf; // exp(-j*2*pi*f/fs)
  float s0,s1; // IIR filter state, s0 is the most recent
};


// Initialize goertzel state to fractional frequency f
void init_goertzel(struct goertzel *gp,float f);
static inline void reset_goertzel(struct goertzel *gp){
  gp->s0 = gp->s1 = 0;
}

inline static void update_goertzel(struct goertzel *gp,float x){
  float s0save = gp->s0;
  gp->s0 = x + gp->coeff * gp->s0 - gp->s1;
  gp->s1 = s0save;
}
float complex output_goertzel(struct goertzel *gp);

// IIR filter operating on real data
#define FILT_ORDER 6

// Direct form II IIR data structure (single feedback array)
// There's some confusion in the literature about notation
// I use a[] for the poles (feedback) coefficients, b[] for the zeroes (feed forward)
struct iir {
  int order;
  float a[FILT_ORDER+1]; // feedback coefficients (poles)
  float b[FILT_ORDER+1]; // feedforward coefficients (zeroes)
  float w[FILT_ORDER+1]; // filter state
};
float applyIIR(struct iir *,float);
void setIIRnotch(struct iir *,float);
void setIIRlp(struct iir * const iir,float f);
void setIIRdc(struct iir * const iir);

#endif
