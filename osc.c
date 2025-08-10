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
#include "misc.h"
#include "osc.h"

// Constants for the sine lookup table
#define TABBITS (10) // Log of table size; 2^10 = 1024
#define TABSIZE (1UL<<TABBITS)
#define FRACTBITS (32 - TABBITS - 2)

// Constants for the complex rotator
const int Renorm_rate = 16384; // Renormalize oscillators this often

// Return 1 if complex phasor appears to be initialized, 0 if not
// Uses the heuristic that the amplitude should be close to 1 after initialization.
static bool is_phasor_init(const double complex x){
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

inline static void renorm_osc(struct osc *osc){
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

// sin(x) from 0 to pi/2 (0-90 deg) **inclusive**
static float Lookup[TABSIZE+1]; // Leave room for == pi/2
static bool Tab_init;

// Initialize sine lookup table
static void dds_init(void){
  for(unsigned long i=0; i <= TABSIZE; i++)
    Lookup[i] = sinf(M_PI * 0.5f * (float)i/TABSIZE);

  Tab_init = true;
}


// Direct digital synthesizer, 32-bit phase accumulator
// 0 .... 0xffffffff => 0 to 2*pi (360 deg)
float sine_dds(uint32_t accum){
  if(!Tab_init)
    dds_init();

  // Sign half   tab index  fraction
  // S    H      TTTTTTTTTT ffffffffffffffffffff

  uint32_t const fract = accum & ((1 << FRACTBITS)-1);
  accum >>= FRACTBITS;
  uint32_t tab = accum & ((1 << TABBITS) - 1);
  accum >>= TABBITS;
  bool const half = accum & 1;
  accum >>= 1;
  bool const sign = accum & 1;

  int next = +1;
  if(half){
    tab = TABSIZE - tab;
    next = -1;
  }

  assert(tab <= TABSIZE && tab + next <= TABSIZE);
  float f = Lookup[tab];
  float const f1 = Lookup[tab+next];

  float const fscale = 1.0f / (1 << FRACTBITS);

  f += (f1 - f) * (float)fract * fscale;
  return sign ? -f : f;
}


// Initialize digital phase lock loop with bandwidth, damping factor, initial VCO frequency and sample rate
void init_pll(struct pll *pll,float samprate){
  assert(pll != NULL);
  assert(samprate != 0);

  memset(pll,0,sizeof(*pll));
  pll->samprate = samprate;
  set_pll_limits(pll,-0.5,+0.5);
  set_pll_params(pll,1.0f,M_SQRT1_2); // 1 Hz, 1/sqrt(2) defaults
}

void set_pll_limits(struct pll *pll,float low,float high){
  assert(pll != NULL);
  assert(pll->samprate != 0);
  if(low > high){
    float t = low;
    low = high;
    high = t;
  }
  pll->lower_limit = low / pll->samprate; // fraction of sample rate
  pll->upper_limit = high / pll->samprate;
}


// Set PLL loop bandwidth & damping factor
void set_pll_params(struct pll *pll,float bw,float damping){
  assert(pll != NULL);
  bw = fabsf(bw);
  if(bw == 0)
    return; // Can't really handle this
  if(bw == pll->bw && damping == pll->damping) // nothing changed
    return;

  pll->bw = bw;
  bw /= pll->samprate;   // cycles per sample
  pll->damping = damping;
  float const freq = pll->integrator * pll->integrator_gain; // Keep current frequency
  
  float const vcogain = 2 * M_PI;          // 2 pi radians/sample per "volt"
  float const pdgain = 1;                  // phase detector gain "volts" per radian (unity from atan2)
  float const natfreq = bw * 2 * M_PI;     // loop natural frequency in rad/sample
  float const tau1 = vcogain * pdgain / (natfreq * natfreq);
  float const tau2 = 2 * damping / natfreq;

  pll->prop_gain = tau2 / tau1;
  pll->integrator_gain = 1 / tau1;
  pll->integrator = freq * tau1; // To give specified frequency
#if 0
  fprintf(stderr,"init_pll(%p,%f,%f,%f,%f)\n",pll,bw,damping,freq,samprate);
  fprintf(stderr,"natfreq %lg tau1 %lg tau2 %lg propgain %lg intgain %lg\n",
	  natfreq,tau1,tau2,pll->prop_gain,pll->integrator_gain);
#endif
}



// Step the PLL through one sample, return VCO control voltage
// Return PLL freq in cycles/sample
float run_pll(struct pll *pll,float phase){
  assert(pll != NULL);

  float feedback = pll->integrator_gain * pll->integrator + pll->prop_gain * phase;
  pll->integrator += phase;
  
  if(pll->integrator_gain * pll->integrator > pll->upper_limit){
    pll->integrator = pll->upper_limit / pll->integrator_gain;
  } else if(pll->integrator_gain * pll->integrator < pll->lower_limit){
    pll->integrator = pll->lower_limit / pll->integrator_gain;
  }
  pll->vco_step = (int32_t)(feedback * (float)(1LL<<32));
  pll->vco_phase += pll->vco_step;
#if 0
  if((random() & 0xffff) == 0){
    fprintf(stderr,"phase %f integrator %g feedback %g pll_freq %g\n",
	    phase,pll->integrator,feedback,feedback * pll->samprate);
  }
#endif
   
  return feedback;
}

