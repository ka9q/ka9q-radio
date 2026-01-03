// General purpose oscillator (complex quadrature and PLL) subroutines for ka9q-radio
// Cpoyright 2022-2023, Phil Karn, KA9Q

#ifndef _OSC_H
#define _OSC_H 1

#define _GNU_SOURCE 1
#include <pthread.h>
#include <complex.h>
#include <math.h>
#include <stdint.h>

struct osc {
  double freq;
  double rate;
  double complex phasor;
  double complex phasor_step;
  double complex phasor_step_step;
  int steps; // Steps since last normalize
};

struct pll {
  double samprate;
  uint32_t vco_phase; // 1 cycle = 2^32
  int32_t vco_step;   // resolution: 1/2^32 cycles
  double integrator_gain;
  double prop_gain;
  double integrator;
  double bw; // loop bandwidth
  double damping; // Damping factor
  double lower_limit; // Lower PLL frequency limit, cycles/sample
  double upper_limit; // Upper PLL frequency limit, cycles/sample
};


// Osc functions -- complex rotator
void set_osc(struct osc *osc,double f,double r);
double complex step_osc(struct osc *osc);

// Osc functions -- direct digital synthesis (sine lookup table)
void nco(uint32_t,double *,double *);


// PLL functions
void init_pll(struct pll *pll,double samprate);
double run_pll(struct pll *pll,double phase);
void set_pll_params(struct pll *pll,double bw,double damping);
void set_pll_limits(struct pll *pll,double low,double high);
static inline double complex pll_phasor(struct pll const *pll){
  double s,c;
  nco(pll->vco_phase,&s,&c);
  return CMPLX(c,s);
}
static inline double pll_freq(struct pll const *pll){
  return (double)pll->vco_step * pll->samprate / (double)(1LL << 32); // Hz
}

#endif

