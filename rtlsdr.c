// Built-in driver for RTL-SDR in radiod
// Adapted from old rtlsdrd.c
// Phil Karn, KA9Q, July 2023
#define _GNU_SOURCE 1
#include <assert.h>
#include <pthread.h>
#include <rtl-sdr.h>
#include <errno.h>
#include <iniparser/iniparser.h>

#include "conf.h"
#include "misc.h"
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

#if 0 // Reimplement this someday
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
  char serial[256];

  bool bias; // Bias tee on/off

  // Tuning
  bool frequency_lock;
  char const *frequency_file; // Local file to store frequency in case we restart

  // AGC
  bool agc;
  int holdoff_counter; // Time delay when we adjust gains
  int gain;      // Gain passed to manual gain setting

  // Sample statistics
  //  int clips;  // Sample clips since last reset
  //  float DC;      // DC offset for real samples

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
  sdr->dev = -1;
  strlcpy(frontend->sdr.description,config_getstring(dictionary,section,"description","rtl-sdr"),
	  sizeof(frontend->sdr.description));
  {
    unsigned const device_count = rtlsdr_get_device_count();
    if(device_count < 1){
      fprintf(stderr,"No RTL-SDR devices\n");
      return -1;
    }
    struct {
      char manufacturer[256];
      char product[256];
      char serial[256];
    } devices[device_count];

    // List all devices
    fprintf(stderr,"Found %d RTL-SDR device%s:\n",device_count,device_count > 1 ? "s":"");
    for(int i=0; i < device_count; i++){
      rtlsdr_get_device_usb_strings(i,devices[i].manufacturer,devices[i].product,devices[i].serial);
      fprintf(stderr,"#%d (%s): %s %s %s\n",i,rtlsdr_get_device_name(i),
	      devices[i].manufacturer,devices[i].product,devices[i].serial);
    }
    char const * const p = config_getstring(dictionary,section,"serial",NULL);
    if(p == NULL){
      // Use first one, if any
      sdr->dev = 0;
    } else {
      for(int i=0; i < device_count; i++){
	if(strcasecmp(p,devices[i].serial) == 0){
	  sdr->dev = i;
	  break;
	}
      }
    }
    if(sdr->dev < 0){
      fprintf(stderr,"RTL-SDR serial %s not found\n",p);
      return -1;
    }
    strlcpy(sdr->serial,devices[sdr->dev].serial,sizeof(sdr->serial));
    fprintf(stderr,"Using RTL-SDR #%d, serial %s\n",sdr->dev,sdr->serial);
  }
  {
    int const ret = rtlsdr_open(&sdr->device,sdr->dev);
    if(ret != 0){
      fprintf(stderr,"rtlsdr_open(%d) failed: %d\n",sdr->dev,ret);
      return -1;
    }
  }
  {
    int ngains = rtlsdr_get_tuner_gains(sdr->device,NULL);
    int gains[ngains];
    rtlsdr_get_tuner_gains(sdr->device,gains);

    uint32_t rtl_freq = 0,tuner_freq = 0;
    int const ret = rtlsdr_get_xtal_freq(sdr->device,&rtl_freq,&tuner_freq);
    if(ret != 0)
      fprintf(stderr,"rtlsdr_get_xtal_freq failed\n");
    fprintf(stderr,"RTL freq %'u, tuner freq %'u, tuner type %'d, tuner gains",(unsigned)rtl_freq,(unsigned)tuner_freq,
	    rtlsdr_get_tuner_type(sdr->device));
    for(int i=0; i < ngains; i++)
      fprintf(stderr," %'d",gains[i]);
    fprintf(stderr,"\n");
    
  }
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
  
  sdr->bias = config_getboolean(dictionary,section,"bias",false);
  {
    int ret = rtlsdr_set_bias_tee(sdr->device,sdr->bias);
    if(ret != 0){
      fprintf(stderr,"rtlsdr_set_bias_tee(%d) failed\n",sdr->bias);
    }
  }
  rtlsdr_set_direct_sampling(sdr->device, 0); // That's for HF
  rtlsdr_set_offset_tuning(sdr->device,0); // Leave the DC spike for now
  frontend->sdr.samprate = config_getint(dictionary,section,"samprate",DEFAULT_SAMPRATE);
  if(frontend->sdr.samprate <= 0){
    fprintf(stderr,"Invalid sample rate, reverting to default\n");
    frontend->sdr.samprate = DEFAULT_SAMPRATE;
  }
  {
    int ret = rtlsdr_set_sample_rate(sdr->device,(uint32_t)frontend->sdr.samprate);
    if(ret != 0){
      fprintf(stderr,"rtlsdr_set_sample_rate(%d) failed\n",frontend->sdr.samprate);
    }
  }
  {
    char *tmp = NULL;
    sdr->frequency_file = NULL;
    int ret = asprintf(&tmp,"%s/tune-rtlsdr.%s",VARDIR,sdr->serial);
    if(ret != -1)
      sdr->frequency_file = tmp;
  }
  double init_frequency = config_getdouble(dictionary,section,"frequency",0);
  if(init_frequency == 0){
    // If not set on command line, load saved frequency
    FILE *fp = fopen(sdr->frequency_file,"r+");
    if(fp == NULL || fscanf(fp,"%lf",&init_frequency) < 0)
      fprintf(stderr,"Can't read stored freq from %s: %s\n",sdr->frequency_file,strerror(errno));
    else
      fprintf(stderr,"Using stored frequency %'.3lf Hz from tuner state file %s\n",init_frequency,sdr->frequency_file);
    if(fp != NULL)
      fclose(fp);
  }
  if(init_frequency == 0){
    // Not set on command line, and not read from file. Use fallback to cover 2m
    init_frequency = 149e6;
    fprintf(stderr,"Fallback initial frequency %'.3lf Hz\n",init_frequency);
  }
  frontend->sdr.calibrate = config_getdouble(dictionary,section,"calibrate",0);
  fprintf(stdout,"%s, samprate %'d Hz, agc %d, gain %d, bias %d, init freq %'.3lf Hz, calibrate %.3g\n",
	  frontend->sdr.description,frontend->sdr.samprate,sdr->agc,sdr->gain,sdr->bias,init_frequency,
	  frontend->sdr.calibrate);

  set_correct_freq(sdr,init_frequency);
 // Just estimates - get the real number somewhere
  frontend->sdr.min_IF = -0.47 * frontend->sdr.samprate;
  frontend->sdr.max_IF = 0.47 * frontend->sdr.samprate;
  frontend->sdr.isreal = false; // Make sure the right kind of filter gets created!
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
  fprintf(stdout,"rtlsdr thread running\n");
  return 0;
}


// Callback called with incoming receiver data from A/D
static void rx_callback(uint8_t *buf, uint32_t len, void *ctx){
  int sampcount = len/2;
  float energy = 0;
  struct frontend *frontend = ctx;
  float complex * const wptr = frontend->in->input_write_pointer.c;
  
  for(int i=0; i < sampcount; i++){
    float complex samp;
    __real__ samp = (int)buf[2*i] - 128; // Excess-128
    __imag__ samp = (int)buf[2*i+1] - 128;
    samp *= SCALE8;
    energy += samp * samp;
    wptr[i] = samp;
  }
  write_cfilter(frontend->in,NULL,sampcount); // Update write pointer, invoke FFT
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



