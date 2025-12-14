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
extern pthread_mutex_t FFTW_planning_mutex;
extern int N_internal_threads;
extern int N_worker_threads; // owned by filter.c

// Input can be REAL or COMPLEX
// Output can be REAL, COMPLEX, BEAM or SPECTRUM
// BEAM is for selecting independent I or Q from complex input or for
// beamforming when I and Q are from independent antennas
enum filtertype {
  NONE,
  COMPLEX,
  BEAM, // Input pseudo-complex: two real signals in I and Q
  REAL,
  SPECTRUM, // On output only
};

// Input and output arrays can be either complex or real
// Used to be a union, but was prone to errors
struct rc {
  float *r;
  float complex *c;
};
struct notch_state {
  int bin;              // Index of bin in frequency domain (output of filter_in)
  double complex state; // averaged spur vector
  double alpha;         // gain of averager, larger -> wider notch
};

#define ND 4
struct filter_in {
  enum filtertype in_type;           // REAL, COMPLEX, BEAM
  int points;               // Size of FFT N = L + M - 1. For complex, == N
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

  struct notch_state *notches;
  float complex *fdomain[ND];
  unsigned int next_jobnum;
  unsigned int completed_jobs[ND];
  bool perform_inline;       // Perform FFT inline, don't use worker threads (better for small FFTs)
};

struct filter_out {
  struct filter_in * restrict master;
  enum filtertype out_type;          // REAL, COMPLEX, BEAM, SPECTRUM
  int points;               // Size N of fft; Same as bins only for complex
  int olen;                 // Length of user portion of output buffer (decimated L)
  int bins;                 // Number of frequency bins; == N for complex, == N/2 + 1 for real output
  double complex alpha;      // For beam synthesis mode, or for selecting I or Q on complex input
  double complex beta;
  float complex * restrict fdomain;  // Filtered signal in frequency domain
  float complex * restrict response; // Filter response in frequency domain
  pthread_mutex_t response_mutex;
  struct rc output_buffer;           // Actual time-domain output buffer, length N/decimate
  struct rc output;                  // Beginning of user output area, length L/decimate
  fftwf_plan rev_plan;               // IFFT (frequency -> time)
  unsigned next_jobnum;
  unsigned block_drops;          // Lost frequency domain blocks, e.g., from late scheduling of slave thread
  int rcnt;                 // Samples read from output buffer
};

int create_filter_input(struct filter_in *,int const L,int const M, enum filtertype const in_type);
int create_filter_output(struct filter_out *slave,struct filter_in * restrict master,float complex * restrict response,int olen, enum filtertype out_type);
int execute_filter_input(struct filter_in * restrict);
int execute_filter_output(struct filter_out * restrict ,int);
int delete_filter_input(struct filter_in * restrict);
int delete_filter_output(struct filter_out * restrict);
int set_filter(struct filter_out * restrict,double,double,double);
void *run_fft(void *);
int write_cfilter(struct filter_in *, float complex const *,int size);
int write_rfilter(struct filter_in *, float const *,int size);
void suggest(int size,int dir,int clex);
unsigned long gcd(unsigned long a,unsigned long b);
unsigned long lcm(unsigned long a,unsigned long b);
int make_kaiser(double * const window,int const M,double const beta);
int make_kaiserf(float * const window,int const M,double const beta);
fftwf_plan plan_complex(int N, float complex *in, float complex *out, int direction);
fftwf_plan plan_r2c(int N, float *in, float complex *out);
fftwf_plan plan_c2r(int N, float complex *in, float *out);
bool goodchoice(unsigned long);
unsigned int ceil_pow2(unsigned int x);
int set_filter_weights(struct filter_out *out,double complex i_weight, double complex q_weight);


// Write complex sample to input side of filter
static inline int put_cfilter(struct filter_in * restrict const f,float complex const s){ // Complex
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
static inline float complex read_cfilter(struct filter_out * restrict const f,int const rotate){
  if(f->rcnt == 0){
    execute_filter_output(f,rotate);
    f->rcnt = f->olen;
  }
  return f->output.c[f->olen - f->rcnt--];
}

#endif
