// Built-in driver for RTL-SDR in radiod
// Adapted from old rtlsdrd.c
// Phil Karn, KA9Q, July 2023
#define _GNU_SOURCE 1
#include <assert.h>
#include <pthread.h>
#include <string.h>
#include <complex.h>
#include <math.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <signal.h>
#include <locale.h>
#include <sys/time.h>
#include <rtl-sdr.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <errno.h>
#include <syslog.h>
#include <sys/stat.h>
#include <getopt.h>
#include <iniparser/iniparser.h>

#include "conf.h"
#include "misc.h"
#include "multicast.h"
#include "status.h"
#include "radio.h"
#include "config.h"

// Define USE_NEW_LIBRTLSDR to use my version of librtlsdr with rtlsdr_get_freq()
// that corrects for synthesizer fractional-N residuals. If not defined, we do the correction
// here assuming an R820 tuner (the most common)
#undef USE_NEW_LIBRTLSDR

//#define REMOVE_DC 1

// Internal clock is 28.8 MHz, and 1.8 MHz * 16 = 28.8 MHz
#define DEFAULT_SAMPRATE (1800000)

// Time in 100 ms update intervals to wait between gain steps
static int const HOLDOFF_TIME = 2;

#if 0
// Configurable parameters
// decibel limits for power
static float const DC_alpha = 1.0e-6;  // high pass filter coefficient for DC offset estimates, per sample
static float const AGC_upper = -20;
static float const AGC_lower = -40;
#endif

// Global variables set by command line options
extern char const *App_path;
extern int Verbose;


struct sdrstate {
  struct frontend *frontend;
  struct rtlsdr_dev *device;    // Opaque pointer

  int dev;
  uint32_t sample_rates[20];
  uint64_t SN; // Serial number

  bool bias; // Bias tee on/off
  bool agc;

  // Tuning
  bool frequency_lock;
  char const *frequency_file; // Local file to store frequency in case we restart
  double init_frequency;

  // AGC
  int holdoff_counter; // Time delay when we adjust gains
  int gain;      // Gain passed to manual gain setting

  // Sample statistics
  int clips;  // Sample clips since last reset
  float power;   // Running estimate of A/D signal power
  float DC;      // DC offset for real samples

  pthread_t read_thread;
};

static double set_correct_freq(struct sdrstate *sdr,double freq);
//static void do_rtlsdr_agc(struct sdrstate *);
static void rx_callback(uint8_t *buf,uint32_t len, void *ctx);
static double true_freq(uint64_t freq);


int rtlsdr_setup(struct frontend *frontend,dictionary *dictionary,char const *section){
  assert(dictionary != NULL);

  struct sdrstate * const sdr = (struct sdrstate *)calloc(1,sizeof(struct sdrstate));
  // Cross-link generic and hardware-specific control structures
  sdr->frontend = frontend;
  frontend->sdr.context = sdr;

  {
    char const *device = config_getstring(dictionary,section,"device",NULL);
    if(strcasecmp(device,"rtlsdr") != 0)
      return -1; // Not for us
  }
  // Enumerate devices, take first successful open - seems to require latest version of libusb
  int device_count = rtlsdr_get_device_count();

  for(int i=0; i < device_count; i++){
    char manufacturer[256],product[256],serial[256];
    rtlsdr_get_device_usb_strings(i,manufacturer,product,serial);
    
    fprintf(stderr,"RTL-SDR %d (%s): %s %s %s\n",i,rtlsdr_get_device_name(i),
	    manufacturer,product,serial);
  }      
  sdr->dev = config_getint(dictionary,section,"number",0);

  // Open
  int ret = rtlsdr_open(&sdr->device,sdr->dev);
  if(ret < 0){
    fprintf(stderr,"rtlsdr_open(%d) failed: %d\n",sdr->dev,ret);
    exit(1);
  }
  sdr->device = sdr->device;
  uint32_t rtl_freq,tuner_freq;
  ret = rtlsdr_get_xtal_freq(sdr->device,&rtl_freq,&tuner_freq);
  fprintf(stderr,"RTL freq = %u, tuner freq = %u\n",(unsigned)rtl_freq,(unsigned)tuner_freq);
  fprintf(stderr,"RTL tuner type %d\n",rtlsdr_get_tuner_type(sdr->device));
  int ngains = rtlsdr_get_tuner_gains(sdr->device,NULL);
  int gains[ngains];
  rtlsdr_get_tuner_gains(sdr->device,gains);
  fprintf(stderr,"Tuner gains:");
  for(int i=0; i < ngains; i++)
    fprintf(stderr," %d",gains[i]);
  fprintf(stderr,"\n");
  rtlsdr_set_freq_correction(sdr->device,0); // don't use theirs, only good to integer ppm
  rtlsdr_set_tuner_bandwidth(sdr->device, 0); // Auto bandwidth
  rtlsdr_set_agc_mode(sdr->device,0);

  sdr->agc = config_getboolean(dictionary,section,"agc",false);

  if(sdr->agc){
    rtlsdr_set_tuner_gain_mode(sdr->device,1); // manual gain mode (i.e., we do it)
    rtlsdr_set_tuner_gain(sdr->device,0);
    sdr->gain = 0;
    sdr->holdoff_counter = HOLDOFF_TIME;
  } else
    rtlsdr_set_tuner_gain_mode(sdr->device,0); // auto gain mode (i.e., the firmware does it)
  
  sdr->SN = 0; // where should we get this??

  
  sdr->bias = config_getboolean(dictionary,section,"bias",false);
  ret = rtlsdr_set_bias_tee(sdr->device,sdr->bias);

  rtlsdr_set_direct_sampling(sdr->device, 0); // That's for HF
  rtlsdr_set_offset_tuning(sdr->device,0); // Leave the DC spike for now
  sdr->frontend->sdr.samprate = config_getint(dictionary,section,"samprate",2000000);
  if(sdr->frontend->sdr.samprate == 0){
    fprintf(stderr,"Select sample rate\n");
    exit(1);
  }
  ret = rtlsdr_set_sample_rate(sdr->device,(uint32_t)sdr->frontend->sdr.samprate);

  strlcpy(frontend->sdr.description,config_getstring(dictionary,section,"description","rtl-sdr"),
	  sizeof(frontend->sdr.description));
  
  char *tmp = NULL;
  ret = asprintf(&tmp,"%s/tune-rtlsdr.%llx",VARDIR,(unsigned long long)sdr->SN);
  if(ret == -1)
    exit(1);

  sdr->frequency_file = tmp;

  sdr->init_frequency = config_getdouble(dictionary,section,"frequency",0);
  if(sdr->init_frequency == 0){
    // If not set on command line, load saved frequency
    FILE *fp = fopen(sdr->frequency_file,"r+");
    if(fp == NULL || fscanf(fp,"%lf",&sdr->init_frequency) < 0)
      fprintf(stderr,"Can't read stored freq from %s: %s\n",sdr->frequency_file,strerror(errno));
    else
      fprintf(stderr,"Using stored frequency %lf from tuner state file %s\n",sdr->init_frequency,sdr->frequency_file);
    if(fp != NULL)
      fclose(fp);
  }
  if(sdr->init_frequency == 0){
    // Not set on command line, and not read from file. Use fallback to cover 2m
    sdr->init_frequency = 149e6; // Fallback default
    fprintf(stderr,"Fallback default frequency %'.3lf Hz\n",sdr->init_frequency);
  }
  fprintf(stdout,"%s: Samprate %'d Hz, agc %d, gain %d, bias %d, init freq %'.3lf Hz\n",
	  frontend->sdr.description,frontend->sdr.samprate,sdr->agc,sdr->gain,sdr->bias,sdr->init_frequency);

  set_correct_freq(sdr,sdr->init_frequency);
  frontend->sdr.min_IF = 0;
  frontend->sdr.max_IF = 0.47 * frontend->sdr.samprate; // Just an estimate - get the real number somewhere
  frontend->sdr.isreal = false; // Make sure the right kind of filter gets created!
  frontend->sdr.calibrate = config_getdouble(dictionary,section,"calibrate",0);

  return 0;
}


static void *rtlsdr_read_thread(void *arg){
  struct sdrstate *sdr = arg;
  struct frontend *frontend = sdr->frontend;

  rtlsdr_reset_buffer(sdr->device);
  rtlsdr_read_async(sdr->device,rx_callback,frontend,0,16*16384); // blocks

  exit(1); // return from read_async is an abort?
  return NULL;
}


int rtlsdr_startup(struct frontend *frontend){
  struct sdrstate *sdr = frontend->sdr.context;
  pthread_create(&sdr->read_thread,NULL,rtlsdr_read_thread,sdr);
  fprintf(stdout,"rtlsdr running\n");
  return 0;
}


// Callback called with incoming receiver data from A/D
static void rx_callback(uint8_t *buf, uint32_t len, void *ctx){
  int sampcount = len;
  int16_t * const samples = (int16_t *)buf;
  float energy = 0;
  struct frontend *frontend = ctx;
  float complex * const wptr = frontend->in->input_write_pointer.c;
  
  for(int i=0; i < sampcount; i++){
    float complex samp;
    __real__ samp = (float)samples[2*i] * samples[2*i];
    __imag__ samp = (float)samples[2*i+1] * samples[2*i+1];
    samp *= SCALE16;
    energy += samp * samp;
    wptr[i] = samp;
  }
  write_rfilter(frontend->in,NULL,sampcount); // Update write pointer, invoke FFT
  frontend->sdr.output_level = energy / sampcount;
  frontend->input.samples += sampcount;
}
#if 0 // use this later
static void do_rtlsdr_agc(struct sdrstate *sdr){
  assert(sdr != NULL);
  if(!sdr->agc)
    return; // Execute only in software AGC mode
    
  if(--sdr->holdoff_counter == 0){
    sdr->holdoff_counter = HOLDOFF_TIME;
    float powerdB = 10*log10f(frontend->sdr.output_level);
    if(powerdB > AGC_upper && sdr->gain > 0){
      sdr->gain -= 20;    // Reduce gain one step
    } else if(powerdB < AGC_lower){
      sdr->gain += 20;    // Increase one step
    } else
      return;
    // librtlsdr inverts its gain tables for some reason
    if(Verbose)
      printf("new tuner gain %.0f dB\n",(float)sdr->gain/10.);
    int r = rtlsdr_set_tuner_gain(sdr->device,sdr->gain);
    if(r != 0)
      printf("rtlsdr_set_tuner_gain returns %d\n",r);
  }
}
#endif

#if ORIGINAL_TRUE_FREQ
static double true_freq(uint64_t freq){
  // Code extracted from tuner_r82xx.c
  int rc, i;
  unsigned sleep_time = 10000;
  uint64_t vco_freq;
  uint32_t vco_fra;	/* VCO contribution by SDM (kHz) */
  uint32_t vco_min = 1770000;
  uint32_t vco_max = vco_min * 2;
  uint32_t freq_khz, pll_ref, pll_ref_khz;
  uint16_t n_sdm = 2;
  uint16_t sdm = 0;
  uint8_t mix_div = 2;
  uint8_t div_buf = 0;
  uint8_t div_num = 0;
  uint8_t vco_power_ref = 2;
  uint8_t refdiv2 = 0;
  uint8_t ni, si, nint, vco_fine_tune, val;
  uint8_t data[5];
  
  /* Frequency in kHz */
  freq_khz = (freq + 500) / 1000;
  //  pll_ref = priv->cfg->xtal;
  pll_ref = 28800000;
  pll_ref_khz = (pll_ref + 500) / 1000;
  
  /* Calculate divider */
  while (mix_div <= 64) {
    if (((freq_khz * mix_div) >= vco_min) &&
	((freq_khz * mix_div) < vco_max)) {
      div_buf = mix_div;
      while (div_buf > 2) {
	div_buf = div_buf >> 1;
	div_num++;
      }
      break;
    }
    mix_div = mix_div << 1;
  }
  
  vco_freq = (uint64_t)freq * (uint64_t)mix_div;
  nint = vco_freq / (2 * pll_ref);
  vco_fra = (vco_freq - 2 * pll_ref * nint) / 1000;

  ni = (nint - 13) / 4;
  si = nint - 4 * ni - 13;
  
  /* sdm calculator */
  while (vco_fra > 1) {
    if (vco_fra > (2 * pll_ref_khz / n_sdm)) {
      sdm = sdm + 32768 / (n_sdm / 2);
      vco_fra = vco_fra - 2 * pll_ref_khz / n_sdm;
      if (n_sdm >= 0x8000)
	break;
    }
    n_sdm <<= 1;
  }
  
  double f;
  {
    int ntot = (nint << 16) + sdm;

    double vco = pll_ref * 2 * (nint + sdm / 65536.);
    f = vco / mix_div;
    return f;
  }
  
}
#else // Cleaned up version
// For a requested frequency, give the actual tuning frequency
// similar to the code in airspy.c since both use the R820T tuner
static double true_freq(uint64_t freq_hz){
  const uint32_t VCO_MIN=1770000000u; // 1.77 GHz
  const uint32_t VCO_MAX=(VCO_MIN << 1); // 3.54 GHz
  const int MAX_DIV = 5;

  // Clock divider set to 2 for the best resolution
  const uint32_t pll_ref = 28800000u / 2; // 14.4 MHz
  
  // Find divider to put VCO = f*2^(d+1) in range VCO_MIN to VCO_MAX (for ref freq 26 MHz)
  //          MHz             step, Hz
  // 0: 885.0     1770.0      190.735
  // 1: 442.50     885.00      95.367
  // 2: 221.25     442.50      47.684
  // 3: 110.625    221.25      23.842
  // 4:  55.3125   110.625     11.921
  // 5:  27.65625   55.312      5.960
  int8_t div_num;
  for (div_num = 0; div_num <= MAX_DIV; div_num++){
    uint32_t vco = freq_hz << (div_num + 1);
    if (VCO_MIN <= vco && vco <= VCO_MAX)
      break;
  }
  if(div_num > MAX_DIV)
    return 0; // Frequency out of range
  
  // PLL programming bits: Nint in upper 16 bits, Nfract in lower 16 bits
  // Freq steps are pll_ref / 2^(16 + div_num) Hz
  // Note the '+ (pll_ref >> 1)' term simply rounds the division to the nearest integer
  uint32_t r = (((uint64_t) freq_hz << (div_num + 16)) + (pll_ref >> 1)) / pll_ref;
  // Compute true frequency; the 1/4 step bias is a puzzle
  return ((double)(r + 0.25) * pll_ref) / (double)(1 << (div_num + 16));
}
#endif

// set the rtlsdr tuner to the requested frequency applying calibration offset,
// true frequency correction model for 820T synthesizer
// the calibration offset is a holdover from the Funcube dongle and doesn't
// really fit the Rtlsdr with its internal factory calibration
// All this really works correctly only with a gpsdo
// Remember, rtlsdr firmware always adds Fs/4 MHz to frequency we give it.

static double set_correct_freq(struct sdrstate *sdr,double freq){
  struct frontend *frontend = sdr->frontend;
  int64_t intfreq = round(freq / (1 + frontend->sdr.calibrate));
  rtlsdr_set_center_freq(sdr->device,intfreq);
#ifdef USE_NEW_LIBRTLSDR
  double tf = rtlsdr_get_freq(sdr->device);
#else
  double tf = true_freq(rtlsdr_get_center_freq(sdr->device)); // We correct the original imprecise version
#endif

  frontend->sdr.frequency = tf * (1 + frontend->sdr.calibrate);
  FILE *fp = fopen(sdr->frequency_file,"w");
  if(fp == NULL || fprintf(fp,"%lf\n",frontend->sdr.frequency) < 0)
    fprintf(stderr,"Can't write to tuner state file %s: %sn",sdr->frequency_file,strerror(errno));
  if(fp != NULL)
    fclose(fp);
  return frontend->sdr.frequency;
}

double rtlsdr_tune(struct frontend *frontend,double freq){
  struct sdrstate *sdr = (struct sdrstate *)frontend->sdr.context;
  assert(sdr != NULL);

  return set_correct_freq(sdr,freq);
}  



