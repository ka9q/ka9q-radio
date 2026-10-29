#include <stdio.h>
#include <math.h>
#include <stdlib.h>

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

void xoshiro256ss_seed(xoshiro256ss_state *st, uint64_t seed);
uint64_t xoshiro256ss_next(xoshiro256ss_state *st);
void xoshiro256ss_jump(xoshiro256ss_state *st);

_Thread_local xoshiro256ss_state Rand_state;
_Thread_local bool Rand_init;

// Rotate left
static inline uint64_t rotl64(uint64_t x, int k){
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
  xoshiro256ss_seed(&Rand_state,key);
  Rand_init = true;
}
float real_gauss(void){
  uint64_t u = xoshiro256ss_next(&Rand_state);
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
  return uniform01_from_u32(xoshiro256ss_next(&Rand_state));
}


#define TABSIZE 256

static struct rtab {
  float r;
  float rp;
  float rpp;
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
  for(int i=0; i < count; i += 2){
    // Generate 2 32-bit RVs at a time
    uint64_t r64 = xoshiro256ss_next(&Rand_state);
    output[i] = (uint32_t)(r64 >> 32);
    output[i+1] = (uint32_t)(r64 & 0xffffffff);
  }
  if(count & 1)
    output[count-1] = xoshiro256ss_next(&Rand_state) & 0xffffffff;
}


void rayleigh_buffer(float *output, int count){
  uint32_t *rands = malloc(count * sizeof *output);
  assert(rands != NULL);
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
  assert(rayrv != NULL);
  if(rayrv == NULL)
    return;
  uint32_t *phaserv = malloc(((count+1)/2) * sizeof *phaserv);
  assert(phaserv != NULL);
  if(phaserv == NULL)
    return;

  rayleigh_buffer(rayrv, (count+1)/2);
  random_buffer(phaserv, (count+1)/2);

  for(int i=0; i < count/2; i++){
    float complex phase = nco(phaserv[i]);
    output[2*i] = rayrv[i] * cimagf(phase);
    output[2*i+1] = rayrv[i] * crealf(phase);
  }
  if(count & 1){
    float complex phase = nco(phaserv[count/2]);
    output[count-1] = rayrv[count/2] * cimagf(phase);
  }
  FREE(rayrv);
  FREE(phaserv);
}


int main(int argc, char *argv[]){
  rayleigh_init();

  int const count = 32 * 1024;

  float buffer[count];
  rayleigh_buffer(buffer, count);
  for(int i=0; i < count; i++){
    assert(isfinite(buffer[i]) && buffer[i] >= 0);
    printf(" %.8f\n",buffer[i]);
  }
  exit(0);
}



int raytest(int argc, char *argv[]){
  rayleigh_init();

  int const steps = 32*1024;
  for(int i=0; i < steps; i++){
    uint32_t rv1 = (uint32_t)i << 17;
    float u1 = (i) / (float)steps;

    printf("%x %.8f %g %g\n",rv1, u1, sqrtf(-2.f * logf(u1)), rayleigh(rv1));
  }


  exit(0);
}
