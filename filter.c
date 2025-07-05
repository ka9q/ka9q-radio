// filter using fast convolution (overlap-save) and the FFTW3 FFT package
// for the ka9q-radio 'radiod' program
// Generates transfer functions using Kaiser window
// Optional output decimation by integer factor
// Complex input and transfer functions, complex or real output
// Copyright 2017-2023, Phil Karn, KA9Q, karn@ka9q.net

//#define LIQUID 1 // Experimental use of parks-mcclellan in filter generation
#define FFT_PRIO 95 // Runs at very high real time priority

#define MYNEW 1
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
// Otherwise generates a bazillion warnings
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#include <liquid/liquid.h>
#pragma GCC diagnostic pop
#endif

#include "conf.h"
#include "misc.h"
#include "filter.h"


//#define FILTER_DEBUG 1 // turn on lots of printfs in the window creation code

// Settable from main
char const *Wisdom_file = "/var/lib/ka9q-radio/wisdom";
char const *System_wisdom_file = "/etc/fftw/wisdomf"; // only valid for float version
double FFTW_plan_timelimit = 30.0;
int N_worker_threads = 1;
int N_internal_threads = 1; // Usually most efficient

// Desired FFTW planning level
// If wisdom at this level is not present for some filter, the command to generate it will be logged and FFTW_ESTIMATE wisdom will be generated at runtime
int FFTW_planning_level = FFTW_PATIENT;

// FFTW3 doc strongly recommends doing your own locking around planning routines, so I now am
pthread_mutex_t FFTW_planning_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool FFTW_init = false;

// FFT job descriptor
struct fft_job {
  struct fft_job *next;
  unsigned int jobnum;
  enum filtertype type;
  fftwf_plan plan;
  void *input;
  void *output;
  size_t input_dropsize;      // byte counts to drop from cache when FFT finishes
  pthread_mutex_t *completion_mutex; // protects completion_jobnum
  pthread_cond_t *completion_cond;   // Signaled when job is complete
  unsigned int *completion_jobnum;   // Written with jobnum when complete
  bool terminate; // set to tell fft thread to quit
};

static struct fft_job *FFT_free_list; // List of spare job descriptors

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


static bool goodchoice(unsigned long n);

// Create fast convolution filters
// The filters are now in two parts, filter_in (the master) and filter_out (the slave)
// Filter_in holds the original time-domain input and its frequency domain version
// Filter_out holds the frequency response and decimation information for one of several output filters that can share the same input

// create_filter_input() parameters, shared by all slaves:
// L = input data blocksize
// M = impulse response duration
// in_type = REAL or COMPLEX

// Set up input (master) half of filter
int create_filter_input(struct filter_in *master,int const L,int const M, enum filtertype const in_type){
  assert(master != NULL);
  assert(master != (void *)-1);
  if(master == NULL)
    return -1;

  assert(L > 0);
  assert(M > 0);
  int const N = L + M - 1;
  int const bins = (in_type == COMPLEX) ? N : (N/2 + 1);
  if(bins < 1)
    return -1; // Unreasonably small - will segfault. Can happen if sample rate is garbled

  if(!goodchoice(N))
    fprintf(stderr,"create_filter_input(L=%d, M=%d): N=%d is not an efficient blocksize for FFTW3\n",L,M,N);

  // It really should already be zeroed.
  // If not, it is probably being reused and the dynamically allocated storage in it may not have been freed
#ifdef NDEBUG
  memset(master,0,sizeof *master); // make sure it's clean, even if it causes a memory leak
#else
  ASSERT_ZEROED(master,sizeof *master);
#endif

  master->points = N;
  // If there are no worker threads, do it inline
  master->perform_inline = (N_worker_threads == 0);

  for(int i=0; i < ND; i++){
    master->fdomain[i] = lmalloc(sizeof(float complex) * bins);
    master->completed_jobs[i] = (unsigned int)-1; // So startup won't drop any blocks
  }

  master->bins = bins;
  master->in_type = in_type;
  master->ilen = L;
  master->impulse_length = M;
  pthread_mutex_init(&master->filter_mutex,NULL);
  pthread_cond_init(&master->filter_cond,NULL);

  int old_prio = norealtime();

  // FFTW itself always runs with a single thread since multithreading didn't seem to do much good
  // But we have a set of worker threads operating on a job queue to allow a controlled number
  // of independent FFTs to execute at the same time
  if(!FFTW_init){
    printf("FFTW version: %s\n", fftwf_version);
    fftwf_init_threads();
    bool sr = fftwf_import_system_wisdom();
    fprintf(stderr,"fftwf_import_system_wisdom() %s\n",sr ? "succeeded" : "failed");
    if(!sr){
      if(access(System_wisdom_file,R_OK) == -1){ // Would really like to use AT_EACCESS flag
	fprintf(stderr,"%s not readable: %s\n",System_wisdom_file,strerror(errno));
      }
    }

    bool lr = fftwf_import_wisdom_from_filename(Wisdom_file);
    fprintf(stderr,"fftwf_import_wisdom_from_filename(%s) %s\n",Wisdom_file,lr ? "succeeded" : "failed");
    if(!lr){
      if(access(Wisdom_file,R_OK) == -1){
	fprintf(stderr,"%s not readable: %s\n",Wisdom_file,strerror(errno));
      }
    }
    if(access(Wisdom_file,W_OK) == -1){
      fprintf(stderr,"Warning: %s not writeable, exports will fail: %s\n",Wisdom_file,strerror(errno));
    }

    fftwf_set_timelimit(FFTW_plan_timelimit);
    if(!sr && !lr)
      fprintf(stderr,"No wisdom read, planning FFTs may take up to %'.0lf sec\n",FFTW_plan_timelimit);

    // Start FFT worker thread(s) if not already running
    pthread_mutex_init(&FFT.queue_mutex,NULL);
    pthread_cond_init(&FFT.queue_cond,NULL);
    if(N_worker_threads > NTHREADS_MAX){
      fprintf(stderr,"fft-threads=%d too high, limiting to %d\n",N_worker_threads,NTHREADS_MAX);
      N_worker_threads = NTHREADS_MAX;
    }
    for(int i=0;i < N_worker_threads;i++)
      pthread_create(&FFT.thread[i],NULL,run_fft,NULL);

    FFTW_init = true;
  }
  pthread_mutex_lock(&FFTW_planning_mutex);
  fftwf_plan_with_nthreads(N_internal_threads);

  switch(in_type){
  default:
    pthread_mutex_unlock(&FFTW_planning_mutex);
    assert(0); // shouldn't happen
    realtime(old_prio);
    return -1;
  case CROSS_CONJ:
  case COMPLEX:
    master->input_buffer_size = round_to_page(ND * N * sizeof(float complex));
    // Allocate input_buffer_size bytes immediately followed by its mirror
    master->input_buffer = mirror_alloc(master->input_buffer_size);
    memset(master->input_buffer,0,master->input_buffer_size);
    master->input_read_pointer.c = master->input_buffer;              // FFT starts reading here
    master->input_write_pointer.c = master->input_read_pointer.c + (M-1); // start writing here
    master->input_read_pointer.r = NULL;
    master->input_write_pointer.r = NULL;
    master->fwd_plan = fftwf_plan_dft_1d(N, master->input_read_pointer.c, master->fdomain[0], FFTW_FORWARD, FFTW_WISDOM_ONLY|FFTW_planning_level);
    if(master->fwd_plan == NULL){
      suggest(FFTW_planning_level,N,FFTW_FORWARD,COMPLEX);
      master->fwd_plan = fftwf_plan_dft_1d(N, master->input_read_pointer.c, master->fdomain[0], FFTW_FORWARD, FFTW_ESTIMATE);
    }
    if(fftwf_export_wisdom_to_filename(Wisdom_file) == 0)
      fprintf(stderr,"fftwf_export_wisdom_to_filename(%s) of cof%d failed\n",Wisdom_file,N);
    break;
  case REAL:
    master->input_buffer_size = round_to_page(ND * N * sizeof(float));
    master->input_buffer = mirror_alloc(master->input_buffer_size);
    memset(master->input_buffer,0,master->input_buffer_size);
    master->input_read_pointer.r = master->input_buffer;
    master->input_write_pointer.r = master->input_read_pointer.r + (M-1); // start writing here
    master->input_read_pointer.c = NULL;
    master->input_write_pointer.c = NULL;
    master->fwd_plan = fftwf_plan_dft_r2c_1d(N, master->input_read_pointer.r, master->fdomain[0], FFTW_WISDOM_ONLY|FFTW_planning_level);
    if(master->fwd_plan == NULL){
      suggest(FFTW_planning_level,N,FFTW_FORWARD,REAL);
      master->fwd_plan = fftwf_plan_dft_r2c_1d(N, master->input_read_pointer.r, master->fdomain[0], FFTW_ESTIMATE);
    }
    if(fftwf_export_wisdom_to_filename(Wisdom_file) == 0)
      fprintf(stderr,"fftwf_export_wisdom_to_filename(%s) of rof%d failed\n",Wisdom_file,N);
    break;
  }
  pthread_mutex_unlock(&FFTW_planning_mutex);
  realtime(old_prio);

  return 0;
}
/* create an instance of an output filter stage attached to some common master stage
   Many output filters can share a common input, each with its own frequency shift, response, output sample rate and type
   But all must operate at the same block rate and with M limited to the time domain overlap in the master

   The number of IFFT frequency points N = L + M - 1; L = length of data, M = length of filter impulse response in time domain
   In the master, L and M refer to the input sample rate Rm; in the slave they refer to the output rate Rs
   I.e., Nm / Ns = Rm / Rs = Lm / Ls = (Mm - 1) / (Ms - 1), where M-1 is the filter 'order' (one less than the # of FIR taps)

   master = pointer to associated shared master (input) filter
   response = complex frequency response; may be NULL here and set later with set_filter()
     This is set in the slave and can be different (indeed, this is the reason to have multiple slaves)
     This is always complex, even if the input and/or output are real in the time domain
     However the length is shorter for real output because the complex spectrum is symmetrical around DC
     response array length = Ns = Ls + Ms - 1 when output is complex
                           = Ns/2 + 1 = (Ls + Ms - 1)/2+1 when output is real
     Must be SIMD-aligned (e.g., allocated with fftw_alloc) and will be freed by delete_filter()

    len = number of time domain points in output = Ls
    out_type = REAL, COMPLEX, CROSS_CONJ (COMPLEX with special processing for ISB) or SPECTRUM (dummy for spectrum analyzer)

    All demodulators currently require COMPLEX output because a complex exponential is applied to the time domain
    output for fine frequency tuning
    Before fine tuning was added, SSB(CW) could (and did) use the REAL mode since the imaginary component is unneeded
    and the c2r IFFT is faster

    Baseband FM audio filtering for de-emphasis and PL separation can use REAL output because there's no baseband fine tuning

    If you provide your own filter response, ensure that it drops to nil well below the Nyquist rate
    to prevent aliasing. Remember that decimation reduces the Nyquist rate by the decimation ratio.
    The set_filter() function uses Kaiser windowing for this purpose

    An output filter must not be used after its master is deleted, a segfault will occur
*/
int create_filter_output(struct filter_out *slave,struct filter_in * master,float complex * const response,int len, enum filtertype const out_type){
  assert(master != NULL);
  if(master == NULL)
    return -1;

  assert(slave != NULL);
  if(slave == NULL)
    return -1;

  assert(out_type == SPECTRUM || len > 0);

  // Should already be zeroed
  // If not, it is probably being reused and the dynamically allocated storage in it may not have been freed
#ifdef NDEBUG
  memset(slave,0,sizeof *slave); // make sure it's clean
#else
  ASSERT_ZEROED(slave,sizeof *slave);
#endif
  // Share all but output fft bins, response, output and output type
  slave->master = master;
  slave->out_type = out_type;

  // N / L = Total FFT points / time domain points
  int const N = master->ilen + master->impulse_length - 1;
  int const L = master->ilen;

  slave->response = response;
  pthread_mutex_init(&slave->response_mutex,NULL);

  switch(slave->out_type){
  default:
  case COMPLEX:
  case CROSS_CONJ:
    {
      slave->olen = len;
      ldiv_t x = ldiv((long)len * N,L);
      if(x.rem != 0){
	fprintf(stderr,"Invalid filter output length %d (fft size %ld) for input N=%d, L=%d\n",len,x.quot,N,L);
	return -1;
      }
      slave->points = x.quot; // Total number of FFT points including overlap
      slave->bins = x.quot;
      slave->fdomain = lmalloc(sizeof(float complex) * slave->bins);
      slave->output_buffer.c = lmalloc(sizeof(float complex) * slave->bins);
      assert(slave->output_buffer.c != NULL);
      slave->output_buffer.r = NULL; // catch erroneous references
      slave->output.c = slave->output_buffer.c + slave->bins - len;
      int old_prio = norealtime(); // Could this cause a priority inversion?
      pthread_mutex_lock(&FFTW_planning_mutex);
      fftwf_plan_with_nthreads(1); // IFFTs are always small, use only one internal thread
      if((slave->rev_plan = fftwf_plan_dft_1d(slave->points,slave->fdomain,slave->output_buffer.c,FFTW_BACKWARD,FFTW_WISDOM_ONLY|FFTW_planning_level)) == NULL){
	suggest(FFTW_planning_level,slave->bins,FFTW_BACKWARD,COMPLEX);
	slave->rev_plan = fftwf_plan_dft_1d(slave->points,slave->fdomain,slave->output_buffer.c,FFTW_BACKWARD,FFTW_ESTIMATE);
      }
      if(fftwf_export_wisdom_to_filename(Wisdom_file) == 0)
	fprintf(stderr,"fftwf_export_wisdom_to_filename(%s) failed\n",Wisdom_file);
      pthread_mutex_unlock(&FFTW_planning_mutex);
      realtime(old_prio);
    }
    break;
  case SPECTRUM: // Like complex, but no IFFT or output time domain buffer
    // Recent change: spectrum will read directly from master frequency bins
    // Also don't set up an IFFT
    break;
  case REAL:
    {
      slave->olen = len;
      ldiv_t x = ldiv((long)len * N,L);
      if(x.rem != 0){
	fprintf(stderr,"Invalid filter output length %d for input N=%d, L=%d\n",len,N,L);
	return -1;
      }
      slave->points = x.quot;
      slave->bins = slave->points / 2 + 1;
      slave->fdomain = lmalloc(sizeof(float complex) * slave->bins);
      assert(slave->fdomain != NULL);
      slave->output_buffer.r = lmalloc(sizeof(float) * slave->points);
      assert(slave->output_buffer.r != NULL);
      slave->output_buffer.c = NULL;
      slave->output.r = slave->output_buffer.r + slave->points - len;
      int old_prio = norealtime();
      pthread_mutex_lock(&FFTW_planning_mutex);
      if((slave->rev_plan = fftwf_plan_dft_c2r_1d(slave->points,slave->fdomain,slave->output_buffer.r,FFTW_WISDOM_ONLY|FFTW_planning_level)) == NULL){
	suggest(FFTW_planning_level,slave->points,FFTW_BACKWARD,REAL);
	slave->rev_plan = fftwf_plan_dft_c2r_1d(slave->points,slave->fdomain,slave->output_buffer.r,FFTW_ESTIMATE);
      }
      if(fftwf_export_wisdom_to_filename(Wisdom_file) == 0)
	fprintf(stderr,"fftwf_export_wisdom_to_filename(%s) failed\n",Wisdom_file);
      pthread_mutex_unlock(&FFTW_planning_mutex);
      realtime(old_prio);
    }
    break;
  }
  if(slave->out_type != SPECTRUM && !goodchoice(slave->points)){
    int const ell = slave->olen;
    int const overlap = slave->points / (slave->points - ell);
    int const step = overlap;

    for(int nn = slave->points + step; nn < master->ilen + master->impulse_length - 1; nn += step){
      if(goodchoice(nn)){
	int nell = nn * ell / slave->points;
	int nm = nn - nell + 1;
	fprintf(stderr,"create_filter_output: N=%d is not an efficient blocksize for FFTW3.",slave->points);
	fprintf(stderr," Next good choice is N = %d (L=%d, M=%d); set samprate = %d * blockrate\n",nn,nell,nm,nell);
	break;
      }
    }
  }
  slave->next_jobnum = master->next_jobnum;
  return 0;
}
// Assist with choosing good blocksizes for FFTW3
static const int small_primes[6] = {2, 3, 5, 7, 11, 13};
static unsigned long factor_small_primes(unsigned long n, int exponents[6]);


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

// Is this a good blocksize for FFTW3?
// Any number of factors of 2, 3, 5, 7 plus one of either 11 or 13
static bool goodchoice(unsigned long n){
  int exponents[6];

  unsigned long r = factor_small_primes(n,exponents);
  if(r != 1 || (exponents[4] + exponents[5] > 1))
    return false;
  else
    return true;
}

// Worker thread(s) that actually execute FFTs
// Used for input FFTs since they tend to be large and CPU-consuming
// Lets the input thread process the next input block in parallel on another core
// Frees the input buffer and the job descriptor when done
void *run_fft(void *p){
  pthread_detach(pthread_self());
  pthread_setname("fft");
  (void)p; // Unused

  realtime(FFT_PRIO);
  stick_core();

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
    drop_cache(job->input,job->input_dropsize);
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
    // Put descriptor on free pool
    pthread_mutex_lock(&FFT.queue_mutex);
    job->next = FFT_free_list;
    FFT_free_list = job;
    pthread_mutex_unlock(&FFT.queue_mutex);

    if(terminate)
      break; // Terminate after this job
  }
  return NULL;
}


// Execute the input side of a filter:
// We use the FFTW3 functions that specify the input and output arrays
int execute_filter_input(struct filter_in * const f){
  assert(f != NULL);
  if(f == NULL)
    return -1;

  if(f->perform_inline){
    // Just execute it here
    int jobnum = f->next_jobnum++;
    float complex *output = f->fdomain[jobnum % ND];
    switch(f->in_type){
    default:
    case CROSS_CONJ:
    case COMPLEX:
      {
	float complex *input = f->input_read_pointer.c;
	f->input_read_pointer.c += f->ilen;
	mirror_wrap((void *)&f->input_read_pointer.c,f->input_buffer,f->input_buffer_size);
	fftwf_execute_dft(f->fwd_plan,input,output);
	drop_cache(input,f->ilen * sizeof(float complex));
      }
      break;
    case REAL:
      {
	float *input = f->input_read_pointer.r;
	f->input_read_pointer.r += f->ilen;
	mirror_wrap((void *)&f->input_read_pointer.r,f->input_buffer,f->input_buffer_size);
	fftwf_execute_dft_r2c(f->fwd_plan,input,output);
	drop_cache(input,f->ilen * sizeof(float));
      }
      break;
    }
    // Signal we're done with this job
    pthread_mutex_lock(&f->filter_mutex);
    f->completed_jobs[jobnum % ND] = jobnum;
    pthread_cond_broadcast(&f->filter_cond);
    pthread_mutex_unlock(&f->filter_mutex);
    return 0;
  }

  // set up a job for the FFT worker threads and enqueue it
  // Take one off the pool, if available
  pthread_mutex_lock(&FFT.queue_mutex);
  struct fft_job *job = FFT_free_list;
  if(job != NULL){
    FFT_free_list = job->next;
    job->next = NULL;
  }
  pthread_mutex_unlock(&FFT.queue_mutex);

  if(job == NULL)
    job = calloc(1,sizeof(struct fft_job)); // Otherwise create a new one

  // A descriptor from the free list won't be blank, but we set everything below
  assert(job != NULL);
  job->jobnum = f->next_jobnum++;
  job->output = f->fdomain[job->jobnum % ND];
  job->type = f->in_type;
  job->plan = f->fwd_plan;
  job->completion_mutex = &f->filter_mutex;
  job->completion_jobnum = &f->completed_jobs[job->jobnum % ND];
  job->completion_cond = &f->filter_cond;
  job->terminate = false;

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
    job->input_dropsize = f->ilen * sizeof(float complex);
    f->input_read_pointer.c += f->ilen;
    mirror_wrap((void *)&f->input_read_pointer.c,f->input_buffer,f->input_buffer_size);
    break;
  case REAL:
    job->input = f->input_read_pointer.r;
    job->input_dropsize = f->ilen * sizeof(float);
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

/* Execute the output side of a filter:
   1 - wait for a forward FFT job to complete
   frequency domain data is in a circular queue ND buffers deep to tolerate scheduling jitter

   2 - multiply the selected frequency bin range by the filter frequency response
   This is the hard part; handle all combinations of real/complex input/output, wraparound, etc

   3 - convert back to time domain with IFFT
   'shift' is the number of FFT bins to shift *down*; a positive 'shift' means that a positive input
   frequency will become zero frequency on output
*/
int execute_filter_output(struct filter_out * const slave,int const shift){
  assert(slave != NULL);
  if(slave == NULL)
    return -1;

  // We do have to modify the master's data structure, notably mutex locks
  // So the dereferenced pointer can't be const
  struct filter_in * const master = slave->master;
  assert(master != NULL);
  if(master == NULL)
    return -1;

  assert(slave->out_type == SPECTRUM || (slave->rev_plan != NULL && slave->bins > 0));
  assert(slave->out_type != NONE);
  assert(master->in_type != NONE);
  assert(master->fdomain != NULL);
  assert(master->bins > 0);

  // DC and positive frequencies up to nyquist frequency are same for all types
  assert(slave->out_type == SPECTRUM || malloc_usable_size(slave->fdomain) >= slave->bins * sizeof(*slave->fdomain));

  // Wait for new block of output data
  pthread_mutex_lock(&master->filter_mutex);
  int blocks_behind = master->completed_jobs[slave->next_jobnum % ND] - slave->next_jobnum;
  if(blocks_behind >= ND){
    // We've fallen too far behind. skip ahead to the oldest block still available
    unsigned nextblock = master->completed_jobs[0];
    for(int i=1; i < ND; i++){
      if((int)(master->completed_jobs[i] - nextblock) < 0) // modular comparison
	nextblock = master->completed_jobs[i];
    }
    slave->block_drops += nextblock - slave->next_jobnum;
    slave->next_jobnum = nextblock;
  }
  while((int)(slave->next_jobnum - master->completed_jobs[slave->next_jobnum % ND]) > 0)
    pthread_cond_wait(&master->filter_cond,&master->filter_mutex);
  // We don't modify the master's output data, we create our own
  float complex const * const fdomain = master->fdomain[slave->next_jobnum % ND];
  // in case we just waited so long that the buffer wrapped, resynch
  slave->next_jobnum = master->completed_jobs[slave->next_jobnum % ND] + 1;
  pthread_mutex_unlock(&master->filter_mutex);

  assert(fdomain != NULL); // Should always be master frequency data

  // In spectrum mode we'll read directly from master. Don't forget the 3dB scale when the input is real
  if(slave->fdomain == NULL || slave->response == NULL)
    return 0;

  /* Multiply the requested frequency segment by the frequency response
     Although frequency domain data is always complex, this is complicated because
     we have to handle the four combinations of the filter input and output time domain data
     being either real or complex.

     In ka9q-radio the input depends on the SDR front end, while the output is complex
     (even for SSB) because of the fine tuning frequency shift after conversion
     back to the time domain. So while real output is supported it is not well tested.
  */
  pthread_mutex_lock(&slave->response_mutex); // Don't let it change while we're using it
  if(master->in_type != REAL && slave->out_type != REAL){
    // Complex -> complex (e.g., fobos (in VHF/UHF mode), funcube, airspyhf, sdrplay)

    int wp = (slave->bins+1)/2; // most negative output bin
    int rp = shift - slave->bins/2; // Start index in master, unwrapped = shift - # output bins

    // Starting below master, zero output until we're in range. Rarely needed.
    while(rp < -(master->bins+1)/2){
      assert(wp >=0 && wp < slave->bins);
      slave->fdomain[wp] = 0;
      rp++;
      if(++wp == (slave->bins+1)/2) // exhausted output buffer
	goto done;
      if(wp == slave->bins)
	wp = 0; // Wrap to DC
    }
    if(rp < 0)
      rp += master->bins; // Starts in negative region of master

    if(rp < 0 || rp >= master->bins){
      // Shift is out of range
      // Zero any remaining output
      while(wp != (slave->bins+1)/2){
	assert(wp >=0 && wp < slave->bins);
	slave->fdomain[wp++] = 0;
	if(wp == slave->bins)
	  wp = 0; // Wrap to DC
      }
      goto done;
    }
    // The actual work is here
    do {
      assert(rp >= 0 && rp < master->bins);
      assert(wp >=0 && wp < slave->bins);
      slave->fdomain[wp] = fdomain[rp] * slave->response[wp];
      if(++rp == master->bins)
	rp = 0; // Master wrapped to DC
      if(++wp == slave->bins)
	wp = 0; // Slave wrapped to DC
    } while (wp != (slave->bins+1)/2 && rp != (master->bins+1)/2); // Until we reach the top of the output or input

    // Zero any remaining output. Rarely needed.
    while(wp != (slave->bins+1)/2){
      assert(wp >=0 && wp < slave->bins);
      slave->fdomain[wp++] = 0;
      if(wp == slave->bins)
	wp = 0; // Wrap to DC
    }
  } else if(master->in_type != REAL && slave->out_type == REAL){
    // Complex -> real UNTESTED! not used in ka9q-radio at present
    for(int si=0; si < slave->bins; si++){
      int const mi = si + shift;
      float complex result = 0;
      if(mi >= -master->bins/2 && mi < master->bins/2)
	result = slave->response[si] * (fdomain[modulo(mi,master->bins)] + conjf(fdomain[modulo(master->bins - mi, master->bins)]));
      slave->fdomain[si] = result;
    }
  } else if(master->in_type == REAL && slave->out_type == REAL){
    // Real -> real (e.g. in wfm stereo decoding)
    // shift is unlikely to be non-zero because of the frequency folding, but handle it anyway
    for(int si=0; si < slave->bins; si++){ // All positive frequencies
      int const mi = si + shift;
      slave->fdomain[si] = (mi >= 0 && mi < master->bins) ? fdomain[mi] * slave->response[si] : 0;
    }
  } else if(master->in_type == REAL && slave->out_type != REAL){
    /* Real->complex (e.g., rx888, fobos (direct sample mode), airspy R2)
       This can be tricky. We treat the input as complex with Hermitian symmetry (both positive and negative spectra)
       If shift >= 0, the input spectrum is positive and right-side up (e.g., rx888, fobos direct sampling)
       If shift < 0, the input spectrum is negative and inverted (e.g., Airspy R2)
       Don't cross input DC as this doesn't seem useful; just blank the output
       For real inputs, set_filter scales +3dB to account for the half energy in the implicit negative spectrum
    */
    int wp = (slave->bins+1)/2; // most negative output bin

    if(shift >= 0){
      // Right side up
      int rp = shift - slave->bins/2; // Start index in master, unwrapped = shift - # output bins
      // Zero-pad start if necessary. Rarely needed
      while(rp < 0){
	assert(wp >=0 && wp <= slave->bins);
	if(wp == slave->bins)
	  wp = 0; // wrap to DC
	slave->fdomain[wp] = 0;
	if(++wp == (slave->bins+1)/2){
	  goto done; // Top of output
	}
	rp++;
      }
      // Actual work
      while(rp < master->bins){
	assert(wp >=0 && wp <= slave->bins);
	assert(rp >= 0 && rp < master->bins);
	if(wp == slave->bins)
	  wp = 0; // Wrap to DC

	slave->fdomain[wp] = fdomain[rp] * slave->response[wp];
	if(++wp == (slave->bins+1)/2){
	  goto done; // Output done
	}
	rp++;
      }
      // zero-pad upper end
      while(wp != (slave->bins+1)/2){
	assert(wp >= 0 && wp <= slave->bins);
	if(wp == slave->bins)
	  wp = 0;
	slave->fdomain[wp] = 0;
	if(++wp == (slave->bins+1)/2){
	  goto done; // Top of output
	}
      }
    } else {
      // Inverted spectrum
      int rp = -(shift - slave->bins/2); // Start at high (negative) input frequency
      // Pad start if necessary
      while(rp >= master->bins){
	assert(wp >=0 && wp <= slave->bins);
	if(wp == slave->bins)
	  wp = 0; // wrap to DC
	slave->fdomain[wp] = 0;
	if(++wp == (slave->bins+1)/2){
	  goto done; // Top of output
	}
	rp--;
      }
      // Actual work
      while(rp >= 0){
	assert(wp >=0 && wp < slave->bins);
	assert(rp >= 0 && rp < master->bins);
	if(wp == slave->bins)
	  wp = 0; // Wrap to DC
	slave->fdomain[wp] = conjf(fdomain[rp]) * slave->response[wp];
	if(++wp == (slave->bins+1)/2){
	  goto done;
	}
	rp--;
      }
      // Zero upper end
      while(wp != (slave->bins+1)/2){
	assert(wp >= 0 && wp <= slave->bins);
	if(wp == slave->bins)
	  wp = 0; // Wrap DC
	slave->fdomain[wp] = 0;
	if(++wp == (slave->bins+1)/2){
	  goto done; // Top of output
	}
      }
    } // end of inverted spectrum
  }
 done:;
  // Zero out Nyquist bin
  slave->fdomain[(slave->bins+1)/2] = 0;

  pthread_mutex_unlock(&slave->response_mutex); // release response[]

  if(slave->out_type == CROSS_CONJ){
    // hack for ISB; forces negative frequencies onto I, positive onto Q
    // Don't really know how to use this anymore; it's incompatible with fine tuning in the time domain
    // Re-implementing ISB will probably require a filter for each sideband
    // Also probably generates time domain ripple effects due to the sharp notch at DC
    assert(malloc_usable_size(slave->fdomain) >= slave->bins * sizeof(*slave->fdomain));
    for(int p=1,dn=slave->bins-1; p < slave->bins/2; p++,dn--){
      assert(p >= 0 && p < slave->bins);
      assert(dn >= 0 && dn < slave->bins);
      float complex const pos = slave->fdomain[p];
      float complex const neg = slave->fdomain[dn];

      slave->fdomain[p]  = pos + conjf(neg);
      slave->fdomain[dn] = neg - conjf(pos);
    }
    slave->fdomain[0] = 0; // Must be a null at DC
  }
  // And finally back to the time domain
  fftwf_execute(slave->rev_plan); // Note: c2r version destroys fdomain[], but it's not used again anyway
  // Drop the cache in the first M-1 points of the time domain buffer that we'll discard
  if(slave->out_type == REAL)
    drop_cache(slave->output_buffer.r,(slave->points - slave->olen) * sizeof (float));
  else
    drop_cache(slave->output_buffer.c,(slave->points - slave->olen) * sizeof (float complex));
  return 0;
}

int delete_filter_input(struct filter_in * master){
  if(master == NULL)
    return -1;

  ASSERT_UNLOCKED(&master->filter_mutex);
  pthread_mutex_destroy(&master->filter_mutex);
  pthread_cond_destroy(&master->filter_cond);
  if(master->fwd_plan != NULL)
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

  ASSERT_UNLOCKED(&slave->response_mutex);
  pthread_mutex_destroy(&slave->response_mutex);
  if(slave->rev_plan != NULL)
    fftwf_destroy_plan(slave->rev_plan);
  slave->rev_plan = NULL;
  // Only one will be non-null but it doesn't hurt to free both
  FREE(slave->output_buffer.c);
  FREE(slave->output_buffer.r);
  FREE(slave->response);
  FREE(slave->fdomain);
  memset(slave,0,sizeof(*slave)); // Wipe it all
  return 0;
}

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

/* Set up a filter with a specified complex bandpass response
   Uses a Kaiser-windowed sinc function - new as of March 2025
   This can occasionally be called with slave == NULL at startup, so don't abort
   NB: 'low' and 'high' are *fractional* frequencies relative to the output sample rate, i.e., -0.5 < f < +0.5
   If invoked on a demod that hasn't run yet, slave->master will be NULL so check for that and quit;
   the filter should get set up when it actually starts (thanks N5TNL for bug report) */
int set_filter(struct filter_out * const slave,float low,float high,float const kaiser_beta){
  if(slave == NULL || isnan(low) || isnan(high) || isnan(kaiser_beta) || slave->master == NULL)
    return -1;

  if(slave->out_type == REAL){
    // Filter edges crossing DC not allowed for real output
    low = fabsf(low);
    high = fabsf(high);
  }
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
  int const N = slave->points;
  int const L = slave->olen;
  int const M = N - L + 1; // Length of impulse response in time domain

  // Real lowpass filter with cutoff = 1/2 bandwidth
  float bw2 = fabsf(high - low)/2;
  if(bw2 == 0)
    bw2 = 1; // Avoid assert failures
  float center = (high + low)/2;
  float kaiser_window[M];
  make_kaiser(kaiser_window,M,kaiser_beta);

  // Form complex impulse response by generating kaiser-windowed sinc pulse and shifting to desired center freq
  float complex impulse[M];
  float window_gain = 0;
  for(int i = 0; i < M; i++){
    float n = i - (float)(M-1)/2;
    float r = kaiser_window[i] * 2 * bw2 * sinc(2 * bw2 * n);
    window_gain += r;
    impulse[i] = cispif(2 * center * n) * r;
#if FILTER_DEBUG
    printf("impulse[%d] = %g + j%g\n",i,crealf(impulse[i]),cimagf(impulse[i]));
#endif
  }
  // gain corrections:
  // 1. real inputs require +3dB for half the power in the implicit negative spectrum
  // 2. the windowed sinc has some loss
  // 3. The un-normalized forward FFT has an implicit power gain of N
  float const gain = (slave->master->in_type == REAL ? M_SQRT2 : 1.0f)
    / (window_gain *  slave->master->points);
  assert(gain != 0 && isfinite(gain));
  for(int i = 0; i < M; i++)
    impulse[i] *= gain; // Normalize for the window gain

  float complex * const response = lmalloc(N * sizeof(float complex));
  assert(response != NULL);
  pthread_mutex_lock(&FFTW_planning_mutex);
  fftwf_plan_with_nthreads(1);
  // Doesn't have to be fast, we're only doing this once
  fftwf_plan fwd_filter_plan = fftwf_plan_dft_1d(N,response,response,FFTW_FORWARD,FFTW_ESTIMATE);
  assert(fwd_filter_plan != NULL);
  pthread_mutex_unlock(&FFTW_planning_mutex);

  memcpy(response,impulse,M * sizeof *response);
  memset(response+M,0,(N-M) * sizeof *response);
  fftwf_execute(fwd_filter_plan);

#if FILTER_DEBUG
  {
    for(int i=0; i < N; i++)
      printf("response[%d] = %g + j%g\n",i,__real__ response[i],__imag__ response[i]);
  }
#endif
  // Hot swap with existing response, if any, using mutual exclusion
  pthread_mutex_lock(&slave->response_mutex);
  float complex * const tmp = slave->response;
  slave->response = response;
  pthread_mutex_unlock(&slave->response_mutex);
  free(tmp);
  return 0;
}

int write_cfilter(struct filter_in *f, float complex const *buffer,int size){
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
  bool executed = false;
  while(f->wcnt >= f->ilen){
    f->wcnt -= f->ilen;
    execute_filter_input(f);
    executed = true;
  }
  return executed;
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
  bool executed = false;
  while(f->wcnt >= f->ilen){
    f->wcnt -= f->ilen;
    execute_filter_input(f);
    executed = true;
  }
  return executed;
};

// Suggest running fftwf-wisdom to generate some FFTW3 wisdom
void suggest(int level,int size,int dir,int clex){
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
  fprintf(stderr,"suggest running \"fftwf-wisdom -v%s -T 1 -w %s/wisdom -o /tmp/wisdomf %co%c%d\", then \"mv /tmp/wisdomf /etc/fftw/wisdomf\" *if* larger than current file. This will take time.\n",
	  opt,
	  VARDIR,
	  clex == COMPLEX ? 'c' : 'r',
	  dir == FFTW_FORWARD ? 'f' : 'b',
	  size);
}
// Greatest common divisor
unsigned long gcd(unsigned long a,unsigned long b){
  while(b != 0){
    unsigned long t = b;
    b = a % b;
    a = t;
  }
  return a;
}


unsigned long lcm(unsigned long a,unsigned long b){
  if(a == 0 || b == 0)
    return 0;
  unsigned long g = gcd(a,b);
  return (a/g) * b;
}

#if 0
// Miscellaneous, alternate and experimental code, currently unused
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


#if LIQUID
// Use Remez or windowed sinc filter design routines in Liquid DSP library
int set_filter(struct filter_out * const slave,float low,float high,float const kaiser_beta){
  assert(slave != NULL && slave->master != NULL && isfinite(low) && isfinite(high) && isfinite(kaiser_beta) && slave->master != NULL);
  if(slave == NULL || slave->master == NULL || isnan(low) || isnan(high) || isnan(kaiser_beta) || slave->master == NULL)
    return -1;

  if(slave->out_type == REAL){
    low = fabsf(low);
    high = fabsf(high);
  }
  // Swap if necessary
  if(low > high){
    float tmp = low;
    low = high;
    high = tmp;
  }
  // Limit filter range to Nyquist rate
  low = low < -0.5 ? -0.5 : low > +0.5 ? +0.5 : low;
  high = high < -0.5 ? -0.5 : high > +0.5 ? +0.5 : high;

  int const N = slave->points;  // Total number of time domain points
  int const L = slave->olen;
  int const M = N - L + 1; // Length of impulse response in time domain
  assert(N != 0 && L != 0 && M != 0);

  // Used for both time and frequency domain, so make full length even for real transforms
  float complex * const response = lmalloc(N * sizeof(*response));
  assert(response != NULL);
  float real_coeff[M];
  memset(real_coeff,0,sizeof(real_coeff));
  float const bw = fabsf(high - low);
  float const fc = (high + low)/2; // center

  //#define REMEZ 1
#if REMEZ
  // Need to understand these parameters better before I actually use it
  int const num_bands = 2;
  float bands[2*num_bands];
  bands[0] = 0;  bands[1] = bw/2 - 0.01; // for now
  bands[1] = bands[1] < 0 ? 0 : bands[1];
  bands[2] = bw/2 + 0.01; bands[3] = 0.5;
  bands[2] = bands[2] > 0.5 ? 0.5 : bands[2];
  float des[num_bands];
  des[0] = 1.0;
  des[1] = 0.0;
  float weights[num_bands];
  weights[0] = 1.0;
  weights[1] = 1.0;
  liquid_firdespm_wtype wtype[num_bands];
  wtype[0] = LIQUID_FIRDESPM_FLATWEIGHT;
  wtype[1] =  LIQUID_FIRDESPM_EXPWEIGHT;

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
  fftwf_plan plan = fftwf_plan_dft_1d(N,response,response,FFTW_FORWARD,FFTW_ESTIMATE); // doesn't need to be fast
  assert(plan != NULL);
  if(fftwf_export_wisdom_to_filename(Wisdom_file) == 0)
    fprintf(stderr,"fftwf_export_wisdom_to_filename(%s) of cif%d failed\n",Wisdom_file,N);
  pthread_mutex_unlock(&FFTW_planning_mutex);

  // Multiply real coefficients by complex exponential for frequency shift, place in FFT time-domain buffer
  memset(response,0,N * sizeof(*response));

  float gain = 1.0f / slave->master->points;
  assert(gain != 0 && isfinite(gain));
  for(int i=0; i < M; i++)
    response[i] = gain * real_coeff[i] * cispif(2 * fc * (i - M/2));

  fftwf_execute_dft(plan,response,response);
  fftwf_destroy_plan(plan);
  plan = NULL;

#if FILTER_DEBUG
  printf("response:");
  for(int i=0; i < N; i++)
    printf(" %g+j%g",crealf(response[i]),cimag(response[i]));
  printf("\n");
#endif

  // Hot swap with existing response, if any, using mutual exclusion
  pthread_mutex_lock(&slave->response_mutex);
  float complex * const tmp = slave->response;
  slave->response = response;
  pthread_mutex_unlock(&slave->response_mutex);
  free(tmp);
  return 0;
}

#else

// Apply Kaiser window to filter frequency response
// "response" is SIMD-aligned array of N float complexs
// Impulse response will be limited to first M samples in the time domain
// Phase is adjusted so "time zero" (center of impulse response) is at M/2
// L and M refer to the decimated output
static int window_filter(int const L,int const M,float complex * const response,float const beta){
  assert(response != NULL);
  if(response == NULL)
    return -1;
  assert(L > 0 && M > 0);
  int const N = L + M - 1;
  assert(malloc_usable_size(response) >= N * sizeof(*response));
  // fftw_plan can overwrite its buffers, so we're forced to make a temp. Ugh.
  float complex * const buffer = lmalloc(sizeof(float complex) * N);
  assert(buffer != NULL);
  pthread_mutex_lock(&FFTW_planning_mutex);
  fftwf_plan_with_nthreads(1);
  fftwf_plan fwd_filter_plan = fftwf_plan_dft_1d(N,buffer,buffer,FFTW_FORWARD,FFTW_ESTIMATE);
  assert(fwd_filter_plan != NULL);
  fftwf_plan rev_filter_plan = fftwf_plan_dft_1d(N,buffer,buffer,FFTW_BACKWARD,FFTW_ESTIMATE);
  assert(rev_filter_plan != NULL);
  if(fftwf_export_wisdom_to_filename(Wisdom_file) == 0)
    fprintf(stderr,"fftwf_export_wisdom_to_filename(%s) of cif%d and cib%d failed\n",
	    Wisdom_file,N,N);
  pthread_mutex_unlock(&FFTW_planning_mutex);

  // Convert to time domain
  memcpy(buffer,response,N * sizeof(*buffer));
  fftwf_execute(rev_filter_plan);
  fftwf_destroy_plan(rev_filter_plan);
  rev_filter_plan = NULL;

  float kaiser_window[M];
  make_kaiser(kaiser_window,M,beta);

#if FILTER_DEBUG
  for(int m = 0; m < M; m++)
    fprintf(stderr,"kaiser[%d] = %g\n",m,kaiser_window[m]);
#endif

  // Compensate for our own FFT/IFFT cycle
  float const gain = 1.0f/N;

  // Shift to beginning of buffer to make causal; apply window and adjust for Kaiser window gain/loss
  for(int n = M - 1; n >= 0; n--)
    buffer[n] = buffer[(n-M/2+N)%N] * kaiser_window[n] * gain;

  // Pad with zeroes on right side
  memset(buffer+M,0,(N-M)*sizeof(*buffer));

#if FILTER_DEBUG
  fprintf(stderr,"window_filter filter impulse response, shifted, windowed and zero padded\n");
  for(int n = 0; n < M; n++)
    fprintf(stderr,"%d %lg %lg\n",n,crealf(buffer[n]),cimagf(buffer[n]));
#endif

  // Now back to frequency domain
  fftwf_execute(fwd_filter_plan);
  fftwf_destroy_plan(fwd_filter_plan);
  fwd_filter_plan = NULL;
#if FILTER_DEBUG
  fprintf(stderr,"window_filter filter response amplitude\n");
  for(int n = 0; n < N; n++)
    fprintf(stderr,"%d %g %g (%.1f dB)\n",n,crealf(buffer[n]),cimagf(buffer[n]),power2dB(cnrmf(buffer[n])));

  fprintf(stderr,"\n");
#endif
  memcpy(response,buffer,N*sizeof(*response));
  free(buffer);
  return 0;
}
// Real-only counterpart to window_filter()
// response[] is only N/2+1 elements containing DC and positive frequencies only
// Negative frequencies are implicitly the conjugate of the positive frequencies
// L and M refer to the decimated output
static int window_rfilter(int const L,int const M,float complex * const response,float const beta){
  assert(response != NULL);
  if(response == NULL)
    return -1;
  assert(L > 0 && M > 0);
  int const N = L + M - 1;
  int const bins = N/2 + 1;
  assert(malloc_usable_size(response) >= bins * sizeof(*response));
  float complex * const buffer = lmalloc(bins * sizeof(float complex)); // plan destroys its input
  assert(buffer != NULL);
  float * const timebuf = lmalloc(sizeof(float) * N);
  assert(timebuf != NULL);
  pthread_mutex_lock(&FFTW_planning_mutex);
  fftwf_plan_with_nthreads(1);
  fftwf_plan fwd_filter_plan = fftwf_plan_dft_r2c_1d(N,timebuf,buffer,FFTW_ESTIMATE);
  assert(fwd_filter_plan != NULL);
  fftwf_plan_with_nthreads(1);
  fftwf_plan rev_filter_plan = fftwf_plan_dft_c2r_1d(N,buffer,timebuf,FFTW_ESTIMATE);
  assert(rev_filter_plan != NULL);
  if(fftwf_export_wisdom_to_filename(Wisdom_file) == 0)
    fprintf(stderr,"fftwf_export_wisdom_to_filename(%s) of rof%d and rob%d failed\n",
	    Wisdom_file,N,N);
  pthread_mutex_unlock(&FFTW_planning_mutex);

  // Convert to time domain
  memcpy(buffer,response,bins * sizeof(*buffer));
  fftwf_execute(rev_filter_plan);
  fftwf_destroy_plan(rev_filter_plan);
  rev_filter_plan = NULL;

  float kaiser_window[M];
  make_kaiser(kaiser_window,M,beta);

#if FILTER_DEBUG
  for(int m = 0; m < M; m++)
    fprintf(stderr,"kaiser[%d] = %g\n",m,kaiser_window[m]);
#endif

  // Compensate for our own FFT/IFFT cycle
  float gain = 1.0f/N;

  // Shift to beginning of buffer to make causal; apply window and adjust for Kaiser window gain/loss
  for(int n = M - 1; n >= 0; n--)
    timebuf[n] = timebuf[(n-M/2+N)%N] * kaiser_window[n] * gain;

  // Pad with zeroes on right side
  memset(timebuf+M,0,(N-M)*sizeof(*timebuf));
#if FILTER_DEBUG
  printf("window_rfilter impulse response, shifted, windowed and zero padded\n");
  for(int n = 0; n < M; n++)
    printf("%d %lg\n",n,timebuf[n]);
#endif

  // Now back to frequency domain
  fftwf_execute(fwd_filter_plan);
  fftwf_destroy_plan(fwd_filter_plan);
  fwd_filter_plan = NULL;
  free(timebuf);
  memcpy(response,buffer,bins * sizeof(*response));
  free(buffer);
#if FILTER_DEBUG
  printf("window_rfilter frequency response\n");
  for(int n = 0; n < bins; n++)
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

  if(slave->out_type == REAL){
    // Negative frequencies are not allowed in real signals
    low = fabsf(low);
    high = fabsf(high);
  }
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
  int const N = slave->points;
  int const L = slave->olen;
  int const M = N - L + 1; // Length of impulse response in time domain

  float complex * const response = lmalloc(sizeof(float complex) * slave->bins);
  assert(response != NULL);
  memset(response,0,slave->bins * sizeof(response[0]));
  assert(malloc_usable_size(response) >= (slave->bins) * sizeof(*response));
  float const gain = 1.0f / slave->master->points; // Adjust for input FFT and overlap
  assert(gain != 0 && isfinite(gain));

  for(int n = 0; n < slave->bins; n++){
    float const f = n < N/2 ? (float)n / N : (float)(n - N) / N; // neg frequency
    if(f == low || f == high)
      response[n] = gain * M_SQRT1_2; // -3dB
    else if(f > low && f < high)
      response[n] = gain;
    // else response[n] = 0;
  }
  if(slave->out_type == REAL)
    window_rfilter(L,M,response,kaiser_beta);
  else
    window_filter(L,M,response,kaiser_beta);

#if FILTER_DEBUG
  {
    // check gain in mid passband
    int midbin = N * (high + low)/2; 
    if(midbin < 0)
      midbin += N; // Can only happen for complex output, will keep midbin in range for real transforms with only (N/2) + 1 bins
    float midgain = cnrmf(response[midbin]);
    fprintf(stderr,"passband middle freq %.3f is bin %d, gain %.3f dB\n",
	    (high + low)/2, midbin,power2dB(midgain));
  }
#endif
  // Hot swap with existing response, if any, using mutual exclusion
  pthread_mutex_lock(&slave->response_mutex);
  float complex * const tmp = slave->response;
  slave->response = response;
  pthread_mutex_unlock(&slave->response_mutex);
  free(tmp);
  return 0;
}
#endif // End of my original filter design routine
#endif // End of experimental code
