#ifndef _RATIONAL_H
#define _RATIONAL_H 1
#include <stdint.h>
typedef struct {
  int64_t num;
  uint64_t den;   // always > 0 unless invalid
} rational_64;
typedef __uint128_t U128;
typedef __int128_t  I128;



// Euclidean greatest common divisor algorithm
static inline uint64_t gcd_u64(uint64_t a, uint64_t b){
  while(b){
    uint64_t t = a % b;
    a = b;
    b = t;
  }
  return a;
}
static inline U128 gcd_u128(U128 a, U128 b){
  while(b){
    U128 t = a % b;
    a = b;
    b = t;
  }
  return a;
}
#endif
