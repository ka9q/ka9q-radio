// filter using fast convolution (overlap-save) and the FFTW3 FFT package
// for the ka9q-radio 'radiod' program
// Generates transfer functions using Kaiser window
// Optional output decimation by integer factor
// Complex input and transfer functions, complex or real output
// Copyright 2017-2023, Phil Karn, KA9Q, karn@ka9q.net

#define LIQUID 0 // Experimental use of parks-mcclellan in filter generation
#define _GNU_SOURCE 1
#include <assert.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdbool.h>
#include <pthread.h>
#include <memory.h>
#include <complex.h>
#include <math.h>
#include <fftw3.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <errno.h>

#if LIQUID
// Otherwise genrates a bazillion warnings
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#include <liquid/liquid.h>
#pragma GCC diagnostic pop
#endif

#include "conf.h"
#include "misc.h"
#include "filter.h"

//#define FILTER_DEBUG 1 # turn on lots of printfs in the window creation code

// Settable from main
char const *Wisdom_file = "/var/lib/ka9q-radio/wisdom";
char const *System_wisdom_file = "/etc/fftw/wisdomf"; // only valid for float version
double FFTW_plan_timelimit = 30.0;
int N_worker_threads = 2;
int N_internal_threads = 1; // Usually most efficient

// Desired FFTW planning level
// If wisdom at this level is not present for some filter, the command to generate it will be logged and FFTW_MEASURE wisdom will be generated at runtime
int FFTW_planning_level = FFTW_PATIENT;

// FFTW3 doc strongly recommends doing your own locking around planning routines, so I now am
static pthread_mutex_t FFTW_planning_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool FFTW_init = false;

// FFT job queue
struct fft_job {
  struct fft_job *next;
  unsigned int jobnum;
  enum filtertype type;
  fftwf_plan plan;
  void *input;
  void *output;
  pthread_mutex_t *completion_mutex; // protects completion_jobnum
  pthread_cond_t *completion_cond;   // Signaled when job is complete
  unsigned int *completion_jobnum;   // Written with jobnum when complete
  bool terminate; // set to tell fft thread to quit
};


#define NTHREADS_MAX 20  // More than I'll ever need
static struct {
  pthread_mutex_t queue_mutex; // protects job_queue
  pthread_cond_t queue_cond;   // signaled when job put on job_queue
  struct fft_job *job_queue;
  pthread_t thread[NTHREADS_MAX];  // Worker threads
} FFT;

static inline int modulo(int x,int const m){
  x = x < 0 ? x + m : x;
  return x > m ? x - m : x;
}


// Custom version of malloc that aligns to a cache line
void *lmalloc(size_t size);

static void suggest(int level,int size,int dir,int clex);
static float noise_gain(struct filter_out const * const slave);
static bool goodchoice(unsigned long n);


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
// out_type = REAL, COMPLEX, CROSS_CONJ (COMPLEX with special processing for ISB) or SPECTRUM (real vector of bin energies)

// All demodulators taking baseband (zero IF) I/Q data require COMPLEX input
// All but SSB require COMPLEX output, with ISB using the special CROSS_CONJ mode
// SSB(CW) could (and did) use the REAL mode since the imaginary component is unneeded, and the c2r IFFT is faster
// Baseband FM audio filtering for de-emphasis and PL separation uses REAL input and output

// If you provide your own filter response, ensure that it drops to nil well below the Nyquist rate
// to prevent aliasing. Remember that decimation reduces the Nyquist rate by the decimation ratio.
// The set_filter() function uses Kaiser windowing for this purpose

// Set up input (master) half of filter
struct filter_in *create_filter_input(struct filter_in *master,int const L,int const M, enum filtertype const in_type){
  assert(L > 0);
  assert(M > 0);
  int const N = L + M - 1;
  int const bins = (in_type == COMPLEX) ? N : (N/2 + 1);
  if(bins < 1)
    return NULL; // Unreasonably small - will segfault. Can happen if sample rate is garbled

  if(master == NULL)
    return NULL;
  if(!goodchoice(N))
    fprintf(stdout,"create_filter_input(L=%d, M=%d): N=%d is not a good blocksize for FFTW3\n",L,M,N);


  for(int i=0; i < ND; i++){
    master->fdomain[i] = lmalloc(sizeof(complex float) * bins);
    master->completed_jobs[i] = (unsigned int)-1; // So startup won't drop any blocks
  }

  assert(master != NULL);
  assert(master != (void *)-1);
  master->bins = bins;
  master->in_type = in_type;
  master->ilen = L;
  master->impulse_length = M;
  pthread_mutex_init(&master->filter_mutex,NULL);
  pthread_cond_init(&master->filter_cond,NULL);

  // FFTW itself always runs with a single thread since multithreading didn't seem to do much good
  // But we have a set of worker threads operating on a job queue to allow a controlled number
  // of independent FFTs to execute at the same time
  if(!FFTW_init){
    fftwf_init_threads();
    bool sr = fftwf_import_system_wisdom();
    fprintf(stdout,"fftwf_import_system_wisdom() %s\n",sr ? "succeeded" : "failed");
    if(!sr){
      if(access(System_wisdom_file,R_OK) == -1){ // Would really like to use AT_EACCESS flag
	fprintf(stdout,"%s not readable: %s\n",System_wisdom_file,strerror(errno));
      }
    }

    bool lr = fftwf_import_wisdom_from_filename(Wisdom_file);
    fprintf(stdout,"fftwf_import_wisdom_from_filename(%s) %s\n",Wisdom_file,lr ? "succeeded" : "failed");
    if(!lr){
      if(access(Wisdom_file,R_OK) == -1){
	fprintf(stdout,"%s not readable: %s\n",Wisdom_file,strerror(errno));
      }
    }
    if(access(Wisdom_file,W_OK) == -1){
      fprintf(stdout,"Warning: %s not writeable, exports will fail: %s\n",Wisdom_file,strerror(errno));
    }

    fftwf_set_timelimit(FFTW_plan_timelimit);
    if(!sr && !lr)
      fprintf(stdout,"No wisdom read, planning FFTs may take up to %'.0lf sec\n",FFTW_plan_timelimit);

    // Start FFT worker thread(s) if not already running
    pthread_mutex_init(&FFT.queue_mutex,NULL);
    pthread_cond_init(&FFT.queue_cond,NULL);
    for(int i=0;i < N_worker_threads;i++){
      if(FFT.thread[i] == (pthread_t)0)
	pthread_create(&FFT.thread[i],NULL,run_fft,NULL);
    }
    FFTW_init = true;

  }
  pthread_mutex_lock(&FFTW_planning_mutex);
  fftwf_plan_with_nthreads(N_internal_threads);

  switch(in_type){
  default:
    pthread_mutex_unlock(&FFTW_planning_mutex);
    assert(0); // shouldn't happen
    return NULL;
  case CROSS_CONJ:
  case COMPLEX:
    master->input_buffer_size = round_to_page(ND * N * sizeof(complex float));
    // Allocate input_buffer_size bytes immediately followed by its mirror
    master->input_buffer = mirror_alloc(master->input_buffer_size);
    master->input_read_pointer.c = master->input_buffer;
    master->input_write_pointer.c = master->input_read_pointer.c + L; // start writing here
    master->input_read_pointer.r = NULL;
    master->input_write_pointer.r = NULL;
    master->fwd_plan = fftwf_plan_dft_1d(N, master->input_read_pointer.c, master->fdomain[0], FFTW_FORWARD, FFTW_WISDOM_ONLY|FFTW_planning_level);
    if(master->fwd_plan == NULL){
      suggest(FFTW_planning_level,N,FFTW_FORWARD,COMPLEX);
      master->fwd_plan = fftwf_plan_dft_1d(N, master->input_read_pointer.c, master->fdomain[0], FFTW_FORWARD, FFTW_MEASURE);
    }
    if(fftwf_export_wisdom_to_filename(Wisdom_file) == 0)
      fprintf(stdout,"fftwf_export_wisdom_to_filename(%s) of cof%d failed\n",Wisdom_file,N);
    break;
  case REAL:
    master->input_buffer_size = round_to_page(ND * N * sizeof(float));
    master->input_buffer = mirror_alloc(master->input_buffer_size);
    master->input_read_pointer.r = master->input_buffer;
    master->input_write_pointer.r = master->input_read_pointer.r + L; // start writing here
    master->input_read_pointer.c = NULL;
    master->input_write_pointer.c = NULL;
    master->fwd_plan = fftwf_plan_dft_r2c_1d(N, master->input_read_pointer.r, master->fdomain[0], FFTW_WISDOM_ONLY|FFTW_planning_level);
    if(master->fwd_plan == NULL){
      suggest(FFTW_planning_level,N,FFTW_FORWARD,REAL);
      master->fwd_plan = fftwf_plan_dft_r2c_1d(N, master->input_read_pointer.r, master->fdomain[0], FFTW_MEASURE);
    }
    if(fftwf_export_wisdom_to_filename(Wisdom_file) == 0)
      fprintf(stdout,"fftwf_export_wisdom_to_filename(%s) of rof%d failed\n",Wisdom_file,N);

    break;
  }
  pthread_mutex_unlock(&FFTW_planning_mutex);

  return master;
}
// Set up output (slave) side of filter (possibly one of several sharing the same input master)
// These output filters should be deleted before their masters
// Segfault will occur if filter_in is deleted and execute_filter_output is executed
// Special case: for type == SPECTRUM, 'len' is the number of FFT bins, not the number of output time domain points (since there aren't any)
struct filter_out *create_filter_output(struct filter_out *slave,struct filter_in * master,complex float * const response,int len, enum filtertype const out_type){
  assert(master != NULL);
  if(master == NULL)
    return NULL;

  assert(slave != NULL);
  if(slave == NULL)
    return NULL;

  assert(len > 0);

  // Share all but output fft bins, response, output and output type
  slave->master = master;
  slave->out_type = out_type;

  // N / L = Total FFT points / time domain points
  float const overlap = (float)(master->ilen + master->impulse_length - 1) / master->ilen;
  slave->response = response;
  slave->noise_gain = (response == NULL) ? NAN : noise_gain(slave);

  pthread_mutex_lock(&FFTW_planning_mutex);
  fftwf_plan_with_nthreads(1); // IFFTs are always small, use only one internal thread
  switch(slave->out_type){
  default:
  case COMPLEX:
  case CROSS_CONJ:
    {
      // This needs fixing for cases where len * overlap is not an integer
      // I think the IFFT needs to be scaled up until its length is an integer,
      // proportionately more samples need to be dropped from the start,
      // and some number of (zero, or near zero) samples need to be dropped from the end
      // This will be zero-padding in reverse
      slave->olen = len;
      slave->bins = ceilf(len * overlap); // Total number of time-domain FFT points including overlap
      slave->fdomain = lmalloc(sizeof(complex float) * slave->bins);
      slave->output_buffer.c = lmalloc(sizeof(complex float) * slave->bins);
      assert(slave->output_buffer.c != NULL);
      slave->output_buffer.r = NULL; // catch erroneous references
      slave->output.c = slave->output_buffer.c + slave->bins - len;
      if((slave->rev_plan = fftwf_plan_dft_1d(slave->bins,slave->fdomain,slave->output_buffer.c,FFTW_BACKWARD,FFTW_WISDOM_ONLY|FFTW_planning_level)) == NULL){
	suggest(FFTW_planning_level,slave->bins,FFTW_BACKWARD,COMPLEX);
	slave->rev_plan = fftwf_plan_dft_1d(slave->bins,slave->fdomain,slave->output_buffer.c,FFTW_BACKWARD,FFTW_MEASURE);
      }
    }
    if(fftwf_export_wisdom_to_filename(Wisdom_file) == 0)
      fprintf(stdout,"fftwf_export_wisdom_to_filename(%s) failed\n",Wisdom_file);
    break;
  case SPECTRUM: // Like complex, but no IFFT or output time domain buffer
    {
      slave->olen = 0;
      slave->bins = len;
      slave->fdomain = lmalloc(sizeof(complex float) * slave->bins); // User reads this directly
      assert(slave->fdomain != NULL);
      // Note: No time domain buffer; slave->output, etc, all NULL
      // Also don't set up an IFFT
    }
    break;
  case REAL:
    {
      slave->olen = len;
      slave->bins = ceilf(len * overlap) / 2 + 1;
      slave->fdomain = lmalloc(sizeof(complex float) * slave->bins);
      assert(slave->fdomain != NULL);
      slave->output_buffer.r = lmalloc(sizeof(float) * slave->bins);
      assert(slave->output_buffer.r != NULL);
      slave->output_buffer.c = NULL;
      slave->output.r = slave->output_buffer.r + slave->bins - len;
      if((slave->rev_plan = fftwf_plan_dft_c2r_1d(slave->bins,slave->fdomain,slave->output_buffer.r,FFTW_WISDOM_ONLY|FFTW_planning_level)) == NULL){
	suggest(FFTW_planning_level,slave->bins,FFTW_BACKWARD,REAL);
	slave->rev_plan = fftwf_plan_dft_c2r_1d(slave->bins,slave->fdomain,slave->output_buffer.r,FFTW_MEASURE);
      }
    }
    if(fftwf_export_wisdom_to_filename(Wisdom_file) == 0)
      fprintf(stdout,"fftwf_export_wisdom_to_filename(%s) failed\n",Wisdom_file);
    break;
  }
  if(!goodchoice(slave->bins))
    fprintf(stdout,"create_filter_output: N=%d is not a good blocksize for FFTW3\n",slave->bins);

  slave->next_jobnum = master->next_jobnum;
  pthread_mutex_unlock(&FFTW_planning_mutex);
  return slave;
}

// Worker thread(s) that actually execute FFTs
// Used for input FFTs since they tend to be large and CPU-consuming
// Lets the input thread process the next input block in parallel on another core
// Frees the input buffer and the job descriptor when done
void *run_fft(void *p){
  pthread_detach(pthread_self());
  pthread_setname("fft");
  (void)p; // Unused

  realtime();

  while(true){
    // Get next job
    pthread_mutex_lock(&FFT.queue_mutex);
    while(FFT.job_queue == NULL)
      pthread_cond_wait(&FFT.queue_cond,&FFT.queue_mutex);
    struct fft_job *job = FFT.job_queue;
    FFT.job_queue = job->next;
    pthread_mutex_unlock(&FFT.queue_mutex);

    if(job->input != NULL && job->output != NULL && job->plan != NULL){
      switch(job->type){
      case COMPLEX:
      case CROSS_CONJ:
	fftwf_execute_dft(job->plan,job->input,job->output);
	break;
      case REAL:
	fftwf_execute_dft_r2c(job->plan,job->input,job->output);
	break;
      default:
	break;
      }
    }
    // Signal we're done with this job
    if(job->completion_mutex)
      pthread_mutex_lock(job->completion_mutex);
    if(job->completion_jobnum)
      *job->completion_jobnum = job->jobnum;
    if(job->completion_cond)
      pthread_cond_broadcast(job->completion_cond);
    if(job->completion_mutex)
      pthread_mutex_unlock(job->completion_mutex);
    // Do NOT destroy job->completion_cond and completion_mutex here, they continue to exist

    bool const terminate = job->terminate; // Don't use job pointer after free
    FREE(job);
    if(terminate)
      break; // Terminate after this job
  }
  return NULL;
}


// Execute the input side of a filter: set up a job for the FFT worker threads and enqueue it
int execute_filter_input(struct filter_in * const f){
  assert(f != NULL);
  if(f == NULL)
    return -1;

  // We use the FFTW3 functions that specify the input and output arrays
  // Execute the FFT in separate worker threads
  struct fft_job * const job = calloc(1,sizeof(struct fft_job));
  assert(job != NULL);
  job->jobnum = f->next_jobnum++;
  job->output = f->fdomain[job->jobnum % ND];
  job->type = f->in_type;
  job->plan = f->fwd_plan;
  job->completion_mutex = &f->filter_mutex;
  job->completion_jobnum = &f->completed_jobs[job->jobnum % ND];
  job->completion_cond = &f->filter_cond;

  // Set up the job and next input buffer
  // We're assuming that the time-domain pointers we're passing to the FFT are always aligned the same
  // as we increment the FFT pointer by f->ilen (L) modulo the mirror buffer size.
  // They seem to be as long as ilen (L) has several factors of 2. For the real->complex transform,
  // each element is 4 bytes long, so if L is divisible by 8 then the pointers will be aligned to 64 bytes,
  // the usual size of a cache line. For complex->complex transforms, L has to be divisible by 4.
  switch(f->in_type){
  default:
  case CROSS_CONJ:
  case COMPLEX:
    job->input = f->input_read_pointer.c;
    f->input_read_pointer.c += f->ilen;
    mirror_wrap((void *)&f->input_read_pointer.c,f->input_buffer,f->input_buffer_size);
    break;
  case REAL:
    job->input = f->input_read_pointer.r;
    f->input_read_pointer.r += f->ilen;
    mirror_wrap((void *)&f->input_read_pointer.r,f->input_buffer,f->input_buffer_size);
    break;
  }
  assert(job->input != NULL); // Should already be allocated in create_filter_input, or in our last call

  // Append job to worker queue, wake FFT worker thread
  struct fft_job *jp_prev = NULL;
  pthread_mutex_lock(&FFT.queue_mutex);
  for(struct fft_job *jp = FFT.job_queue; jp != NULL; jp = jp->next)
    jp_prev = jp;

  if(jp_prev)
    jp_prev->next = job;
  else
    FFT.job_queue = job; // Head of list

  pthread_cond_signal(&FFT.queue_cond); // Alert only one FFT worker
  pthread_mutex_unlock(&FFT.queue_mutex);

  return 0;
}

// Execute the output side of a filter:
// 1 - wait for a forward FFT job to complete
//     frequency domain data is in a circular queue ND buffers deep to tolerate scheduling jitter
// 2 - multiply the selected frequency bin range by the filter frequency response
//     This is the hard part; handle all combinations of real/complex input/output, wraparound, etc
// 3 - convert back to time domain with IFFT
int execute_filter_output(struct filter_out * const slave,int const rotate){
  assert(slave != NULL);
  if(slave == NULL)
    return -1;

  // We do have to modify the master's data structure, notably mutex locks
  // So the derefenced pointer can't be const
  struct filter_in * const master = slave->master;
  assert(master != NULL);
  if(master == NULL)
    return -1;

  assert(slave->out_type == SPECTRUM || slave->rev_plan != NULL);
  assert(slave->out_type != NONE);
  assert(master->in_type != NONE);
  assert(master->fdomain != NULL);
  assert(slave->fdomain != NULL);
  assert(master->bins > 0);
  assert(slave->bins > 0);

  // DC and positive frequencies up to nyquist frequency are same for all types
  assert(malloc_usable_size(slave->fdomain) >= slave->bins * sizeof(*slave->fdomain));

  // Wait for new block of output data
  pthread_mutex_lock(&master->filter_mutex);
  int blocks_to_wait = slave->next_jobnum - master->completed_jobs[slave->next_jobnum % ND];
  if(blocks_to_wait <= -ND){
    // Circular buffer overflow (for us)
    slave->next_jobnum -= blocks_to_wait;
    slave->block_drops -= blocks_to_wait;
  }
  while((int)(slave->next_jobnum - master->completed_jobs[slave->next_jobnum % ND]) > 0)
    pthread_cond_wait(&master->filter_cond,&master->filter_mutex);
  // We don't modify the master's output data, we create our own
  complex float const * const fdomain = master->fdomain[slave->next_jobnum % ND];
  slave->next_jobnum++;
  pthread_mutex_unlock(&master->filter_mutex);

  assert(fdomain != NULL);
  if(fdomain == NULL)
    return -1;

  // Copy the requested frequency segment in preparation for multiplication by the filter response
  // Although frequency domain data is always complex, this is complicated because
  // we have to handle the four combinations of the filter input and output time domain data
  // being either real or complex.

  // In ka9q-radio the input depends on the SDR front end, while the output is complex
  // (even for SSB) because of the fine tuning frequency shift after conversion
  // back to the time domain. So while real output is supported it is not well tested.
  if(master->in_type != REAL && slave->out_type != REAL){    // Complex -> complex
    // Rewritten to avoid modulo computations and complex branches inside loops
    int si = slave->bins/2;
    int mi = rotate - si;

    if(mi >= master->bins/2 || mi <= -master->bins/2 - slave->bins){
      // Completely out of range of master; blank output
      memset(slave->fdomain,0,slave->bins * sizeof(slave->fdomain[0]));
      goto copy_done;
    }
    while(mi < -master->bins/2){
      // Below start of master; zero output
      mi++;
      assert(si >= 0 && si < slave->bins);
      slave->fdomain[si++] = 0;
      if(si == slave->bins)
	si = 0; // Wrap to positive output
      assert(si != slave->bins/2); // Completely blank output should be detected by initial check
    }
    if(mi < 0)
      mi += master->bins; // start in neg region of master
    do {    // At least one master bin is in range
      assert(si >= 0 && si < slave->bins);
      assert(mi >= 0 && mi < master->bins);
      slave->fdomain[si++] = fdomain[mi++];
      if(mi == master->bins)
	mi = 0; // Not necessary if it starts positive, and master->bins > slave->bins?
      if(si == slave->bins)
	si = 0;
      if(si == slave->bins/2)
	goto copy_done; // All done
    } while(mi != master->bins/2); // Until we hit high end of master
    while(si != slave->bins/2){
      // Above end of master; zero out remainder
      slave->fdomain[si++] = 0;
      if(si == slave->bins)
	si = 0;
    }
  } else if(master->in_type != REAL && slave->out_type == REAL){
    // Complex -> real UNTESTED!
    for(int si=0; si < slave->bins; si++){
      int const mi = si + rotate;
      complex float result = 0;
      if(mi >= -master->bins/2 && mi < master->bins/2)
	result = (fdomain[modulo(mi,master->bins)] + conjf(fdomain[modulo(master->bins - mi, master->bins)]));
      slave->fdomain[si] = result;
    }
  } else if(master->in_type == REAL && slave->out_type == REAL){
    // Real -> real
    for(int si=0; si < slave->bins; si++){ // All positive frequencies
      int const mi = si + rotate;
      complex float result = 0;
      if(mi >= 0 && mi < master->bins)
	result = fdomain[mi];

      slave->fdomain[si] = result;
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
	slave->fdomain[si] = fdomain[mi++];

      // Positive half of output
      for(int si = 0; si < slave->bins/2; si++)
	slave->fdomain[si] = fdomain[mi++];
    } else if(-rotate >= slave->bins/2 && -rotate <= master->bins - slave->bins/2){
      // Negative input spectrum
      // Negative half of output
      int mi = -(rotate - slave->bins/2);
      for(int si = slave->bins/2; si < slave->bins; si++)
	slave->fdomain[si] = conjf(fdomain[mi--]);

      // Positive half of output
      for(int si = 0; si < slave->bins/2; si++)
	slave->fdomain[si] = conjf(fdomain[mi--]);
    } else {
      // Some of the bins are out of range
      int si = slave->bins/2; // Most negative output frequency
      int mi = -si + rotate;

#if 1 // faster!
      int i;
      for(i = 0; -mi >= master->bins && i < slave->bins; i++,mi++){
	slave->fdomain[si++] = 0;
	si = (si == slave->bins) ? 0 : si;
      }
      for(; mi < 0 && i < slave->bins; i++,mi++){
	// neg freq component is conjugate of corresponding positive freq
	slave->fdomain[si++] = conjf(fdomain[-mi]);
	si = (si == slave->bins) ? 0 : si;
      }
      for(; mi < master->bins && i < slave->bins; i++,mi++){
	slave->fdomain[si++] = fdomain[mi];
	si = (si == slave->bins) ? 0 : si;
      }
      for(; i < slave->bins; i++){
	slave->fdomain[si++] = 0;
	si = (si == slave->bins) ? 0 : si;
      }
#else    // slower
      for(int i = 0; i < slave->bins; i++,mi++){
	complex float result = 0;
	if(abs(mi) < master->bins){
	  // neg freq component is conjugate of corresponding positive freq
	  result = (mi >= 0 ?  fdomain[mi] : conjf(fdomain[-mi]));
	}
	slave->fdomain[si++] = result;
	si = (si == slave->bins) ? 0 : si;
      }
#endif
    }
  }
 copy_done:;

  // Apply channel filter response
  if(slave->response != NULL){
    assert(malloc_usable_size(slave->response) >= slave->bins * sizeof(*slave->response));
    assert(malloc_usable_size(slave->fdomain) >= slave->bins * sizeof(*slave->fdomain));

    pthread_mutex_lock(&slave->response_mutex); // Don't let it change while we're using it
    for(int i=0; i < slave->bins; i++)
      slave->fdomain[i] *= slave->response[i];
    pthread_mutex_unlock(&slave->response_mutex); // release response[]
  }

  if(slave->out_type == CROSS_CONJ){
    // hack for ISB; forces negative frequencies onto I, positive onto Q
    // Don't really know how to use this anymore; it's incompatible with fine tuning in the time domain
    // Re-implementing ISB will probably require a filter for each sideband
    // Also probably generates time domain ripple effects due to the sharp notch at DC
    assert(malloc_usable_size(slave->fdomain) >= slave->bins * sizeof(*slave->fdomain));
    for(int p=1,dn=slave->bins-1; p < slave->bins; p++,dn--){
      complex float const pos = slave->fdomain[p];
      complex float const neg = slave->fdomain[dn];

      slave->fdomain[p]  = pos + conjf(neg);
      slave->fdomain[dn] = neg - conjf(pos);
    }
    slave->fdomain[0] = 0; // Must be a null at DC
  }
  // And finally back to the time domain (except in spectrum mode)
  if(slave->out_type != SPECTRUM)
    fftwf_execute(slave->rev_plan); // Note: c2r version destroys fdomain[]
  return 0;
}

#if 0
// Send terminate job to FFT thread
// We never actually kill a FFT thread (which is why it's turned off) but it's here if we ever do
static void terminate_fft(struct filter_in *f){
  struct fft_job * const job = calloc(1,sizeof(struct fft_job));
  assert(job != NULL);
  job->terminate = true;
  // Append job to queue, wake FFT thread
  pthread_mutex_lock(&FFT.queue_mutex);
  struct fft_job *jp_prev = NULL;
  for(struct fft_job *jp = FFT.job_queue; jp != NULL; jp = jp->next)
    jp_prev = jp;

  if(jp_prev)
    jp_prev->next = job;
  else
    FFT.job_queue = job; // Head of list

  pthread_cond_broadcast(&FFT.queue_cond); // Alert FFT thread
  pthread_mutex_unlock(&FFT.queue_mutex);
}
#endif

int delete_filter_input(struct filter_in * master){
  if(master == NULL)
    return -1;

  pthread_mutex_destroy(&master->filter_mutex);
  pthread_cond_destroy(&master->filter_cond);
  fftwf_destroy_plan(master->fwd_plan);
  master->fwd_plan = NULL;
  mirror_free(&master->input_buffer,master->input_buffer_size); // Don't use free() !

  for(int i=0; i < ND; i++)
    FREE(master->fdomain[i]);
  memset(master,0,sizeof(*master)); // Wipe it all
  return 0;

}
int delete_filter_output(struct filter_out *slave){
  if(slave == NULL)
    return -1;

  pthread_mutex_destroy(&slave->response_mutex);
  fftwf_destroy_plan(slave->rev_plan);
  slave->rev_plan = NULL;
  FREE(slave->output_buffer.c);
  FREE(slave->output_buffer.r);
  FREE(slave->response);
  FREE(slave->fdomain);
  memset(slave,0,sizeof(*slave)); // Wipe it all
  return 0;
}


// Gain of filter (output / input) on uniform gaussian noise
static float noise_gain(struct filter_out const * const slave){
  if(slave == NULL)
    return NAN;
  struct filter_in const * const master = slave->master;
  if(master == NULL)
    return NAN;

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


#if LIQUID
int set_filter(struct filter_out * const slave,float low,float high,float const kaiser_beta){
  if(slave == NULL || isnan(low) || isnan(high) || isnan(kaiser_beta) || slave->master == NULL)
    return -1;

  // Limit filter range to Nyquist rate
  // Limit filter range to Nyquist rate
  low = low < -0.5 ? -0.5 : low > +0.5 ? +0.5 : low;
  high = high < -0.5 ? -0.5 : high > +0.5 ? +0.5 : high;

  // Total number of time domain points
  int const N = (slave->out_type == REAL) ? 2 * (slave->bins - 1) : slave->bins;
  int const L = slave->olen;
  int const M = N - L + 1; // Length of impulse response in time domain

  float const gain = (slave->out_type == COMPLEX ? 1.0 : M_SQRT1_2) / (float)slave->master->bins;
  complex float * const response = lmalloc(sizeof(*response) * N);
  assert(response != NULL);
  float real_coeff[M];
  memset(real_coeff,0,sizeof(real_coeff));
  float const bw = fabsf(high - low);
  float const fc = (high + low)/2; // center

#define REMEZ 1
#if REMEZ
  int const num_bands = 2;
  float bands[2*num_bands];
  bands[0] = 0;  bands[1] = bw/2 - 0.01; // for now
  bands[1] = bands[1] < 0 ? 0 : bands[1];
  bands[2] = bw/2 + 0.01; bands[3] = 0.5;
  bands[2] = bands[2] > 0.5 ? 0.5 : bands[2];
  float des[num_bands] = { 1.0, 0.0 };
  float weights[num_bands] = { 1.0, 1.0 };
  liquid_firdespm_wtype wtype[num_bands] = {
    LIQUID_FIRDESPM_FLATWEIGHT,
    LIQUID_FIRDESPM_EXPWEIGHT,
  };
  firdespm_run(M,num_bands,bands,des,weights,wtype,LIQUID_FIRDESPM_BANDPASS,real_coeff);
#else // liquid kaiser filter gen
  // Create a real filter, we'll shift it later
  float const atten = 108.52; // yields β = 11 by his empirical formula (my default)
  float const mu = 0;

  // The real filter's bandwidth is doubled because it includes negative frequencies
  liquid_firdes_kaiser(M,bw/2,atten,mu,real_coeff);
#endif // kaiser

  pthread_mutex_lock(&FFTW_planning_mutex);
  fftwf_plan_with_nthreads(1);
  fftwf_plan plan = fftwf_plan_dft_1d(N,response,response,FFTW_FORWARD,FFTW_MEASURE); // doesn't need to be fast
  assert(plan != NULL);
  if(fftwf_export_wisdom_to_filename(Wisdom_file) == 0)
    fprintf(stdout,"fftwf_export_wisdom_to_filename(%s) of cif%d failed\n",Wisdom_file,N);
  pthread_mutex_unlock(&FFTW_planning_mutex);

  // Multiply real coefficients by complex exponential for frequency shift, place in FFT time-domain buffer
  memset(response,0,sizeof(*response) * N);

  for(int i=0; i < M; i++)
    response[i] = gain * real_coeff[i] * cispif(2 * fc * (i - M/2));

  fftwf_execute_dft(plan,response,response);
  fftwf_destroy_plan(plan);
  plan = NULL;

#if 0
  printf("response:");
  for(int i=0; i < N; i++)
    printf(" %g+j%g",crealf(response[i]),cimag(response[i]));
  printf("\n");
#endif


  // Hot swap with existing response, if any, using mutual exclusion
  pthread_mutex_lock(&slave->response_mutex);
  complex float * const tmp = slave->response;
  slave->response = response;
  slave->noise_gain = noise_gain(slave);
  pthread_mutex_unlock(&slave->response_mutex);
  free(tmp);
  return 0;
}

#else // Use my windowed Kaiser generation

// Compute an entire Kaiser window
// More efficient than repeatedly calling kaiser(n,M,beta)
static int make_kaiser(float * const window,int const M,float const beta){
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
static int window_filter(int const L,int const M,complex float * const response,float const beta){
  assert(response != NULL);
  if(response == NULL)
    return -1;

  assert(L > 0 && M > 0);

  int const N = L + M - 1;
  assert(malloc_usable_size(response) >= N * sizeof(*response));
  // fftw_plan can overwrite its buffers, so we're forced to make a temp. Ugh.
  complex float * const buffer = lmalloc(sizeof(complex float) * N);
  pthread_mutex_lock(&FFTW_planning_mutex);
  fftwf_plan_with_nthreads(1);
  fftwf_plan fwd_filter_plan = fftwf_plan_dft_1d(N,buffer,buffer,FFTW_FORWARD,FFTW_ESTIMATE);
  assert(fwd_filter_plan != NULL);
  fftwf_plan_with_nthreads(1);
  fftwf_plan rev_filter_plan = fftwf_plan_dft_1d(N,buffer,buffer,FFTW_BACKWARD,FFTW_ESTIMATE);
  assert(rev_filter_plan != NULL);
  if(fftwf_export_wisdom_to_filename(Wisdom_file) == 0)
    fprintf(stdout,"fftwf_export_wisdom_to_filename(%s) of cif%d and cib%d failed\n",
	    Wisdom_file,N,N);
  pthread_mutex_unlock(&FFTW_planning_mutex);

  // Convert to time domain
  memcpy(buffer,response,N * sizeof(*buffer));
  fftwf_execute(rev_filter_plan);
  fftwf_destroy_plan(rev_filter_plan);
  rev_filter_plan = NULL;
#if FILTER_DEBUG
  fprintf(stderr,"window_filter raw time domain\n");
  for(int n=0; n < N; n++){
    fprintf(stderr,"%d %lg %lg\n",n,crealf(buffer[n]),cimagf(buffer[n]));
  }
#endif

  float kaiser_window[M];
  make_kaiser(kaiser_window,M,beta);

#if FILTER_DEBUG
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

#if FILTER_DEBUG
  fprintf(stderr,"window_filter filter impulse response, shifted, windowed and zero padded\n");
  for(int n=0;n< M;n++)
    fprintf(stderr,"%d %lg %lg\n",n,crealf(buffer[n]),cimagf(buffer[n]));
#endif

  // Now back to frequency domain
  fftwf_execute(fwd_filter_plan);
  fftwf_destroy_plan(fwd_filter_plan);
  fwd_filter_plan = NULL;
#if FILTER_DEBUG
  fprintf(stderr,"window_filter filter response amplitude\n");
  for(int n=0;n<N;n++)
    fprintf(stderr,"%d %g %g (%.1f dB)\n",n,crealf(buffer[n]),cimagf(buffer[n]),power2dB(cnrmf(buffer[n])));

  fprintf(stderr,"\n");
#endif
  memcpy(response,buffer,N*sizeof(*response));
  free(buffer);
  return 0;
}
// Real-only counterpart to window_filter()
// response[] is only N/2+1 elements containing DC and positive frequencies only
// Negative frequencies are inplicitly the conjugate of the positive frequencies
// L and M refer to the decimated output
static int window_rfilter(int const L,int const M,complex float * const response,float const beta){
  assert(response != NULL);
  if(response == NULL)
    return -1;
  assert(L > 0 && M > 0);

  int const N = L + M - 1;

  assert(malloc_usable_size(response) >= (N/2+1)*sizeof(*response));
  complex float * const buffer = lmalloc(sizeof(complex float) * (N/2 + 1)); // plan destroys its input
  assert(buffer != NULL);
  float * const timebuf = lmalloc(sizeof(float) * N);
  assert(timebuf != NULL);
  pthread_mutex_lock(&FFTW_planning_mutex);
  fftwf_plan_with_nthreads(1);
  // Hopefully these won't take too long, we'll remember the wisdom anyway
  fftwf_plan fwd_filter_plan = fftwf_plan_dft_r2c_1d(N,timebuf,buffer,FFTW_MEASURE);
  assert(fwd_filter_plan != NULL);
  fftwf_plan_with_nthreads(1);
  fftwf_plan rev_filter_plan = fftwf_plan_dft_c2r_1d(N,buffer,timebuf,FFTW_MEASURE);
  assert(rev_filter_plan != NULL);
  if(fftwf_export_wisdom_to_filename(Wisdom_file) == 0)
    fprintf(stdout,"fftwf_export_wisdom_to_filename(%s) of rof%d and rob%d failed\n",
	    Wisdom_file,N,N);
  pthread_mutex_unlock(&FFTW_planning_mutex);

  // Convert to time domain
  memcpy(buffer,response,(N/2+1)*sizeof(*buffer));
  fftwf_execute(rev_filter_plan);
  fftwf_destroy_plan(rev_filter_plan);
#if FILTER_DEBUG
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
#if FILTER_DEBUG
  printf("window_rfilter impulse response, shifted, windowed and zero padded\n");
  for(int n=0;n< M;n++)
    printf("%d %lg\n",n,timebuf[n]);
#endif

  // Now back to frequency domain
  fftwf_execute(fwd_filter_plan);
  fftwf_destroy_plan(fwd_filter_plan);
  free(timebuf);
  memcpy(response,buffer,(N/2+1)*sizeof(*response));
  free(buffer);
#if FILTER_DEBUG
  printf("window_rfilter frequency response\n");
  for(int n=0; n < N/2 + 1; n++)
    printf("%d %g %g (%.1f dB)\n",n,crealf(response[n]),cimagf(response[n]),power2dB(cnrmf(response[n])));
#endif

  return 0;
}

// This can occasionally be called with slave == NULL at startup, so don't abort
// NB: 'low' and 'high' are *fractional* frequencies relative to the output sample rate, i.e., -0.5 < f < +0.5
// If invoked on a demod that hasn't run yet, slave->master will be NULL so check for that and quit;
// the filter should get set up when it actually starts (thanks N5TNL for bug report)
int set_filter(struct filter_out * const slave,float low,float high,float const kaiser_beta){
  if(slave == NULL || isnan(low) || isnan(high) || isnan(kaiser_beta) || slave->master == NULL)
    return -1;

  // Swap if necessary
  if(low > high){
    float tmp = low;
    low = high;
    high = tmp;
  }
  // Limit filter range to Nyquist rate
  low = low < -0.5 ? -0.5 : low > +0.5 ? +0.5 : low;
  high = high < -0.5 ? -0.5 : high > +0.5 ? +0.5 : high;

 // Total number of time domain points
  int const N = (slave->out_type == REAL) ? 2 * (slave->bins - 1) : slave->bins;
  int const L = slave->olen;
  int const M = N - L + 1; // Length of impulse response in time domain

  float const gain = (slave->out_type == COMPLEX ? 1.0 : M_SQRT1_2) / (float)slave->master->bins;

  complex float * const response = lmalloc(sizeof(complex float) * slave->bins);
  assert(response != NULL);
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
  free(tmp);
  return 0;
}
#endif

int write_cfilter(struct filter_in *f, complex float const *buffer,int size){
  if(f == NULL)
    return -1;
  if(sizeof(*buffer) * (f->wcnt + size) >= f->input_buffer_size)
    return -1; // Write is so large it wrapped the input buffer. Should handle this more cleanly

  // Even though writes can now wrap past the primary copy of the input buffer, their start should always be in it
  assert((void *)(f->input_write_pointer.c) >= f->input_buffer);
  assert((void *)(f->input_write_pointer.c) < f->input_buffer + f->input_buffer_size);

  if(buffer != NULL)
    memcpy(f->input_write_pointer.c, buffer, size * sizeof(*buffer));
  f->input_write_pointer.c += size;
  mirror_wrap((void *)&f->input_write_pointer.c, f->input_buffer, f->input_buffer_size);
  f->wcnt += size;
  while(f->wcnt >= f->ilen){
    f->wcnt -= f->ilen;
    execute_filter_input(f);
  }
  return size;
}

int write_rfilter(struct filter_in *f, float const *buffer,int size){
  if(f == NULL)
    return -1;
  if(sizeof(*buffer) * (f->wcnt + size) >= f->input_buffer_size)
    return -1; // Write is so large it wrapped the input buffer. Should handle this more cleanly

  // Even though writes can now wrap past the primary copy of the input buffer, their start should always be in it
  assert((void *)(f->input_write_pointer.r) >= f->input_buffer);
  assert((void *)(f->input_write_pointer.r) < f->input_buffer + f->input_buffer_size);

  if(buffer != NULL)
    memcpy(f->input_write_pointer.r, buffer, size * sizeof(*buffer));
  f->input_write_pointer.r += size;
  mirror_wrap((void *)&f->input_write_pointer.r, f->input_buffer, f->input_buffer_size);
  f->wcnt += size;
  while(f->wcnt >= f->ilen){
    f->wcnt -= f->ilen;
    execute_filter_input(f);
  }
  return size;
};

// Custom version of malloc that aligns to a cache line
// This is 64 bytes on most modern machines, including the x86 and the ARM 2711 (Pi 4)
// This is stricter than a complex float or double, which is required by fftwf/fftw
void *lmalloc(size_t size){
  void *ptr;
  int r;
  if((r = posix_memalign(&ptr,64,size)) == 0){
    assert(ptr != NULL);
    return ptr;
  }
  errno = r;
  assert(false);
  return NULL;
}
// Suggest running fftwf-wisdom to generate some FFTW3 wisdom
static void suggest(int level,int size,int dir,int clex){
  const char *opt = NULL;

  switch(level){
  case FFTW_ESTIMATE:
    opt = " -e";
    break;
  case FFTW_MEASURE:
    opt = " -m";
    break;
  case FFTW_PATIENT: // is the default
    opt = "";
    break;
  case FFTW_EXHAUSTIVE:
    opt = " -x";
    break;
  }
  fprintf(stdout,"suggest running \"fftwf-wisdom -v%s -T 1 -w %s/wisdom -o /tmp/wisdomf %co%c%d\", then \"mv /tmp/wisdomf /etc/fftw/wisdomf\" *if* larger than current file. This will take time.\n",
	  opt,
	  VARDIR,
	  clex == COMPLEX ? 'c' : 'r',
	  dir == FFTW_FORWARD ? 'f' : 'b',
	  size);
}
#if 0 // Available if you ever want them

// Hamming window
const static float hamming(int const n,int const M){
  const float alpha = 25./46.;
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
  float const p = 2.0*n/(M-1) - 1;
  return i0(beta*sqrtf(1-p*p)) * old_inv_denom;
}
#endif

// Assist with choosing good blocksizes for FFTW3
static const int small_primes[6] = {2, 3, 5, 7, 11, 13};
static unsigned long factor_small_primes(unsigned long n, int exponents[6]);


// Is this a good blocksize for FFTW3?
// Any number of factors of 2, 3, 7 plus one of either 11 or 13
static bool goodchoice(unsigned long n){
  int exponents[6];

  unsigned long r = factor_small_primes(n,exponents);
  if(r != 1 || (exponents[4] + exponents[5] > 1))
    return false;
  else
    return true;
}

#if 0 // Use these later for determining valid output sample rates
static unsigned long gcd(unsigned long a,unsigned long b);
static unsigned long lcm(unsigned long a,unsigned long b);

// Greatest common divisor
static unsigned long gcd(unsigned long a,unsigned long b){
  while(b != 0){
    unsigned long t = b;
    b = a % b;
    a = t;
  }
  return a;
}

static unsigned long lcm(unsigned long a,unsigned long b){
  if(a == 0 || b == 0)
    return 0;
  unsigned long g = gcd(a,b);
  return (a/g) * b;
}
#endif

/**
 * Factor n into the primes 2,3,5,7,11,13.
 * exponents[] should be an array of length 6, each slot will hold
 *   the exponent for 2,3,5,7,11,13 respectively.
 *
 * Return value:
 *   - If the returned value is 1, then n was fully factored by {2,3,5,7,11,13}.
 *   - Otherwise, the leftover (return value) is the part that couldn't
 *     be factored into those primes.
 */
static unsigned long factor_small_primes(unsigned long n, int exponents[6]){
  // Initialize exponents
  for (int i = 0; i < 6; i++)
    exponents[i] = 0;

  // Divide out each prime in turn
  for (int i = 0; i < 6; i++) {
    while (n % small_primes[i] == 0) {
      exponents[i]++;
      n /= small_primes[i];
    }
  }
  return n;  // The remainder is what's left
}
