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

// Constants for the sine lookup table
#define TAB_BITS (10) // Log of table size; 2^10 = 1024
#define TAB_SIZE (1<<TAB_BITS)
#define FRACT_BITS (32 - TAB_BITS - 2)

// sin(x) from 0 to pi/2 (0-90 deg) **inclusive** in TAB_BITS steps
static double Lookup[TAB_SIZE+1]; // Leave room for == pi/2
static bool Tab_init;

// Initialize sine lookup table
static void nco_init(void){
  for(int i=0; i <= TAB_SIZE; i++)
    Lookup[i] = sin(M_PI * 0.5 * (double)i/TAB_SIZE);

  Tab_init = true;
}

// Direct digital synthesizer, 32-bit phase accumulator
// 0 .... 0xffffffff => 0 to 2*pi (360 deg)
void nco(uint32_t accum,double *s,double *c){
  if(!Tab_init)
    nco_init();

  /* Sign half   tab index  fraction
  // S    H      TTTTTTTTTT ffffffffffffffffffff
  // or
  // QQ          TTTTTTTTTT ffffffffffffffffffff
  //  = quadrant: 00: I  0-90 deg
                  01: II 90-180
                  10: III 180-270
		  11  IV  270-360
  */
  uint32_t const fract = accum & ((1U << FRACT_BITS)-1);
  uint32_t tab = (accum >> FRACT_BITS) & ((1U << TAB_BITS) - 1);
  uint32_t quad = accum >> (FRACT_BITS + TAB_BITS);  // 0-3: 0-90, 90-180, 180-270, 270-360

  // Index the lookup table in the proper direction
  tab = (quad & 1) ? TAB_SIZE - tab : tab; // up, down, up, down
  // Approx sine with proper sign
  double sine = Lookup[tab] * (quad & 2 ? -1 : +1); // +,  +,    -,  -

  // Approx cos with propr sign (derivative of sine)
  double cosine = Lookup[TAB_SIZE - tab] * ((quad + 1) & 2 ? -1 : +1); // +down, -up, -down, +up
  // Use approx cos as slope to interpolate fraction
  double diff = 2 * M_PI * ldexp((double)fract, -32);
  if(s)
    *s = sine + cosine * diff - 0.5 * (sine * diff * diff);

  if(c)
    *c = cosine - sine * diff - 0.5 * (cosine * diff * diff);
}


// Initialize digital phase lock loop with bandwidth, damping factor, initial VCO frequency and sample rate
void init_pll(struct pll *pll,double samprate){
  assert(pll != NULL);
  assert(samprate != 0);

  memset(pll,0,sizeof(*pll));
  pll->samprate = samprate;
  set_pll_limits(pll,-0.5,+0.5);
  set_pll_params(pll,1.0f,M_SQRT1_2); // 1 Hz, 1/sqrt(2) defaults
}

void set_pll_limits(struct pll *pll,double low,double high){
  assert(pll != NULL);
  assert(pll->samprate != 0);
  if(low > high){
    double t = low;
    low = high;
    high = t;
  }
  pll->lower_limit = low / pll->samprate; // fraction of sample rate
  pll->upper_limit = high / pll->samprate;
}


// Set PLL loop bandwidth & damping factor
void set_pll_params(struct pll *pll,double bw,double damping){
  assert(pll != NULL);
  bw = fabs(bw);
  if(bw == 0)
    return; // Can't really handle this
  if(bw == pll->bw && damping == pll->damping) // nothing changed
    return;

  pll->bw = bw;
  bw /= pll->samprate;   // cycles per sample
  pll->damping = damping;
  double const freq = pll->integrator * pll->integrator_gain; // Keep current frequency
  
  double const vcogain = 2 * M_PI;          // 2 pi radians/sample per "volt"
  double const pdgain = 1;                  // phase detector gain "volts" per radian (unity from atan2)
  double const natfreq = bw * 2 * M_PI;     // loop natural frequency in rad/sample
  double const tau1 = vcogain * pdgain / (natfreq * natfreq);
  double const tau2 = 2 * damping / natfreq;

  pll->prop_gain = tau2 / tau1;
  pll->integrator_gain = 1 / tau1;
  pll->integrator = freq * tau1; // To give specified frequency
#if 0
  fprintf(stderr,"init_pll(%p,%lf,%lf,%lf,%lf)\n",pll,bw,damping,freq,samprate);
  fprintf(stderr,"natfreq %lg tau1 %lg tau2 %lg propgain %lg intgain %lg\n",
	  natfreq,tau1,tau2,pll->prop_gain,pll->integrator_gain);
#endif
}



// Step the PLL through one sample, return VCO control voltage
// Return PLL freq in cycles/sample
double run_pll(struct pll *pll,double phase){
  assert(pll != NULL);

  double feedback = pll->integrator_gain * pll->integrator + pll->prop_gain * phase;
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
    fprintf(stderr,"phase %lf integrator %lg feedback %lg pll_freq %lg\n",
	    phase,pll->integrator,feedback,feedback * pll->samprate);
  }
#endif
   
  return feedback;
}

