// General purpose oscillator (complex quadrature and PLL) subroutines for ka9q-radio
// Cpoyright 2022-2023, Phil Karn, KA9Q

#ifndef _OSC_H
#define _OSC_H 1

#include <pthread.h>
#include <complex.h>
#include <math.h>
#include <stdint.h>
#include "misc.h"

struct osc {
  double freq;
  double rate;
  double complex phasor;
  double complex phasor_step;
  double complex phasor_step_step;
  int steps; // Steps since last normalize
};

// Osc functions -- complex rotator
void set_osc(struct osc *osc,double f,double r);
void renorm_osc(struct osc *osc);
double complex step_osc(struct osc *osc);
// Inline version, doesn't normalize
static inline double complex STEP_OSC(struct osc *osc){
  double complex const r = osc->phasor;
  if(osc->rate != 0)
    osc->phasor_step *= osc->phasor_step_step;

  osc->phasor *= osc->phasor_step;
  return r;
}


#endif

