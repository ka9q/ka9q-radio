// Parse a string with a frequency of the form
// 12345 (12345 Hz)
// 12k345 (12.345 kHz)
// 12m345 (12.345 MHz)
// 12g345 (12.345 GHz)
// If no g/m/k and number is too small, make a heuristic guess
// NB! This assumes radio covers 100 kHz - 2 GHz; should make more general
// Two versions are provided:
//   the core routine produces a rational number (ratio of two 64-bit integers)
//   a second convenience routine calls the first one and returns a double precision float
// Copyright 2026 Phil Karn, KA9Q

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include "rational.h"

static rational_64 rational_reduce_64(rational_64 x){
  if(x.den == 0)
    return x; // Invalid

  uint64_t num = (uint64_t)llabs(x.num);
  uint64_t den = x.den;
  uint64_t const g = gcd_u64(num,den);
  num /= g;
  den /= g;
  rational_64 result = {
    (int64_t)num,
    den
  };
  if(x.num < 0)
    result.num = -result.num;
  return result;
}

// Parse frequency, return a rational approximation to avoid floating point
rational_64 parse_frequency_rational(char const *s, bool heuristics){
  // Make lower case copy of input string
  size_t const len = strlen(s);
  unsigned char * const ss = alloca(len+1);
  for(size_t i=0; i<len; i++)
    ss[i] = (unsigned char)tolower((unsigned char)s[i]);
  ss[len] = '\0';
  bool radix_seen = false;
  bool sign = false;
  int mult = 0;
  uint64_t num = 0;
  uint64_t den = 1;
  uint64_t const cutoff = UINT64_MAX / 10; // largest before overflow when multiplying by 10
  for(unsigned char const *cp = ss; *cp != '\0'; cp++){
    unsigned char const c = *cp;
    if(c == '-'){
      sign = true;
    } else if(isdigit(c)){
      unsigned const d = c - '0';
      if(num < cutoff || (num == cutoff && d <= UINT64_MAX % 10)){
	num = num * 10 + d;
	if(radix_seen)
	  mult--;
      } else
	goto fail;
    } else if(c == 'g'){
      mult += 9;
      radix_seen = true;
    } else if(c == 'm'){
      mult += 6;
      radix_seen = true;
    } else if(c == 'k'){
      mult += 3;
      radix_seen = true;
    } else if(c == '.' || c == ','){
      radix_seen = true;
    }
    // unknown chars are ignored
  }
  if(radix_seen){
    for(int i=0; i < mult; i++){
      if(num > cutoff)
	goto fail;
      num *= 10;
    }
    for(int i = 0; i > mult; i--){
      if(den > cutoff)
	goto fail;
      den *= 10;
    }
  } else if(heuristics){
    if(num < 500)
      num *= 1000000; // assume megahertz, won't overflow
    else if(num < 100000)
      num *= 1000;    // assume kilohertz, won't overflow
  }
  // Form result
  if(num > INT64_MAX)
    goto fail; // must allow room for sign bit
  rational_64 result = {
    .num = (int64_t)num,
    .den = den
  };
  result = rational_reduce_64(result);
  if(sign)
    result.num = -result.num;
  return result;
 fail:;
  {
    rational_64 const result = {0}; // zero denom is invalid
    return result;
  }
}
// Version that returns ordinary floating point from rational approximation
double parse_frequency(char const *s, bool heuristics){
  rational_64 const r = parse_frequency_rational(s,heuristics);
  return (double)r.num / (double)r.den;
}
