// $Id: filter.c,v 1.85 2022/03/18 00:22:34 karn Exp karn $
// General purpose filter package using fast convolution (overlap-save)
// and the FFTW3 FFT package
// Generates transfer functions using Kaiser window
// Optional output decimation by integer factor
// Complex input and transfer functions, complex or real output
// Copyright 2017, Phil Karn, KA9Q, karn@ka9q.net

#undef DUAL_FFT_THREAD

#define _GNU_SOURCE 1
#include <assert.h>
#include <stdlib.h>
#include <pthread.h>
#include <memory.h>
#include <complex.h>
#include <math.h>
#include <fftw3.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "misc.h"
#include "filter.h"

int Nthreads = 1; 

static inline int modulo(int x,int const m){
  x = x < 0 ? x + m : x;
  return x > m ? x - m : x;
}


// Create fast convolution filters
// The filters are now in two parts, filter_in (the master) and filter_out (the slave)
// Filter_in holds the original time-domain input and its frequency domain version
// Filter_out holds the frequency response and decimation information for one of several output filters that can share the same input

// filter_create_input() parameters, shared by all slaves:
// L = input data blocksize
// M = impulse response duration
// in_type = REAL or COMPLEX

// filter_create_output() parameters, distinct per slave
// master - pointer to associated master (input) filter
// response = complex frequency response; may be NULL here and set later with set_filter()
// This is set in the slave and can be different (indeed, this is the reason to have multiple slaves)
//            NB: response is always complex even when input and/or output is real, though it will be shorter
//            bins = (L + M - 1)/decimate when output is complex
//            length = (bins/2+1) when output is real
//            Must be SIMD-aligned (e.g., allocated with fftw_alloc) and will be freed by delete_filter()

// decimate = input/output sample rate ratio, only tested for powers of 2
// out_type = REAL, COMPLEX or CROSS_CONJ (COMPLEX with special processing for ISB)

// All demodulators taking baseband (zero IF) I/Q data require COMPLEX input
// All but SSB require COMPLEX output, with ISB using the special CROSS_CONJ mode
// SSB(CW) could (and did) use the REAL mode since the imaginary component is unneeded, and the c2r IFFT is faster
// Baseband FM audio filtering for de-emphasis and PL separation uses REAL input and output

// If you provide your own filter response, ensure that it drops to nil well below the Nyquist rate
// to prevent aliasing. Remember that decimation reduces the Nyquist rate by the decimation ratio.
// The set_filter() function uses Kaiser windowing for this purpose

// Set up input (master) half of filter
struct filter_in *create_filter_input(int const L,int const M, enum filtertype const in_type){
  assert(L > 0);
  assert(M > 0);
  int const N = L + M - 1;
  int const bins = (in_type == COMPLEX) ? N : (N/2 + 1);
  if(bins < 1)
    return NULL; // Unreasonably small - will segfault. Can happen if sample rate is garbled


  struct filter_in * const master = calloc(1,sizeof(struct filter_in));
  for(int i=0; i < ND; i++)
    master->fdomain[i] = fftwf_alloc_complex(bins);

  assert(master != NULL);
  assert(master != (void *)-1);
  master->bins = bins;
  master->in_type = in_type;
  master->ilen = L;
  master->impulse_length = M;
  pthread_mutex_init(&master->filter_mutex,NULL);
  pthread_mutex_init(&master->queue_mutex,NULL);
  pthread_cond_init(&master->filter_cond,NULL);
  pthread_cond_init(&master->queue_cond,NULL);

  // Use multithreading (if configured) only for large forward FFTs
  fftwf_plan_with_nthreads(Nthreads);

  switch(in_type){
  default:
    assert(0); // shouldn't happen
    return NULL;
  case COMPLEX:
    master->input_buffer.c = fftwf_alloc_complex(N);
    master->input_buffer.r = NULL; // Catch erroneous uses
    assert(malloc_usable_size(master->input_buffer.c) >= N * sizeof(*master->input_buffer.c));
    memset(master->input_buffer.c, 0, (M-1)*sizeof(*master->input_buffer.c)); // Clear earlier state
    master->input.c = master->input_buffer.c + M - 1;
    master->fwd_plan = fftwf_plan_dft_1d(N, master->input_buffer.c, master->fdomain[0], FFTW_FORWARD, FFTW_ESTIMATE);
    break;
  case REAL:
    master->input_buffer.c = NULL;
    master->input_buffer.r = fftwf_alloc_real(N);
    assert(malloc_usable_size(master->input_buffer.r) >= N * sizeof(*master->input_buffer.r));
    memset(master->input_buffer.r, 0, (M-1)*sizeof(*master->input_buffer.r)); // Clear earlier state
    master->input.r = master->input_buffer.r + M - 1;
    master->fwd_plan = fftwf_plan_dft_r2c_1d(N, master->input_buffer.r, master->fdomain[0], FFTW_ESTIMATE);
    break;
  }
  return master;
}
// Set up output (slave) side of filter (possibly one of several sharing the same input master)
// These output filters should be deleted before their masters
// Segfault will occur if filter_in is deleted and execute_filter_output is executed
struct filter_out *create_filter_output(struct filter_in * master,complex float * const response,int const olen, enum filtertype const out_type){
  assert(master != NULL);
  if(master == NULL)
    return NULL;

  assert(olen > 0);
  if(olen > master->ilen)
    return NULL; // Interpolation not yet supported
  
  struct filter_out * const slave = calloc(1,sizeof(*slave));
  if(slave == NULL)
    return NULL;
  // Share all but output fft bins, response, output and output type
  slave->master = master;
  slave->out_type = out_type;
  slave->olen = olen;
  
  float const overlap = (float)(master->ilen + master->impulse_length - 1) / master->ilen; // Total FFT time points / used time points
  int const osize = round(olen * overlap); // Total number of time-domain FFT points including overlap
  
  slave->response = response;
  if(response != NULL)
    slave->noise_gain = noise_gain(slave);
  else
    slave->noise_gain = NAN;
  
  // Usually too small to benefit from multithreading
  fftwf_plan_with_nthreads(1);
  switch(slave->out_type){
  default:
  case COMPLEX:
  case CROSS_CONJ:
    slave->bins = osize; // Same as total number of time domain points
    slave->f_fdomain = fftwf_alloc_complex(slave->bins);
    slave->output_buffer.c = fftwf_alloc_complex(osize);
    assert(slave->output_buffer.c != NULL);
    slave->output_buffer.r = NULL; // catch erroneous references
    slave->output.c = slave->output_buffer.c + osize - olen;
    slave->rev_plan = fftwf_plan_dft_1d(osize,slave->f_fdomain,slave->output_buffer.c,FFTW_BACKWARD,FFTW_ESTIMATE);
    break;
  case REAL:
    slave->bins = osize / 2 + 1;
    slave->f_fdomain = fftwf_alloc_complex(slave->bins);
    assert(slave->f_fdomain != NULL);    
    
    slave->output_buffer.r = fftwf_alloc_real(osize);
    assert(slave->output_buffer.r != NULL);
    slave->output_buffer.c = NULL;
    slave->output.r = slave->output_buffer.r + osize - olen;
    slave->rev_plan = fftwf_plan_dft_c2r_1d(osize,slave->f_fdomain,slave->output_buffer.r,FFTW_ESTIMATE);
    break;
  }
  slave->blocknum = master->blocknum;
  return slave;
}

// Thread that actually performs the forward FFT
// Allows the thread running execute_filter_input() to continue
// processing the next input block in parallel on another core
// Frees the input buffer and the job descriptor when done
void *run_fft(void *p){
  pthread_detach(pthread_self());
  pthread_setname("fft");

  struct filter_in *f = (struct filter_in *)p;
  assert(f != NULL);
  assert(f->fwd_plan != NULL);

  while(1){
    // take job descriptor from head of queue
    pthread_mutex_lock(&f->queue_mutex);
    while(f->job_queue == NULL)
      pthread_cond_wait(&f->queue_cond,&f->queue_mutex);
    
    struct fft_job *job = f->job_queue;
    f->job_queue = job->next;
    int const jobnum = f->jobnum++;
    pthread_mutex_unlock(&f->queue_mutex);
    
    if(job->input != NULL){
      switch(f->in_type){
      case COMPLEX:
      case CROSS_CONJ:
	fftwf_execute_dft(f->fwd_plan,job->input,f->fdomain[jobnum % ND]);
	break;
      case REAL:
	fftwf_execute_dft_r2c(f->fwd_plan,job->input,f->fdomain[jobnum % ND]);
	break;
      default:
	break;
      }
      free(job->input); job->input = NULL;
    }

    pthread_mutex_lock(&f->filter_mutex);
#ifdef DUAL_FFT_THREAD
    // Wait for any previous pending jobs to finish, if they got out of sequence due to scheduling
    // Still works if only one thread is running, but let's avoid the theoretical deadlock anyway
    while(jobnum != f->blocknum + 1)
      pthread_cond_wait(&f->filter_cond,&f->filter_mutex);
#endif
    // Signal listeners that we're done
    f->blocknum = jobnum + 1;
    pthread_cond_broadcast(&f->filter_cond);
    pthread_mutex_unlock(&f->filter_mutex);
    free(job); job = NULL;
  }
  return NULL; // not reached
}


int execute_filter_input(struct filter_in * const f){
  assert(f != NULL);
  if(f == NULL)
    return -1;

  // Start FFT thread if not already running
  if(f->fft_thread == (pthread_t)0)
    pthread_create(&f->fft_thread,NULL,run_fft,f);
#if DUAL_FFT_THREAD
  // Experimental: create second FFT processing thread
  // Only really useful if FFT cannot be completed in one frame time
  // Otherwise the scheduler usually gives the job to the same thread, and the other sits idle
  if(f->fft_thread2 == (pthread_t)0)
    pthread_create(&f->fft_thread2,NULL,run_fft,f);    
#endif

  // We now use the FFTW3 functions that specify the input and output arrays
  // Execute the FFT in a detached thread so we can process more input data while the FFT executes
  struct fft_job * const job = calloc(1,sizeof(struct fft_job));

  switch(f->in_type){
  default:
  case CROSS_CONJ:
  case COMPLEX:
    job->input = f->input_buffer.c;
    break;
  case REAL:
    job->input = f->input_buffer.r;
    break;
  }
  assert(job->input != NULL); // Should already be allocated in create_filter_input, or in our last call

  // Set up next input buffer and perform overlap-and-save operation for fast convolution
  // Although it would decrease latency to notify the fft thead first, that would set up a race condition
  // since the FFT thread frees its input buffer on completion
  int const N = f->ilen + f->impulse_length - 1;
  switch(f->in_type){
  default:
  case COMPLEX:
    {
      complex float * const newbuf = fftwf_alloc_complex(N);
      memmove(newbuf,f->input_buffer.c + f->ilen,(f->impulse_length-1)*sizeof(*f->input_buffer.c));
      f->input_buffer.c = newbuf;
      f->input.c = f->input_buffer.c + f->impulse_length -1;
    }
    break;
  case REAL:
    {
      float * const newbuf = fftwf_alloc_real(N);
      memmove(newbuf,f->input_buffer.r + f->ilen,(f->impulse_length-1)*sizeof(*f->input_buffer.r));
      f->input_buffer.r = newbuf;
      f->input.r = f->input_buffer.r + f->impulse_length -1;
    }
    break;
  }
  f->wcnt = 0; // In case it's not already reset to 0

  // Append job to queue, wake FFT thread
  pthread_mutex_lock(&f->queue_mutex);
  struct fft_job *jp_prev = NULL;
  for(struct fft_job *jp = f->job_queue; jp != NULL; jp = jp->next)
    jp_prev = jp;

  if(jp_prev)
    jp_prev->next = job;
  else
    f->job_queue = job; // Head of list

  pthread_cond_signal(&f->queue_cond); // Alert FFT thread
  pthread_mutex_unlock(&f->queue_mutex);

  return 0;
}

// Dummy execution of output filter
// Simply wait for a block and then exit
int execute_filter_output_idle(struct filter_out * const slave){
  assert(slave != NULL);
  struct filter_in * const master = slave->master;
  assert(master != NULL);
  // Wait for new block of data
  pthread_mutex_lock(&master->filter_mutex); // Protect access to master->blocknum
  while(slave->blocknum == master->blocknum)
    pthread_cond_wait(&master->filter_cond,&master->filter_mutex);
  if((int)(master->blocknum - slave->blocknum) > ND){
    // Fell behind
    slave->block_drops += (int)(master->blocknum - slave->blocknum) - ND;
    slave->blocknum = master->blocknum;
  } else
    slave->blocknum++;
  pthread_mutex_unlock(&master->filter_mutex); 
  return 0;
}


int execute_filter_output(struct filter_out * const slave,int const rotate){
  assert(slave != NULL);
  if(slave == NULL)
    return -1;

  // We do have to modify date in the master's data structure, notably mutex locks
  // So the derefenced pointer can't be const
  struct filter_in * const master = slave->master;
  assert(master != NULL);

  assert(slave->rev_plan != NULL);
  assert(slave->out_type != NONE);
  assert(master->in_type != NONE);
  assert(master->fdomain != NULL);
  assert(slave->f_fdomain != NULL);  
  assert(slave->response != NULL);
  assert(master->bins > 0);
  assert(slave->bins > 0);

  // DC and positive frequencies up to nyquist frequency are same for all types
  assert(malloc_usable_size(slave->f_fdomain) >= slave->bins * sizeof(*slave->f_fdomain));

  // Wait for new block of data
  // master->blocknum is the next block that the master will produce
  pthread_mutex_lock(&master->filter_mutex); // Protect access to master->blocknum
  while(slave->blocknum == master->blocknum)
    pthread_cond_wait(&master->filter_cond,&master->filter_mutex);
  // We don't modify the master's output data, we create our own
  if((int)(master->blocknum - slave->blocknum) > ND){
    // Fell behind, catch up
    slave->block_drops += (int)(master->blocknum - slave->blocknum) - ND;
    slave->blocknum = master->blocknum - 1;
  }
  complex float const * const fdomain = master->fdomain[slave->blocknum % ND];
  slave->blocknum++;
  pthread_mutex_unlock(&master->filter_mutex); 

  assert(fdomain != NULL);

  pthread_mutex_lock(&slave->response_mutex); // Protect access to response[] array
  assert(malloc_usable_size(slave->response) >= slave->bins * sizeof(*slave->response));
  assert(malloc_usable_size(slave->f_fdomain) >= slave->bins * sizeof(*slave->f_fdomain));

  // Apply frequency response curve
  // Frequency domain is always complex, but the sizes depend on the time domain input/output being real or complex
  if(master->in_type != REAL && slave->out_type != REAL){    // Complex -> complex
    // Rewritten to avoid modulo computations and complex branches inside loops
    int si = slave->bins/2;
    int mi = rotate - si;

    if(mi >= master->bins/2 || mi <= -master->bins/2 - slave->bins){
      // Completely out of range of master; blank output
      memset(slave->f_fdomain,0,slave->bins * sizeof(slave->f_fdomain[0]));
      goto mult_done;
    }
    while(mi < -master->bins/2){
      // Below start of master; zero output
      mi++;
      assert(si >= 0 && si < slave->bins);
      slave->f_fdomain[si++] = 0;
      if(si == slave->bins)
	si = 0; // Wrap to positive output
      assert(si != slave->bins/2); // Completely blank output should be detected by initial check
    }
    if(mi < 0)
      mi += master->bins; // start in neg region of master
    do {    // At least one master bin is in range
      assert(si >= 0 && si < slave->bins);
      assert(mi >= 0 && mi < master->bins);      
      slave->f_fdomain[si] = slave->response[si] * fdomain[mi++];
      si++; // Can't imbed in previous statement; ambiguous
      if(mi == master->bins)
	mi = 0; // Not necessary if it starts positive, and master->bins > slave->bins?
      if(si == slave->bins)
	si = 0;
      if(si == slave->bins/2) 
	goto mult_done; // All done
    } while(mi != master->bins/2); // Until we hit high end of master
    for(;si != slave->bins/2;){
      // Above end of master; zero out remainder
      slave->f_fdomain[si++] = 0;
      if(si == slave->bins)
	si = 0;
    }

  } else if(master->in_type != REAL && slave->out_type == REAL){
    // Complex -> real UNTESTED!
    for(int si=0; si < slave->bins; si++){
      int const mi = si + rotate;
      complex float result = 0;
      if(mi >= -master->bins/2 && mi < master->bins/2)
	result = slave->response[si] * (fdomain[modulo(mi,master->bins)] + conjf(fdomain[modulo(master->bins - mi, master->bins)]));
      slave->f_fdomain[si] = result;
    }
  } else if(master->in_type == REAL && slave->out_type == REAL){
    // Real -> real
    for(int si=0; si < slave->bins; si++){ // All positive frequencies
      int const mi = si + rotate;
      complex float result = 0;
      if(mi >= 0 && mi < master->bins)
	result = slave->response[si] * fdomain[mi];

      slave->f_fdomain[si] = result;
    }
  } else if(master->in_type == REAL && slave->out_type != REAL){
    // Real->complex 
    // This can be tricky. We treat the input as complex with Hermitian symmetry (both positive and negative spectra)
    // We don't allow the output to span the zero input frequency range as this doesn't seem useful
    // The most common case is that m is entirely in range and always < 0 or > 0
    if(rotate >= slave->bins/2 && rotate <= master->bins - slave->bins/2){
      // Positive input spectrum
      // Negative half of output
      int mi = rotate - slave->bins/2;
      for(int si = slave->bins/2; si < slave->bins; si++)
	slave->f_fdomain[si] = slave->response[si] * fdomain[mi++];

      // Positive half of output
      for(int si = 0; si < slave->bins/2; si++)
	slave->f_fdomain[si] = slave->response[si] * fdomain[mi++];
    } else if(-rotate >= slave->bins/2 && -rotate <= master->bins - slave->bins/2){
      // Negative input spectrum
      // Negative half of output
      int mi= -(rotate - slave->bins/2);
      for(int si = slave->bins/2; si < slave->bins; si++)
	slave->f_fdomain[si] = slave->response[si] * conjf(fdomain[mi--]);

      // Positive half of output
      for(int si = 0; si < slave->bins/2; si++)
	slave->f_fdomain[si] = slave->response[si] * conjf(fdomain[mi--]);
    } else {
      // Some of the bins are out of range
      int si = slave->bins/2; // Most negative output frequency
      int mi = -si + rotate;

#if 1 // faster!
      int i;
      for(i = 0; -mi >= master->bins && i < slave->bins; i++){
	slave->f_fdomain[si] = 0;
	si++;
	si = (si == slave->bins) ? 0 : si;
	mi++;
      }
      for(; mi < 0 && i < slave->bins; i++){
	slave->f_fdomain[si] = slave->response[si] * conjf(fdomain[-mi]); // neg freq component is conjugate of corresponding positive freq      
	si++;
	si = (si == slave->bins) ? 0 : si;
	mi++;
      }
      for(; mi < master->bins && i < slave->bins; i++){
	slave->f_fdomain[si] = slave->response[si] * fdomain[mi];
	si++;
	si = (si == slave->bins) ? 0 : si;
	mi++;
      }
      for(; i < slave->bins; i++){
	slave->f_fdomain[si] = 0;
	si++;
	si = (si == slave->bins) ? 0 : si;
      }    
#else    // slower
      for(int i = 0; i < slave->bins; i++){
	complex float result = 0;
	if(abs(mi) < master->bins){
	  // neg freq component is conjugate of corresponding positive freq
	  result = slave->response[si] * (mi >= 0 ?  fdomain[mi] : conjf(fdomain[-mi]));
	}
	slave->f_fdomain[si] = result;
	si++;
	si = (si == slave->bins) ? 0 : si;
	mi++;
      }	  
#endif
    }
  }
 mult_done:;

  pthread_mutex_unlock(&slave->response_mutex); // release response[]
  if(slave->out_type == CROSS_CONJ){
    // hack for ISB; forces negative frequencies onto I, positive onto Q
    assert(malloc_usable_size(slave->f_fdomain) >= slave->bins * sizeof(*slave->f_fdomain));
    for(int p=1,dn=slave->bins-1; p < slave->bins; p++,dn--){
      complex float const pos = slave->f_fdomain[p];
      complex float const neg = slave->f_fdomain[dn];
      
      slave->f_fdomain[p]  = pos + conjf(neg);
      slave->f_fdomain[dn] = neg - conjf(pos);
    }
  }
  fftwf_execute(slave->rev_plan); // Note: c2r version destroys f_fdomain[]
  return 0;
}

int delete_filter_input(struct filter_in ** p){
  if(p == NULL)
    return -1;

  struct filter_in *master = *p;

  if(master == NULL)
    return -1;
  
  if(master->fft_thread)
    pthread_cancel(master->fft_thread);
  if(master->fft_thread2)
    pthread_cancel(master->fft_thread2);

  fftwf_destroy_plan(master->fwd_plan);
  fftwf_free(master->input_buffer.c);
  for(int i=0; i < ND; i++)
    fftwf_free(master->fdomain[i]);
  free(master);
  *p = NULL;
  return 0;
}
int delete_filter_output(struct filter_out **p){
  if(p == NULL)
    return -1;
  struct filter_out *slave = *p;

  if(slave == NULL)
    return 1;
  
  pthread_mutex_destroy(&slave->response_mutex);
  fftwf_destroy_plan(slave->rev_plan);  
  fftwf_free(slave->output_buffer.c);
  fftwf_free(slave->response);
  fftwf_free(slave->f_fdomain);
  free(slave);
  *p = NULL;
  return 0;
}

#if 0 // Available if you ever want them

// Hamming window
const static float hamming(int const n,int const M){
  const float alpha = 25./46;
  const float beta = (1-alpha);

  return alpha - beta * cosf(2*M_PI*n/(M-1));
}

// Hann / "Hanning" window
const static float hann(int const n,int const M){
    return 0.5 - 0.5 * cosf(2*M_PI*n/(M-1));
}

// Exact Blackman window
const static float blackman(int const n,int const M){
  float const a0 = 7938./18608;
  float const a1 = 9240./18608;
  float const a2 = 1430./18608;
  return a0 - a1*cosf(2*M_PI*n/(M-1)) + a2*cosf(4*M_PI*n/(M-1));
}

// Jim Kaiser was in my Bellcore department in the 1980s. Really friendly guy.
// Superseded by make_kaiser() routine that more efficiently computes entire window at once
static float const kaiser(int const n,int const M, float const beta){
  static float old_beta = NAN;
  static float old_inv_denom;

  // Cache old value of beta, since it rarely changes
  // Not thread safe
  if(beta != old_beta){
    old_beta = beta;
    old_inv_denom = 1. / i0(beta);
  }
  const float p = 2.0*n/(M-1) - 1;
  return i0(beta*sqrtf(1-p*p)) * old_inv_denom;
}
#endif

// Compute an entire Kaiser window
// More efficient than repeatedly calling kaiser(n,M,beta)
int make_kaiser(float * const window,int const M,float const beta){
  assert(window != NULL);
  if(window == NULL)
    return -1;

  // Precompute unchanging partial values
  float const inv_denom = 1. / i0(beta); // Inverse of denominator
  float const pc = 2.0 / (M-1);

  // The window is symmetrical, so compute only half of it and mirror
  // this won't compute the middle value in an odd-length sequence
  for(int n = 0; n < M/2; n++){
    float const p = pc * n  - 1;
    window[M-1-n] = window[n] = i0(beta * sqrtf(1-p*p)) * inv_denom;
  }
  // If sequence length is odd, middle value is unity
  if(M & 1)
    window[(M-1)/2] = 1; // The -1 is actually unnecessary

  return 0;
}


// Apply Kaiser window to filter frequency response
// "response" is SIMD-aligned array of N complex floats
// Impulse response will be limited to first M samples in the time domain
// Phase is adjusted so "time zero" (center of impulse response) is at M/2
// L and M refer to the decimated output
int window_filter(int const L,int const M,complex float * const response,float const beta){
  assert(response != NULL);
  if(response == NULL)
    return -1;

  assert(L > 0 && M > 0);

  const int N = L + M - 1;
  assert(malloc_usable_size(response) >= N * sizeof(*response));
  // fftw_plan can overwrite its buffers, so we're forced to make a temp. Ugh.
  complex float * const buffer = fftwf_alloc_complex(N);
  fftwf_plan fwd_filter_plan = fftwf_plan_dft_1d(N,buffer,buffer,FFTW_FORWARD,FFTW_ESTIMATE);
  fftwf_plan rev_filter_plan = fftwf_plan_dft_1d(N,buffer,buffer,FFTW_BACKWARD,FFTW_ESTIMATE);

  // Convert to time domain
  memcpy(buffer,response,N * sizeof(*buffer));
  fftwf_execute(rev_filter_plan);
  fftwf_destroy_plan(rev_filter_plan);
#if 0
  fprintf(stderr,"window_filter raw time domain\n");
  for(int n=0; n < N; n++){
    fprintf(stderr,"%d %lg %lg\n",n,crealf(buffer[n]),cimagf(buffer[n]));
  }
#endif  

  float kaiser_window[M];
  make_kaiser(kaiser_window,M,beta);

#if 0
  for(int m = 0; m < M; m++)
    fprintf(stderr,"kaiser[%d] = %g\n",m,kaiser_window[m]);
#endif  

  // Round trip through FFT/IFFT scales by N
  float const gain = 1./N;
  // Shift to beginning of buffer to make causal; apply window and gain
  for(int n = M - 1; n >= 0; n--)
    buffer[n] = buffer[(n-M/2+N)%N] * kaiser_window[n] * gain;
  // Pad with zeroes on right side
  memset(buffer+M,0,(N-M)*sizeof(*buffer));

#if 0
  fprintf(stderr,"window_filter filter impulse response, shifted, windowed and zero padded\n");
  for(int n=0;n< M;n++)
    fprintf(stderr,"%d %lg %lg\n",n,crealf(buffer[n]),cimagf(buffer[n]));
#endif
  
  // Now back to frequency domain
  fftwf_execute(fwd_filter_plan);
  fftwf_destroy_plan(fwd_filter_plan);

#if 0
  fprintf(stderr,"window_filter filter response amplitude\n");
  for(int n=0;n<N;n++)
    fprintf(stderr,"%d %g %g (%.1f dB)\n",n,crealf(buffer[n]),cimagf(buffer[n]),power2dB(cnrmf(buffer[n])));

  fprintf(stderr,"\n");
#endif
  memcpy(response,buffer,N*sizeof(*response));
  fftwf_free(buffer);
  return 0;
}
// Real-only counterpart to window_filter()
// response[] is only N/2+1 elements containing DC and positive frequencies only
// Negative frequencies are inplicitly the conjugate of the positive frequencies
// L and M refer to the decimated output
int window_rfilter(int const L,int const M,complex float * const response,float const beta){
  assert(response != NULL);
  if(response == NULL)
    return -1;
  assert(L > 0 && M > 0);

  const int N = L + M - 1;

  assert(malloc_usable_size(response) >= (N/2+1)*sizeof(*response));
  complex float * const buffer = fftwf_alloc_complex(N/2 + 1); // plan destroys its input
  assert(buffer != NULL);
  float * const timebuf = fftwf_alloc_real(N);
  assert(timebuf != NULL);
  fftwf_plan fwd_filter_plan = fftwf_plan_dft_r2c_1d(N,timebuf,buffer,FFTW_ESTIMATE);
  assert(fwd_filter_plan != NULL);
  fftwf_plan rev_filter_plan = fftwf_plan_dft_c2r_1d(N,buffer,timebuf,FFTW_ESTIMATE);
  assert(rev_filter_plan != NULL);

  // Convert to time domain
  memcpy(buffer,response,(N/2+1)*sizeof(*buffer));
  fftwf_execute(rev_filter_plan);
  fftwf_destroy_plan(rev_filter_plan);
#if 0
  fprintf(stderr,"window_rfilter impulse response after IFFT before windowing\n");
  for(int n=0;n< M;n++)
    fprintf(stderr,"%d %lg\n",n,timebuf[n]);
#endif


  // Shift to beginning of buffer, apply window and scale (N*N)
  float kaiser_window[M];
  make_kaiser(kaiser_window,M,beta);
  // Round trip through FFT/IFFT scales by N
  float const gain = 1./N;
  for(int n = M - 1; n >= 0; n--)
    timebuf[n] = timebuf[(n-M/2+N)%N] * kaiser_window[n] * gain;
  
  // Pad with zeroes on right side
  memset(timebuf+M,0,(N-M)*sizeof(*timebuf));
#if 0
  printf("window_rfilter impulse response, shifted, windowed and zero padded\n");
  for(int n=0;n< M;n++)
    printf("%d %lg\n",n,timebuf[n]);
#endif
  
  // Now back to frequency domain
  fftwf_execute(fwd_filter_plan);
  fftwf_destroy_plan(fwd_filter_plan);
  fftwf_free(timebuf);
  memcpy(response,buffer,(N/2+1)*sizeof(*response));
  fftwf_free(buffer);
#if 0
  printf("window_rfilter frequency response\n");
  for(int n=0; n < N/2 + 1; n++)
    printf("%d %g %g (%.1f dB)\n",n,crealf(response[n]),cimagf(response[n]),power2dB(cnrmf(response[n])));
#endif

  return 0;
}

// Gain of filter (output / input) on uniform gaussian noise
float const noise_gain(struct filter_out const * const slave){
  if(slave == NULL)
    return NAN;
  struct filter_in const * const master = slave->master;

  float sum = 0;
  for(int i=0;i<slave->bins;i++)
    sum += cnrmf(slave->response[i]);

  // the factor N compensates for the unity gain scaling
  // Amplitude is pre-scaled 1/N for the concatenated (FFT/IFFT) round trip, so the overall power
  // is scaled 1/N^2. Multiplying by N gives us correct power in the frequency domain (just the FFT)

  // The factor of 2 undoes the 1/sqrt(2) amplitude scaling required for unity signal gain in these two modes
  if(slave->out_type == REAL || slave->out_type == CROSS_CONJ)
    return 2 * master->bins * sum;
  else
    return master->bins * sum;
}


// This can occasionally be called with slave == NULL at startup, so don't abort
// NB: 'low' and 'high' are *fractional* frequencies relative to the output sample rate, i.e., -0.5 < f < +0.5
int set_filter(struct filter_out * const slave,float low,float high,float const kaiser_beta){
  if(slave == NULL || isnan(low) || isnan(high) || isnan(kaiser_beta))
    return -1;

  // Swap if necessary
  if(low > high){
    float tmp = low;
    low = high;
    high = tmp;
  }
  // Limit filter range to Nyquist rate
  if(fabsf(low) > 0.5)
    low = (low > 0 ? +1 : -1) * 0.5;
  if(fabsf(high) > 0.5)
    high = (high > 0 ? +1 : -1) * 0.5;

 // Total number of time domain points
  int const N = (slave->out_type == REAL) ? 2 * (slave->bins - 1) : slave->bins;
  int const L = slave->olen;
  int const M = N - L + 1; // Length of impulse response in time domain

  float const gain = (slave->out_type == COMPLEX ? 1.0 : M_SQRT1_2) / (float)slave->master->bins;

  complex float * const response = fftwf_alloc_complex(slave->bins);
  memset(response,0,slave->bins * sizeof(response[0]));
  assert(malloc_usable_size(response) >= (slave->bins) * sizeof(*response));
  for(int n=0; n < slave->bins; n++){
    float const f = n < N/2 ? (float)n / N : (float)(n - N) / N; // neg frequency
    if(f == low || f == high)
      response[n] = gain * M_SQRT1_2; // -3dB
    else if(f > low && f < high)
      response[n] = gain;
    else
      response[n] = 0;
#if 0
    fprintf(stderr,"f = %.3f response[%d] = %.1f\n",f,n,10*log10f(crealf(response[n])));
#endif
  }

  if(slave->out_type == REAL){
    window_rfilter(L,M,response,kaiser_beta);
  } else {
    window_filter(L,M,response,kaiser_beta);
  }

  // Hot swap with existing response, if any, using mutual exclusion
  pthread_mutex_lock(&slave->response_mutex);
  complex float * const tmp = slave->response;
  slave->response = response;
  slave->noise_gain = noise_gain(slave);
  pthread_mutex_unlock(&slave->response_mutex);
  fftwf_free(tmp);

  return 0;
}


