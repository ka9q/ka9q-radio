#include <stdint.h>
#include <stdbool.h>
#include "misc.h"

/* xoshiro256** PRNG
 *
 * Public domain reference implementation adapted from:
 *   http://prng.di.unimi.it/
 *
 * State must be nonzero. Use the seed function below.
 */

_Thread_local xoshiro256ss_state Rand_state;
_Thread_local bool Rand_init;

/* Rotate left */
static inline uint64_t rotl64(uint64_t x, int k){
    return (x << k) | (x >> (64 - k));
}

/* SplitMix64 for seeding xoshiro256** */
static uint64_t splitmix64(uint64_t *x){
    uint64_t z = (*x += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

/* Initialize xoshiro256** state from a 64-bit seed */
void xoshiro256ss_seed(xoshiro256ss_state *st, uint64_t seed){
    /* Expand a single 64-bit seed into 4 nonzero 64-bit words */
    uint64_t x = seed;
    st->s[0] = splitmix64(&x);
    st->s[1] = splitmix64(&x);
    st->s[2] = splitmix64(&x);
    st->s[3] = splitmix64(&x);

    /* Extremely unlikely, but ensure not all zeros */
    if ((st->s[0] | st->s[1] | st->s[2] | st->s[3]) == 0) {
        st->s[0] = 1; /* arbitrary nonzero */
    }
}

/* Generate next 64-bit output */
uint64_t xoshiro256ss_next(xoshiro256ss_state *st){
    const uint64_t result = rotl64(st->s[1] * 5, 7) * 9;

    const uint64_t t = st->s[1] << 17;

    st->s[2] ^= st->s[0];
    st->s[3] ^= st->s[1];
    st->s[1] ^= st->s[2];
    st->s[0] ^= st->s[3];

    st->s[2] ^= t;
    st->s[3] = rotl64(st->s[3], 45);

    return result;
}

/* Optional: jump function for 2^128 steps ahead (independent streams) */
void xoshiro256ss_jump(xoshiro256ss_state *st){
    static const uint64_t JUMP[] = {
        0x180ec6d33cfd0abaULL,
        0xd5a61266f0c9392cULL,
        0xa9582618e03fc9aaULL,
        0x39abdc4529b1661cULL
    };

    uint64_t s0 = 0;
    uint64_t s1 = 0;
    uint64_t s2 = 0;
    uint64_t s3 = 0;

    for (int i = 0; i < 4; i++) {
        for (int b = 0; b < 64; b++) {
            if (JUMP[i] & (1ULL << b)) {
                s0 ^= st->s[0];
                s1 ^= st->s[1];
                s2 ^= st->s[2];
                s3 ^= st->s[3];
            }
            (void)xoshiro256ss_next(st);
        }
    }

    st->s[0] = s0;
    st->s[1] = s1;
    st->s[2] = s2;
    st->s[3] = s3;
}
// Fast Gaussian approximation
void rand_init(void){
  if(Rand_init)
    return; // already done
  
  xoshiro256ss_seed(&Rand_state,1);
  Rand_init = true;
}

double real_gauss(void){
  uint64_t u = xoshiro256ss_next(&Rand_state);

  double x = __builtin_popcountll(u*0x2c1b3c6dULL) +
    __builtin_popcountll(u*0x297a2d39ULL) - 64;
  x += (int64_t)u * (1 / 9223372036854775808.);
  x *= 0.1765469659009499; /* sqrt(1/(32 + 4/12)) */
  return x;
}
