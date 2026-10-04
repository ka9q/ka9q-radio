// General purpose oscillator (complex quadrature and PLL) subroutines for ka9q-radio
// Cpoyright 2022-2023, Phil Karn, KA9Q

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <assert.h>
#include <math.h>
#include <complex.h>
#include <memory.h>
#include <stdatomic.h>
#include "misc.h"
#include "osc.h"

// Constants for the complex rotator
const int Renorm_rate = 16384; // Renormalize oscillators this often

// Return 1 if complex phasor appears to be initialized, 0 if not
// Uses the heuristic that the amplitude should be close to 1 after initialization.
static inline bool is_phasor_init(const double complex x){
  if(isnan(creal(x)) || isnan(cimag(x)) || cnrm(x) < 0.9)
    return false;
  return true;
}

// Set oscillator frequency and sweep rate
// Units are cycles/sample and cycles/sample^2
void set_osc(struct osc *osc,double f,double r){
  if(!is_phasor_init(osc->phasor)){
    osc->phasor = 1; // Don't jump phase if already initialized
    osc->steps = Renorm_rate;
    osc->freq = 0;
    osc->rate = 0;
    osc->phasor_step = 1;
    osc->phasor_step_step = 1;
  }
  if(f != osc->freq){
    osc->freq = f;
    osc->phasor_step = cispi(2 * osc->freq);
  }
  if(r != osc->rate){
    osc->rate = r;
    osc->phasor_step_step = cispi(2 * osc->rate);
  }
}

void renorm_osc(struct osc *osc){
  if(!is_phasor_init(osc->phasor))
    osc->phasor = 1; // In case we've been stepping an uninitialized osc

  osc->steps = Renorm_rate;
#if 0
  osc->phasor /= cabs(osc->phasor);
#else
  osc->phasor *= 1.5 - 0.5 * cnrm(osc->phasor); // near-unity approximation, avoids sqrt
#endif

  if(osc->rate != 0){
    assert(is_phasor_init(osc->phasor_step)); // was init by set_osc()
#if 0
    osc->phasor_step /= cabs(osc->phasor_step);
#else
    osc->phasor_step *= 1.5 - 0.5 * cnrm(osc->phasor_step); // near-unity approximation, avoids sqrt
#endif
  }
}

// Step oscillator through one sample, return complex phase
// There's also STEP_OSC in osc.h, an inline version that doesn't renormalize
double complex step_osc(struct osc *osc){
  if(--osc->steps <= 0)   // do first, in case osc is not initialized
    renorm_osc(osc);
  double complex const r = osc->phasor;
  if(osc->rate != 0)
    osc->phasor_step *= osc->phasor_step_step;

  osc->phasor *= osc->phasor_step;
  return r;
}
