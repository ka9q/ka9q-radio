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
// Direct digital synthesizer, 64-bit phase accumulator
// Designed to be easy to vectorize

// sin(x) from 0 to pi/2 (0-90 deg) **inclusive** in TAB_BITS steps
float NCO_lookup[TAB_SIZE+1]; // Leave room for == pi/2

// Initialize sine lookup table
static bool NCO_init;
void nco_init(void){
  for(int i=0; i <= TAB_SIZE; i++)
    NCO_lookup[i] = sinpi(0.5 * (double)i/TAB_SIZE);
  NCO_init = true;
}

// Initialize digital phase lock loop with sample rate and some reasonable defaults
void init_pll(struct pll *pll){
  assert(pll != NULL);
  memset(pll,0,sizeof *pll);
  set_pll_limits(pll, -0.5f, +0.5f); // absolute upper bound
  set_pll_params(pll, 0.01f, M_SQRT1_2f); // 0.01 cycles/sample, 1/sqrt(2) defaults
}
// Set NCO frequency limits, cycles per sample
void set_pll_limits(struct pll *pll,float low,float high){
  assert(pll != NULL);
  if(low > high){
    float t = low;
    low = high;
    high = t;
  }
  pll->lower_limit = low;
  pll->upper_limit = high;
}
// Set PLL loop bandwidth & damping factor
void set_pll_params(struct pll *pll,float bw,float damping){
  assert(pll != NULL);
  if(bw == 0 || (bw == pll->bw && damping == pll->damping)) // nothing changed
    return;
  float const denom = damping + 1.0f / (4.0f * damping);
  float const wn = 4.0f * M_PI * fabsf(bw)/denom;
  pll->bw = bw; // cycles/sample (< 0.5)
  pll->damping = damping; // dimensionless
  float const theta = wn;
  float const D = 1.0f + 2.0f * damping * theta + theta * theta;
  pll->K1 = 4.0f * damping * theta/ D;
  pll->K2 = 4.0f * theta * theta / D;
}
// Step the PLL through one sample, return VCO control voltage
// phase error input in cycles
// Return PLL freq in Hz
float run_pll(struct pll *pll,float phase){
  assert(pll != NULL);
  float u_new = pll->u + pll->K2 * phase; // integrated frequency
  float dphi = u_new + pll->K1 * phase; // new vco freq input
  // Limit maximum VCO frequency
  if(dphi > pll->upper_limit){
    dphi = pll->upper_limit;
    if(phase > 0)
      u_new = pll->u; // freeze
  } else if(dphi < pll->lower_limit){
    dphi = pll->lower_limit;
    if(phase < 0)
      u_new = pll->u;
  }
  pll->u = u_new;
  // count vco phase wraps
  pll->phi += dphi;
  if(pll->phi > 1){
    pll->phi -= 1;
    pll->wraps++;
  } else if(pll->phi < -1){
    pll->phi += 1;
    pll->wraps--;
  }
  pll->vco_step = (int64_t)(dphi * 0x1p+64);
  pll->vco_phase += pll->vco_step;
  return pll->u; // cycles per sample
}
