// Routines for programming the dividers in the ubiquitous Si5351 clock generator chip

#define _GNU_SOURCE 1
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <math.h>
#include "misc.h"
#include "si5351.h"

#ifndef NDEBUG
static bool multisynth_is_legal(unsigned D, unsigned E, unsigned F);
#endif
static void best_rational_approx(uint64_t p, uint64_t q, uint64_t max_den,
                                 uint64_t *out_num, uint64_t *out_den);
static bool pll_is_legal(unsigned A, unsigned B, unsigned C);
static bool pll_freq_in_range(double fref, unsigned A, unsigned B, unsigned C);
static void compute_ratio(unsigned A,unsigned B,unsigned C,
                                  unsigned D,unsigned E,unsigned F,
                                  unsigned R,
                                  uint64_t *out_num, uint64_t *out_den);
static int preference_rank(unsigned D,unsigned E,unsigned F, unsigned C);
static si5351_pvals_t pack_abc(unsigned a, unsigned b, unsigned c);


static uint64_t const SI5351_DEN_MAX = 1048575u;
static unsigned const MIN_VCO = 600000000;
static unsigned const MAX_VCO = 900000000;


static double factual(double fref, unsigned A, unsigned B, unsigned C, unsigned D, unsigned E, unsigned F, unsigned R){
  return fref * (A + (double)B/C) / (R * (D + (double)E/F));
}
// Solve for one output: given fref and desired fout, return best solution.
// Strategy:
//  1) Reduce r = fout/fref to P/Q exactly.
//  2) Enumerate R divider (1..128 power-of-two).
//  3) Try integer MS modes first (4,6,8), then fractional MS (D=8..2048),
//     picking E/F by approximating a target MS value derived from a target PLL freq.
//  4) For each candidate MS, compute required PLL ratio X = r*Y and approximate its fractional part by B/C (C<=1,048,575).
//  5) Score by absolute frequency error + preference rank.
// Spec says output range is 8 kHz to 160 MHz, crystal is 25-27 MHz
bool si5351_solve(double fref, double fout, si5351_solution_t *best){
  if(!best || !isfinite(fref) || fref <= 0 || !isfinite(fout) || fout <= 0)
    return false;
  uint64_t P,Q;
  // Compute floating point ratio, then turn into rational fraction
  double const ratio = fout / fref;
  assert(isfinite(ratio) && ratio > 0);
  int exponent = 0;
  (void)frexp(ratio, &exponent);
  int shft = 53 - exponent;
  shft = shft < 0 ? 0 : shft > 63 ? 63 : shft;
  // Convert to rational
  P = (uint64_t)nearbyint(ldexp(ratio,shft));
  Q = 1ULL << shft;
#if TEST
  fprintf(stderr,"ratio = %'llu / %'llu\n", (unsigned long long)P, (unsigned long long)Q);
#endif
  assert(P != 0);
  uint64_t g = gcd_u64(P, Q);
  if(g != 1){
    P /= g;
    Q /= g;
#if TEST
    fprintf(stderr,"reduced ratio = %'llu / %'llu\n", (unsigned long long)P, (unsigned long long)Q);
#endif
  }
  // Initialize best as "infinite error"
  best->prefer_rank = 255;
  best->err_num = INFINITY;
  // Candidate PLL targets (Hz) to bias MS approximation.
  const double pll_targets[] = { 900e6, 864e6, 800e6, 750e6, 600e6, };
  const unsigned n_pll_targets = sizeof(pll_targets)/sizeof(pll_targets[0]);
  // R divider values: 1,2,4,...,128
  for(unsigned R = 1; R <= 128; R <<= 1){
    // ---- 3a) integer MS first: D in {4,6,8}, E=0,F=1
    const unsigned D_ints[] = {4,6,8};
    for(unsigned i=0; i<3; i++){
      unsigned D = D_ints[i], E = 0, F = 1;
      assert(multisynth_is_legal(D, E, F)); // should always be OK
      // Compute PLL ratio X = fpll / fref = fout/fref * (D*R); since E/F = 0
      // X = (P/Q) * (D*R) = (P*(D*R))/Q
      uint64_t Xn = P * (D*R);
      uint64_t Xd = Q;
      unsigned A = Xn / Xd; // Whole part of PLL divisor
      if(A > 90 || fref * A > MAX_VCO)
	goto done; // Already too high, no point in continuing
      if(A < 15 || fref * (A + 1) < MIN_VCO)
	continue; // Too low
      uint64_t rem = Xn % Xd;
      // fractional part rem/Xd approximated by B/C
      unsigned B=0, C=1;
      if(rem != 0){
	uint64_t b64,c64;
	best_rational_approx(rem, Xd, SI5351_DEN_MAX, &b64, &c64);
	assert(b64 <= SI5351_DEN_MAX);
	assert(c64 <= SI5351_DEN_MAX);
	B = (unsigned)b64;
	C = (unsigned)c64;
      }
      assert(pll_is_legal(A, B, C));
      if(!pll_is_legal(A, B, C) || !pll_freq_in_range(fref, A, B, C))
	continue;
      uint64_t fn, fd;
      compute_ratio(A, B, C, D, E, F, R, &fn, &fd);
      // error = |fn/fd - P/Q|
      long double err = fabsl((long double)fn / (long double)fd - (long double)P / (long double)Q);
      int pref = preference_rank(D, E, F, C);
      // Select best: smallest err, then pref
      if(err < best->err_num || (err == best->err_num && pref < best->prefer_rank)){
#if TEST
	  double fact = factual(fref, A, B, C, D, E, F, R);
	  fprintf(stderr,"factual = %'lf, pref %d, fn %'llu fd %'llu err = %Le\n",
		  fact, pref,
		  (unsigned long long)fn, (unsigned long long)fd,
		  err);
#endif
	best->A=A; best->B=B; best->C=C;
	best->D=D; best->E=E; best->F=F;
	best->R=R;
	best->err_num=err;
	best->prefer_rank=pref;
      }
    }
    // ---- 3b) fractional MS: D=8..2048, E/F chosen by approximating target MS ratio
    for(unsigned D=8; D<=2048; D++){
      for(unsigned t=0; t < n_pll_targets; t++){
	double const fpll_target = pll_targets[t];
	// target multisynth value Ytarget = fpll_target / (fout * R)
	// Ytarget in [D, D+1)
	// Compute fractional part exactly: frac = Ytarget - D
	// frac = fpll_target / (fout*R) - D = (fpll_target - D*fout*R) / (fout*R)
	uint64_t denom = llrint(ldexp(fout * R,20)); // Scale both by 2^20
	uint64_t numer = llrint(ldexp(fpll_target,20));
	// Skip if D band doesn't straddle target
	uint64_t const Dden = D * denom;
	uint64_t const D1den = (D+1) * denom;
	if(numer < Dden || numer >= D1den)
	  continue;
	uint64_t const frac_num = numer - Dden;   // in [0, denom)
	unsigned E=0, F=1;
	// Approx frac_num/denom by E/F with F<=DEN_MAX, and require E>0 if D==8
	if(frac_num != 0){
	  uint64_t e64,f64;
	  best_rational_approx(frac_num, denom, SI5351_DEN_MAX, &e64, &f64);
	  assert(e64 <= SI5351_DEN_MAX);
	  assert(f64 <= SI5351_DEN_MAX);
	  E = (unsigned)e64;
	  F = (unsigned)f64;
	  if(E == 1 && F == 1){
	    // Best fraction is 1/1; carry into whole part of divisor
	    D++; // this is the loop variable, should I modify it?
	    E = 0;
	    F = 1;
	    if(D > 2048)
	      goto done; // unlikely but possible
	  }
	}
	assert(multisynth_is_legal(D, E, F));
	// Now Y = (D*F+E)/F exactly.
	// Compute required PLL ratio X = r * Y = (P/Q)*((D*F+E)/F)
	U128 const Xn = (U128)P * (D * F + E);
	U128 Xd = (U128)Q * F;
	unsigned const A = Xn / Xd;
	if(A > 90 || fref * A > MAX_VCO)
	  goto done; // Already too high, no point in continuing
	if(A < 15 || fref * (A + 1) < MIN_VCO)
	  continue; // Too low
	U128 rem = Xn % Xd;
	unsigned B=0, C=1;
	if(rem != 0){
	  U128 const g = gcd_u128(rem, Xd);
	  rem /= g; Xd /= g;
	  if(rem > UINT64_MAX || Xd > UINT64_MAX)
	    continue; // don't bother
	  uint64_t b64,c64;
	  best_rational_approx((uint64_t)rem, (uint64_t)Xd, SI5351_DEN_MAX, &b64, &c64); // questionable casts
	  assert(b64 <= SI5351_DEN_MAX);
	  assert(c64 <= SI5351_DEN_MAX);
	  B = (unsigned)b64;
	  C = (unsigned)c64;
	}
	assert(pll_is_legal(A, B, C));
	if(!pll_is_legal(A, B, C) || !pll_freq_in_range(fref, A, B, C))
	  continue;
	uint64_t fn, fd;
	compute_ratio(A, B, C, D, E, F, R, &fn, &fd);
	long double err = fabsl((long double)fn / (long double)fd - (long double)P / (long double)Q);
	int pref = preference_rank(D,E,F,C);
	if(err < best->err_num || (err == best->err_num && pref < best->prefer_rank)){
#if TEST
	  double fact = factual(fref, A, B, C, D, E, F, R);
	  fprintf(stderr,"factual = %'lf, pref %d, fn %'llu fd %'llu err = %Le\n",
		  fact, pref,
		  (unsigned long long)fn, (unsigned long long)fd,
		  err);
#endif
	  best->A=A; best->B=B; best->C=C;
	  best->D=D; best->E=E; best->F=F;
	  best->R=R;
	  best->err_num=err;
	  best->prefer_rank=pref;
	}
      }
    }
  }
  done:;
  return isfinite(best->err_num);
}
// Helpers to get packed register parameters:
void si5351_get_pll_pvals(const si5351_solution_t *s, si5351_pvals_t *pll){
  assert(s != NULL);
  assert(pll != NULL);
  *pll = pack_abc(s->A, s->B, s->C);
}
void si5351_get_ms_pvals(const si5351_solution_t *s, si5351_pvals_t *ms){
  assert(s != NULL);
  assert(ms != NULL);
  *ms = pack_abc(s->D, s->E, s->F);
}
// Best rational approximation to p/q with denominator <= max_den.
// Uses continued fractions + semiconvergent step.
static void best_rational_approx(uint64_t p, uint64_t q, uint64_t max_den,
                                 uint64_t *out_num, uint64_t *out_den){
  assert(out_num != NULL && out_den != NULL && max_den >= 1);
  if(q == 0){
    // Invalid
    *out_num = p;
    *out_den = q;
    return;
  }
  if(p == 0){
    *out_num = 0;
    *out_den = 1;
    return;
  }
  {
    // Reduce p and q in case the caller didn't do it
    uint64_t const g = gcd_u64(p,q);
    p /= g;
    q /= g;
  }
  if(q <= max_den){
    // No approximation necessary, we're done
    *out_num = p;
    *out_den = q;
    return;
  }
  // Convergents: num[-2]=0,num[-1]=1 ; den[-2]=1,den[-1]=0
  uint64_t num_m2 = 0, num_m1 = 1; // numerators
  uint64_t den_m2 = 1, den_m1 = 0; // denominators
  uint64_t pp = p, qq = q;
  while(qq != 0){
    uint64_t const a = pp / qq;
    uint64_t const r = pp % qq;
    uint64_t const num = a * num_m1 + num_m2;
    uint64_t const den = a * den_m1 + den_m2;
    if(den > max_den){
      assert(den_m1 != 0); // cannot fail if q > 0 && max_den > 0
      // semiconvergent: den_m2 + t*den_m1 <= max_den
      uint64_t const t = (max_den - den_m2) / den_m1;
      if(t == 0){
	// Return the previous convergent
	*out_num = num_m1;
	*out_den = den_m1;
	return;
      }
      uint64_t const s_num = t * num_m1 + num_m2;
      uint64_t const s_den = t * den_m1 + den_m2;
      // Which is better? This one or num_m1 / den_m1 ?
      long double const target = (long double)p / (long double)q;
      long double const r1 = (long double)s_num / (long double)s_den;
      long double const r2 = (long double)num_m1 / (long double)den_m1;
      if(fabsl(r1 - target) < fabsl(r2 - target)){
	*out_num = s_num;
	*out_den = s_den;
      } else {
	*out_num = num_m1;
	*out_den = den_m1;
      }
      return;
    }
    num_m2 = num_m1;
    num_m1 = num;
    den_m2 = den_m1;
    den_m1 = den;
    pp = qq;
    qq = r;
  }
  // exact convergent fits
  *out_num = num_m1;
  *out_den = den_m1;
}
static si5351_pvals_t pack_abc(unsigned a, unsigned b, unsigned c){
  si5351_pvals_t v;
  if(b == 0){
    // integer mode
    v.P1 = 128u*a - 512u;
    v.P2 = 0;
    v.P3 = 1;
    return v;
  }
  // t = floor(128*b/c)
  uint64_t t = 128u * b / c;
  v.P1 = 128u*a + t - 512u;
  v.P2 = 128u*b - c*t;
  v.P3 = c;
  return v;
}
#ifndef NDEBUG
static bool multisynth_is_legal(unsigned D, unsigned E, unsigned F){
  if(F == 0)
    return false;
  if(E >= F)
    return false;

  // Special integer-only cases: 4,6,8 are allowed.
  if(E == 0 && (D == 4 || D == 6 || D == 8))
    return true;

  // Fractional MS valid range: >= 8 + 1/den ... 2048 (AN619/AN1234) :contentReference[oaicite:6]{index=6}
  // That means:
  // - D must be >= 8
  // - if D == 8 then E must be > 0
  if(D < 8 || D > 2048)
    return false;
  return true;
}
#endif
static bool pll_is_legal(unsigned A, unsigned B, unsigned C){
  // Typical legal A range for PLL multiplier is ~15..90 (app notes) :contentReference[oaicite:7]{index=7}
  if(C == 0 || B >= C || A < 15 || A > 90 || C > SI5351_DEN_MAX)
    return false;
  return true;
}
static bool pll_freq_in_range(double fref, unsigned A, unsigned B, unsigned C){
  double pll = fref * (A + (double)B/C);
  if(pll >= MIN_VCO && pll <= MAX_VCO)
    return true;
  return false;
}
// Compute achieved ratio as rational ratio (num/den)
static void compute_ratio(unsigned A,unsigned B,unsigned C,
			  unsigned D,unsigned E,unsigned F,
			  unsigned R,
			  uint64_t *out_num, uint64_t *out_den){
  assert(out_num != NULL && out_den != NULL);
  // ratio =  ( (A*C+B)/C ) / ( (D*F+E)/F ) / R
  //       = (A * C + B) * F   /    [C * (D * F + E) * R]   (cross-multiplying and separating into num and denom)
  uint64_t const num = ((uint64_t)A * C + B) * F;
  uint64_t const den = C * ((uint64_t)D * F + E) * R;
  // Reduce if possible
  uint64_t const g = gcd_u64(num, den);
  *out_num = num / g;
  *out_den = den / g;
}
// Spur-aware preference rank: 0 best.
static int preference_rank(unsigned D,unsigned E,unsigned F, unsigned C){
  // Prefer integer MS (E==0, D in {4,6,8}) most.
  if(E == 0 && (D == 4 || D == 6 || D == 8))
    return 0;
  // Next: fractional MS but small denominators
  // Penalize larger denominators loosely.
  int r = 1;
  if(F > 1000)
    r++;
  if(F > 10000)
    r++;
  if(C > 1000)
    r++;
  if(C > 10000)
    r++;
  return r;
}
#ifdef TEST
// Unit test of si5351 solver with random reference and output frequencies
#include <locale.h>
static char const *Locale = "en_US.UTF-8";
int main(int argc,char *argv[]){
  si5351_solution_t best;
  char const * const cp = getenv("LANG");
  if(cp != NULL)
    Locale = cp;
  setlocale(LC_ALL,Locale); // Set either the hardwired default or the value of $LANG if it exists
  rand_init();
  for(int i=0; i < 10000000; i++){
    double fref = 25e6 + 2e6 * uniform_rv();
    double fout = 16e6 + (130e6 - 16e6) * uniform_rv();
    fprintf(stderr,"fref %'lf fout_requested %'lf\n",fref, fout);
    bool ok = si5351_solve(fref, fout, &best);
    if(!ok){
      fprintf(stderr,"**FAIL**\n");
      continue;
    }
    fprintf(stderr,"A %u B %u C %u D %u E %u F %u R %u err %Le\n",
	   best.A, best.B, best.C, best.D, best.E, best.F, best.R, best.err_num);
  }
  exit(0);
}
#endif
