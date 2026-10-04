#ifndef _PLL_H
#define _PLL_H 1
#include <stdint.h>
#include <complex.h>
#include "nco.h"

struct pll {
  uint64_t vco_phase; // 1 cycle = 2^64
  int64_t vco_step;   // resolution: 1/2^64 cycles
  float bw; // loop noise bandwidth (not natural frequency), cycles/sample
  float damping; // Damping factor
  float lower_limit; // Lower PLL frequency limit, cycles/sample
  float upper_limit; // Upper PLL frequency limit, cycles/sample
  float u; // frequency cycles/sample
  float phi; // cycles
  float K1,K2; // gains
  int64_t wraps; // complete phase wraps of NCO
};

// PLL functions
void init_pll(struct pll *pll);
float run_pll(struct pll *pll,float phase);
void set_pll_params(struct pll *pll,float bw,float damping); // bw in cycles/sample
void set_pll_limits(struct pll *pll,float low,float high); // low, high in cycles/sample
static inline float complex pll_phasor(struct pll const *pll){
  return nco(pll->vco_phase);
}
// PLL frequency in Hz
static inline double pll_freq(struct pll const *pll){
  return (double)pll->vco_step * 0x1p-64;
}

// PLL phase in radians
static inline double pll_phase(struct pll const *pll){
  return M_PI * pll->vco_phase * 0x1p-63;
}
static inline int64_t pll_rotations(struct pll const *pll){
  return pll->wraps;
}
#endif
