#include <stdio.h>
#include <complex.h>
#include <math.h>
#include <time.h>
#include "misc.h"
#include "osc.h"
#define COUNT (10000000)

int Verbose = 0;
static float complex v[COUNT];

int bar(float complex *v);

int main(){
  struct timespec start,t,d;

  for(int i=0; i < COUNT; i++)
    v[i] = uniform_rv() + I * uniform_rv();

  struct osc o;
  set_osc(&o, .45, 0.0);
  clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &start);
  for(int i=0; i < COUNT; i++)
    v[i] *= (float complex)STEP_OSC(&o);

  clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t);
  time_sub(&d,&t, &start);
  printf("complex oscillator time: %ld sec + %ld nsec\n",d.tv_sec,d.tv_nsec);
  bar(v);

  for(int i=0; i < COUNT; i++)
    v[i] = uniform_rv() + I * uniform_rv();

  nco_init();
  clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &start);
  uint64_t phase = 0;
  uint64_t step = 1234ULL << 35;
  for(int i=0; i < COUNT; i++){
    v[i] *= nco(phase);
    phase += step;
  }
  clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t);
  time_sub(&d, &t, &start);
  printf("nco time: %ld sec + %ld nsec\n",d.tv_sec,d.tv_nsec);
  bar(v);
}
