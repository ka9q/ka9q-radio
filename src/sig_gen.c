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
#include <samplerate.h>

#include "conf.h"
#include "fcd.h"
#include "misc.h"
#include "config.h"
#include "radio.h"

#define INPUT_PRIORITY 95
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
  "n0",
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

double complex complex_gaussian(double);
double real_gaussian(double);

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
  struct sdrstate * const sdr = calloc(1,sizeof *sdr);
  assert(sdr != NULL);
  sdr->frontend = frontend;
  frontend->context = sdr;

  {
    frontend->samprate = 30e6; // Default 30 MHz
    char const *p = config_getstring(dictionary,section,"samprate",NULL);
    if(p != NULL)
      frontend->samprate = (int)parse_frequency(p,false);
  }
  frontend->rf_gain = NAN;
  frontend->rf_atten = NAN;
  frontend->rf_level_cal = NAN;
  frontend->isreal = config_getboolean(dictionary,section,"real",true);
  frontend->isreal = ! config_getboolean(dictionary,section,"complex",! frontend->isreal);
  frontend->bitspersample = 1; // Input is floating point with no scaling
  if(frontend->isreal){
    frontend->min_IF = 0;
    frontend->max_IF = 0.5 * frontend->samprate;
    frontend->frequency = 0;
  } else {
    frontend->min_IF = -0.5 * frontend->samprate;
    frontend->max_IF = +0.5 * frontend->samprate;
    frontend->frequency = 0.5 * frontend->samprate;
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
    double adb = config_getdouble(dictionary,section,"amplitude",-10.0); // Carrier amplitude, default -10 dBFS
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
  {
    double noise = config_getdouble(dictionary,section,"noise",101.0); // Noise amplitude dBFS, default off
    double n0 = config_getdouble(dictionary,section,"n0",101.0); // Noise amplitude dBFS, default off
    if(noise != 101.0)
      sdr->noise = dB2voltage(noise);
    else if (n0 != 101.0){
      sdr->noise = dB2voltage(n0) * 0.5 * sqrt(frontend->samprate);
    }
    fprintf(stderr,"Sig gen %s, samprate %'lf, %s, LO freq %'.3f Hz, carrier %'.3lf Hz, amplitude %.1f dBFS, modulation %s, source %s, N0 %'.1lf dBJ\n",
	    frontend->description, frontend->samprate, frontend->isreal ? "real":"complex",frontend->frequency,
	    sdr->carrier,
	    voltage2dB(sdr->amplitude),
	    sdr->modulation == CW ? "none" : sdr->modulation == AM ? "AM" : sdr->modulation == FM ? "FM" : "?",
	    sdr->source,
	    voltage2dB(2.0 * sdr->noise / sqrt(frontend->samprate))
	  );
  }
  return 0;
}

FILE *Source = NULL;
#define MODBUFSIZE (96000)
static float src_input_data[MODBUFSIZE];
static int16_t inbuf[MODBUFSIZE];
static long src_callback(void *cb_data,float **data){
  (void)cb_data;
  int len = fread(inbuf, sizeof inbuf[0], MODBUFSIZE, Source);
  src_short_to_float_array (inbuf,src_input_data,len);
  *data = src_input_data;
  return len;
}


static void *proc_sig_gen(void *arg){
  pthread_setname("proc_siggen");
  struct sdrstate * const sdr = (struct sdrstate *)arg;
  assert(sdr != NULL);
  struct frontend * const frontend = sdr->frontend;
  assert(frontend != NULL);

  int64_t timesnap = gps_time_ns();
  realtime(INPUT_PRIORITY);

  struct osc carrier = {0};
  if(frontend->isreal)
    set_osc(&carrier,sdr->carrier / frontend->samprate,0.0); // No sweep just yet
  else
    set_osc(&carrier,(sdr->carrier - frontend->frequency)/frontend->samprate,0.0); // Offset down

  rand_init();

  if(sdr->modulation != CW && sdr->source != NULL){
    Source = popen(sdr->source,"r");
    if(Source == NULL)
      perror("popen");
  }
  if(Source == NULL)
    sdr->modulation = CW; // Turn it off

  int const mod_samprate = FULL_SAMPRATE; // Fixed for now
  double const upsample_ratio = (double)frontend->samprate / mod_samprate;

  // Input buffer with modulation to sample rate converter
  long const output_size = lrint(Blocktime * frontend->samprate);
  // Output buffer with modulation at virtual A/D rate
  float * const dac_modulation = malloc(output_size * sizeof *dac_modulation);

  // Set up sample rate converter for modulation
  int error = 0;
  //  SRC_STATE * const src_state = src_callback_new(src_callback,SRC_SINC_FASTEST, 1, &error,NULL);
  SRC_STATE * const src_state = src_callback_new(src_callback,SRC_LINEAR, 1, &error,NULL);
  if(error != 0)
    fprintf(stderr,"src_callback_new: %s\n",src_strerror(error));
  assert(error == 0);
  assert(src_state != NULL);
  while(!Stop_transfers){
    // How long since last call?
    int64_t now = gps_time_ns();
    int64_t interval = now - timesnap;

    long blocksize = lrint((interval * frontend->samprate) / BILLION);
    // Limit how much we can do in one iteration after a long delay so we don't overwrite the buffer and its mirror
    // or the sample rate converter buffer
    if(blocksize > output_size)
      blocksize = output_size;
    timesnap = now;

    double in_energy = 0;
    // Note lack of bandpass filtering on modulation - this creates alias images across the spectrum
    // at multiples of mod_samprate when the synthetic noise is very low
    // This needs to be redone using sample rate conversion
    double const dc = sdr->modulation == AM ? 1.0 : 0.0; // carrier component amplitude

    if(frontend->isreal){
      // Real signal
      float * wptr = frontend->in.input_write_pointer.r;
      if(sdr->modulation == CW || sdr->modulation == FM){ // FM to be implemented
	for(long i=0; i < blocksize; i++){
	  double samp = sdr->amplitude * creal(step_osc(&carrier)) + sdr->noise * real_gauss();
	  in_energy += samp * samp;
	  wptr[i] = samp * sdr->scale;
	}
      } else {
	long r = src_callback_read(src_state, upsample_ratio, blocksize, dac_modulation);
	assert(r == blocksize);
	blocksize = r;
	for(long int i=0; i < blocksize; i++){
	  double samp = (dc + dac_modulation[i]) * sdr->amplitude * creal(step_osc(&carrier)) + sdr->noise * real_gauss();
	  in_energy += samp * samp;
	  wptr[i] = samp * sdr->scale;
	}
      }
      int r = write_rfilter(&frontend->in, NULL, blocksize); // Update write pointer, invoke FFT
      assert(r != -1);
      (void)r;
    } else {
      // Front end is complex
      float complex * wptr = frontend->in.input_write_pointer.c;
      if(sdr->modulation == CW || sdr->modulation == FM){ // FM to be implemented
	for(long i=0; i < blocksize; i++){
	  double complex samp = sdr->amplitude * step_osc(&carrier) + sdr->noise * complex_gauss();
	  in_energy += samp * samp;
	  wptr[i] = samp * sdr->scale;
	}
      } else {
	long r = src_callback_read(src_state, upsample_ratio, blocksize, dac_modulation);
	assert(r == blocksize);
	blocksize = r;
	for(long i=0; i < blocksize; i++){
	  double complex samp = (dc + dac_modulation[i]) * sdr->amplitude * step_osc(&carrier) + sdr->noise * real_gauss();
	  in_energy += samp * samp;
	  wptr[i] = samp * sdr->scale;
	}
      }
      int r = write_cfilter(&frontend->in, NULL, blocksize); // Update write pointer, invoke FFT
      assert(r != -1);
      (void)r;
    }
    frontend->samples += blocksize;
    // The variability in blocksize due to scheduling variability causes the energy integrated into frontend->if_power
    // to vary, causing the reported input level to bobble around the nominal value. Long refresh intervals with 'control'
    // will smooth this out, but it's annoying

    frontend->if_power += Power_alpha * (in_energy / blocksize - frontend->if_power);
    // Get status timestamp from UNIX TOD clock
    // Request a half block sleep since this is only the minimum
    {
      struct timespec ts = {
	.tv_sec = 0,
	.tv_nsec = lrint(Blocktime * BILLION/2) // s -> ns
      };
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

