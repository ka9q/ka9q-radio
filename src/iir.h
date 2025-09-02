// Various simple IIR filters
// Copyright 2022-2023, Phil Karn, KA9Q
#ifndef _IIR_H
#define _IIR_H 1
#define _GNU_SOURCE 1
#include <complex.h>
#include <math.h>
#include <stdlib.h>
#include "misc.h"
#include "iir.h"

// Experimental complex notch filter
struct notchfilter {
  double complex osc_phase; // Phase of local complex mixer
  double complex osc_step;  // mixer phase increment (frequency)
  float complex dcstate;    // Average signal at mixer frequency
  float bw;                 // Relative bandwidth of notch
};


struct notchfilter *notch_create(double,float);
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
  double a[FILT_ORDER+1]; // feedback coefficients (poles)
  double b[FILT_ORDER+1]; // feedforward coefficients (zeroes)
  double w[FILT_ORDER+1]; // filter state
};
double applyIIR(struct iir *,double);
void setIIRnotch(struct iir *,double);
void setIIRlp(struct iir * const iir,double f);
void setIIRdc(struct iir * const iir);

#endif
