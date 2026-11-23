// Reed-Solomon codes over GF(64) with errors and erasures, the P25 codes among them,
// and the BCH(63,16,23) code of the P25 NID decoded as a binary subcode of one
// Copyright 2026, Matthew Kaufman, KA6SQG
#ifndef _RS63_H
#define _RS63_H 1

#include <stdbool.h>
#include <stdint.h>

// Conventions, unless stated otherwise:
// Bits are one per uint8_t (0 or 1), in transmitted order, msb of each field first.
// A decoder corrects in place and returns the number of bits (or symbols) it changed,
// 0 for a clean word, or -1 if the word is uncorrectable, in which case it is left as received.

// Reed-Solomon over GF(64), primitive polynomial x^6 + x + 1, roots alpha^1 .. alpha^parity.
// sym[i] (0-63) is the coefficient of x^i; all P25 codes are (63,k) codes shortened by
// zeroing the top positions, and the caller passes the whole 63-symbol word with them zero.
// parity: n - k (1-28). length: the shortened length (63 for none); a correction at
// position >= length is a decoding failure. erasures: positions (0-62) known to be
// unreliable, nerasures of them (erasures may be NULL when nerasures is 0); distinct.
// Corrects e errors and f erasures when 2e + f <= parity.
// Returns the number of symbols changed, or -1 (sym untouched). A "correction" that does
// not produce a codeword of the (shortened) code is rejected as uncorrectable.
int rs63_decode(uint8_t sym[63],int parity,int length,int const *erasures,int nerasures);
bool rs63_is_codeword(uint8_t const sym[63],int parity);

// The P25 codes. Phase 1 places parity at positions 0 .. parity-1 and data above it
// (the transmitted hexbits in reverse order); Phase 2 MAC and ESS likewise
static inline int rs_24_12_13_decode(uint8_t sym[63],int const *erasures,int nerasures){ // LDU1, TDULC LC
  return rs63_decode(sym,12,24,erasures,nerasures);
}
static inline int rs_24_16_9_decode(uint8_t sym[63],int const *erasures,int nerasures){ // LDU2 ESS
  return rs63_decode(sym,8,24,erasures,nerasures);
}
static inline int rs_36_20_17_decode(uint8_t sym[63],int const *erasures,int nerasures){ // HDU
  return rs63_decode(sym,16,36,erasures,nerasures);
}
static inline int rs_63_35_29_decode(uint8_t sym[63],int const *erasures,int nerasures){ // Phase 2 FACCH/SACCH
  return rs63_decode(sym,28,63,erasures,nerasures);
}
static inline int rs_44_16_29_decode(uint8_t sym[63],int const *erasures,int nerasures){ // Phase 2 ESS
  return rs63_decode(sym,28,44,erasures,nerasures);
}

// P25 NID: BCH(63,16,23), decoded as the binary subcode of RS(63,41) over GF(64)
// bits[k], k = 0..62, in transmitted order: bits 0-11 NAC, 12-15 DUID, 16-62 parity
// (bit k is the coefficient of x^(62-k)); the NID's 64th (parity) bit is not part of the code.
// erasures: bit indices (0-62, entries outside are ignored) known to be unreliable.
// Accepts only a BCH codeword with 2 * errors + erasures <= 22.
// Returns the number of bits changed (erased bits included), or -1 (bits untouched).
int bch_63_16_decode(uint8_t bits[63],int const *erasures,int nerasures);
// Soft NID decode: errors-only first; if that fails, erase the one ambiguous bit of each dibit
// whose confidence is below threshold (weakest first, at most 22) and decode again.
// Near a quadrant boundary only one bit of a dibit is in doubt: the lsb near +-90 degrees
// (00|01 and 10|11), the msb near 0 / 180 degrees (00|10 and 01|11).
// confidence[j], lsb_ambiguous[j]: NID dibit j (bits 2j, 2j+1), j = 0..31. Same return as above.
int bch_63_16_decode_soft(uint8_t bits[63],float const confidence[32],bool const lsb_ambiguous[32],float threshold);
#define BCH_NID_ERASURE_THRESHOLD 0.3f

#endif
