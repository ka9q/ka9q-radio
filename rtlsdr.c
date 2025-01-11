// Built-in driver for RTL-SDR in radiod
// Adapted from old rtlsdrd.c
// Copyright July 2023, Phil Karn, KA9Q
#define _GNU_SOURCE 1
#include <assert.h>
#include <pthread.h>
#include <rtl-sdr.h>
#include <errno.h>
#include <iniparser/iniparser.h>
#include <sysexits.h>
#include <strings.h>

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

static float Power_smooth = 0.05; // Calculate this properly someday

// Global variables set by command line options
extern char const *App_path;
extern int Verbose;

struct sdr {
  struct frontend *frontend;
  struct rtlsdr_dev *device;    // Opaque pointer

  int dev;
  char serial[256];

  bool bias; // Bias tee on/off

  // AGC
  bool agc;
  int holdoff_counter; // Time delay when we adjust gains
  int gain;      // Gain passed to manual gain setting
  float scale;         // Scale samples for #bits and front end gain

  // Sample statistics
  //  int clips;  // Sample clips since last reset
  //  float DC;      // DC offset for real samples

  pthread_t read_thread;
};

char const *Rtlsdr_keys[] = {
  "agc",
  "bias",
  "calibrate",
  "device",
  "description",
  "frequency",
  "gain",
  "hardware",
  "samprate",
  "serial",
  NULL
};


static double set_correct_freq(struct sdr *sdr,double freq);
//static void do_rtlsdr_agc(struct sdr *);
static void rx_callback(uint8_t *buf,uint32_t len, void *ctx);
static double true_freq(uint64_t freq);


int rtlsdr_setup(struct frontend *frontend,dictionary *dictionary,char const *section){
  assert(dictionary != NULL);
  config_validate_section(stdout,dictionary,"hardware",Rtlsdr_keys,NULL);

  struct sdr * const sdr = (struct sdr *)calloc(1,sizeof(struct sdr));
  // Cross-link generic and hardware-specific control structures
  sdr->frontend = frontend;
  frontend->context = sdr;

  {
    char const *device = config_getstring(dictionary,section,"device",NULL);
    if(strcasecmp(device,"rtlsdr") != 0)
      return -1; // Not for us
  }
  sdr->dev = -1;
  FREE(frontend->description);
  frontend->description = strdup(config_getstring(dictionary,section,"description","rtl-sdr"));
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
    for(unsigned int i=0; i < device_count; i++){
      rtlsdr_get_device_usb_strings(i,devices[i].manufacturer,devices[i].product,devices[i].serial);
      fprintf(stderr,"#%d (%s): %s %s %s\n",i,rtlsdr_get_device_name(i),
	      devices[i].manufacturer,devices[i].product,devices[i].serial);
    }
    char const * const p = config_getstring(dictionary,section,"serial",NULL);
    if(p == NULL){
      // Use first one, if any
      sdr->dev = 0;
    } else {
      for(unsigned int i=0; i < device_count; i++){
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
  rtlsdr_set_direct_sampling(sdr->device, 0); // That's for HF
  rtlsdr_set_offset_tuning(sdr->device,0); // Leave the DC spike for now
  rtlsdr_set_freq_correction(sdr->device,0); // don't use theirs, only good to integer ppm
  rtlsdr_set_tuner_bandwidth(sdr->device, 0); // Auto bandwidth
  rtlsdr_set_agc_mode(sdr->device,0);

  sdr->agc = config_getboolean(dictionary,section,"agc",false);

  if(sdr->agc){
    rtlsdr_set_tuner_gain_mode(sdr->device,0);  // auto gain mode (i.e., the firmware does it)
    sdr->gain = 0;
    frontend->rf_gain = 0; // needs conversion to dB
    sdr->holdoff_counter = HOLDOFF_TIME;
  } else {
    rtlsdr_set_tuner_gain_mode(sdr->device,1); // manual gain mode (i.e., we do it)
    sdr->gain = (int)(config_getfloat(dictionary,section,"gain",0) * 10);
    rtlsdr_set_tuner_gain(sdr->device,sdr->gain);
    frontend->rf_gain = sdr->gain / 10.0f;
  }
  sdr->scale = scale_AD(frontend);
  sdr->bias = config_getboolean(dictionary,section,"bias",false);
  {
    int ret = rtlsdr_set_bias_tee(sdr->device,sdr->bias);
    if(ret != 0){
      fprintf(stderr,"rtlsdr_set_bias_tee(%d) failed\n",sdr->bias);
    }
  }
  frontend->samprate = config_getint(dictionary,section,"samprate",DEFAULT_SAMPRATE);
  if(frontend->samprate <= 0){
    fprintf(stderr,"Invalid sample rate, reverting to default\n");
    frontend->samprate = DEFAULT_SAMPRATE;
  }
  {
    int ret = rtlsdr_set_sample_rate(sdr->device,(uint32_t)frontend->samprate);
    if(ret != 0){
      fprintf(stderr,"rtlsdr_set_sample_rate(%d) failed\n",frontend->samprate);
    }
  }

  double init_frequency = 0;
  {
    char const *p = config_getstring(dictionary,section,"frequency",NULL);
    if(p != NULL)
      init_frequency = parse_frequency(p,false);
  }

  if(init_frequency != 0){
    set_correct_freq(sdr,init_frequency);
    frontend->lock = true;
  }

  frontend->calibrate = config_getdouble(dictionary,section,"calibrate",0);
  fprintf(stdout,"%s, samprate %'d Hz, agc %d, gain %d, bias %d, init freq %'.3lf Hz, calibrate %.3g\n",
	  frontend->description,frontend->samprate,sdr->agc,sdr->gain,sdr->bias,init_frequency,
	  frontend->calibrate);


 // Just estimates - get the real number somewhere
  frontend->min_IF = -0.47 * frontend->samprate;
  frontend->max_IF = 0.47 * frontend->samprate;
  frontend->isreal = false; // Make sure the right kind of filter gets created!
  frontend->bitspersample = 8;
  return 0;
}


static void *rtlsdr_read_thread(void *arg){
  struct sdr *sdr = arg;
  struct frontend *frontend = sdr->frontend;

  rtlsdr_reset_buffer(sdr->device);
  rtlsdr_read_async(sdr->device,rx_callback,frontend,0,16*16384); // blocks

  exit(EX_NOINPUT); // return from read_async is an abort?
  return NULL;
}


int rtlsdr_startup(struct frontend * const frontend){
  struct sdr * const sdr = frontend->context;
  pthread_create(&sdr->read_thread,NULL,rtlsdr_read_thread,sdr);
  fprintf(stdout,"rtlsdr thread running\n");
  return 0;
}


// Callback called with incoming receiver data from A/D
static void rx_callback(uint8_t * const buf, uint32_t len, void * const ctx){
  int sampcount = len/2;
  float energy = 0;
  struct frontend *frontend = ctx;
  struct sdr *sdr = (struct sdr *)frontend->context;
  float complex * const wptr = frontend->in.input_write_pointer.c;

  for(int i=0; i < sampcount; i++){
    float complex samp;
    if(buf[2*i] == 0 || buf[2*i] == 255){
      frontend->overranges++;
      frontend->samp_since_over = 0;
    } else
      frontend->samp_since_over++;

    if(buf[2*i+1] == 0 || buf[2*i+1] == 255){
      frontend->overranges++;
      frontend->samp_since_over = 0;
    } else
      frontend->samp_since_over++;
    __real__ samp = (int)buf[2*i] - 128; // Excess-128
    __imag__ samp = (int)buf[2*i+1] - 128;
    energy += cnrmf(samp);
    wptr[i] = sdr->scale * samp;
  }
  frontend->timestamp = gps_time_ns();
  write_cfilter(&frontend->in,NULL,sampcount); // Update write pointer, invoke FFT
  frontend->if_power_instant = energy / sampcount;
  frontend->if_power += Power_smooth * (frontend->if_power_instant - frontend->if_power);
  frontend->samples += sampcount;
}
#if 0 // use this later
static void do_rtlsdr_agc(struct sdr * const sdr){
  assert(sdr != NULL);
  if(!sdr->agc)
    return; // Execute only in software AGC mode

  if(--sdr->holdoff_counter == 0){
    sdr->holdoff_counter = HOLDOFF_TIME;
    float powerdB = 10*log10f(frontend->output_level);
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
    frontend->rf_gain = sdr->gain; // Convert to dB?
    sdr->scale = scale_AD(frontend);
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

static double set_correct_freq(struct sdr * const sdr,double freq){
  struct frontend * const frontend = sdr->frontend;
  int64_t intfreq = round(freq / (1 + frontend->calibrate));
  rtlsdr_set_center_freq(sdr->device,intfreq);
#ifdef USE_NEW_LIBRTLSDR
  double tf = rtlsdr_get_freq(sdr->device);
#else
  double tf = true_freq(rtlsdr_get_center_freq(sdr->device)); // We correct the original imprecise version
#endif

  frontend->frequency = tf * (1 + frontend->calibrate);
  return frontend->frequency;
}

double rtlsdr_tune(struct frontend * const frontend,double freq){
  struct sdr * const sdr = (struct sdr *)frontend->context;
  assert(sdr != NULL);
  if(frontend->lock)
    return frontend->frequency; // Don't change frequency

  return set_correct_freq(sdr,freq);
}
