#include "nco.h"
#include "pll.h"

// Initialize digital phase lock loop with sample rate and some reasonable defaults
void init_pll(struct pll *pll){
  assert(pll != NULL);
  memset(pll,0,sizeof *pll);
  set_pll_limits(pll, -0.5f, +0.5f); // absolute upper bound
  set_pll_params(pll, 0.01f, M_SQRT1_2f); // 0.01 cycles/sample, 1/sqrt(2) defaults
}
// Set PLL frequency limits, cycles per sample
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
