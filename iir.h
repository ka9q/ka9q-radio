// $Id: iir.h,v 1.2 2021/04/14 01:55:06 karn Exp $
// Various simple IIR filters
#ifndef _IIR_H
#define _IIR_H 1
#include <complex.h>
#include <math.h>
#include <stdlib.h>
#include "misc.h"
#include "iir.h"

// Experimental complex notch filter
struct notchfilter {
  complex double osc_phase; // Phase of local complex mixer
  complex double osc_step;  // mixer phase increment (frequency)
  complex float dcstate;    // Average signal at mixer frequency
  float bw;                 // Relative bandwidth of notch
};


struct notchfilter *notch_create(double,float);
#define notch_delete(x) free(x)
complex float notch(struct notchfilter *,complex float);

#endif
