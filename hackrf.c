// ka9q-radio driver for Great Scott Gadgets Hack RF
#define _GNU_SOURCE 1 // allow bind/connect/recvfrom without casting sockaddr_in6
#include <assert.h>
#include <pthread.h>
#include <string.h>
#include <complex.h>
#include <math.h>
#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/types.h>
#include <signal.h>
#include <locale.h>
#include <sys/time.h>
#include <libhackrf/hackrf.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <errno.h>
#include <syslog.h>
#include <sys/stat.h>

#include "radio.h"
#include "misc.h"
#include "config.h"

struct sdrstate {
  struct frontend *frontend;  // Avoid references to external globals
  hackrf_device *device;
  int clips;                // Sample clips since last reset

  // Smoothed error estimates
  complex float DC;      // DC offset
  float sinphi;          // I/Q phase error
  float imbalance;       // Ratio of I power to Q power
  // Gain and phase corrections. These will be updated every block
  float gain_q;
  float gain_i;
  float secphi;
  float tanphi;
  
  double frequency;
  bool software_agc;
  int lna_gain;
  int mixer_gain;
  int if_gain;
  pthread_t agc_thread;
  float scale;
};


// Configurable parameters
// decibel limits for power
static float const Upper_limit = -15;
static float const Lower_limit = -25;
static int Default_samprate = 5000000;
static float const DC_alpha = 1.0e-7;  // high pass filter coefficient for DC offset estimates, per sample
static float const Power_alpha= 1.0; // time constant (seconds) for smoothing power and I/Q imbalance estimates

static char const *HackRF_keys[] = {
  "library",
  "device",
  "serial",
  "lna-gain",
  "mixer-gain",
  "vga-gain",
  "reference",
  "calibrate",
  "samprate",
  "description",
  "frequency",
  NULL
};
static int rx_callback(hackrf_transfer *transfer);

static void *hackrf_agc(void *arg);
#if 0
static double rffc5071_freq(uint16_t lo);
static uint32_t max2837_freq(uint32_t freq);
#endif


int hackrf_setup(struct frontend * const frontend,dictionary const * const dictionary,char const * const section){
  assert(dictionary != NULL);
  // Hardware-dependent setup
  {
    char const *device = config_getstring(dictionary,section,"device",section);
    if(strcasecmp(device,"hackrf") != 0)
      return -1; // Not for us
  }
  config_validate_section(stdout,dictionary,section,HackRF_keys,NULL);
  struct sdrstate * const sdr = calloc(1,sizeof(struct sdrstate));
  assert(sdr != NULL);
  // Cross-link generic and hardware-specific control structures
  sdr->frontend = frontend;
  frontend->context = sdr;
  frontend->isreal = false; // Make sure the right kind of filter gets created!
  frontend->bitspersample = 8; // For gain scaling
  frontend->rf_agc = true; // On by default unless gain or atten is specified

  int ret;
  if((ret = hackrf_init()) != HACKRF_SUCCESS){
    fprintf(stdout,"hackrf_init() failed: %s\n",hackrf_error_name(ret));
    hackrf_exit(); // Necessary?
    return -1;
  }
  // Enumerate devices
  hackrf_device_list_t *dlist = hackrf_device_list();

  fprintf(stdout,"Found %d HackRF device(s): ",dlist->devicecount);
  for(int i=0; i < dlist->devicecount; i++){
    fprintf(stdout,"%d %s\n",i,dlist->serial_numbers[i]);
  }

  int index = config_getint(dictionary, section, "index", 0);
#if 0 // add ability to select by serial #
  char const *p = config_getstring(dictionary,section,"serial",NULL); // is serial specified?
  if(p != NULL){
    sdr->serial = strtoll(p,NULL,16);
  }
#endif
  if((ret = hackrf_device_list_open(dlist,index,&sdr->device)) != HACKRF_SUCCESS){
    fprintf(stdout,"hackrf_open(%d) failed: %s\n",index,hackrf_error_name(ret));
    hackrf_exit();
    return -1;
  }
  hackrf_device_list_free(dlist); dlist = NULL;
  if(sdr->device == NULL){
    fprintf(stdout,"hackrf_open(%d) returned NULL\n",index);
    hackrf_exit();
    return -1;
  }
  double samprate = Default_samprate;
  {
    char const *p = config_getstring(dictionary, section, "samprate", NULL);
    if(p != NULL)
      samprate = parse_frequency(p,false);
  }
  frontend->samprate = samprate;
  ret = hackrf_set_sample_rate(sdr->device,(uint32_t)samprate);
  if(ret != HACKRF_SUCCESS){
    fprintf(stdout,"hackrf_set_sample_rate(%lf): %s\n",samprate,hackrf_error_name(ret));
    hackrf_exit();
    return -1;
  }

  uint32_t bw = hackrf_compute_baseband_filter_bw_round_down_lt(samprate);
  ret = hackrf_set_baseband_filter_bandwidth(sdr->device,bw);
  if(ret != HACKRF_SUCCESS){
    fprintf(stdout,"hackrf_set_baseband_filter_bandwidth(%ud): %s\n",bw,hackrf_error_name(ret));
    hackrf_exit();
    return -1;
  }
  // Are these right?
  frontend->max_IF = min((int)bw,frontend->samprate/2);
  frontend->min_IF = -min((int)bw,frontend->samprate/2);


  // NOTE: what we call mixer gain, they call lna gain
  // What we call lna gain, they call antenna enable
  sdr->software_agc = true;
  sdr->lna_gain = config_getint(dictionary, section, "lna-gain", -1);
  if(sdr->lna_gain != -1)
    sdr->software_agc = false;
  else
    sdr->lna_gain = 14;
  
  frontend->lna_gain = sdr->lna_gain;
  ret = hackrf_set_antenna_enable(sdr->device,sdr->lna_gain ? true : false);
  if(ret != HACKRF_SUCCESS){
    fprintf(stdout,"hackrf_set_antenna_enable(%d): %s\n",sdr->lna_gain,hackrf_error_name(ret));
    hackrf_exit();
    return -1;
  }

  sdr->mixer_gain = config_getint(dictionary, section, "mixer-gain", -1);
  if(sdr->mixer_gain != -1)
    sdr->software_agc = false;
  else
    sdr->mixer_gain = 24;
      
  frontend->mixer_gain = sdr->mixer_gain;
  fprintf(stdout,"set mixer gain %d\n",frontend->mixer_gain);
  ret = hackrf_set_lna_gain(sdr->device,sdr->mixer_gain);
  if(ret != HACKRF_SUCCESS){
    fprintf(stdout,"hackrf_set_lna_gain(%d): %s\n",sdr->mixer_gain,hackrf_error_name(ret));
    hackrf_exit();
    return -1;
  }
  sdr->if_gain = config_getint(dictionary, section, "if-gain", -1);
  if(sdr->if_gain != -1)
    sdr->software_agc = false;
  else
    sdr->if_gain = 20;
  frontend->if_gain = sdr->if_gain;
  fprintf(stdout,"set if gain %d\n",frontend->if_gain);
  ret = hackrf_set_vga_gain(sdr->device,sdr->if_gain);
  if(ret != HACKRF_SUCCESS){
    fprintf(stdout,"hackrf_set_vga_gain(%d): %s\n",sdr->if_gain,hackrf_error_name(ret));
    hackrf_exit();
    return -1;
  }


  frontend->rf_gain = sdr->lna_gain + sdr->mixer_gain + sdr->if_gain;
  frontend->rf_atten = 0;
  frontend->rf_level_cal = 0; // To be measured
  sdr->scale = scale_AD(frontend);

  double frequency = config_getdouble(dictionary, section, "frequency", 0);
  if(frequency != 0){
    frontend->lock = true;
    frontend->frequency = frequency;
    uint64_t intfreq = frequency;
    ret = hackrf_set_freq(sdr->device,intfreq);
    if(ret != HACKRF_SUCCESS){
      fprintf(stdout,"hackrf_set_freq(%llu): %s\n",(long long unsigned)intfreq,hackrf_error_name(ret));
      hackrf_exit();
      return -1;
    }
  }
  sdr->sinphi = 0;
  sdr->tanphi = 0;
  sdr->secphi = 1;
  sdr->gain_i = 1;
  sdr->gain_q = 1;

  fprintf(stdout,"device %d; A/D sample rate %'lf Hz freq %'.1f Hz lna gain %d mix gain %d if gain %d agc %s\n",
	  index,samprate,frequency,
	  frontend->lna_gain,
	  frontend->mixer_gain,
	  frontend->if_gain,
	  sdr->software_agc ? "on" : "off");
  return 0;
}

int hackrf_startup(struct frontend * const frontend){
  assert(frontend != NULL);
  struct sdrstate *sdr = frontend->context;
  //  pthread_create(&Process_thread,NULL,hackrf_proc,sdr);

  sdr->scale = scale_AD(frontend);
  int ret = hackrf_start_rx(sdr->device,rx_callback,sdr);
  assert(ret == HACKRF_SUCCESS);
  (void)ret;
  
  if(sdr->software_agc)
    pthread_create(&sdr->agc_thread,NULL,hackrf_agc,sdr);
  return 0;
}


static bool Name_set = false;

// Callback called with incoming receiver data from A/D
static int rx_callback(hackrf_transfer *transfer){
  struct sdrstate *sdr = transfer->rx_ctx;
  assert(sdr != NULL);
  struct frontend *frontend = sdr->frontend;

  if(!Name_set){
    pthread_setname("hackrf-cb");
    Name_set = true;
    realtime();
  }
  int remain = transfer->valid_length; // Count of individual samples; divide by 2 to get complex samples
  int sampcount = remain / 2;            // Complex samples
  uint8_t *dp = transfer->buffer;

  complex float samp_sum = 0;
  float i_energy=0,q_energy=0;
  float dotprod = 0;                           // sum of I*Q, for phase balance
  float rate_factor = 1./(frontend->samprate * Power_alpha);

  complex float * const wptr = frontend->in.input_write_pointer.c;
  for(int i=0; i < sampcount; i++){

    int isamp_i = (int8_t)*dp++;
    int isamp_q = (int8_t)*dp++;

    if(isamp_q == -128){
      sdr->clips++;
      isamp_q = -127;
    }
    if(isamp_i == -128){
      sdr->clips++;
      isamp_i = -127;
    }
    complex float samp = CMPLXF(isamp_i,isamp_q);
    samp_sum += samp;

    // remove DC offset (which can be fractional)
    samp -= sdr->DC;
    
    // Must correct gain and phase before frequency shift
    // accumulate I and Q energies before gain correction
    i_energy += crealf(samp) * crealf(samp);
    q_energy += cimagf(samp) * cimagf(samp);
    
    // Balance gains, keeping constant total energy
    __real__ samp *= sdr->gain_i;
    __imag__ samp *= sdr->gain_q;
    
    // Accumulate phase error
    dotprod += crealf(samp) * cimagf(samp);

    // Correct phase
    __imag__ samp = sdr->secphi * cimagf(samp) - sdr->tanphi * crealf(samp);
    wptr[i] = sdr->scale * samp;
  }
  write_cfilter(&frontend->in,NULL,sampcount); // Update write pointer, invoke FFT if block is complete
  frontend->timestamp = gps_time_ns();

  // Update every block
  // estimates of DC offset, signal powers and phase error
  sdr->DC += DC_alpha * (samp_sum - sampcount*sdr->DC);
  float block_energy = 0.5 * (i_energy + q_energy); // Normalize for complex pairs

  // These blocks are kinda small, so exponentially smooth the power readings
  frontend->if_power += sampcount * rate_factor * (block_energy/sampcount - frontend->if_power);
  frontend->samples += sampcount; // Count original samples
  if(block_energy > 0){ // Avoid divisions by 0, etc
    sdr->imbalance += rate_factor * sampcount * ((i_energy / q_energy) - sdr->imbalance);
    float dpn = dotprod / block_energy;
    sdr->sinphi += rate_factor  * sampcount * (dpn - sdr->sinphi);
    sdr->gain_q = sqrtf(0.5 * (1 + sdr->imbalance));
    sdr->gain_i = sqrtf(0.5 * (1 + 1./sdr->imbalance));
    sdr->secphi = 1/sqrtf(1 - sdr->sinphi * sdr->sinphi); // sec(phi) = 1/cos(phi)
    sdr->tanphi = sdr->sinphi * sdr->secphi;                     // tan(phi) = sin(phi) * sec(phi) = sin(phi)/cos(phi)
  }
  return 0;
}


static void *hackrf_agc(void *arg){
  struct sdrstate *sdr = (struct sdrstate *)arg;
  struct frontend *frontend = sdr->frontend;

  while(true){
    usleep(100000);
    float powerdB = power2dB(frontend->if_power*scale_ADpower2FS(frontend));
    int change;
    if(powerdB > Upper_limit)
      change = Upper_limit - powerdB;
    else if(powerdB < Lower_limit)
      change = Lower_limit - powerdB;
    else
      continue;
    
#if 0
    printf("if_power %.0f scale %g, DC (%f+j%f) sinphi %f gain_i %f gain_q %f agc change %d dB\n",
	   powerdB,sdr->scale,crealf(sdr->DC),cimagf(sdr->DC),
	   sdr->sinphi,
	   sdr->gain_i,sdr->gain_q,
	   change);
#endif
    if(change > 0){
      // Increase gain, LNA first, then mixer, and finally IF
      if(change >= 14 && sdr->lna_gain < 14){
	sdr->lna_gain = 14;
	change -= 14;
	int ret = hackrf_set_antenna_enable(sdr->device,sdr->lna_gain ? true : false);
	assert(ret == HACKRF_SUCCESS);
	(void)ret;
      }
      int old_mixer_gain = sdr->mixer_gain;
      int new_mixer_gain = min(40,old_mixer_gain + 8*(change/8));
      if(new_mixer_gain != old_mixer_gain){
	sdr->mixer_gain = new_mixer_gain;
	change -= new_mixer_gain - old_mixer_gain;
	int ret = hackrf_set_lna_gain(sdr->device,sdr->mixer_gain);
	assert(ret == HACKRF_SUCCESS);
	(void)ret;
      }
      int old_if_gain = sdr->if_gain;
      int new_if_gain = min(62,old_if_gain + 2*(change/2));
      if(new_if_gain != old_if_gain){
	sdr->if_gain = new_if_gain;
	change -= new_if_gain - old_if_gain;
	int ret = hackrf_set_vga_gain(sdr->device,sdr->if_gain);
	assert(ret == HACKRF_SUCCESS);
	(void)ret;
      }
    } else if(change < 0){
      // Reduce gain (IF first), start counter
      int old_if_gain = sdr->if_gain;
      int new_if_gain = max(0,old_if_gain + 2*(change/2));
      if(new_if_gain != old_if_gain){
	sdr->if_gain = new_if_gain;
	change -= new_if_gain - old_if_gain;
	int ret = hackrf_set_vga_gain(sdr->device,sdr->if_gain);
	assert(ret == HACKRF_SUCCESS);
	(void)ret;
      }
      int old_mixer_gain = sdr->mixer_gain;
      int new_mixer_gain = max(0,old_mixer_gain + 8*(change/8));
      if(new_mixer_gain != old_mixer_gain){
	sdr->mixer_gain = new_mixer_gain;
	change -= new_mixer_gain - old_mixer_gain;
	int ret = hackrf_set_lna_gain(sdr->device,sdr->mixer_gain);
	assert(ret == HACKRF_SUCCESS);
	(void)ret;
      }
      int old_lna_gain = sdr->lna_gain;
      int new_lna_gain = max(0,old_lna_gain + 14*(change/14));
      if(new_lna_gain != old_lna_gain){
	sdr->lna_gain = new_lna_gain;
	change -= new_lna_gain - old_lna_gain;
	int ret = hackrf_set_antenna_enable(sdr->device,sdr->lna_gain ? true : false);
	assert(ret == HACKRF_SUCCESS);
	(void)ret;
      }
    }
    frontend->lna_gain = sdr->lna_gain;
    frontend->mixer_gain = sdr->mixer_gain;
    frontend->if_gain = sdr->if_gain;
    frontend->rf_gain = sdr->lna_gain + sdr->mixer_gain + sdr->if_gain;
    frontend->rf_atten = 0;
    sdr->scale = scale_AD(frontend);
#if 0
    fprintf(stdout,"hackrf agc gains: %d %d %d %lf\n",frontend->lna_gain,frontend->mixer_gain,frontend->if_gain,
	    frontend->rf_gain);
#endif
  }
  return NULL;
}
double hackrf_tune(struct frontend * const frontend,double const frequency){
  
  // replace this with precise tuning?
  struct sdrstate *sdr = (struct sdrstate *)frontend->context;
  if(frontend->lock)
    return frontend->frequency;

  uint64_t intfreq = frequency;
  int ret = hackrf_set_freq(sdr->device,intfreq);
  assert(ret == HACKRF_SUCCESS);
  (void)ret;
  frontend->frequency = frequency;
  return frequency;

}

#if 0
// extracted from hackRF firmware/common/rffc5071.c
// Used to set RFFC5071 upconverter to multiples of 1 MHz
// for future use in determining exact tuning frequency

#define LO_MAX 5400.0
#define REF_FREQ 50.0
#define FREQ_ONE_MHZ (1000.0*1000.0)

static double  rffc5071_freq(uint16_t lo) {
  uint8_t lodiv;
  uint16_t fvco;
  uint8_t fbkdiv;
  
  /* Calculate n_lo */
  uint8_t n_lo = 0;
  uint16_t x = LO_MAX / lo;
  while ((x > 1) && (n_lo < 5)) {
    n_lo++;
    x >>= 1;
  }
  
  lodiv = 1 << n_lo;
  fvco = lodiv * lo;
  
  if (fvco > 3200) {
    fbkdiv = 4;
  } else {
    fbkdiv = 2;
  }
  
  uint64_t tmp_n = ((uint64_t)fvco << 29ULL) / (fbkdiv*REF_FREQ) ;
  
  return (REF_FREQ * (tmp_n >> 5ULL) * fbkdiv * FREQ_ONE_MHZ)
    / (lodiv * (1 << 24ULL));
}

static uint32_t max2837_freq(uint32_t freq){
  //  uint32_t div_frac;
  //	uint32_t div_int;
  uint32_t div_rem;
  uint32_t div_cmp;
  int i;
  
  /* ASSUME 40MHz PLL. Ratio = F*(4/3)/40,000,000 = F/30,000,000 */
  //	div_int = freq / 30000000;
  div_rem = freq % 30000000;
  //  div_frac = 0;
  div_cmp = 30000000;
  for( i = 0; i < 20; i++) {
    //    div_frac <<= 1;
    div_cmp >>= 1;
    if (div_rem > div_cmp) {
      //      div_frac |= 0x1;
      div_rem -= div_cmp;
    }
  }
  return div_rem;
}

#define MIN_LP_FREQ_MHZ (0)
#define MAX_LP_FREQ_MHZ (2150)

#define MIN_BYPASS_FREQ_MHZ (2150)
#define MAX_BYPASS_FREQ_MHZ (2750)

#define MIN_HP_FREQ_MHZ (2750)
#define MID1_HP_FREQ_MHZ (3600)
#define MID2_HP_FREQ_MHZ (5100)
#define MAX_HP_FREQ_MHZ (7250)

#define MIN_LO_FREQ_HZ (84375000)
#define MAX_LO_FREQ_HZ (5400000000ULL)

static uint32_t max2837_freq_nominal_hz=2560000000;

static uint64_t freq_cache = 100000000;
/*
 * Set freq/tuning between 0MHz to 7250 MHz (less than 16bits really used)
 * hz between 0 to 999999 Hz (not checked)
 * return false on error or true if success.
 */
static bool set_freq(const uint64_t freq)
{
  bool success;
  uint32_t RFFC5071_freq_mhz;
  uint32_t MAX2837_freq_hz;
  uint64_t real_RFFC5071_freq_hz;
  
  const uint32_t freq_mhz = freq / 1000000;
  const uint32_t freq_hz = freq % 1000000;
  
  success = true;
  
  const max2837_mode_t prior_max2837_mode = max2837_mode(&max2837);
  max2837_set_mode(&max2837, MAX2837_MODE_STANDBY);
  if(freq_mhz < MAX_LP_FREQ_MHZ)
    {
      rf_path_set_filter(&rf_path, RF_PATH_FILTER_LOW_PASS);
      /* IF is graduated from 2650 MHz to 2343 MHz */
      max2837_freq_nominal_hz = 2650000000 - (freq / 7);
      RFFC5071_freq_mhz = (max2837_freq_nominal_hz / FREQ_ONE_MHZ) + freq_mhz;
      /* Set Freq and read real freq */
      real_RFFC5071_freq_hz = rffc5071_set_frequency(&rffc5072, RFFC5071_freq_mhz);
      max2837_set_frequency(&max2837, real_RFFC5071_freq_hz - freq);
      sgpio_cpld_stream_rx_set_q_invert(&sgpio_config, 1);
    }else if( (freq_mhz >= MIN_BYPASS_FREQ_MHZ) && (freq_mhz < MAX_BYPASS_FREQ_MHZ) )
    {
      rf_path_set_filter(&rf_path, RF_PATH_FILTER_BYPASS);
      MAX2837_freq_hz = (freq_mhz * FREQ_ONE_MHZ) + freq_hz;
      /* RFFC5071_freq_mhz <= not used in Bypass mode */
      max2837_set_frequency(&max2837, MAX2837_freq_hz);
      sgpio_cpld_stream_rx_set_q_invert(&sgpio_config, 0);
    }else if(  (freq_mhz >= MIN_HP_FREQ_MHZ) && (freq_mhz <= MAX_HP_FREQ_MHZ) )
    {
      if (freq_mhz < MID1_HP_FREQ_MHZ) {
	/* IF is graduated from 2150 MHz to 2750 MHz */
	max2837_freq_nominal_hz = 2150000000 + (((freq - 2750000000) * 60) / 85);
      } else if (freq_mhz < MID2_HP_FREQ_MHZ) {
	/* IF is graduated from 2350 MHz to 2650 MHz */
	max2837_freq_nominal_hz = 2350000000 + ((freq - 3600000000) / 5);
      } else {
	/* IF is graduated from 2500 MHz to 2738 MHz */
	max2837_freq_nominal_hz = 2500000000 + ((freq - 5100000000) / 9);
      }
      rf_path_set_filter(&rf_path, RF_PATH_FILTER_HIGH_PASS);
      RFFC5071_freq_mhz = freq_mhz - (max2837_freq_nominal_hz / FREQ_ONE_MHZ);
      /* Set Freq and read real freq */
      real_RFFC5071_freq_hz = rffc5071_set_frequency(&rffc5072, RFFC5071_freq_mhz);
      max2837_set_frequency(&max2837, freq - real_RFFC5071_freq_hz);
      sgpio_cpld_stream_rx_set_q_invert(&sgpio_config, 0);
    }else
    {
      /* Error freq_mhz too high */
      success = false;
    }
  max2837_set_mode(&max2837, prior_max2837_mode);
  if( success ) {
    freq_cache = freq;
  }
  return success;
}
#endif
