// signal generator - looks like a pseudo-front-end
// Aug 2023 KA9Q
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

#include "conf.h"
#include "fcd.h"
#include "misc.h"
#include "config.h"
#include "radio.h"

struct sdrstate {
  struct frontend *frontend;
  double tone; // Tone frequency to generate
  float amplitude; // Amplitude of tone
  float noise; // Amplitude of noise

  pthread_t proc_thread;
};

// Variables set by command line options
// A larger blocksize makes more efficient use of each frame, but the receiver generally runs on
// frames that match the Opus codec: 2.5, 5, 10, 20, 40, 60, 180, 100, 120 ms
// So to minimize latency, make this a common denominator:
// 240 samples @ 16 bit stereo = 960 bytes/packet; at 192 kHz, this is 1.25 ms (800 pkt/sec)
static int Blocksize;
extern bool Stop_transfers;

static int const Random_samples = 30000000;


// One second of noise in requested format
complex float *Complex_noise;
float *Real_noise;

complex float complex_gaussian(void);
float real_gaussian(void);

double sig_gen_tune(struct frontend * const frontend,double const freq);

int sig_gen_setup(struct frontend * const frontend, dictionary * const dictionary, char const * const section){
  assert(dictionary != NULL);
  {
    char const * const device = config_getstring(dictionary,section,"device",NULL);
    if(strcasecmp(device,"sig_gen") != 0)
      return -1; // Not for us
  }
  // Cross-link generic and hardware-specific control structures
  struct sdrstate * const sdr = calloc(1,sizeof(*sdr));
  sdr->frontend = frontend;
  frontend->context = sdr;

  frontend->samprate = config_getint(dictionary,section,"samprate",60000000); // Default 60 MHz
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
  frontend->reference = 1 << (frontend->bitspersample-1);
  if(frontend->isreal){
    frontend->min_IF = 0;
    frontend->max_IF = frontend->samprate / 2;
  } else {
    frontend->min_IF = -frontend->samprate/2;
    frontend->max_IF = -frontend->min_IF;
  }
  {
    char const * const description = config_getstring(dictionary,section,"description","funcube dongle+");
    strlcpy(frontend->description,description,sizeof(frontend->description));
  }

  //  double initfreq = config_getint(dictionary,section,"frequency",0);
  // Direct sampling only for now
  frontend->frequency = 0;
  frontend->lock = true;

  // Generate a single tone at specified frequency and amplitude
  sdr->tone = config_getdouble(dictionary,section,"tone",10000000.0); // Tone to generate, default 10 MHz
  sdr->amplitude = config_getfloat(dictionary,section,"amplitude",-10.0); // Tone amplitude, default -10 dBFS
  sdr->amplitude = dB2voltage(sdr->amplitude); // Convert from dBFS to peak amplitude
  sdr->noise = config_getfloat(dictionary,section,"noise",101.0); // Noise amplitude dBFS, default off
  if(sdr->noise == 101.0)
    sdr->noise = 0;
  else
    sdr->noise = dB2voltage(sdr->noise);

  fprintf(stdout,"Sig gen %s, samprate %'d, %s, LO freq %'.3f Hz, tone %'.3lf Hz, amplitude %.1f dBFS, noise %'.1lf dBFS\n",
	  frontend->description, frontend->samprate, frontend->isreal ? "real":"complex",frontend->frequency,
	  sdr->tone,
	  voltage2dB(sdr->amplitude),
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
void *proc_sig_gen(void *arg){
  pthread_setname("proc_siggen");
  struct sdrstate * const sdr = (struct sdrstate *)arg;
  assert(sdr != NULL);
  struct frontend * const frontend = sdr->frontend;
  assert(frontend != NULL);
  
  frontend->timestamp = gps_time_ns();
  float scale = 1 << (frontend->bitspersample-1);

  realtime();

  struct osc tone;
  memset(&tone,0,sizeof(tone));
  set_osc(&tone,sdr->tone / frontend->samprate,0.0); // No sweep just yet


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
    float if_energy = 0;
    if(frontend->isreal){
      // Real signal
      float * wptr = frontend->in->input_write_pointer.r;
      if(Real_noise != NULL){
	for(int i=0; i < blocksize; i++){
	  wptr[i] = (Real_noise[noise_index+i] + sdr->amplitude * creal(step_osc(&tone))) * scale;
	  frontend->if_energy += wptr[i] * wptr[i];
	}
      } else {
	for(int i=0; i < blocksize; i++){
	  wptr[i] = (sdr->amplitude * creal(step_osc(&tone))) * scale;
	  frontend->if_energy += wptr[i] * wptr[i];
	}
      }
      write_rfilter(frontend->in,NULL,blocksize); // Update write pointer, invoke FFT      
    } else {
      // Complex signal
      complex float * wptr = frontend->in->input_write_pointer.c;
      if(Complex_noise != NULL){
	for(int i=0; i < blocksize; i++){
	  wptr[i] = (Complex_noise[noise_index+i] + sdr->amplitude * step_osc(&tone)) * scale;
	  frontend->if_energy += cnrmf(wptr[i]);
	}
      } else {
	for(int i=0; i < blocksize; i++){
	  wptr[i] = (sdr->amplitude * step_osc(&tone)) * scale;
	  frontend->if_energy += cnrmf(wptr[i]);
	}
      }
      write_cfilter(frontend->in,NULL,blocksize); // Update write pointer, invoke FFT      
    }
    frontend->samples += blocksize;    
    frontend->if_power = if_energy / blocksize;
    frontend->if_energy += if_energy;
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
  pthread_create(&sdr->proc_thread,NULL,proc_sig_gen,sdr);
  fprintf(stdout,"signal generator running\n");
  return 0;
}

double sig_gen_tune(struct frontend * const frontend,double const freq){
  assert(frontend != NULL);

  if(frontend->lock)
    return frontend->frequency; // Don't change if locked

  return frontend->frequency; // Not implemented anyway
}

// Marsaglia polar method for generating gaussian RVs
complex float complex_gaussian(void){
  complex float result;
  float u,v,s;
  do {
    // Generate pair of gaussians using polar method
    u = 2 * (float)arc4random() / (float)UINT32_MAX - 1.0;
    v = 2 * (float)arc4random() / (float)UINT32_MAX - 1.0;
    s = u*u + v*v;
  } while(s >= 1);
  float a = sqrtf(-2 * logf(s) / s);
  __real__ result = a * u;
  __imag__ result = a * v;
  return result;
}

float real_gaussian(void){
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
