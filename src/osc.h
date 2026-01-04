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
  double bw; // loop noise bandwidth (not natural frequency)
  double damping; // Damping factor
  double lower_limit; // Lower PLL frequency limit, cycles/sample
  double upper_limit; // Upper PLL frequency limit, cycles/sample
  double u; // frequency cycles/sample
  double phi; // cycles
  double K1,K2; // gains
  int32_t wraps; // complete phase wraps of NCO
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
// PLL frequency in Hz
static inline double pll_freq(struct pll const *pll){
  return ldexp(((double)pll->vco_step * pll->samprate),-32);
}

// PLL frequency in radians
static inline double pll_phase(struct pll const *pll){
  return ldexp(2 * M_PI * pll->vco_phase,-32);
}
static inline int32_t pll_rotations(struct pll const *pll){
  return pll->wraps;
}

#endif

