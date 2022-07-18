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
  complex double phasor;
  complex double phasor_step;
  complex double phasor_step_step;
  int steps; // Steps since last normalize
};

struct pll {
  float samprate;
  uint32_t vco_phase; // 1 cycle = 2^32
  int32_t vco_step;   // resolution: 1/2^32 cycles
  float integrator_gain;
  float prop_gain;
  float integrator;
  float bw; // loop bandwidth
  float damping; // Damping factor
  float lower_limit; // Lower PLL frequency limit, cycles/sample
  float upper_limit; // Upper PLL frequency limit, cycles/sample
};


// Osc functions -- complex rotator
void set_osc(struct osc *osc,double f,double r);
complex double step_osc(struct osc *osc);

// Osc functions -- direct digital synthesis (sine lookup table)
float sine_dds(uint32_t accum);
static inline float cos_dds(uint32_t accum){
  return sine_dds(accum + (uint32_t)0x40000000); // cos(x) = sin(x + 90 deg)
}
static inline complex float comp_dds(uint32_t accum){
  complex float f;
  __imag__ f = sine_dds(accum);
  __real__ f = cos_dds(accum);
  return f;
}


// PLL functions
void init_pll(struct pll *pll,float samprate);
float run_pll(struct pll *pll,float phase);
void set_pll_params(struct pll *pll,float bw,float damping);
void set_pll_limits(struct pll *pll,float low,float high);
static inline complex float pll_phasor(struct pll const *pll){
  return comp_dds(pll->vco_phase);
}
static inline float pll_freq(struct pll const *pll){
  return (float)pll->vco_step * pll->samprate / (float)(1LL << 32); // Hz
}

#endif

