#ifndef _SI5351_H
#define _SI5351_H

#include <stdint.h>
#include <stdbool.h>

#ifndef U128
#define U128 __uint128_t
#endif

typedef struct {
  // PLL: A + B/C
  unsigned A, B, C;

  // MultiSynth: D + E/F
  unsigned D, E, F;

  // Output R divider (power of two: 1,2,4,...,128)
  unsigned R;

  // Scoring
  long double err_num;
  int prefer_rank; // lower is better
} si5351_solution_t;
// ----------------- Si5351 packing -----------------
// For a+b/c, compute P1,P2,P3 per AN619 style formulas:
// P1 = 128*a + floor(128*b/c) - 512
// P2 = 128*b - c*floor(128*b/c)
// P3 = c
// (Same structure for PLL and MultiSynth fractional dividers.)  (See AN619) :contentReference[oaicite:5]{index=5}
typedef struct {
  unsigned P1, P2, P3;
} si5351_pvals_t;

bool si5351_solve(rational_64 fref, double fout, si5351_solution_t *best);
void si5351_get_pll_pvals(const si5351_solution_t *s, si5351_pvals_t *pll);
void si5351_get_ms_pvals(const si5351_solution_t *s, si5351_pvals_t *ms);

#endif
