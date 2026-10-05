// Numerically controlled oscillator (aka Direct Dignal Synthesizer) for ka9q-radio
// 64-bit phase accumulator
// Designed to be easy to vectorize
// Copyright 2026, Phil Karn, KA9Q

#ifndef _NCO_H
#define _NCO_H 1
#include <pthread.h>
#include <complex.h>
#include <math.h>
#include <stdint.h>
#include "misc.h"

// Constants for the sine lookup table
#define TAB_BITS (8) // log of table size (256); seems to be sweet spot where Taylor errors meet floating precision
#define TAB_SIZE (1<<TAB_BITS)
#define TAB_MASK (TAB_SIZE-1)
#define FRACT_BITS (32 - TAB_BITS - 2)
#define FRACT_MASK ((1U << FRACT_BITS)-1)

extern float const NCO_lookup[TAB_SIZE+1]; // Leave room for pi/2 (90 deg, 1/4 rotation)

static inline float complex nco(uint64_t const a){
  /* QQ TTTTTTTT ffffffffffffffffffffff
     00: I     0-90 deg
     01: II   90-180
     10: III 180-270
     11: IV  270-360
  */
  uint32_t accum = a >> 32; // Use only top bits, bottom bits are for long-term phase/frequency accuracy
  uint32_t const fract = accum & FRACT_MASK; // lowest 22 bits: 0-21
  unsigned const t = (accum >> FRACT_BITS) & TAB_MASK; // middle 8 bits: 22-29
  unsigned const quad = accum >> (FRACT_BITS + TAB_BITS);  // highest 2 bits: 30-31: 0-90, 90-180, 180-270, 270-360
  // Index the lookup table in the proper direction
  unsigned const tab = (quad & 1) ? TAB_SIZE - t : t; // up, down, up, down
  union {
    float f;
    uint32_t i;
  } bits;
  bits.f = NCO_lookup[tab];
  bits.i ^= (quad & 2) << 30; // negative in quadrants III and IV
  float const sine = bits.f;
  // Approx cos with proper sign (derivative of sine)
  bits.f = NCO_lookup[TAB_SIZE - tab]; // down, up, down, up
  bits.i ^= ((quad+1) & 2) << 30; // negative in quadrants II and III
  float const cosine = bits.f;
  // Use approx cos as slope to interpolate fraction
  float const diff = (float)fract * (M_PIf * 0x1p-31f); // divide by 2^31
  float const halfdiff = 0.5f * diff;
  float const cdiff = cosine * diff;
  float const sdiff = sine * diff;
  // Interpolate with 2nd order Taylor expansion
  return CMPLXF(cosine - sdiff - cdiff * halfdiff, sine + cdiff - sdiff * halfdiff);
}
// f in cycles/sample; requires -0.5 <= f < 0.5
static inline uint64_t set_nco(double f){
  return (uint64_t)(int64_t)(0x1p64 * f);
}

#endif
