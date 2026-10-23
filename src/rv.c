// Routines for generating uniform and gaussian-distributed random variables, ie, white noise
// Copyright 2026 Phil Karn KA9Q
#include <stdint.h>
#include <stdbool.h>
#include <fcntl.h>
#include <unistd.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "misc.h"

/* xoshiro256** PRNG
 *
 * Public domain reference implementation adapted from:
 *   http://prng.di.unimi.it/
 *
 * State must be nonzero. Use the seed function below.
 */

// Gaussian (normal) RV generation
typedef struct {
    uint64_t s[4];
} xoshiro256ss_state;

// SIMD-friendly version
enum { RNG_LANES = 4 };

typedef struct {
    uint64_t a[RNG_LANES];
    uint64_t b[RNG_LANES];
    uint64_t c[RNG_LANES];
    uint64_t d[RNG_LANES];
} rng4_state;

static _Thread_local rng4_state Rand4;
_Thread_local xoshiro256ss_state Rand_state[4];
_Thread_local bool Rand_init;

// Rotate left
inline uint64_t rotl64(uint64_t x, int k){
    return (x << k) | (x >> (64 - k));
}
// SplitMix64 for seeding xoshiro256**
static uint64_t splitmix64(uint64_t *x){
    uint64_t z = (*x += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}
// Initialize xoshiro256** state from a 64-bit seed
void xoshiro256ss_seed(xoshiro256ss_state *st, uint64_t seed){
    // Expand a single 64-bit seed into 4 nonzero 64-bit words
    uint64_t x = seed;
    st->s[0] = splitmix64(&x);
    st->s[1] = splitmix64(&x);
    st->s[2] = splitmix64(&x);
    st->s[3] = splitmix64(&x);
    // Extremely unlikely, but ensure not all zeros
    if ((st->s[0] | st->s[1] | st->s[2] | st->s[3]) == 0) {
        st->s[0] = 1; // arbitrary nonzero
    }
}
// Generate next 64-bit output
inline uint64_t xoshiro256ss_next(xoshiro256ss_state *st){
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
// Generate 4 64-bit results at once, written to be SIMD friendly
static inline void rng4_next(rng4_state *restrict st, uint64_t out[restrict RNG_LANES]){
  for (int lane = 0; lane < RNG_LANES; ++lane) {
    uint64_t a = st->a[lane];
    uint64_t b = st->b[lane];
    uint64_t c = st->c[lane];
    uint64_t d = st->d[lane];
    out[lane] = rotl64(b * 5, 7) * 9;
    uint64_t t = b << 17;
    c ^= a;
    d ^= b;
    b ^= c;
    a ^= d;
    c ^= t;
    d = rotl64(d, 45);
    st->a[lane] = a;
    st->b[lane] = b;
    st->c[lane] = c;
    st->d[lane] = d;
  }
}

// Optional: jump function for 2^128 steps ahead (independent streams)
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
  int fd = open("/dev/random",O_RDONLY);
  uint64_t key;
  int n = read(fd,&key,sizeof key);
  if(n != sizeof key)
    fprintf(stderr,"Read from /dev/random failed: %s\n",strerror(errno));
  close(fd);
  xoshiro256ss_seed(&Rand_state[0],key);
  Rand_state[1] = Rand_state[0];
  xoshiro256ss_jump(&Rand_state[1]);
  Rand_state[2] = Rand_state[1];
  xoshiro256ss_jump(&Rand_state[2]);
  Rand_state[3] = Rand_state[2];
  xoshiro256ss_jump(&Rand_state[3]);
  for (int lane = 0; lane < RNG_LANES; ++lane) {
    Rand4.a[lane] = Rand_state[lane].s[0];
    Rand4.b[lane] = Rand_state[lane].s[1];
    Rand4.c[lane] = Rand_state[lane].s[2];
    Rand4.d[lane] = Rand_state[lane].s[3];
  }
  Rand_init = true;
}

float real_gauss(void){
  uint64_t u = xoshiro256ss_next(&Rand_state[0]);
  float x = __builtin_popcountll(u*0x2c1b3c6dULL) +
    __builtin_popcountll(u*0x297a2d39ULL) - 64;
  x += (float)(int64_t)u * 0x1p-63f;
  x *= 0.1765469659009499f; /* sqrt(1/(32 + 4/12)) */
  return x;
}
// Uniform RV between 0 and 1
static inline float uniform01_from_u32(uint32_t x){
  return (x >> 8) * 0x1p-24f;   // [0,1)
}
float uniform_rv(void){
  return uniform01_from_u32(xoshiro256ss_next(&Rand_state[1]));
}

#define TABSIZE 256

// Table for Taylor interpolation
static struct rtab {
  float r;             // sqrt(-2 * log(u))
  float rp;            // dr/du
  float rpp;           // d^2r / du^2
} Rayleigh[TABSIZE];
static bool Rayleigh_init;

// Produce a unit scale Rayleigh distributed RV from uniform 32-bit RV interpreted as [0,1)
static inline float rayleigh(uint32_t x){
  unsigned int const i = x >> 24; // top 8 bits
  int32_t const offset = (int32_t)(x & 0xffffffu) - 0x800000; // bottom 24 bits, -8 meg to +8 meg - 1
  float delta = (float)offset * 0x1p-32; // range [-1/512 to +1/512)
  assert(i < TABSIZE);
  struct rtab const *t = &Rayleigh[i];
  return t->r + delta * (t->rp + 0.5f * delta * t->rpp); // Taylor series expansion
}

void random_buffer(uint32_t *output, int count){
  int i = 0;
  for (; i + 7 < count; i += 8) {
    uint64_t out[RNG_LANES];
    rng4_next(&Rand4, out);

    output[i    ] = out[0] >> 32;
    output[i + 1] = (uint32_t)out[0];
    output[i + 2] = out[1] >> 32;
    output[i + 3] = (uint32_t)out[1];
    output[i + 4] = out[2] >> 32;
    output[i + 5] = (uint32_t)out[2];
    output[i + 6] = out[3] >> 32;
    output[i + 7] = (uint32_t)out[3];
  }
  for(; i+1 < count; i += 2){
    uint64_t a = xoshiro256ss_next(&Rand_state[i/2 & 3]);
    output[i] = a >> 32;
    output[i+1] = (uint32_t)a;
  }
  if(i < count){
    uint64_t a = xoshiro256ss_next(&Rand_state[i/2 & 3]);
    output[i] = (uint32_t)a;
  }
}


void rayleigh_buffer(float *output, int count){
  uint32_t *rands = malloc(count * sizeof *output);
  if(rands == NULL)
    return;
  random_buffer(rands, count);
  for(int i=0; i < count; i++)
    output[i] = rayleigh(rands[i]);

  // Now patch up entries in 0th bin where the derivatives blow up
  for(int i=0; i < count; i++){
    if(rands[i] < (1U << 24)){
      float u = (float)rands[i] * 0x1p-32;
      if(u == 0)
	u = 0x1p-32; // small value, avoid log(0) exception
      output[i] = sqrtf(-2.0f * logf(u));
    }
  }
  FREE(rands);
}
void rayleigh_init(void){
  if(Rayleigh_init)
    return;
  rand_init();
  for(int i = 0; i < TABSIZE; i++){
    double const u = (i + 0.5) / TABSIZE; // Offset 1/2 step
    double const r = sqrt(-2.0 * log(u));
    Rayleigh[i].r = (float)r;
    Rayleigh[i].rp = (float)(-1.0 / (u * r));
    Rayleigh[i].rpp = (float)(r * r - 1.0) / (u * u * r * r * r);
    //    printf("u = %f, r = %f, rp = %f, rpp = %f\n", u, Rayleigh[i].r, Rayleigh[i].rp, Rayleigh[i].rpp);
  }
  Rayleigh_init = true;
}
#include "nco.h"
void gaussian_buffer(float *output, int count){
  float *rayrv = malloc(((count+1)/2) * sizeof *rayrv);
  if(rayrv == NULL)
    return;
  uint32_t *phaserv = malloc(((count+1)/2) * sizeof *phaserv);
  if(phaserv == NULL){
    FREE(rayrv);
    return;
  }
  rayleigh_buffer(rayrv, (count+1)/2);
  random_buffer(phaserv, (count+1)/2);

  for(int i=0; i < count/2; i++){
    float complex phase = nco((uint64_t)phaserv[i] << 32);
    output[2*i] = rayrv[i] * cimagf(phase);
    output[2*i+1] = rayrv[i] * crealf(phase);
  }
  if(count & 1){
    float complex phase = nco((uint64_t)phaserv[count/2] << 32);
    output[count-1] = rayrv[count/2] * cimagf(phase);
  }
  FREE(rayrv);
  FREE(phaserv);
}
