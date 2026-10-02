// General purpose oscillator (complex quadrature and PLL) subroutines for ka9q-radio
// Cpoyright 2022-2023, Phil Karn, KA9Q

#ifndef _OSC_H
#define _OSC_H 1

#include <pthread.h>
#include <complex.h>
#include <math.h>
#include <stdint.h>
#include "misc.h"

// Direct digital synthesizer, 64-bit phase accumulator
// Designed to be easy to vectorize

// Constants for the sine lookup table
#define TAB_BITS (8) // log of table size (256); seems to be sweet spot where Taylor errors meet floating precision
#define TAB_SIZE (1<<TAB_BITS)
#define TAB_MASK (TAB_SIZE-1)
#define FRACT_BITS (32 - TAB_BITS - 2)
#define FRACT_MASK ((1U << FRACT_BITS)-1)

extern float NCO_lookup[TAB_SIZE+1]; // Leave room for == pi/2
extern bool NCO_init;

typedef union {
  float f;
  uint32_t i;
} bits_t;

// 0 .... 0xffffffffffffffff => 0 to 2*pi (360 deg)
static inline float complex nco(uint64_t const a){
  /* QQ          TTTTTTTTTT ffffffffffffffffffff
     00: I     0-90 deg
     01: II   90-180
     10: III 180-270
     11: IV  270-360
  */
  assert(NCO_init);
  uint32_t accum = a >> 32;
  uint32_t const fract = accum & FRACT_MASK;
  unsigned tab = (accum >> FRACT_BITS) & TAB_MASK;
  unsigned quad = accum >> (FRACT_BITS + TAB_BITS);  // 0-3: 0-90, 90-180, 180-270, 270-360
  // Index the lookup table in the proper direction
  tab = (quad & 1) ? TAB_SIZE - tab : tab; // up, down, up, down
  // Approx sine with proper sign
  bits_t sinebits;
  sinebits.f = NCO_lookup[tab];
  sinebits.i ^= (quad & 2) << 30; // negative in quadrants II and IV
  float const sine = sinebits.f;
  // Approx cos with proper sign (derivative of sine)
  tab = TAB_SIZE - tab;   // down, up, down, up
  quad++;
  bits_t cosbits;
  cosbits.f = NCO_lookup[tab];
  cosbits.i ^= (quad & 2) << 30;
  float const cosine = cosbits.f;
  // Use approx cos as slope to interpolate fraction
  float const diff = M_PIf * (float)fract * 0x1p-31f; // divide by 2^31
  float const cdiff = cosine * diff;
  float const sdiff = sine * diff;
  // Interpolate with 2nd order Taylor expansion
  return CMPLXF(cosine - sdiff - 0.5f * cdiff * diff, sine + cdiff - 0.5f * sdiff * diff);
}


struct osc {
  double freq;
  double rate;
  double complex phasor;
  double complex phasor_step;
  double complex phasor_step_step;
  int steps; // Steps since last normalize
};

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

// Osc functions -- direct digital synthesis (sine lookup table)
float complex nco(uint64_t);
void nco_init(void);

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

