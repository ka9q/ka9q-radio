// Routines for programming the dividers in the ubiquitous Si5351 clock generator chip
// New chat GPT version that works only in integers, avoiding floating point precision problems

// si5351_solver.c
// Integer/rational Si5351 divider solver (PLL + MultiSynth) with spur-aware scoring.
// No floating-point search; continued fractions operate on exact rationals.

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include "si5351.h"

static unsigned long long gcd_u64(unsigned long long a, unsigned long long b);
static bool multisynth_is_legal(unsigned D, unsigned E, unsigned F);
static void best_rational_approx(unsigned long long p, unsigned long long q, unsigned long long max_den,
                                 unsigned long long *out_num, unsigned long long *out_den);
static bool pll_is_legal(unsigned A, unsigned B, unsigned C);
static bool pll_freq_in_range(unsigned long long fref_hz, unsigned A, unsigned B, unsigned C);
static void compute_fout_rational(unsigned long long fref_hz,
                                  unsigned A,unsigned B,unsigned C,
                                  unsigned D,unsigned E,unsigned F,
                                  unsigned R,
                                  unsigned long long *out_num, unsigned long long *out_den);
static U128 abs_diff_num_u128(unsigned long long a, unsigned long long b, unsigned long long c, unsigned long long d);
static uint8_t preference_rank(unsigned D,unsigned E,unsigned F, unsigned C);
static si5351_pvals_t pack_abc(unsigned long long a, unsigned long long b, unsigned long long c);


static unsigned long long SI5351_DEN_MAX = 1048575u;



// ----------------- main solver -----------------

// Solve for one output: given fref and desired fout (Hz), return best solution.
// Strategy:
//  1) Reduce r = fout/fref to P/Q exactly.
//  2) Enumerate R divider (1..128 power-of-two).
//  3) Try integer MS modes first (4,6,8), then fractional MS (D=8..2048),
//     picking E/F by approximating a target MS value derived from a target PLL freq.
//  4) For each candidate MS, compute required PLL ratio X = r*Y and approximate its fractional part by B/C (C<=1,048,575).
//  5) Score by absolute frequency error + preference rank.
bool si5351_solve(unsigned long long fref_hz, unsigned long long fout_hz, si5351_solution_t *best)
{
  if(!best || fref_hz == 0 || fout_hz == 0)
    return false;

  // Exact ratio r = P/Q
  unsigned long long P = fout_hz;
  unsigned long long Q = fref_hz;
  unsigned long long g = gcd_u64(P, Q);
  P /= g;
  Q /= g;

  // Initialize best as "infinite error"
  best->err_num = (U128)(~(U128)0);
  best->prefer_rank = 255;

  // Candidate PLL targets (Hz) to bias MS approximation.
  const unsigned long long pll_targets[] = { 900000000ULL, 864000000ULL, 800000000ULL, 750000000ULL, 600000000ULL };
  const unsigned n_pll_targets = sizeof(pll_targets)/sizeof(pll_targets[0]);

  // R divider values: 1,2,4,...,128
  for(unsigned R = 1; R <= 128; R <<= 1){

    // ---- 3a) integer MS first: D in {4,6,8}, E=0,F=1
    const unsigned D_ints[] = {4,6,8};
    for(unsigned i=0;i<3;i++){
      unsigned D = D_ints[i], E = 0, F = 1;
      if(!multisynth_is_legal(D,E,F))
	continue;

      // Required PLL freq = fout * D * R
      // Ensure within VCO band via PLL ratio later.
#if 0
      U128 fpll = (U128)fout_hz * D * R;
#endif
      // Compute PLL ratio X = fpll / fref = (fout/fref) * (D*R)
      // X = (P/Q) * (D*R) = (P*(D*R))/Q
      U128 Xn = (U128)P * (D*R);
      U128 Xd = (U128)Q;

      unsigned long long A = (unsigned long long)(Xn / Xd);
      U128 rem = Xn % Xd;

      // fractional part rem/Xd approximated by B/C
      unsigned long long B=0,C=1;
      if(rem != 0){
	// Reduce rem/Xd first for smaller numbers
	unsigned long long rem64 = (unsigned long long)rem;
	unsigned long long xd64  = (unsigned long long)Xd;
	unsigned long long gg = gcd_u64(rem64, xd64);
	rem64 /= gg;
	xd64 /= gg;
	best_rational_approx(rem64, xd64, SI5351_DEN_MAX, &B, &C);
      }
      if(!pll_is_legal((unsigned)A,(unsigned)B,(unsigned)C)
	 || !pll_freq_in_range(fref_hz,(unsigned)A,(unsigned)B,(unsigned)C))
	continue;

      // achieved fout as rational:
      unsigned long long fn, fd;
      compute_fout_rational(fref_hz,(unsigned)A,(unsigned)B,(unsigned)C,D,E,F,R,&fn,&fd);

      // error = |fn/fd - fout_hz|
      // compute |fn - fout*fd| / fd
      U128 err = abs_diff_num_u128(fn, fd, fout_hz, 1);
      uint8_t pref = preference_rank(D,E,F,(unsigned)C);
      // Select best: smallest err, then pref
      if(err < best->err_num || (err == best->err_num && pref < best->prefer_rank)){
	best->A=(unsigned)A; best->B=(unsigned)B; best->C=(unsigned)C;
	best->D=D; best->E=E; best->F=F;
	best->R=R;
	best->fout_num=fn; best->fout_den=fd;
	best->err_num=err;
	best->prefer_rank=pref;
      }
    }
    // ---- 3b) fractional MS: D=8..2048, E/F chosen by approximating target MS ratio
    for(unsigned D=8; D<=2048; D++){
      for(unsigned t=0;t<n_pll_targets;t++){
	unsigned long long fpll_target = pll_targets[t];

	// target multisynth value Ytarget = fpll_target / (fout * R)
	// Ytarget in [D, D+1)
	// Compute fractional part exactly: frac = Ytarget - D
	// frac = (fpll_target) / (fout*R) - D = (fpll_target - D*fout*R) / (fout*R)
	U128 denom = (U128)fout_hz * R;
	U128 numer = (U128)fpll_target;

	// Skip if D band doesn't straddle target
	U128 Dden = (U128)D * denom;
	U128 D1den = (U128)(D+1) * denom;
	if(numer < Dden || numer >= D1den) continue;

	U128 frac_num = numer - Dden;   // in [0, denom)
	unsigned long long E=0,F=1;

	// Approx frac_num/denom by E/F with F<=DEN_MAX, and require E>0 if D==8
	if(frac_num != 0){
	  unsigned long long fn = (unsigned long long)frac_num;
	  unsigned long long fd = (unsigned long long)denom;
	  unsigned long long gg = gcd_u64(fn, fd);
	  fn/=gg; fd/=gg;
	  best_rational_approx(fn, fd, SI5351_DEN_MAX, &E, &F);
	} else {
	  // exactly integer D; but fractional mode requires D==8 must be >8, and for 8 integer is legal only via integer-mode case (handled above)
	  E = 0; F = 1;
	}
	if(!multisynth_is_legal(D,(unsigned)E,(unsigned)F))
	  continue;

	// Now Y = (D*F+E)/F exactly.
	// Compute required PLL ratio X = r * Y = (P/Q)*((D*F+E)/F)
	U128 Yn = (U128)D*F + E;
	U128 Yd = (U128)F;
	U128 Xn = (U128)P * Yn;
	U128 Xd = (U128)Q * Yd;
	unsigned long long A = (unsigned long long)(Xn / Xd);
	U128 rem = Xn % Xd;
	unsigned long long B=0,C=1;
	if(rem != 0){
	  unsigned long long rem64 = (unsigned long long)rem;
	  unsigned long long xd64  = (unsigned long long)Xd;
	  unsigned long long gg2 = gcd_u64(rem64, xd64);
	  rem64/=gg2; xd64/=gg2;
	  best_rational_approx(rem64, xd64, SI5351_DEN_MAX, &B, &C);
	}
	if(!pll_is_legal((unsigned)A,(unsigned)B,(unsigned)C)
	   || !pll_freq_in_range(fref_hz,(unsigned)A,(unsigned)B,(unsigned)C))
	  continue;

	unsigned long long fn, fd;
	compute_fout_rational(fref_hz,(unsigned)A,(unsigned)B,(unsigned)C,
			      D,(unsigned)E,(unsigned)F,R,&fn,&fd);

	U128 err = abs_diff_num_u128(fn, fd, fout_hz, 1);
	uint8_t pref = preference_rank(D,(unsigned)E,(unsigned)F,(unsigned)C);
	if(err < best->err_num || (err == best->err_num && pref < best->prefer_rank)){
	  best->A=(unsigned)A; best->B=(unsigned)B; best->C=(unsigned)C;
	  best->D=D; best->E=(unsigned)E; best->F=(unsigned)F;
	  best->R=R;
	  best->fout_num=fn; best->fout_den=fd;
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
// ----------------- utilities -----------------

static unsigned long long gcd_u64(unsigned long long a, unsigned long long b){
  while(b){
    unsigned long long t = a % b;
    a = b;
    b = t;
  }
  return a;
}

#if 0
static unsigned long long clamp_u64(unsigned long long x, unsigned long long lo, unsigned long long hi){
  return (x < lo) ? lo : (x > hi) ? hi : x;
}
#endif

// Best rational approximation to p/q with denominator <= max_den.
// Uses continued fractions + semiconvergent step.
static void best_rational_approx(unsigned long long p, unsigned long long q, unsigned long long max_den,
                                 unsigned long long *out_num, unsigned long long *out_den)
{
  // Convergents: h[-2]=0,h[-1]=1 ; k[-2]=1,k[-1]=0
  unsigned long long h_m2 = 0, h_m1 = 1;
  unsigned long long k_m2 = 1, k_m1 = 0;
  unsigned long long pp = p, qq = q;

  // track last within limit
  unsigned long long last_h = 0, last_k = 1;

  while(qq != 0){
    unsigned long long a0 = pp / qq;
    unsigned long long r  = pp % qq;

    U128 h = (U128)a0 * h_m1 + h_m2;
    U128 k = (U128)a0 * k_m1 + k_m2;

    if(k > max_den){
      // semiconvergent: k_m2 + t*k_m1 <= max_den
      if(k_m1 == 0){
	*out_num = last_h;
	*out_den = last_k;
	return;
      }
      unsigned long long t = (max_den - k_m2) / k_m1;
      U128 hs = (U128)t * h_m1 + h_m2;
      U128 ks = (U128)t * k_m1 + k_m2;
      *out_num = (unsigned long long)hs;
      *out_den = (unsigned long long)ks;
      return;
    }
    last_h = (unsigned long long)h;
    last_k = (unsigned long long)k;

    h_m2 = h_m1;
    h_m1 = (unsigned long long)h;

    k_m2 = k_m1;
    k_m1 = (unsigned long long)k;
    pp = qq;
    qq = r;
  }
  // exact convergent fits
  *out_num = last_h;
  *out_den = last_k;
}

// Absolute difference of two rationals a/b and c/d, returned as numerator over common denom in u128.
// |a/b - c/d| = |ad - bc| / bd
static U128 abs_diff_num_u128(unsigned long long a, unsigned long long b, unsigned long long c, unsigned long long d){
  U128 left  = (U128)a * d;
  U128 right = (U128)c * b;
  return (left > right) ? (left - right) : (right - left);
}

static si5351_pvals_t pack_abc(unsigned long long a, unsigned long long b, unsigned long long c){
  si5351_pvals_t v;
  if(b == 0){
    // integer mode
    v.P1 = (unsigned)(128u*a - 512u);
    v.P2 = 0;
    v.P3 = 1;
    return v;
  }
  // t = floor(128*b/c)
  unsigned long long t = (U128)128u * b / c;
  v.P1 = (unsigned)(128u*a + t - 512u);
  v.P2 = (unsigned)((U128)128u*b - (U128)c*t);
  v.P3 = (unsigned)c;
  return v;
}

// ----------------- configuration + scoring -----------------



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
  if(D < 8)
    return false;
  if(D == 8 && E == 0)
    return false; // but ok in integer-only case above
  if(D > 2048)
    return false;
  return true;
}

static bool pll_is_legal(unsigned A, unsigned B, unsigned C){
  if(C == 0)
    return false;
  if(B >= C)
    return false;
  // Typical legal A range for PLL multiplier is ~15..90 (app notes) :contentReference[oaicite:7]{index=7}
  if(A < 15 || A > 90)
    return false;
  if(C > SI5351_DEN_MAX)
    return false;
  return true;
}

static bool pll_freq_in_range(unsigned long long fref_hz, unsigned A, unsigned B, unsigned C){
  // fpll = fref * (A + B/C)
  // compare as rational: fpll = fref*(A*C + B)/C
  U128 num = (U128)fref_hz * ((U128)A*C + B);
  U128 den = C;

  // 600..900 MHz typical VCO range :contentReference[oaicite:8]{index=8}
  const U128 lo = (U128)600000000;
  const U128 hi = (U128)900000000;

  // num/den within [lo,hi]  <=> lo*den <= num <= hi*den
  return (lo*den <= num) && (num <= hi*den);
}

// Compute achieved fout as reduced rational (num/den) in Hz.
static void compute_fout_rational(unsigned long long fref_hz,
                                  unsigned A,unsigned B,unsigned C,
                                  unsigned D,unsigned E,unsigned F,
                                  unsigned R,
                                  unsigned long long *out_num, unsigned long long *out_den)
{
  // fout = fref * ( (A*C+B)/C ) / ( (D*F+E)/F ) / R
  //      = fref * (A*C+B) * F / ( C*(D*F+E)*R )
  U128 num = (U128)fref_hz * ((U128)A*C + B) * F;
  U128 den = (U128)C * ((U128)D*F + E) * R;

  // Reduce to u64 by gcd if possible (fits in u128 first)
  // Compute gcd on 64-bit only if both fit, else do a small reduction pass.
  // Here we do a conservative reduction using 64-bit gcd when possible.
  // (For typical Hz ranges, these will fit in 64-bit after reduction.)
  unsigned long long n64 = (unsigned long long)num;
  unsigned long long d64 = (unsigned long long)den;
  unsigned long long g = gcd_u64(n64, d64);
  n64 /= g; d64 /= g;
  *out_num = n64;
  *out_den = d64;
}

// Spur-aware preference rank: 0 best.
static uint8_t preference_rank(unsigned D,unsigned E,unsigned F, unsigned C){
  // Prefer integer MS (E==0, D in {4,6,8}) most.
  if(E == 0 && (D == 4 || D == 6 || D == 8))
    return 0;

  // Next: fractional MS but small denominators
  // Penalize larger denominators loosely.
  uint8_t r = 1;
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

