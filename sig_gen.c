// signal generator - looks like a pseudo-front-end to radiod
// Copyright Phil Karn, KA9Q, Aug 2023 KA9Q
#define _GNU_SOURCE 1
#include <complex.h>
#include <math.h>
#include <pthread.h>
#include <portaudio.h>
#include <errno.h>
#include <iniparser/iniparser.h>
#if defined(linux)
#include <bsd/string.h>
#include <bsd/stdlib.h>
#else
#include <stdlib.h>
#endif
#include <sysexits.h>
#include <strings.h>

#include "conf.h"
#include "fcd.h"
#include "misc.h"
#include "config.h"
#include "radio.h"

enum modulation {
  CW = 0, // No modulation
  DSB, // AM without a carrier
  AM,
  FM // Not yet implemented
};


struct sdrstate {
  struct frontend *frontend;
  double carrier; // Carrier frequency to generate
  float amplitude; // Amplitude of carrier
  float noise; // Amplitude of noise
  enum modulation modulation;
  char *source;

  pthread_t proc_thread;
};

// A larger blocksize makes more efficient use of each frame, but the receiver generally runs on
// frames that match the Opus codec: 2.5, 5, 10, 20, 40, 60, 180, 100, 120 ms
// So to minimize latency, make this a common denominator:
// 240 samples @ 16 bit stereo = 960 bytes/packet; at 192 kHz, this is 1.25 ms (800 pkt/sec)
static int Blocksize;
extern bool Stop_transfers;

static int const Random_samples = 30000000;
static float Power_smooth = 0.05; // Calculate this properly someday

// One second of noise in requested format
// Will be played with a random starting point every block
complex float *Complex_noise;
float *Real_noise;

static complex float complex_gaussian(void);
static float real_gaussian(void);

double sig_gen_tune(struct frontend * const frontend,double const freq);

int sig_gen_setup(struct frontend * const frontend, dictionary * const dictionary, char const * const section){
  assert(dictionary != NULL);
  {
    char const * const device = config_getstring(dictionary,section,"device",section);
    if(strcasecmp(device,"sig_gen") != 0)
      return -1; // Not for us
  }
  // Cross-link generic and hardware-specific control structures
  struct sdrstate * const sdr = calloc(1,sizeof(*sdr));
  assert(sdr != NULL);
  sdr->frontend = frontend;
  frontend->context = sdr;

  {
    frontend->samprate = 30e6; // Default 30 MHz
    char const *p = config_getstring(dictionary,section,"samprate",NULL);
    if(p != NULL)
      frontend->samprate = parse_frequency(p,false);
  }
  {
    double const eL = frontend->samprate * Blocktime / 1000.0; // Blocktime is in milliseconds
    Blocksize = lround(eL);
  }
  frontend->isreal = config_getboolean(dictionary,section,"real",true);
  frontend->isreal = ! config_getboolean(dictionary,section,"complex",! frontend->isreal);
  frontend->bitspersample = config_getint(dictionary,section,"bitspersample",16);
  if(frontend->bitspersample < 1 || frontend->bitspersample > 32){
    fprintf(stdout,"unreasonable bits per sample %d, setting to 16\n",frontend->bitspersample);
    frontend->bitspersample = 16;
  }
  if(frontend->isreal){
    frontend->min_IF = 0;
    frontend->max_IF = frontend->samprate / 2;
    frontend->frequency = 0;
  } else {
    frontend->min_IF = -frontend->samprate/2;
    frontend->max_IF = +frontend->samprate/2;
    frontend->frequency = frontend->samprate/2;
  }
  {
    char const * const p = config_getstring(dictionary,section,"description","funcube dongle+");
    if(p != NULL)
      strlcpy(frontend->description,p,sizeof(frontend->description));
  }

  //  double initfreq = config_getint(dictionary,section,"frequency",0);
  // Not implemented for now
  frontend->lock = true;

  // Generate a single carrier at specified frequency and amplitude
  sdr->carrier = 10e6; // Default 10 MHz
  {
    char const *p = config_getstring(dictionary,section,"carrier",NULL);
    if(p != NULL)
      sdr->carrier = parse_frequency(p,false);
  }
  sdr->amplitude = config_getfloat(dictionary,section,"amplitude",-10.0); // Carrier amplitude, default -10 dBFS
  sdr->amplitude = dB2voltage(sdr->amplitude); // Convert from dBFS to peak amplitude
  sdr->modulation = CW; // Default
  {
    char const *m = config_getstring(dictionary,section,"modulation","CW");
    if(strcasecmp(m,"AM") == 0)
      sdr->modulation = AM;
    else if(strcasecmp(m,"DSB") == 0)
      sdr->modulation = DSB;
    else if(strcasecmp(m,"FM") == 0)
      sdr->modulation = FM;
  }
  {
    char const *p = config_getstring(dictionary,section,"source",NULL);
    if(p != NULL)
      sdr->source = strdup(p);
  }
  sdr->noise = config_getfloat(dictionary,section,"noise",101.0); // Noise amplitude dBFS, default off
  if(sdr->noise == 101.0)
    sdr->noise = 0;
  else
    sdr->noise = dB2voltage(sdr->noise);

  fprintf(stdout,"Sig gen %s, samprate %'d, %s, LO freq %'.3f Hz, carrier %'.3lf Hz, amplitude %.1f dBFS, modulation %s, source %s, noise %'.1lf dBFS\n",
	  frontend->description, frontend->samprate, frontend->isreal ? "real":"complex",frontend->frequency,
	  sdr->carrier,
	  voltage2dB(sdr->amplitude),
	  sdr->modulation == CW ? "none" : sdr->modulation == AM ? "AM" : sdr->modulation == FM ? "FM" : "?",
	  sdr->source,
	  voltage2dB(sdr->noise)
	  );


  // Generate noise to be played repeatedly
  if(sdr->noise != 0){
    if(frontend->isreal){
      Real_noise = malloc(sizeof(*Real_noise) * Random_samples);
      for(int i = 0; i < Random_samples; i++)
	Real_noise[i] = M_SQRT1_2 * real_gaussian() * sdr->noise; // 
    } else {
      Complex_noise = malloc(sizeof(*Complex_noise) * Random_samples);
      for(int i = 0; i < Random_samples; i++)
	Complex_noise[i] = complex_gaussian() * sdr->noise * M_SQRT1_2; // Power in both I and Q
    }
    fprintf(stdout,"Noise generated\n");
  }
  return 0;
}
static void *proc_sig_gen(void *arg){
  pthread_setname("proc_siggen");
  struct sdrstate * const sdr = (struct sdrstate *)arg;
  assert(sdr != NULL);
  struct frontend * const frontend = sdr->frontend;
  assert(frontend != NULL);
  
  frontend->timestamp = gps_time_ns();
  float scale = 1 << (frontend->bitspersample-1);

  realtime();

  struct osc carrier;
  memset(&carrier,0,sizeof(carrier));
  if(frontend->isreal)
    set_osc(&carrier,sdr->carrier / frontend->samprate,0.0); // No sweep just yet
  else
    set_osc(&carrier,(sdr->carrier - frontend->frequency)/frontend->samprate,0.0); // Offset down


  FILE *src = NULL;

  if(sdr->source != NULL){
    src = popen(sdr->source,"r");
    if(src == NULL)
      perror("popen");
  }
  if(src == NULL)
    sdr->modulation = CW; // Turn it off

  const int mod_samprate = 48000; // Fixed for now
  const int samps_per_samp = frontend->samprate / mod_samprate;

  while(!Stop_transfers){
    // How long since last call?
    int64_t now = gps_time_ns();
    int64_t interval = now - frontend->timestamp;

    int blocksize = (interval * frontend->samprate) / BILLION;
    // Limit how much we can do in one iteration after a long delay so we don't overwrite the buffer and its mirror
    if(blocksize > Blocksize + Blocksize / 2)
      blocksize = Blocksize + Blocksize / 2;
    frontend->timestamp = now;

    // Pick a random starting point in the noise buffer
    int noise_index = arc4random_uniform(Random_samples - blocksize);
    int modcount = samps_per_samp;
    float modsample = 0;
    float if_energy = 0;
    if(frontend->isreal){
      // Real signal
      float * wptr = frontend->in.input_write_pointer.r;
      for(int i=0; i < blocksize; i++){
	wptr[i] = sdr->amplitude * creal(step_osc(&carrier));
	switch(sdr->modulation){
	case CW:
	  break;
	case DSB:
	  if(modcount-- <= 0){
	    int16_t s = getc(src);
	    s += getc(src) << 8;
	    modsample = (float)s * SCALE16;
	    modcount = samps_per_samp;
	  }
	  wptr[i] *= modsample;
	  break;
	case AM:
	  if(modcount-- <= 0){
	    int16_t s = getc(src);
	    s += getc(src) << 8;
	    modsample = (float)s * SCALE16;
	    modcount = samps_per_samp;
	  }
	  wptr[i] *= 1 + (modsample/2); // Add carrier
	  break;
	case FM: // to be done
	  break;
	}
	if(Real_noise != NULL)
	  wptr[i] += Real_noise[noise_index+i];
	wptr [i] *= scale;
	if_energy += wptr[i] * wptr[i];
      }
      write_rfilter(&frontend->in,NULL,blocksize); // Update write pointer, invoke FFT      
    } else {
      // Complex signal
      complex float * wptr = frontend->in.input_write_pointer.c;
      for(int i=0; i < blocksize; i++){
	wptr[i] = sdr->amplitude * step_osc(&carrier);
	switch(sdr->modulation){
	case CW:
	  break;
	case DSB:
	  if(modcount-- <= 0){
	    int16_t s = getc(src);
	    s += getc(src) << 8;
	    modsample = (float)s / 32767;
	    modsample = 1 + modsample/2; // Add carrier
	    modcount = samps_per_samp;
	  }
	  wptr[i] *= modsample;
	  break;
	case AM:
	  if(modcount-- <= 0){
	    int16_t s = getc(src);
	    s += getc(src) << 8;
	    modsample = (float)s / 32767;
	    modcount = samps_per_samp;
	  }
	  wptr[i] *= 1 + modsample/2; // Add carrier
	  break;
	case FM: // to be done
	  break;
	}
	if(Complex_noise != NULL)
	  wptr[i] += Complex_noise[noise_index+i];
	wptr [i] *= scale;
	if_energy += cnrmf(wptr[i]);
      }
      write_cfilter(&frontend->in,NULL,blocksize); // Update write pointer, invoke FFT      
    }
    // The variability in blocksize due to scheduling variability causes the energy integrated into frontend->if_energy
    // to vary, causing the reported input level to bobble around the nominal value. Long refresh intervals with 'control'
    // will smooth this out, but it's annoying
    frontend->samples += blocksize;    
    frontend->if_power_instant = if_energy / blocksize;
    frontend->if_power = Power_smooth * (frontend->if_power_instant - frontend->if_power);
    // Get status timestamp from UNIX TOD clock
    // Request a half block sleep since this is only the minimum
    {
      struct timespec ts;
      ts.tv_sec = 0;
      ts.tv_nsec = Blocktime * MILLION / 2; // ms -> ns
      nanosleep(&ts,NULL);
    }
  }
  exit(EX_NOINPUT); // Can't get here
}
int sig_gen_startup(struct frontend *frontend){
  assert(frontend != NULL);
  struct sdrstate * const sdr = (struct sdrstate *)frontend->context;
  assert(sdr != NULL);

  // Start processing A/D data
  ASSERT_ZEROED(&sdr->proc_thread, sizeof(sdr->proc_thread));
  pthread_create(&sdr->proc_thread,NULL,proc_sig_gen,sdr);
  fprintf(stdout,"signal generator running\n");
  return 0;
}

double sig_gen_tune(struct frontend * const frontend,double const freq){
  (void)freq; // not used
  assert(frontend != NULL);

  if(frontend->lock)
    return frontend->frequency; // Don't change if locked

  return frontend->frequency; // Not implemented anyway
}

#if 0
// Marsaglia polar method for generating gaussian RVs
// Slow on modern machines because of random branch and pipeline stalls
static complex float complex_gaussian(void){
  complex float result;
  float u,v,s;
  do {
    // range -1, +1
    u = 2 * (float)arc4random() / (float)UINT32_MAX - 1.0;
    v = 2 * (float)arc4random() / (float)UINT32_MAX - 1.0;
    s = u*u + v*v;
  } while(s >= 1);
  float a = sqrtf(-2 * logf(s) / s);
  __real__ result = a * u;
  __imag__ result = a * v;
  return result;
}
#else
// Box-Mueller method that avoids rejection
// Seems faster on i7 despite sincos call
static complex float expif(float x){
  float s = sin(x);
  float c = cos(x);
  return c + I*s;
}

static complex float complex_gaussian(void){
  // RVs uniformly distributed over (0,1)
#if 0
  float u = (float)arc4random() / (float)UINT32_MAX;
  float v = (float)arc4random() / (float)UINT32_MAX;  
#else
  // Not crypto quality (who cares?) but vastly faster.
  float u = (float)random() / (float)INT32_MAX;
  float v = (float)random() / (float)INT32_MAX;  
#endif  
  float s = sqrtf(-2 * logf(u));
  return s * expif(2 * M_PI * v);
}
#endif

// Not thread safe
static float real_gaussian(void){
  static float saved;
  static bool got_saved;
  if(got_saved){
    got_saved = false;
    return saved;
  }
  complex float r = complex_gaussian();
  got_saved = true;
  saved = __imag__ r;
  return __real__ r;
}

#if 0
// From https://www.reddit.com/r/algorithms/comments/yyz59u/fast_approximate_gaussian_generator/
// Double changed to float
// Incomplete and untested
static float real_gaussian(void){
  // input: ulong get_random_uniform() - gets 64 stochastic bits from a prng
  // output: double x - normal deviate (mean 0.0 stdev 1.0) (**more at bottom)

  const float delta = (1.0 / 4294967296.0); // (1 / 2^32)

#if 0
 ulong u = get_random_uniform(); // fast generator that returns 64 randomized bits
  
  uint major = (uint)(u >> 32);	// split into 2 x 32 bits
  uint minor = (uint)u;		// the sus bits of lcgs end up in minor
#else
  uint32_t major = mrand48();
  uint32_t minor = mrand48();
#endif  
  
  float x = PopCount(major);     // x = random binomially distributed integer 0 to 32
  x += minor * delta; 		// linearly fill the gaps between integers
  x -= 16.5;			// re-center around 0 (the mean should be 16+0.5)
  x *= 0.3535534;			// scale to ~1 standard deviation
  return x;
  
  // x now has a mean of 0.0
  // a standard deviation of approximately 1.0
  // and is strictly within +/- 5.833631
  //
  // a good long sampling will reveal that the distribution is approximated 
  // via 33 equally spaced intervals and each interval is itself divided 
  // into 2^32 equally spaced points
  //
  // there are exactly 33 * 2^32 possible outputs (about 37 bits of entropy)
  // the special values -inf, +inf, and NaN are not among the outputs
}


// Even faster version from same discussion thread
static inline double
dist_normal_fast(uint64_t u){
	double x = __builtin_popcountll(u*0x2c1b3c6dU) +
	           __builtin_popcountll(u*0x297a2d39U) - 64;
	x += (int64_t)u * (1 / 9223372036854775808.);
	x *= 0.1765469659009499; /* sqrt(1/(32 + 4/12)) */
	return x;
}
#endif
