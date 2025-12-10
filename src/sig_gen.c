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

#define INPUT_PRIORITY 95
static int const Random_samples = 30000000;
static double Power_alpha = 0.01; // Calculate this properly someday


enum modulation {
  CW = 0, // No modulation
  DSB, // AM without a carrier
  AM,
  FM // Not yet implemented
};
static char const *Sig_gen_keys[] = {
  "device",
  "samprate",
  "real",
  "complex",
  "description",
  "carrier",
  "amplitude",
  "modulation",
  "source",
  "noise",
  NULL
};



struct sdrstate {
  struct frontend *frontend;
  double carrier; // Carrier frequency to generate
  double amplitude; // Amplitude of carrier
  double noise; // Amplitude of noise
  enum modulation modulation;
  char *source;
  double scale;

  pthread_t proc_thread;
};

// A larger blocksize makes more efficient use of each frame, but the receiver generally runs on
// frames that match the Opus codec: 2.5, 5, 10, 20, 40, 60, 180, 100, 120 ms
// So to minimize latency, make this a common denominator:
// 240 samples @ 16 bit stereo = 960 bytes/packet; at 192 kHz, this is 1.25 ms (800 pkt/sec)
extern bool Stop_transfers;
extern char const *Description;

// One second of noise in requested format
// Will be played with a random starting point every block
float complex *Complex_noise;
float *Real_noise;

static double complex complex_gaussian(void);
static double real_gaussian(void);

double sig_gen_tune(struct frontend * const frontend,double const freq);

int sig_gen_setup(struct frontend * const frontend, dictionary * const dictionary, char const * const section){


  assert(dictionary != NULL);
  {
    char const * const device = config_getstring(dictionary,section,"device",section);
    if(strcasecmp(device,"sig_gen") != 0)
      return -1; // Not for us
  }
  config_validate_section(stderr,dictionary,section,Sig_gen_keys,NULL);

  // Cross-link generic and hardware-specific control structures
  struct sdrstate * const sdr = calloc(1,sizeof(*sdr));
  assert(sdr != NULL);
  sdr->frontend = frontend;
  frontend->context = sdr;

  {
    frontend->samprate = 30e6; // Default 30 MHz
    char const *p = config_getstring(dictionary,section,"samprate",NULL);
    if(p != NULL)
      frontend->samprate = (int)parse_frequency(p,false);
  }
  frontend->rf_gain = 0;
  frontend->rf_atten = 0;
  frontend->rf_level_cal = 0;
  frontend->isreal = config_getboolean(dictionary,section,"real",true);
  frontend->isreal = ! config_getboolean(dictionary,section,"complex",! frontend->isreal);
  frontend->bitspersample = 1; // Input is floating point with no scaling
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
    char const * const p = config_getstring(dictionary,section,"description",Description ? Description : "signal generator");
    if(p != NULL){
      strlcpy(frontend->description,p,sizeof(frontend->description));
      Description = p;
    }
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
  {
    float adb = config_getfloat(dictionary,section,"amplitude",-10.0); // Carrier amplitude, default -10 dBFS
    sdr->amplitude = dB2voltage(adb); // Convert from dBFS to peak amplitude
  }
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

  fprintf(stderr,"Sig gen %s, samprate %'lf, %s, LO freq %'.3f Hz, carrier %'.3lf Hz, amplitude %.1f dBFS, modulation %s, source %s, noise %'.1lf dBFS\n",
	  frontend->description, frontend->samprate, frontend->isreal ? "real":"complex",frontend->frequency,
	  sdr->carrier,
	  voltage2dB(sdr->amplitude),
	  sdr->modulation == CW ? "none" : sdr->modulation == AM ? "AM" : sdr->modulation == FM ? "FM" : "?",
	  sdr->source,
	  voltage2dB(sdr->noise)
	  );


  // Generate noise to be played repeatedly
  // Redo this!
  if(sdr->noise != 0){
    if(frontend->isreal){
      Real_noise = malloc(sizeof(*Real_noise) * Random_samples);
      for(int i = 0; i < Random_samples; i++)
	Real_noise[i] = (float)(M_SQRT1_2 * real_gaussian() * sdr->noise);
    } else {
      Complex_noise = malloc(sizeof(*Complex_noise) * Random_samples);
      for(int i = 0; i < Random_samples; i++)
	Complex_noise[i] = (float complex)(complex_gaussian() * sdr->noise * M_SQRT1_2); // Power in both I and Q
    }
    fprintf(stderr,"Noise generated\n");
  }
  return 0;
}
static void *proc_sig_gen(void *arg){
  pthread_setname("proc_siggen");
  struct sdrstate * const sdr = (struct sdrstate *)arg;
  assert(sdr != NULL);
  struct frontend * const frontend = sdr->frontend;
  assert(frontend != NULL);
  
  int64_t timesnap = gps_time_ns();
  realtime(INPUT_PRIORITY);

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
  const int samps_per_samp = (int)round(frontend->samprate / mod_samprate);

  while(!Stop_transfers){
    // How long since last call?
    int64_t now = gps_time_ns();
    int64_t interval = now - timesnap;

    int blocksize = (int)round((interval * frontend->samprate) / BILLION);
    // Limit how much we can do in one iteration after a long delay so we don't overwrite the buffer and its mirror
    if(blocksize > frontend->L + frontend->L / 2)
      blocksize = frontend->L + frontend->L / 2;
    timesnap = now;

    // Pick a random starting point in the noise buffer
    int noise_index = arc4random_uniform(Random_samples - blocksize);
    int modcount = samps_per_samp;
    float modsample = 0;
    double in_energy = 0;
    // Note lack of bandpass filtering on modulation - this creates alias images across the spectrum
    // at multiples of mod_samprate when the synthetic noise is very low
    if(frontend->isreal){
      // Real signal
      float * wptr = frontend->in.input_write_pointer.r;
      for(int i=0; i < blocksize; i++){
	wptr[i] = (float)(sdr->amplitude * creal(step_osc(&carrier)));
	switch(sdr->modulation){
	case CW:
	  break;
	case DSB:
	  if(modcount-- <= 0){
	    int s = getc(src);
	    s += getc(src) << 8;
	    modsample = (float)s * SCALE16;
	    modcount = samps_per_samp;
	  }
	  wptr[i] *= modsample;
	  break;
	case AM:
	  if(modcount-- <= 0){
	    int s = getc(src);
	    s += getc(src) << 8;
	    modsample = (float)s * SCALE16;
	    modcount = samps_per_samp;
	  }
	  wptr[i] *= 1 + (modsample/2); // Add carrier
	  break;
	case FM: // to be done
	  break;
	}
	double s = wptr[i];
	if(Real_noise != NULL)
	  s += Real_noise[noise_index+i];
	in_energy += s * s;
	wptr [i] = (float)(s * sdr->scale);
      }
      write_rfilter(&frontend->in,NULL,blocksize); // Update write pointer, invoke FFT      
    } else {
      // Complex signal
      float complex * wptr = frontend->in.input_write_pointer.c;
      for(int i=0; i < blocksize; i++){
	wptr[i] = (float)(sdr->amplitude * step_osc(&carrier));
	switch(sdr->modulation){
	case CW:
	  break;
	case DSB:
	  if(modcount-- <= 0){
	    int s = getc(src);
	    s += getc(src) << 8;
	    modsample = (float)s / 32767;
	    modsample = 1 + modsample/2; // Add carrier
	    modcount = samps_per_samp;
	  }
	  wptr[i] *= modsample;
	  break;
	case AM:
	  if(modcount-- <= 0){
	    int s = getc(src);
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
	in_energy += cnrmf(wptr[i]);
	wptr [i] *= sdr->scale;
      }
      write_cfilter(&frontend->in,NULL,blocksize); // Update write pointer, invoke FFT      
    }
    // The variability in blocksize due to scheduling variability causes the energy integrated into frontend->if_power
    // to vary, causing the reported input level to bobble around the nominal value. Long refresh intervals with 'control'
    // will smooth this out, but it's annoying
    frontend->samples += blocksize;    
    frontend->if_power += Power_alpha * (in_energy / blocksize - frontend->if_power);
    // Get status timestamp from UNIX TOD clock
    // Request a half block sleep since this is only the minimum
    {
      struct timespec ts;
      ts.tv_sec = 0;
      ts.tv_nsec = (int)round(Blocktime * BILLION / 2); // s -> ns
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
  sdr->scale = scale_AD(frontend);
  pthread_create(&sdr->proc_thread,NULL,proc_sig_gen,sdr);
  fprintf(stderr,"signal generator running\n");
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
static float complex complex_gaussian(void){
  float complex result;
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
static double complex expi(double x){
  double s = sin(x);
  double c = cos(x);
  return c + I*s;
}

static double complex complex_gaussian(void){
  // RVs uniformly distributed over (0,1)
#if 0
  double u = (double)arc4random() / (double)UINT32_MAX;
  double v = (double)arc4random() / (double)UINT32_MAX;  
#else
  // Not crypto quality (who cares?) but vastly faster.
  double u = (double)random() / (double)INT32_MAX;
  double v = (double)random() / (double)INT32_MAX;  
#endif  
  double s = sqrt(-2 * log(u));
  return s * expi(2 * M_PI * v);
}
#endif

// Not thread safe
static double real_gaussian(void){
  static double saved;
  static bool got_saved;
  if(got_saved){
    got_saved = false;
    return saved;
  }
  double complex r = complex_gaussian();
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
#if 0
// Beginnings of a new generator from ChatGPT
#include <stdio.h>
#include <stdint.h>
#include <math.h>


/*
 * Build a 16-bit Gaussian noise table.
 *
 * table[u] gives a signed 16-bit Gaussian-distributed sample corresponding
 * to uniform 16-bit input u in [0, 65535].
 *
 * sigma is the desired standard deviation IN ADC COUNTS (LSBs), i.e.
 * if sigma = 1000.0, then RMS noise is 1000 counts.
 *
 * The tails of the continuous Gaussian beyond +/-32768 are folded into
 * the int16_t saturation values -32768 and +32767 automatically.
 */

#define GAUSS_TABLE_SIZE 65536
#define INT16_MIN_VAL (-32768)
#define INT16_MAX_VAL (32767)

/* Standard normal PDF: phi(x) */
static double normal_pdf(double x)
{
    const double inv_sqrt_2pi = 0.39894228040143267793994605993438; /* 1/sqrt(2*pi) */
    return inv_sqrt_2pi * exp(-0.5 * x * x);
}

/* Standard normal CDF: Phi(x) using erfc */
static double normal_cdf(double x)
{
    /* Phi(x) = 0.5 * erfc(-x / sqrt(2)) */
    return 0.5 * erfc(-x / M_SQRT2);
}

/*
 * Inverse standard normal CDF Phi^{-1}(p), 0 < p < 1
 * Using Newton–Raphson with a simple tail-based initial guess.
 * This runs only 65k times at startup, so we favor clarity over speed.
 */
static double normal_inv_cdf(double p)
{
    /* Clamp p away from 0 and 1 to avoid log(0) and infinities */
    if (p <= 1e-12) p = 1e-12;
    if (p >= 1.0 - 1e-12) p = 1.0 - 1e-12;

    /* Initial guess: rough tail approximation */
    double x;
    if (p < 0.5) {
        double t = sqrt(-2.0 * log(p));   /* large negative tail */
        x = -t;
    } else {
        double t = sqrt(-2.0 * log1p(-p)); /* large positive tail */
        x = t;
    }

    /* Newton–Raphson refinement */
    for (int iter = 0; iter < 10; iter++) {
        double phi  = normal_pdf(x);
        double Phi  = normal_cdf(x);
        double diff = Phi - p;

        /* If PDF is tiny, avoid blowing up the step */
        if (phi < 1e-12)
            break;

        double step = diff / phi;
        x -= step;

        if (fabs(step) < 1e-12)
            break;
    }

    return x;
}

/*
 * Build the Gaussian lookup table.
 */
void build_gauss_table(int16_t table[GAUSS_TABLE_SIZE], double sigma)
{
    for (uint32_t i = 0; i < GAUSS_TABLE_SIZE; i++) {
        /* Center p in the middle of each 1/65536 interval */
        double p = ( (double)i + 0.5 ) / (double)GAUSS_TABLE_SIZE;  /* (0,1) */

        /* Standard normal quantile */
        double z = normal_inv_cdf(p);

        /* Scale to desired sigma (in ADC counts) */
        double y = sigma * z;

        /* Round to nearest integer and saturate to int16_t range */
        long v = lround(y);
        if (v < INT16_MIN_VAL)
            v = INT16_MIN_VAL;
        else if (v > INT16_MAX_VAL)
            v = INT16_MAX_VAL;

        table[i] = (int16_t)v;
    }
}

/* Example driver to write the table out as a C array */
int main(void)
{
    static int16_t gauss_table[GAUSS_TABLE_SIZE];

    /* Example: sigma = 1000 ADC counts RMS */
    double sigma = 1000.0;
    build_gauss_table(gauss_table, sigma);

    /* For demonstration: print a few entries */
    for (int i = 0; i < 16; i++) {
        printf("gauss_table[%5d] = %6d\n", i, gauss_table[i]);
    }

    /* In your real code, you’d probably write this table to a file,
       or keep it in memory for your noise generator. */

    return 0;
}
#include <stdint.h>

/* xoshiro256** PRNG
 *
 * Public domain reference implementation adapted from:
 *   http://prng.di.unimi.it/
 *
 * State must be nonzero. Use the seed function below.
 */

typedef struct {
    uint64_t s[4];
} xoshiro256ss_state;

/* Rotate left */
static inline uint64_t rotl64(uint64_t x, int k)
{
    return (x << k) | (x >> (64 - k));
}

/* SplitMix64 for seeding xoshiro256** */
static uint64_t splitmix64(uint64_t *x)
{
    uint64_t z = (*x += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

/* Initialize xoshiro256** state from a 64-bit seed */
static inline void xoshiro256ss_seed(xoshiro256ss_state *st, uint64_t seed)
{
    /* Expand a single 64-bit seed into 4 nonzero 64-bit words */
    uint64_t x = seed;
    st->s[0] = splitmix64(&x);
    st->s[1] = splitmix64(&x);
    st->s[2] = splitmix64(&x);
    st->s[3] = splitmix64(&x);

    /* Extremely unlikely, but ensure not all zeros */
    if ((st->s[0] | st->s[1] | st->s[2] | st->s[3]) == 0) {
        st->s[0] = 1; /* arbitrary nonzero */
    }
}

/* Generate next 64-bit output */
static inline uint64_t xoshiro256ss_next(xoshiro256ss_state *st)
{
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

/* Optional: jump function for 2^128 steps ahead (independent streams) */
static inline void xoshiro256ss_jump(xoshiro256ss_state *st)
{
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

static int16_t gaussian_sample(){

uint64_t r = xoshiro256starstar(&state);

uint16_t u0 = (r >>  0) & 0xFFFF;
uint16_t u1 = (r >> 16) & 0xFFFF;
uint16_t u2 = (r >> 32) & 0xFFFF;
uint16_t u3 = (r >> 48) & 0xFFFF;

int16_t n0 = gauss_table[u0];
int16_t n1 = gauss_table[u1];
int16_t n2 = gauss_table[u2];
int16_t n3 = gauss_table[u3];

/* Then add to your synthetic WWV samples, or convert to float, etc. */
}


#endif
