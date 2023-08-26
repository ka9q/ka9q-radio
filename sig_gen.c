// signal generator - looks like a pseudo-front-end
// Aug 2023 KA9Q
#define _GNU_SOURCE 1

#include <pthread.h>
#include <portaudio.h>
#include <errno.h>
#include <iniparser/iniparser.h>
#if defined(linux)
#include <bsd/string.h>
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

  pthread_t proc_thread;
};

// Variables set by command line options
// A larger blocksize makes more efficient use of each frame, but the receiver generally runs on
// frames that match the Opus codec: 2.5, 5, 10, 20, 40, 60, 180, 100, 120 ms
// So to minimize latency, make this a common denominator:
// 240 samples @ 16 bit stereo = 960 bytes/packet; at 192 kHz, this is 1.25 ms (800 pkt/sec)
static int Blocksize;
extern bool Stop_transfers;


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
  sdr->amplitude = pow(10.0,sdr->amplitude / 20.0); // Convert from dBFS to peak amplitude

  fprintf(stdout,"Sig gen %s, samprate %'d, %s, LO freq %'.3f Hz, tone %'.3lf Hz, amplitude %.1f dB\n",
	  frontend->description, frontend->samprate, frontend->isreal ? "real":"complex",frontend->frequency,
	  sdr->tone,
	  voltage2dB(sdr->amplitude)
	  );


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

    if(frontend->isreal){
      // Real tone
      float * wptr = frontend->in->input_write_pointer.r;
      for(int i=0; i < blocksize; i++)
	wptr[i] = creal(step_osc(&tone) * sdr->amplitude * scale);

      write_rfilter(frontend->in,NULL,blocksize); // Update write pointer, invoke FFT      
    } else {
      // Complex tone
      complex float * wptr = frontend->in->input_write_pointer.c;
      for(int i=0; i < blocksize; i++)
	wptr[i] = step_osc(&tone) * sdr->amplitude * scale;

      write_cfilter(frontend->in,NULL,blocksize); // Update write pointer, invoke FFT      
    }
    frontend->samples += blocksize;    
    frontend->if_power = (sdr->amplitude * scale) * (sdr->amplitude * scale); // normalized to 1.0 = 0 dB
    frontend->if_energy += frontend->if_power * (float)blocksize; // Total energy in all samples
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

