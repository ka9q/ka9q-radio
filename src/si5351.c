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
static bool pll_freq_in_range(rational_64 fref, unsigned A, unsigned B, unsigned C);
static void compute_ratio(unsigned A,unsigned B,unsigned C,
                                  unsigned D,unsigned E,unsigned F,
                                  unsigned R,
                                  uint64_t *out_num, uint64_t *out_den);
static int preference_rank(unsigned D,unsigned E,unsigned F, unsigned C);
static si5351_pvals_t pack_abc(uint64_t a, uint64_t b, uint64_t c);


static uint64_t const SI5351_DEN_MAX = 1048575u;

static double ff(rational_64 fref, unsigned A, unsigned B, unsigned C, unsigned D, unsigned E, unsigned F, unsigned R){
  return (double)fref.num / (double)fref.den * (A + (double)B/C) / (R * (D + (double)E/F));
}




// ----------------- main solver -----------------

// Solve for one output: given fref_uhz and desired fout (uHz), return best solution.
// Strategy:
//  1) Reduce r = fout_uhz/fref_uhz to P/Q exactly.
//  2) Enumerate R divider (1..128 power-of-two).
//  3) Try integer MS modes first (4,6,8), then fractional MS (D=8..2048),
//     picking E/F by approximating a target MS value derived from a target PLL freq.
//  4) For each candidate MS, compute required PLL ratio X = r*Y and approximate its fractional part by B/C (C<=1,048,575).
//  5) Score by absolute frequency error + preference rank.

// Spec says output range is 8 kHz to 160 MHz, crystal is 25-27 MHz

bool si5351_solve(rational_64 fref, double fout, si5351_solution_t *best)
{
  if(!best || fref.den == 0 || !isfinite(fout) || fref.num <= 0 || fout <= 0)
    return false;

  uint64_t P,Q;
  double iout;
  if(modf(fout,&iout) == 0.0 && iout <= (double)UINT64_MAX){
    // Frequency is integral, just use directly in rational ratio
    P = fref.den * iout;
    Q = fref.num;
  } else {
    // Compute floating point ratio, then turn into rational fraction
    // I could still get really fancy here and handle typed decimal input frequency values as rational
    // fractions without losing precision in a double, eg, 100.01 = 10001/100
    double const ratio = fout * fref.den / (double)fref.num;
    assert(isfinite(ratio) && ratio > 0);
    int exponent = 0;

    (void)frexp(ratio, &exponent);
    int shft = 53 - exponent;
    shft = shft < 0 ? 0 : shft > 63 ? 63 : shft;
    // Convert to rational
    P = (uint64_t)nearbyint(ldexp(ratio,shft));
    Q = 1ULL << shft;
    fprintf(stderr,"fp ratio  = %'llu / %'llu\n",(unsigned long long)P, (unsigned long long)Q);
  }
  assert(P != 0);
  uint64_t g = gcd_u64(P, Q);
  P /= g;
  Q /= g;
  fprintf(stderr,"reduced ratio  = %'llu / %'llu\n", (unsigned long long)P, (unsigned long long)Q);

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
    for(unsigned i=0;i<3;i++){
      unsigned D = D_ints[i], E = 0, F = 1;
      assert(multisynth_is_legal(D,E,F)); // should always be OK

      // Compute PLL ratio X = fpll / fref = fout/fref * (D*R); since E/F = 0
      // X = (P/Q) * (D*R) = (P*(D*R))/Q
      uint64_t Xn = P * (D*R);
      uint64_t Xd = Q;

      uint64_t A = Xn / Xd;
      uint64_t rem = Xn % Xd;

      // fractional part rem/Xd approximated by B/C
      uint64_t B=0,C=1;
      if(rem != 0)
	best_rational_approx(rem, Xd, SI5351_DEN_MAX, &B, &C);
      if(!pll_is_legal((unsigned)A,(unsigned)B,(unsigned)C)
	 || !pll_freq_in_range(fref,(unsigned)A,(unsigned)B,(unsigned)C))
	continue;

      uint64_t fn, fd;
      compute_ratio((unsigned)A,(unsigned)B,(unsigned)C,D,E,F,R,&fn,&fd);
      // error = |fn/fd - P/Q|
      long double err = fabsl((long double)fn / (long double)fd - (long double)P / (long double)Q);
      int pref = preference_rank(D,E,F,(unsigned)C);
      // Select best: smallest err, then pref
      if(err < best->err_num || (err == best->err_num && pref < best->prefer_rank)){
#if 1
	  double fact = ff(fref,(unsigned)A,(unsigned)B,(unsigned)C,
			   D,(unsigned)E,(unsigned)F,R);
	  fprintf(stderr,"ff = %lf, pref %d, fn %'llu fd %'llu P %'llu Q %'llu err = %Le\n",fact,pref,
		  (unsigned long long)fn, (unsigned long long)fd, (unsigned long long)P, (unsigned long long)Q,
		  err);
#endif
	best->A=(unsigned)A; best->B=(unsigned)B; best->C=(unsigned)C;
	best->D=D; best->E=E; best->F=F;
	best->R=R;
	best->err_num=err;
	best->prefer_rank=pref;
      }
    }
    // ---- 3b) fractional MS: D=8..2048, E/F chosen by approximating target MS ratio
    for(unsigned D=8; D<=2048; D++){
      for(unsigned t=0;t<n_pll_targets;t++){
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
	uint64_t E=0,F=1;

	// Approx frac_num/denom by E/F with F<=DEN_MAX, and require E>0 if D==8
	if(frac_num != 0)
	  best_rational_approx(frac_num, denom, SI5351_DEN_MAX, &E, &F);
	assert(multisynth_is_legal(D,(unsigned)E,(unsigned)F));

	// Now Y = (D*F+E)/F exactly.
	// Compute required PLL ratio X = r * Y = (P/Q)*((D*F+E)/F)
	uint64_t const Yn = D*F + E;
	uint64_t const Yd = F;
	uint64_t const Xn = P * Yn;
	uint64_t Xd = Q * Yd;
	uint64_t const A = Xn / Xd;
	uint64_t rem = Xn % Xd;
	uint64_t B=0,C=1;
	if(rem != 0){
	  uint64_t const g = gcd_u64(rem, Xd);
	  rem /= g; Xd /= g;
	  best_rational_approx(rem, Xd, SI5351_DEN_MAX, &B, &C);
	}
	if(!pll_is_legal((unsigned)A,(unsigned)B,(unsigned)C)
	   || !pll_freq_in_range(fref,(unsigned)A,(unsigned)B,(unsigned)C))
	  continue;

	uint64_t fn, fd;
	compute_ratio((unsigned)A,(unsigned)B,(unsigned)C,
			      D,(unsigned)E,(unsigned)F,R,&fn,&fd);

	long double err = fabsl((long double)fn / (long double)fd - (long double)P / (long double)Q);
	int pref = preference_rank(D,(unsigned)E,(unsigned)F,(unsigned)C);
	if(err < best->err_num || (err == best->err_num && pref < best->prefer_rank)){
#if 1
	  double fact = ff(fref,(unsigned)A,(unsigned)B,(unsigned)C,
			   D,(unsigned)E,(unsigned)F,R);
	  fprintf(stderr,"ff = %lf, pref %d, fn %'llu fd %'llu P %'llu Q %'llu err = %Le\n",fact,pref,
		  (unsigned long long)fn, (unsigned long long)fd, (unsigned long long)P, (unsigned long long)Q,
		  err);
#endif

	  best->A=(unsigned)A; best->B=(unsigned)B; best->C=(unsigned)C;
	  best->D=D; best->E=(unsigned)E; best->F=(unsigned)F;
	  best->R=R;
	  best->err_num=err;
	  best->prefer_rank=pref;
	}
      }
    }
  }
  return best->err_num != (U128)(~(U128)0);
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
    uint64_t a = pp / qq;
    uint64_t r = pp % qq;

    uint64_t num = a * num_m1 + num_m2;
    uint64_t den = a * den_m1 + den_m2;

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
      num = t * num_m1 + num_m2;
      den = t * den_m1 + den_m2;
      // Which is better? This one or num_m1 / den_m1 ?
      long double target = (long double)p / (long double)q;
      long double r1 = (long double)num / (long double)den;
      long double r2 = (long double)num_m1 / (long double)den_m1;
      if(fabsl(r1 - target) < fabsl(r2 - target)){
	*out_num = num;
	*out_den = den;
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

static si5351_pvals_t pack_abc(uint64_t a, uint64_t b, uint64_t c){
  si5351_pvals_t v;
  if(b == 0){
    // integer mode
    v.P1 = (unsigned)(128u*a - 512u);
    v.P2 = 0;
    v.P3 = 1;
    return v;
  }
  // t = floor(128*b/c)
  uint64_t t = (U128)128u * b / c;
  v.P1 = (unsigned)(128u*a + t - 512u);
  v.P2 = (unsigned)((U128)128u*b - (U128)c*t);
  v.P3 = (unsigned)c;
  return v;
}

// ----------------- configuration + scoring -----------------



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

static bool pll_freq_in_range(rational_64 fref, unsigned A, unsigned B, unsigned C){
  double pll = (double)fref.num / (double)fref.den * (A + (double)B/C);
  if(pll >= 600e6 && pll <= 900e6)
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
  uint64_t num = ((uint64_t)A * C + B) * F;
  uint64_t den = C * ((uint64_t)D * F + E) * R;

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

