// fast kernels for computing RMS energy and mixing down a buffer with and without a chirp
// These should vectorize well on newer machines
// Copyright 2026 Phil Karn, KA9Q

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "misc.h"
#include "nco.h"

#if defined(__x86_64__)
__attribute__((target("avx512f,fma")))
static float rms_avx512(float complex *buffer, int count){
  float energy = 0;
  for(int n=0; n < count; n++)
    energy += cnrmf(buffer[n]);
  return energy / count;
}
__attribute__((target("avx2,fma")))
static float rms_avx2(float complex *buffer, int count){
  float energy = 0;
  for(int n=0; n < count; n++)
    energy += cnrmf(buffer[n]);
  return energy / count;
}
__attribute__((target("avx512f,fma")))
static uint64_t mixdown_avx512(float complex *buffer, int count, uint64_t phase, int64_t increment){
  for(int n=0; n < count; n++){
    buffer[n] *= nco(phase);
    phase += increment;
  }
  return phase;
}
__attribute__((target("avx2,fma")))
static uint64_t mixdown_avx2(float complex *buffer, int count, uint64_t phase, int64_t increment){
  for(int n=0; n < count; n++){
    buffer[n] *= nco(phase);
    phase += increment;
  }
  return phase;
}
__attribute__((target("avx512f,fma")))
static uint64_t mixdown_chirp_avx512(float complex *buffer, int count, uint64_t phase, int64_t increment, int64_t chirp){
  for(int n=0; n < count; n++){
    buffer[n] *= nco(phase);
    phase += increment;
    increment += chirp;
  }
  return phase;
}
__attribute__((target("avx2,fma")))
static uint64_t mixdown_chirp_avx2(float complex *buffer, int count, uint64_t phase, int64_t increment, int64_t chirp){
  for(int n=0; n < count; n++){
    buffer[n] *= nco(phase);
    phase += increment;
    increment += chirp;
  }
  return phase;
}
#endif

static float rms_portable(float complex *buffer, int count){
  float energy = 0;
  for(int n=0; n < count; n++)
    energy += cnrmf(buffer[n]);
  return energy / count;
}
static uint64_t mixdown_portable(float complex *buffer, int count, uint64_t phase, int64_t increment){
  for(int n=0; n < count; n++){
    buffer[n] *= nco(phase);
    phase += increment;
  }
  return phase;
}
static uint64_t mixdown_chirp_portable(float complex *buffer, int count, uint64_t phase, int64_t increment, int64_t chirp){
  for(int n=0; n < count; n++){
    buffer[n] *= nco(phase);
    phase += increment;
    increment += chirp;
  }
  return phase;
}
static float (*chosen_rms)(float complex *buffer, int count) = rms_portable;
static uint64_t (*chosen_mixdown)(float complex *buffer, int count, uint64_t phase, int64_t increment) = mixdown_portable;
static uint64_t (*chosen_mixdown_chirp)(float complex *buffer, int count, uint64_t phase, int64_t increment, int64_t chirp) = mixdown_chirp_portable;

// Call this once at startup (main.c) to select the one for this system
void kernel_init(void){
#if defined(__x86_64__)
  if(__builtin_cpu_supports("fma")){
    if(__builtin_cpu_supports("avx512f")){
      chosen_rms = rms_avx512;
      chosen_mixdown = mixdown_avx512;
      chosen_mixdown_chirp = mixdown_chirp_avx512;
      fprintf(stderr,"Using avx512 dsp kernels\n");
    } else if(__builtin_cpu_supports("avx2")){
      chosen_rms = rms_avx2;
      chosen_mixdown = mixdown_avx2;
      chosen_mixdown_chirp = mixdown_chirp_avx2;
      fprintf(stderr,"Using avx2 dsp kernels\n");
    }
  }
#endif
}


float rms(float complex *buffer, int count){
  return (*chosen_rms)(buffer,count);
}
uint64_t mixdown(float complex *buffer, int count, uint64_t phase, int64_t increment){
  return (*chosen_mixdown)(buffer, count, phase, increment);
}
uint64_t mixdown_chirp(float complex *buffer, int count, uint64_t phase, int64_t increment, int64_t chirp){
  return (*chosen_mixdown_chirp)(buffer, count, phase, increment, chirp);
}

