// filter using fast convolution (overlap-save) and the FFTW3 FFT package
// for the ka9q-radio 'radiod' program
// Generates transfer functions using Kaiser window
// Optional output decimation by integer factor
// Complex input and transfer functions, complex or real output
// Copyright 2017-2023, Phil Karn, KA9Q, karn@ka9q.net

#ifndef _FILTER_H
#define _FILTER_H 1

#include <pthread.h>
#include <complex.h>
#include <stdbool.h>
#include <fftw3.h>
#include "misc.h"

extern double Fftw_plan_timelimit;
extern char const *Wisdom_file;
extern int Nthreads;
extern int FFTW_planning_level;
extern double FFTW_plan_timelimit;

// Input can be REAL or COMPLEX
// Output can be REAL, COMPLEX, CROSS_CONJ, i.e., COMPLEX with special cross conjugation for ISB, or SPECTRUM (noncoherent power)
enum filtertype {
  NONE,
  COMPLEX,
  CROSS_CONJ,
  REAL,
  SPECTRUM,
};

// Input and output arrays can be either complex or real
// Used to be a union, but was prone to errors
struct rc {
  float *r;
  complex float *c;
};

#define ND 4
struct filter_in {
  enum filtertype in_type;           // REAL or COMPLEX
  int ilen;                 // Length of user portion of input buffer, aka 'L'
  int bins;                 // Total number of frequency bins. Complex: L + M - 1;  Real: (L + M - 1)/2 + 1
  int impulse_length;       // Length of filter impulse response, aka 'M'
  int wcnt;                 // Samples written to unexecuted input buffer
  void *input_buffer;                // Beginning of mirrored ring buffer
  size_t input_buffer_size;          // size of input buffer in **bytes**
  struct rc input_write_pointer;     // For incoming samples
  struct rc input_read_pointer;      // For FFT input
  fftwf_plan fwd_plan;               // FFT (time -> frequency)

  pthread_mutex_t filter_mutex;      // Synchronization for sequence number
  pthread_cond_t filter_cond;

  complex float *fdomain[ND];
  int next_jobnum;
  int completed_jobs[ND];
};

struct filter_out {
  struct filter_in * restrict master;
  enum filtertype out_type;          // REAL, COMPLEX or CROSS_CONJ
  int olen;                 // Length of user portion of output buffer (decimated L)
  int bins;                 // Number of frequency bins; == N for complex, == N/2 + 1 for real output
  complex float * restrict fdomain;  // Filtered signal in frequency domain
  complex float * restrict response; // Filter response in frequency domain
  pthread_mutex_t response_mutex;
  struct rc output_buffer;           // Actual time-domain output buffer, length N/decimate
  struct rc output;                  // Beginning of user output area, length L/decimate
  fftwf_plan rev_plan;               // IFFT (frequency -> time)
  int next_jobnum;
  float noise_gain;                  // Filter gain on uniform noise (ratio < 1)
  int block_drops;          // Lost frequency domain blocks, e.g., from late scheduling of slave thread
  int rcnt;                 // Samples read from output buffer
};

int create_filter_input(struct filter_in *,int const L,int const M, enum filtertype const in_type);
int create_filter_output(struct filter_out *slave,struct filter_in * restrict master,complex float * restrict response,int olen, enum filtertype out_type);
int execute_filter_input(struct filter_in * restrict);
int execute_filter_output(struct filter_out * restrict ,int);
int execute_filter_output_idle(struct filter_out * const slave);
int delete_filter_input(struct filter_in * restrict);
int delete_filter_output(struct filter_out * restrict);
int set_filter(struct filter_out * restrict,float,float,float);
void *run_fft(void *);
int write_cfilter(struct filter_in *, complex float const *,int size);
int write_rfilter(struct filter_in *, float const *,int size);


// Write complex sample to input side of filter
static inline int put_cfilter(struct filter_in * restrict const f,complex float const s){ // Complex
  assert((void *)(f->input_write_pointer.c) >= f->input_buffer);
  assert((void *)(f->input_write_pointer.c) < f->input_buffer + f->input_buffer_size);
  *f->input_write_pointer.c++ = s;
  mirror_wrap((void *)&f->input_write_pointer.c, f->input_buffer,f->input_buffer_size);
  if(++f->wcnt >= f->ilen){
    f->wcnt -= f->ilen;
    execute_filter_input(f);
    return 1; // may now execute filter output without blocking
  }
  return 0;
}

static inline int put_rfilter(struct filter_in * restrict const f,float const s){
  assert((void *)(f->input_write_pointer.r) >= f->input_buffer);
  assert((void *)(f->input_write_pointer.r) < f->input_buffer + f->input_buffer_size);
  *f->input_write_pointer.r++ = s;
  mirror_wrap((void *)&f->input_write_pointer.r, f->input_buffer,f->input_buffer_size);
  if(++f->wcnt >= f->ilen){
    f->wcnt -= f->ilen;
    execute_filter_input(f);
    return 1; // may now execute filter output without blocking
  }
  return 0;
}

// Read real samples from output side of filter
static inline float read_rfilter(struct filter_out * restrict const f,int const rotate){
  if(f->rcnt == 0){
    execute_filter_output(f,rotate);
    f->rcnt = f->olen;
  }
  return f->output.r[f->olen - f->rcnt--];
}

// Read complex samples from output side of filter
static inline complex float read_cfilter(struct filter_out * restrict const f,int const rotate){
  if(f->rcnt == 0){
    execute_filter_output(f,rotate);
    f->rcnt = f->olen;
  }
  return f->output.c[f->olen - f->rcnt--];
}

#endif
