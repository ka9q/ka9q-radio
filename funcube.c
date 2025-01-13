// funcube driver module linked into radiod
// Copyright July 2023, Phil Karn,  KA9Q
#define _GNU_SOURCE 1

#include <pthread.h>
#include <portaudio.h>
#include <errno.h>
#include <iniparser/iniparser.h>
#if defined(linux)
#include <bsd/string.h>
#endif
#include <sysexits.h>
#include <strings.h>

#include "conf.h"
#include "fcd.h"
#include "misc.h"
#include "config.h"
#include "radio.h"

struct sdrstate {
  struct frontend *frontend;

  // Stuff for sending commands
  void *phd;               // Opaque pointer to type hid_device

  int number;

  // Smoothed error estimates
  complex float DC;      // DC offset
  float sinphi;          // I/Q phase error
  float imbalance;       // Ratio of I power to Q power
  double calibration;    // TCXO Offset (0 = on frequency)

  uint8_t bias_tee;
  bool agc;             // enable/disable agc
  float scale;          // Scale samples for #bits and front end gain

  // portaudio parameters
  PaStream *Pa_Stream;       // Portaudio handle
  char sdr_name[50];         // name of associated audio device for A/D
  int overrun;               // A/D overrun count
  int overflows;

  pthread_t proc_thread;
};

// constants, some of which you might want to tweak
static float const AGC_upper = -15;
static float const AGC_lower = -50;
static int const ADC_samprate = 192000;
static float const DC_alpha = 1.0e-6;  // high pass filter coefficient for DC offset estimates, per sample
static float const Power_alpha = 1.0; // time constant (seconds) for smoothing power and I/Q imbalance estimates

static float Power_smooth = 0.05; // Calculate this properly someday

// Empirical: noticeable aliasing beyond this noticed on strong 40m SSB signals
static float const LowerEdge = -75000;
static float const UpperEdge = +75000;

// Variables set by command line options
// A larger blocksize makes more efficient use of each frame, but the receiver generally runs on
// frames that match the Opus codec: 2.5, 5, 10, 20, 40, 60, 80, 100, 120 ms
// So to minimize latency, make this a common denominator:
// 240 samples @ 16 bit stereo = 960 bytes/packet; at 192 kHz, this is 1.25 ms (800 pkt/sec)
static int Blocksize;
static bool Hold_open = false;

static char const *Funcube_keys[] = {
  "bias"
  "calibrate",
  "description",
  "device",
  "frequency",
  "number",
  NULL
};


static void do_fcd_agc(struct sdrstate *);
static double fcd_actual(unsigned int);

double funcube_tune(struct frontend * const frontend,double const freq);

int funcube_setup(struct frontend * const frontend, dictionary * const dictionary, char const * const section){
  assert(dictionary != NULL);
  {
    char const * const device = config_getstring(dictionary,section,"device",NULL);
    if(strcasecmp(device,"funcube") != 0)
      return -1; // Not for us
  }
  config_validate_section(stdout,dictionary,section,Funcube_keys,NULL);
  // Cross-link generic and hardware-specific control structures
  struct sdrstate * const sdr = calloc(1,sizeof(*sdr));
  sdr->frontend = frontend;
  frontend->context = sdr;

  sdr->number = config_getint(dictionary,section,"number",0);
  frontend->samprate = ADC_samprate;
  {
    double const eL = frontend->samprate * Blocktime / 1000.0; // Blocktime is in milliseconds
    Blocksize = lround(eL);
  }

  frontend->isreal = false; // Complex sample stream
  frontend->bitspersample = 16;
  frontend->min_IF = LowerEdge;
  frontend->max_IF = UpperEdge;
  frontend->calibrate = config_getdouble(dictionary,section,"calibrate",0);
  {
    char const * const p = config_getstring(dictionary,section,"description","funcube dongle+");
    FREE(frontend->description);
    frontend->description = strdup(p);
  }
  Pa_Initialize();

  if(sdr->phd == NULL && (sdr->phd = fcdOpen(sdr->sdr_name,sizeof(sdr->sdr_name),sdr->number)) == NULL){
    fprintf(stdout,"fcdOpen(%d) failed\n",sdr->number);
    return -1;
  }
  sdr->bias_tee = config_getboolean(dictionary,section,"bias",false);
  fcdAppSetParam(sdr->phd,FCD_CMD_APP_SET_BIAS_TEE,&sdr->bias_tee,sizeof(sdr->bias_tee));
  int r;
  if((r = fcdGetMode(sdr->phd)) == FCD_MODE_APP){
    char caps_str[100];
    fcdGetCapsStr(sdr->phd,caps_str);
    fprintf(stdout,"audio device name '%s', caps '%s'\n",sdr->sdr_name,caps_str);
  } else if(r == FCD_MODE_NONE){
    fprintf(stdout," No FCD detected!\n");
    r = -1;
    goto done;
  } else if (r == FCD_MODE_BL){
    fprintf(stdout," is in bootloader mode\n");
    r = -1;
    goto done;
  }
  // Set initial frequency
  double initfreq = 0;
  {
    char const *p = config_getstring(dictionary,section,"frequency",NULL);
    if(p != NULL)
      initfreq = parse_frequency(p,false);
  }
  if(initfreq != 0){
    funcube_tune(frontend,initfreq);
    frontend->lock = true;
  }
  // Set up sample stream through portaudio subsystem
  // Search audio devices

  int const numDevices = Pa_GetDeviceCount();
  int inDevNum = paNoDevice;
  for(int i = 0; i < numDevices; i++){
    PaDeviceInfo const *deviceInfo = Pa_GetDeviceInfo(i);
    if(strstr(deviceInfo->name,sdr->sdr_name) != NULL){
      inDevNum = i;
      fprintf(stdout,"portaudio name: %s\n",deviceInfo->name);
      break;
    }
  }
  if(inDevNum == paNoDevice){
    fprintf(stdout,"Can't find portaudio name\n");
    r = -1;
    goto done;
  }
  PaStreamParameters inputParameters;
  memset(&inputParameters,0,sizeof(inputParameters));
  inputParameters.channelCount = 2;
  inputParameters.device = inDevNum;
  inputParameters.sampleFormat = paInt16;
  inputParameters.suggestedLatency = 0.020;
  r = Pa_OpenStream(&sdr->Pa_Stream,&inputParameters,NULL,ADC_samprate,
		    paFramesPerBufferUnspecified, 0, NULL, NULL);
  if(r < 0){
    fprintf(stdout,"Pa_OpenStream error: %s\n",Pa_GetErrorText(r));
    goto done;
  }
  r = Pa_StartStream(sdr->Pa_Stream);
  if(r < 0){
    fprintf(stdout,"Pa_StartStream error: %s\n",Pa_GetErrorText(r));
    goto done;
  }

  fprintf(stdout,"Funcube %d: software AGC %d, samprate %'d, freq %'.3f Hz, bias %d, lna_gain %d, mixer gain %d, if_gain %d\n",
	  sdr->number, sdr->agc, frontend->samprate, frontend->frequency, sdr->bias_tee, frontend->lna_gain, frontend->mixer_gain, frontend->if_gain);

 done:; // Also the abort target: close handle before returning
  if(!Hold_open && sdr->phd != NULL){
    fcdClose(sdr->phd);
    sdr->phd = NULL;
  }
  return r;
}
static void *proc_funcube(void *arg){
  pthread_setname("proc_funcube");
  struct sdrstate * const sdr = (struct sdrstate *)arg;
  assert(sdr != NULL);
  struct frontend * const frontend = sdr->frontend;
  assert(frontend != NULL);

  // Gain and phase corrections. These will be updated every block
  float gain_q = 1;
  float gain_i = 1;
  float secphi = 1;
  float tanphi = 0;

  frontend->timestamp = gps_time_ns();
  float const rate_factor = Blocksize/(ADC_samprate * Power_alpha);
  int ConsecPaErrs = 0;
  int16_t * sampbuf = malloc(2 * Blocksize * sizeof(*sampbuf)); // complex samples have two integers

  realtime();

  while(true){
    // Read block of I/Q samples from A/D converter
    // The timer is necessary because portaudio will go into a tight loop if the device is unplugged
    struct itimerval itime;
    memset(&itime,0,sizeof(itime));
    itime.it_value.tv_sec = 1; // 1 second should be more than enough
    if(setitimer(ITIMER_VIRTUAL,&itime,NULL) == -1){
      perror("setitimer start");
      goto terminate;
    }
    int const r = Pa_ReadStream(sdr->Pa_Stream,sampbuf,Blocksize);
    memset(&itime,0,sizeof(itime));
    if(setitimer(ITIMER_VIRTUAL,&itime,NULL) == -1){
      perror("setitimer stop");
      goto terminate;
    }
    if(r < 0){
      if(r == paInputOverflowed){
	sdr->overflows++; // Not fatal
	ConsecPaErrs = 0;
      } else if(++ConsecPaErrs < 10){
	fprintf(stdout,"Pa_ReadStream: %s\n",Pa_GetErrorText(r));
      } else {
	fprintf(stdout,"Pa_ReadStream: %s, exiting\n",Pa_GetErrorText(r));
	goto terminate;
      }
    } else
      ConsecPaErrs = 0;

    float i_energy=0, q_energy=0;
    complex float samp_sum = 0;
    float dotprod = 0;

    complex float * wptr = frontend->in.input_write_pointer.c;

    for(int i=0; i<Blocksize; i++){
      if(sampbuf[2*i] == 32767 || sampbuf[2*i] <= -32767){
	frontend->overranges++;
	frontend->samp_since_over = 0;
      } else
	frontend->samp_since_over++;

      if(sampbuf[2*i+1] == 32767 || sampbuf[2*i+1] <= -32767){
	frontend->overranges++;
	frontend->samp_since_over = 0;
      } else
	frontend->samp_since_over++;

      complex float samp = CMPLXF(sampbuf[2*i],sampbuf[2*i+1]);
      samp_sum += samp; // Accumulate average DC values
      samp -= sdr->DC;   // remove smoothed DC offset (which can be fractional)

      // Must correct gain and phase before frequency shift
      // accumulate I and Q energies before gain correction
      i_energy += crealf(samp) * crealf(samp);
      q_energy += cimagf(samp) * cimagf(samp);

      // Balance gains, keeping constant total energy
      __real__ samp *= gain_i;
      __imag__ samp *= gain_q;

      // Accumulate phase error
      dotprod += crealf(samp) * cimagf(samp);

      // Correct phase
      __imag__ samp = secphi * cimagf(samp) - tanphi * crealf(samp);

      wptr[i] = samp * sdr->scale;
    }
    write_cfilter(&frontend->in,NULL,Blocksize); // Update write pointer, invoke FFT
    frontend->samples += Blocksize;
    float const block_energy = i_energy + q_energy; // Normalize for complex pairs
    frontend->if_power_instant = block_energy / Blocksize;
    frontend->if_power += Power_smooth * (frontend->if_power_instant - frontend->if_power); // Average A/D output power per channel

#if 1
    // Get status timestamp from UNIX TOD clock -- but this might skew because of inexact sample rate
    frontend->timestamp = gps_time_ns();
#else
    // Simply increment by number of samples
    // But what if we lose some? Then the clock will always be off
    frontend->timestamp += 1.e9 * Blocksize / ADC_samprate;
#endif

    // Update every block
    // estimates of DC offset, signal powers and phase error
    sdr->DC += DC_alpha * (samp_sum - Blocksize*sdr->DC);
    if(block_energy > 0){ // Avoid divisions by 0, etc
      sdr->imbalance += rate_factor * ((i_energy / q_energy) - sdr->imbalance);
      float const dpn = 2 * dotprod / block_energy;
      sdr->sinphi += rate_factor * (dpn - sdr->sinphi);
      gain_q = sqrtf(0.5 * (1 + sdr->imbalance));
      gain_i = sqrtf(0.5 * (1 + 1./sdr->imbalance));
      secphi = 1/sqrtf(1 - sdr->sinphi * sdr->sinphi); // sec(phi) = 1/cos(phi)
      tanphi = sdr->sinphi * secphi;      // tan(phi) = sin(phi) * sec(phi) = sin(phi)/cos(phi)
    }
    if(sdr->agc)
      do_fcd_agc(sdr);
  }
 terminate:
  Pa_Terminate();
  exit(EX_NOINPUT); // Let systemd restart us
}
int funcube_startup(struct frontend *frontend){
  assert(frontend != NULL);
  struct sdrstate * const sdr = (struct sdrstate *)frontend->context;
  assert(sdr != NULL);

  // Start processing A/D data
  pthread_create(&sdr->proc_thread,NULL,proc_funcube,sdr);
  fprintf(stdout,"funcube running\n");
  return 0;
}



// Crude analog AGC just to keep signal roughly within A/D range
// Executed only if -o option isn't specified; this allows manual control with, e.g., the fcdpp command
static void do_fcd_agc(struct sdrstate *sdr){
  struct frontend * const frontend = sdr->frontend;
  assert(frontend != NULL);

  float const powerdB = power2dB(frontend->if_power * scale_ADpower2FS(frontend));

  if(powerdB > AGC_upper){
    if(frontend->if_gain > 0){
      // Decrease gain in 10 dB steps, down to 0
      uint8_t val = frontend->if_gain = max(0,frontend->if_gain - 10);
      fcdAppSetParam(sdr->phd,FCD_CMD_APP_SET_IF_GAIN1,&val,sizeof(val));
      if(Verbose)
	fprintf(stdout,"AGC power %.1f dBFS, new lower if gain = %d\n",powerdB,val);
    } else if(frontend->mixer_gain > 0){
      uint8_t val = frontend->mixer_gain = 0; // mixer gain is on or off?
      fcdAppSetParam(sdr->phd,FCD_CMD_APP_SET_MIXER_GAIN,&val,sizeof(val));
      if(Verbose)
	fprintf(stdout,"AGC power %.1f dBFS, new lower mixer gain = %d\n",powerdB,val);
    } else if(frontend->lna_gain > 0){
      uint8_t val = frontend->lna_gain = 0;
      fcdAppSetParam(sdr->phd,FCD_CMD_APP_SET_LNA_GAIN,&val,sizeof(val));
      if(Verbose)
	fprintf(stdout,"AGC power %.1f dBFS, new lower lna gain = %d\n",powerdB,val);
    }
  } else if(powerdB < AGC_lower){
    if(frontend->lna_gain == 0){
      frontend->lna_gain = 24;
      uint8_t val = 1;
      fcdAppSetParam(sdr->phd,FCD_CMD_APP_SET_LNA_GAIN,&val,sizeof(val));
      if(Verbose)
	fprintf(stdout,"AGC power %.1f dBFS, new higher if gain = %d\n",powerdB,val);
    } else if(frontend->mixer_gain == 0){
      frontend->mixer_gain = 19;
      uint8_t val = 1;
      if(Verbose)
	fprintf(stdout,"AGC power %.1f dBFS, new higher mixer gain = %d\n",powerdB,val);
      fcdAppSetParam(sdr->phd,FCD_CMD_APP_SET_MIXER_GAIN,&val,sizeof(val));
    } else if(frontend->if_gain < 20){ // Limit to 20 dB - seems enough to keep A/D going even on noise
      uint8_t val = frontend->if_gain = min(20,frontend->if_gain + 10);
      fcdAppSetParam(sdr->phd,FCD_CMD_APP_SET_IF_GAIN1,&val,sizeof(val));
      if(Verbose)
	fprintf(stdout,"AGC power %.1f dBFS, new higher lna gain = %d\n",powerdB,val);
    }
  }
  frontend->rf_gain = frontend->lna_gain + frontend->mixer_gain + frontend->if_gain;
  sdr->scale = scale_AD(frontend);
}

// The funcube device uses the Mirics MSi001 tuner. It has a fractional N synthesizer that can't actually do integer frequency steps.
// This formula is hacked down from code from Howard Long; it's what he uses in the firmware so I can figure out
// the *actual* frequency. Of course, we still have to correct it for the TCXO offset.

// This needs to be generalized since other tuners will be completely different!
static double fcd_actual(unsigned int u32Freq){
  typedef uint32_t UINT32;
  typedef uint64_t UINT64;

  UINT32 const u32Thresh = 3250U;
  UINT32 const u32FRef = 26000000U;

  struct {
    UINT32 u32Freq;
    UINT32 u32FreqOff;
    UINT32 u32LODiv;
  } *pts,ats[]= {
	{4000000U,130000000U,16U},
	{8000000U,130000000U,16U},
	{16000000U,130000000U,16U},
	{32000000U,130000000U,16U},
	{75000000U,130000000U,16U},
	{125000000U,0U,32U},
	{142000000U,0U,16U},
	{148000000U,0U,16U},
	{300000000U,0U,16U},
	{430000000U,0U,4U},
	{440000000U,0U,4U},
	{875000000U,0U,4U},
	{UINT32_MAX,0U,2U},
	{0U,0U,0U}
  };
  for(pts = ats; u32Freq >= pts->u32Freq; pts++)
    ;

  if (pts->u32Freq == 0)
    pts--;

  // Frequency of synthesizer before divider - can possibly exceed 32 bits, so it's stored in 64
  UINT64 const u64FSynth = ((UINT64)u32Freq + pts->u32FreqOff) * pts->u32LODiv;

  // Integer part of divisor ("INT")
  UINT32 const u32Int = u64FSynth / (u32FRef*4);

  // Subtract integer part to get fractional and AFC parts of divisor ("FRAC" and "AFC")
  UINT32 const u32Frac4096 =  (u64FSynth<<12) * u32Thresh/(u32FRef*4) - (u32Int<<12) * u32Thresh;

  // FRAC is higher 12 bits
  UINT32 const u32Frac = u32Frac4096>>12;

  // AFC is lower 12 bits
  UINT32 const u32AFC = u32Frac4096 - (u32Frac<<12);

  // Actual tuner frequency, in floating point, given specified parameters
  double const f64FAct = (4.0 * u32FRef / (double)pts->u32LODiv) * (u32Int + ((u32Frac * 4096.0 + u32AFC) / (u32Thresh * 4096.))) - pts->u32FreqOff;

  // double f64step = ( (4.0 * u32FRef) / (pts->u32LODiv * (double)u32Thresh) ) / 4096.0;
  //      printf("f64step = %'lf, u32LODiv = %'u, u32Frac = %'d, u32AFC = %'d, u32Int = %'d, u32Thresh = %'d, u32FreqOff = %'d, f64FAct = %'lf err = %'lf\n",
  //	     f64step, pts->u32LODiv, u32Frac, u32AFC, u32Int, u32Thresh, pts->u32FreqOff,f64FAct,f64FAct - u32Freq);
  return f64FAct;
}

double funcube_tune(struct frontend * const frontend,double const freq){
  struct sdrstate * const sdr = (struct sdrstate *)frontend->context;
  assert(sdr != NULL);

  if(frontend->lock)
    return freq; // Don't change if locked


  int const intfreq = freq;

  if(sdr->phd == NULL && (sdr->phd = fcdOpen(sdr->sdr_name,sizeof(sdr->sdr_name),sdr->number)) == NULL){
    fprintf(stdout,"fcdOpen(%d): can't re-open control port\n",sdr->number);
    return frontend->frequency; // nothing changes
  }
  fcdAppSetFreq(sdr->phd,intfreq);
  frontend->frequency = fcd_actual(intfreq) * (1 + frontend->calibrate);
  // LNA gain is frequency-dependent
  if(frontend->lna_gain){
    if(intfreq >= 420e6)
      frontend->lna_gain = 7;
    else
      frontend->lna_gain = 24;
    frontend->rf_gain = frontend->lna_gain + frontend->mixer_gain + frontend->if_gain;
  }
  return frontend->frequency;
}
