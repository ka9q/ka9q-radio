// General purpose oscillator (complex quadrature and PLL) subroutines for ka9q-radio
// Cpoyright 2022-2023, Phil Karn, KA9Q

#define _GNU_SOURCE 1
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

static inline void renorm_osc(struct osc *osc){
  if(!is_phasor_init(osc->phasor))
    osc->phasor = 1; // In case we've been stepping an uninitialized osc

  osc->steps = Renorm_rate;
  osc->phasor /= cabs(osc->phasor);

  if(osc->rate != 0){
    assert(is_phasor_init(osc->phasor_step)); // was init by set_osc()
    osc->phasor_step /= cabs(osc->phasor_step);
  }
}

// Step oscillator through one sample, return complex phase
double complex step_osc(struct osc *osc){
  if(--osc->steps <= 0)   // do first, in case osc is not initialized
    renorm_osc(osc);
  double complex const r = osc->phasor;
  if(osc->rate != 0)
    osc->phasor_step *= osc->phasor_step_step;

  osc->phasor *= osc->phasor_step;
  return r;
}

// Sine lookup table

// Constants for the sine lookup table
#define TAB_BITS (10) // Log of table size; 2^10 = 1024
#define TAB_SIZE (1<<TAB_BITS)
#define FRACT_BITS (32 - TAB_BITS - 2)

// sin(x) from 0 to pi/2 (0-90 deg) **inclusive** in TAB_BITS steps
static double Lookup[TAB_SIZE+1]; // Leave room for == pi/2
static atomic_flag NCO_init = ATOMIC_FLAG_INIT;

// Initialize sine lookup table
static void nco_init(void){
  for(int i=0; i <= TAB_SIZE; i++)
    Lookup[i] = sin(M_PI * 0.5 * (double)i/TAB_SIZE);
}

// Direct digital synthesizer, 32-bit phase accumulator
// 0 .... 0xffffffff => 0 to 2*pi (360 deg)
void nco(uint32_t accum,double *s,double *c){
  if(!atomic_flag_test_and_set_explicit(&NCO_init,memory_order_relaxed))
    nco_init();

  /* QQ          TTTTTTTTTT ffffffffffffffffffff
     00: I     0-90 deg
     01: II   90-180
     10: III 180-270
     11: IV  270-360
  */
  uint32_t const fract = accum & ((1U << FRACT_BITS)-1);
  uint32_t tab = (accum >> FRACT_BITS) & ((1U << TAB_BITS) - 1);
  uint32_t quad = accum >> (FRACT_BITS + TAB_BITS);  // 0-3: 0-90, 90-180, 180-270, 270-360

  // Index the lookup table in the proper direction
  tab = (quad & 1) ? TAB_SIZE - tab : tab; // up, down, up, down

  // Approx sine with proper sign
  double sine = (quad & 2) ? -Lookup[tab] : +Lookup[tab]; // +,  +,    -,  -

  // Approx cos with proper sign (derivative of sine)
  tab = TAB_SIZE - tab;
  quad++;
  double cosine = (quad & 2) ? -Lookup[tab] : +Lookup[tab]; // +down, -up, -down, +up

  // Use approx cos as slope to interpolate fraction
  double diff = 2 * M_PI * ldexp((double)fract, -32);
  double cdiff = cosine * diff;
  double sdiff = sine * diff;
  // Interpolate with 2nd order Taylor expansion
  if(s != NULL)
    *s = sine + cdiff - 0.5 * sdiff * diff;

  if(c != NULL)
    *c = cosine - sdiff - 0.5 * cdiff * diff;
}


// Initialize digital phase lock loop with sample rate and some reasonable defaults
void init_pll(struct pll *pll){
  assert(pll != NULL);

  memset(pll,0,sizeof(*pll));
  set_pll_limits(pll, -0.5, +0.5); // absolute upper bound
  set_pll_params(pll, 0.01, M_SQRT1_2); // 0.01 cycles/sample, 1/sqrt(2) defaults
}

// Set NCO frequency limits, cycles per sample
void set_pll_limits(struct pll *pll,double low,double high){
  assert(pll != NULL);
  if(low > high){
    double t = low;
    low = high;
    high = t;
  }
  pll->lower_limit = low;
  pll->upper_limit = high;
}


// Set PLL loop bandwidth & damping factor
void set_pll_params(struct pll *pll,double bw,double damping){
  assert(pll != NULL);
  if(bw == 0 || (bw == pll->bw && damping == pll->damping)) // nothing changed
    return;

  double denom = damping + 1.0/(4.0 * damping);
  double wn = 4.0 * M_PI * fabs(bw)/denom;

  pll->bw = bw; // cycles/sample (< 0.5)
  pll->damping = damping; // dimensionless

  double theta = wn;
  double D = 1.0 + 2.0 * damping * theta + theta * theta;
  pll->K1 = 4.0 * damping * theta/ D;
  pll->K2 = 4.0 * theta * theta / D;
}



// Step the PLL through one sample, return VCO control voltage
// phase error input in cycles
// Return PLL freq in Hz
double run_pll(struct pll *pll,double phase){
  assert(pll != NULL);

  double u_new = pll->u + pll->K2 * phase; // integrated frequency
  double dphi = u_new + pll->K1 * phase; // new vco freq input

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
  pll->vco_step = (int32_t)ldexp(dphi,+32);
  pll->vco_phase += pll->vco_step;

  return pll->u; // cycles per sample
}
