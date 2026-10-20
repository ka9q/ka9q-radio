// fast kernels for computing energy and mixing down a buffer with and without a chirp
// These should vectorize well on newer machines
// Copyright 2026 Phil Karn, KA9Q

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "misc.h"
#include "nco.h"

#define VECTORIZE_SWEEP 1


#if defined(__x86_64__)
__attribute__((target("avx512f,fma")))
static float energy_avx512(float complex const *buffer, int const count){
  float energy = 0;
  for(int n=0; n < count; n++)
    energy += cnrmf(buffer[n]);
  return energy;
}
__attribute__((target("avx2,fma")))
static float energy_avx2(float complex const *buffer, int const count){
  float energy = 0;
  for(int n=0; n < count; n++)
    energy += cnrmf(buffer[n]);
  return energy;
}
#if defined(VECTORIZE_SWEEP)
static inline uint64_t triangular(uint64_t n){
  return (n & 1) ? n * ((n - 1) / 2) : (n / 2) * (n - 1);
}
#endif

__attribute__((target("avx512f,fma")))
static uint64_t mixdown_avx512(float complex * restrict buffer, int const count, uint64_t phase, uint64_t increment){
  for(int n=0; n < count; n++){
    buffer[n] *= nco(phase);
    phase += increment;
  }
  return phase;
}
__attribute__((target("avx2,fma")))
static uint64_t mixdown_avx2(float complex * restrict buffer, int const count, uint64_t phase, uint64_t increment){
  for(int n=0; n < count; n++){
    buffer[n] *= nco(phase);
    phase += increment;
  }
  return phase;
}
__attribute__((target("avx512f,fma")))
static uint64_t mixdown_chirp_avx512(float complex * restrict buffer, int const count, uint64_t phase, uint64_t increment, uint64_t chirp){
#if defined(VECTORIZE_SWEEP)
  if(count <= 0)
    return phase;
  uint64_t const length = (uint64_t)count;
  for(uint64_t n=0; n < length; n++){
    uint64_t p = phase + n * increment + triangular(n) * chirp;
    buffer[n] *= nco(p);
  }
  return phase + length * increment + triangular(length) * chirp;
#else
  for(int n=0; n < count; n++){
    buffer[n] *= nco(phase);
    phase += increment;
    increment += chirp;
  }
  return phase;
#endif
}
__attribute__((target("avx2,fma")))
static uint64_t mixdown_chirp_avx2(float complex * restrict buffer, int const count, uint64_t phase, uint64_t increment, uint64_t chirp){
#if defined(VECTORIZE_SWEEP)
  if(count <= 0)
    return phase;
  uint64_t const length = (uint64_t)count;
  for(uint64_t n=0; n < length; n++){
    uint64_t p = phase + n * increment + triangular(n) * chirp;
    buffer[n] *= nco(p);
  }
  return phase + length * increment + triangular(length) * chirp;
#else
  for(int n=0; n < count; n++){
    buffer[n] *= nco(phase);
    phase += increment;
    increment += chirp;
  }
  return phase;
#endif
}
#endif // __x86_64__

static float energy_portable(float complex const *buffer, int const count){
  float energy = 0;
  for(int n=0; n < count; n++)
    energy += cnrmf(buffer[n]);
  return energy;
}

static uint64_t mixdown_portable(float complex * restrict buffer, int const count, uint64_t phase, uint64_t increment){
  for(int n=0; n < count; n++){
    buffer[n] *= nco(phase);
    phase += increment;
  }
  return phase;
}
static uint64_t mixdown_chirp_portable(float complex * restrict buffer, int const count, uint64_t phase, uint64_t increment, uint64_t chirp){
  for(int n=0; n < count; n++){
    buffer[n] *= nco(phase);
    phase += increment;
    increment += chirp;
  }
  return phase;
}
static float (*chosen_energy)(float complex const *buffer, int const count) = energy_portable;
static uint64_t (*chosen_mixdown)(float complex * restrict buffer, int const count, uint64_t phase, uint64_t increment) = mixdown_portable;
static uint64_t (*chosen_mixdown_chirp)(float complex * restrict buffer, int const count, uint64_t phase, uint64_t increment, uint64_t chirp) = mixdown_chirp_portable;

// Call this once at startup (main.c) to select the one for this system
void kernel_init(void){
#if defined(__x86_64__)
  if(__builtin_cpu_supports("fma")){
    if(__builtin_cpu_supports("avx512f")){
      chosen_energy = energy_avx512;
      chosen_mixdown = mixdown_avx512;
      chosen_mixdown_chirp = mixdown_chirp_avx512;
      fprintf(stderr,"Using avx512 dsp kernels\n");
    } else if(__builtin_cpu_supports("avx2")){
      chosen_energy = energy_avx2;
      chosen_mixdown = mixdown_avx2;
      chosen_mixdown_chirp = mixdown_chirp_avx2;
      fprintf(stderr,"Using avx2 dsp kernels\n");
    }
  }
#endif
}


float energy(float complex const *buffer, int const count){
  return (*chosen_energy)(buffer,count);
}
uint64_t mixdown(float complex * restrict buffer, int const count, uint64_t phase, uint64_t increment){
  return (*chosen_mixdown)(buffer, count, phase, increment);
}
uint64_t mixdown_chirp(float complex * restrict buffer, int const count, uint64_t phase, uint64_t increment, uint64_t chirp){
  return (*chosen_mixdown_chirp)(buffer, count, phase, increment, chirp);
}

