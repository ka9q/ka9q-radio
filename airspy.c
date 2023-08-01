// Read from Airspy SDR
// Accept control commands from UDP socket
// Version linked into radiod
#undef DEBUG_AGC
#define _GNU_SOURCE 1
#include <assert.h>
#include <pthread.h>
#include <libairspy/airspy.h>
#include <errno.h>
#include <iniparser/iniparser.h>
#if defined(linux)
#include <bsd/string.h>
#endif

#include "conf.h"
#include "misc.h"
#include "multicast.h"
#include "status.h"
#include "radio.h"
#include "config.h"

extern int Status_ttl;

// Global variables set by config file options
extern int Verbose;
extern int Overlap;
extern const char *App_path;
extern int Verbose;
static bool Software_agc = true; // default
static float Low_threshold;
static float High_threshold;

// Anything generic should be in 'struct frontend' section 'sdr' in radio.h
struct sdrstate {
  struct frontend *frontend;  // Avoid references to external globals
  struct airspy_device *device;    // Opaque pointer

  uint32_t sample_rates[20];
  uint64_t SN; // Serial number

  bool antenna_bias; // Bias tee on/off

  // Tuning
  double converter;   // Upconverter base frequency (usually 120 MHz)
  int offset; // 1/4 of sample rate in real mode; 0 in complex mode
  char const *frequency_file; // Local file to store frequency in case we restart

  // AGC
  int holdoff;  // Holdoff counter after automatic change to allow settling
  bool linearity; // Use linearity gain tables; default is sensitivity
  int gainstep; // Airspy gain table steps (0-21), higher numbers == higher gain

  pthread_t cmd_thread;
  pthread_t monitor_thread;
};

// Taken from Airspy library driver source
#define GAIN_COUNT (22)
uint8_t airspy_linearity_vga_gains[GAIN_COUNT] = {     13, 12, 11, 11, 11, 11, 11, 10, 10, 10, 10, 10, 10, 10, 10, 10, 9, 8, 7, 6, 5, 4 };
uint8_t airspy_linearity_mixer_gains[GAIN_COUNT] = {   12, 12, 11,  9,  8,  7,  6,  6,  5,  0,  0,  1,  0,  0,  2,  2, 1, 1, 1, 1, 0, 0 };
uint8_t airspy_linearity_lna_gains[GAIN_COUNT] = {     14, 14, 14, 13, 12, 10,  9,  9,  8,  9,  8,  6,  5,  3,  1,  0, 0, 0, 0, 0, 0, 0 };
uint8_t airspy_sensitivity_vga_gains[GAIN_COUNT] = {   13, 12, 11, 10,  9,  8,  7,  6,  5,  5,  5,  5,  5,  4,  4,  4, 4, 4, 4, 4, 4, 4 };
uint8_t airspy_sensitivity_mixer_gains[GAIN_COUNT] = { 12, 12, 12, 12, 11, 10, 10,  9,  9,  8,  7,  4,  4,  4,  3,  2, 2, 1, 0, 0, 0, 0 };
uint8_t airspy_sensitivity_lna_gains[GAIN_COUNT] = {   14, 14, 14, 14, 14, 14, 14, 14, 14, 13, 12, 12,  9,  9,  8,  7, 6, 5, 3, 2, 1, 0 };


static double set_correct_freq(struct sdrstate *sdr,double freq);
static int rx_callback(airspy_transfer *transfer);
static void *airspy_monitor(void *p);
static double true_freq(uint64_t freq);
static void set_gain(struct sdrstate *sdr,int gainstep);

int airspy_setup(struct frontend * const frontend,dictionary * const Dictionary,char const * const section){
  assert(Dictionary != NULL);

  struct sdrstate * const sdr = calloc(1,sizeof(struct sdrstate));
  // Cross-link generic and hardware-specific control structures
  sdr->frontend = frontend;
  frontend->sdr.context = sdr;
  {
    char const *device = config_getstring(Dictionary,section,"device",NULL);
    if(strcasecmp(device,"airspy") != 0)
      return -1; // Not for us
  }
  {
    int ret;
    if((ret = airspy_init()) != AIRSPY_SUCCESS){
      fprintf(stdout,"airspy_init() failed: %s\n",airspy_error_name(ret));
      return -1;
    }
  }
  {
    char const *sn = config_getstring(Dictionary,section,"serial",NULL);
    if(sn != NULL){
      char *endptr = NULL;
      sdr->SN = 0;
      sdr->SN = strtoull(sn,&endptr,16);
      if(endptr == NULL || *endptr != '\0'){
	fprintf(stdout,"Invalid serial number %s in section %s\n",sn,section);
	return -1;
      }
    } else {
      // Serial number not specified, enumerate and pick one
      int n_serials = 100; // ridiculously large
      uint64_t serials[n_serials];

      n_serials = airspy_list_devices(serials,n_serials); // Return actual number
      if(n_serials <= 0){
	fprintf(stdout,"No airspy devices found\n");
	return -1;
      }
      fprintf(stdout,"Discovered airspy device serial%s:",n_serials > 1 ? "s" : "");
      for(int i = 0; i < n_serials; i++){
	fprintf(stdout," %llx",(long long)serials[i]);
      }
      fprintf(stdout,"\n");
      fprintf(stdout,"Selecting %llx; to select another, add 'serial = ' to config file\n",(long long)serials[0]);
      sdr->SN = serials[0];
    }
  }
  {
    int ret = airspy_open_sn(&sdr->device,sdr->SN);
    if(ret != AIRSPY_SUCCESS){
      fprintf(stdout,"airspy_open(%llx) failed: %s\n",(long long)sdr->SN,airspy_error_name(ret));
      return -1;
    } 
  }
  {
    airspy_lib_version_t version;
    airspy_lib_version(&version);

    const int VERSION_LOCAL_SIZE = 128; // Library doesn't define, but says should be >= 128
    char hw_version[VERSION_LOCAL_SIZE];
    airspy_version_string_read(sdr->device,hw_version,sizeof(hw_version));

    fprintf(stdout,"Airspy serial %llx, hw version %s, library version %d.%d.%d\n",
	    (long long unsigned)sdr->SN,
	    hw_version,
	    version.major_version,version.minor_version,version.revision);
  }
  // Initialize hardware first
  {
    int ret __attribute__ ((unused));
    ret = airspy_set_packing(sdr->device,1);
    assert(ret == AIRSPY_SUCCESS);
    
    // Set this now, as it affects the list of supported sample rates
    ret = airspy_set_sample_type(sdr->device,AIRSPY_SAMPLE_RAW);
    assert(ret == AIRSPY_SUCCESS);
    // Get and list sample rates
    ret = airspy_get_samplerates(sdr->device,sdr->sample_rates,0);
    assert(ret == AIRSPY_SUCCESS);
    int const number_sample_rates = sdr->sample_rates[0];
    if(number_sample_rates <= 0){
      fprintf(stdout,"error, no valid sample rates!\n");
      return -1;
    }
    fprintf(stdout,"%'d sample rate%s:",number_sample_rates,number_sample_rates > 1 ? "s":"");
    ret = airspy_get_samplerates(sdr->device,sdr->sample_rates,number_sample_rates);
    assert(ret == AIRSPY_SUCCESS);
    for(int n = 0; n < number_sample_rates; n++){
      fprintf(stdout," %'d",sdr->sample_rates[n]);
      if(sdr->sample_rates[n] < 1)
	break;
    }
    fprintf(stdout,"\n");
  }
  // Default to first (highest) sample rate on list
  frontend->sdr.samprate = config_getint(Dictionary,section,"samprate",sdr->sample_rates[0]);
  frontend->sdr.isreal = true;
  sdr->offset = frontend->sdr.samprate/4;
  sdr->converter = config_getfloat(Dictionary,section,"converter",0);
  frontend->sdr.calibrate = config_getdouble(Dictionary,section,"calibrate",0);

  fprintf(stdout,"Set sample rate %'u Hz, offset %'d Hz\n",frontend->sdr.samprate,sdr->offset);
  {
    int ret __attribute__ ((unused));
    ret = airspy_set_samplerate(sdr->device,(uint32_t)frontend->sdr.samprate);
    assert(ret == AIRSPY_SUCCESS);
  }
  frontend->sdr.calibrate = 0;
  frontend->sdr.max_IF = -600000;
  frontend->sdr.min_IF = -0.47 * frontend->sdr.samprate;

  sdr->gainstep = -1; // Force update first time

  // Hardware device settings
  sdr->linearity = config_getboolean(Dictionary,section,"linearity",false);
  int const lna_agc = config_getboolean(Dictionary,section,"lna-agc",false); // default off
  airspy_set_lna_agc(sdr->device,lna_agc);
  if(lna_agc)
    Software_agc = false;

  int const mixer_agc = config_getboolean(Dictionary,section,"mixer-agc",false); // default off
  airspy_set_mixer_agc(sdr->device,mixer_agc);
  if(mixer_agc)
    Software_agc = false;
  
  int const lna_gain = config_getint(Dictionary,section,"lna-gain",-1);
  if(lna_gain != -1){
    frontend->sdr.lna_gain = lna_gain;
    airspy_set_lna_gain(sdr->device,lna_gain);
    Software_agc = false;
  }      
  int const mixer_gain = config_getint(Dictionary,section,"mixer-gain",-1);
  if(mixer_gain != -1){
    frontend->sdr.mixer_gain = mixer_gain;
    airspy_set_mixer_gain(sdr->device,mixer_gain);
    Software_agc = false;
  }
  int const vga_gain = config_getint(Dictionary,section,"vga-gain",-1);
  if(vga_gain != -1){
    frontend->sdr.if_gain = vga_gain;
    airspy_set_vga_gain(sdr->device,vga_gain);
    Software_agc = false;
  }
  int gainstep = config_getint(Dictionary,section,"gainstep",-1);
  if(gainstep >= 0){
    if(gainstep > GAIN_COUNT-1)
      gainstep = GAIN_COUNT-1;
    set_gain(sdr,gainstep); // Start AGC with max gain step
  } else if(Software_agc){
    gainstep = GAIN_COUNT-1;
    set_gain(sdr,gainstep); // Start AGC with max gain step
  }
  sdr->antenna_bias = config_getboolean(Dictionary,section,"bias",false);
  {
    int ret __attribute__ ((unused));
    ret = airspy_set_rf_bias(sdr->device,sdr->antenna_bias);
    assert(ret == AIRSPY_SUCCESS);
  }  
  {
    char const *p = config_getstring(Dictionary,section,"description",NULL);
    if(p != NULL){
      strlcpy(frontend->sdr.description,p,sizeof(frontend->sdr.description));
      fprintf(stdout,"%s: ",frontend->sdr.description);
    }
  }
  fprintf(stdout,"Software AGC %d; linearity %d, LNA AGC %d, Mix AGC %d, LNA gain %d, Mix gain %d, VGA gain %d, gainstep %d, bias tee %d\n",
	  Software_agc,sdr->linearity,lna_agc,mixer_agc,frontend->sdr.lna_gain,frontend->sdr.mixer_gain,frontend->sdr.if_gain,gainstep,sdr->antenna_bias);

  {
    float const dh = config_getdouble(Dictionary,section,"agc-high-threshold",-10.0);
    High_threshold = dB2power(-fabs(dh));
    float const dl = config_getdouble(Dictionary,section,"agc-low-threshold",-40.0);
    Low_threshold = dB2power(-fabs(dl));
  }
  double init_frequency = config_getdouble(Dictionary,section,"frequency",0);
  if(init_frequency != 0)
    frontend->sdr.lock = 1;
  {
    char *tmp;
    int ret __attribute__ ((unused));
    // space is malloc'ed by asprintf but not freed
    ret = asprintf(&tmp,"%s/tune-airspy.%llx",VARDIR,(unsigned long long)sdr->SN);
    if(ret != -1){
      sdr->frequency_file = tmp;
    }
  }
  if(init_frequency == 0){
    // If not set on command line, load saved frequency
    FILE *fp = fopen(sdr->frequency_file,"r+");
    if(fp == NULL)
      fprintf(stdout,"Can't open tuner state file %s: %s\n",sdr->frequency_file,strerror(errno));
    else {
      fprintf(stdout,"Using tuner state file %s\n",sdr->frequency_file);
      int r;
      if((r = fscanf(fp,"%lf",&init_frequency)) < 0)
	fprintf(stdout,"Can't read stored freq. r = %'d: %s\n",r,strerror(errno));
      fclose(fp);
    }
  }
  if(init_frequency == 0){
    // Not set in config file or from cache file. Use fallback to cover 2m
    init_frequency = 149e6; // Fallback default
    fprintf(stdout,"Fallback default frequency %'.3lf Hz\n",init_frequency);
  }
  fprintf(stdout,"Setting initial frequency %'.3lf Hz, %s\n",init_frequency,frontend->sdr.lock ? "locked" : "not locked");
  set_correct_freq(sdr,init_frequency);
  return 0;
}
int airspy_startup(struct frontend *frontend){
  struct sdrstate *sdr = (struct sdrstate *)frontend->sdr.context;
  pthread_create(&sdr->monitor_thread,NULL,airspy_monitor,sdr);
  return 0;
}

static void *airspy_monitor(void *p){
  struct sdrstate *sdr = (struct sdrstate *)p;
  assert(sdr != NULL);
  pthread_setname("airspy-mon");

  realtime();
  int ret __attribute__ ((unused));
  ret = airspy_start_rx(sdr->device,rx_callback,sdr);
  assert(ret == AIRSPY_SUCCESS);
  fprintf(stdout,"airspy running\n");
  // Periodically poll status to ensure device hasn't reset
  while(1){
    sleep(1);
    if(!airspy_is_streaming(sdr->device))
      break; // Device seems to have bombed. Exit and let systemd restart us
  }
  fprintf(stdout,"Device is no longer streaming, exiting\n");
  airspy_close(sdr->device);
  airspy_exit();
  return NULL;
}


static bool Name_set = false;
// Callback called with incoming receiver data from A/D
static int rx_callback(airspy_transfer *transfer){
  assert(transfer != NULL);
  struct sdrstate * const sdr = (struct sdrstate *)transfer->ctx;
  assert(sdr != NULL);
  struct frontend * const frontend = sdr->frontend;
  assert(frontend != NULL);

  if(!Name_set){
    pthread_setname("airspy-cb");
    Name_set = true;
  }
  if(transfer->dropped_samples){
    fprintf(stdout,"dropped %'lld\n",(long long)transfer->dropped_samples);
  }
  assert(transfer->sample_type == AIRSPY_SAMPLE_RAW);
  int const sampcount = transfer->sample_count;
  float * wptr = frontend->in->input_write_pointer.r;
  uint32_t const *up = (uint32_t *)transfer->samples;
  assert(wptr != NULL);
  assert(up != NULL);
  uint64_t in_energy = 0;
  // Libairspy could do this for us, but this minimizes mem copies
  // This could probably be vectorized someday
  for(int i=0; i < sampcount; i+= 8){ // assumes multiple of 8
#if 1
    int s[8];
    s[0] =  up[0] >> 20;
    s[1] =  up[0] >> 8;
    s[2] =  (up[0] << 4) | (up[1] >> 28);
    s[3] =  up[1] >> 16;
    s[4] =  up[1] >> 4;
    s[5] =  (up[1] << 8) | (up[2] >> 24);
    s[6] =  up[2] >> 12;
    s[7] =  up[2];
    for(int j=0; j < 8; j++){
      int const x = (s[j] & 0xfff) - 2048; // mask not actually necessary for s[0]
      in_energy += x * x;
      wptr[j] = x * SCALE12;
    }
#else
    // Unrolled version, not really faster on Pi
    int x;
    x = (int)(up[0] >> 20) - 2048;
    in_energy += x * x;
    wptr[0] = x * SCALE12;

    x = (int)((up[0] >> 8) & 0xfff) - 2048;
    in_energy += x * x;
    wptr[1] = x * SCALE12;

    x = (int)(((up[0] << 4) | (up[1] >> 28)) & 0xfff) - 2048;
    in_energy += x * x;
    wptr[2] = x * SCALE12;

    x = (int)((up[1] >> 16) & 0xfff) - 2048;
    in_energy += x * x;
    wptr[3] = x * SCALE12;

    x = (int)((up[1] >> 4) & 0xfff) - 2048;
    in_energy += x * x;
    wptr[4] = x * SCALE12;

    x = (int)(((up[1] << 8) | (up[2] >> 24)) & 0xfff) - 2048;
    in_energy += x * x;
    wptr[5] = x * SCALE12;

    x = (int)((up[2] >> 12) & 0xfff) - 2048;
    in_energy += x * x;
    wptr[6] = x * SCALE12;

    x = (int)(up[2] & 0xfff) - 2048;
    in_energy += x * x;
    wptr[7] = x * SCALE12;
#endif
    wptr += 8;
    up += 3;
  }
  frontend->input.samples += sampcount;
  write_rfilter(frontend->in,NULL,sampcount); // Update write pointer, invoke FFT
  frontend->sdr.output_level = 2 * in_energy * SCALE16 * SCALE16 / sampcount;
  frontend->input.samples += sampcount;

  if(Software_agc){
    // Scale by 2 / 2048^2 = 2^-21 for 0 dBFS = full scale sine wave
    float const power = frontend->sdr.output_level;
    if(power < Low_threshold && sdr->holdoff == 0){
      if(Verbose)
	printf("Power %.1f dB\n",power2dB(power));
      set_gain(sdr,sdr->gainstep + 1);
      sdr->holdoff = 2; // seems to settle down in 2 blocks
    } else if(power > High_threshold && sdr->holdoff == 0){
      if(Verbose)
	printf("Power %.1f dB\n",power2dB(power));
      set_gain(sdr,sdr->gainstep - 1);
      sdr->holdoff = 2;
    } else if(sdr->holdoff > 0){
      sdr->holdoff--;
      if(Verbose > 1)
	printf("Power %.1f dB\n",power2dB(power));
    }
  }
  return 0;
}

// For a requested frequency, give the actual tuning frequency
// Many thanks to Youssef Touil <youssef@airspy.com> who gave me most of it
// This "mostly" works except that the calibration correction inside the unit
// shifts the tuning steps, so the result here can be off one
// Not easy to fix without knowing the calibration parameters.
// Best workaround is a GPSDO, which disables the correction
static double true_freq(uint64_t freq_hz){
  uint32_t const VCO_MIN=1770000000u; // 1.77 GHz
  uint32_t const VCO_MAX=(VCO_MIN << 1); // 3.54 GHz
  int const MAX_DIV = 5;

  // Clock divider set to 2 for the best resolution
  uint32_t const pll_ref = 25000000u / 2; // 12.5 MHz
  
  // Find divider to put VCO = f*2^(d+1) in range VCO_MIN to VCO_MAX
  //          MHz             step, Hz
  // 0: 885.0     1770.0      190.735
  // 1: 442.50     885.00      95.367
  // 2: 221.25     442.50      47.684
  // 3: 110.625    221.25      23.842
  // 4:  55.3125   110.625     11.921
  // 5:  27.65625   55.312      5.960
  int8_t div_num;
  for (div_num = 0; div_num <= MAX_DIV; div_num++){
    uint32_t const vco = freq_hz << (div_num + 1);
    if (VCO_MIN <= vco && vco <= VCO_MAX)
      break;
  }
  if(div_num > MAX_DIV)
    return 0; // Frequency out of range
  
  // r = PLL programming bits: Nint in upper 16 bits, Nfract in lower 16 bits
  // Freq steps are pll_ref / 2^(16 + div_num) Hz
  // Note the '+ (pll_ref >> 1)' term simply rounds the division to the nearest integer
  uint32_t const r = ((freq_hz << (div_num + 16)) + (pll_ref >> 1)) / pll_ref;
  
  // This is a puzzle; is it related to spur suppression?
  double const offset = 0.25;
  // Compute true frequency
  return (((double)r + offset) * pll_ref) / (double)(1 << (div_num + 16));
}

// set the airspy tuner to the requested frequency, applying:
// Spyverter converter offset (120 MHz, or 0 if not in use)
// TCXO calibration offset
// Fs/4 = 5 MHz offset (firmware assumes library real->complex conversion, which we don't use)
// Apply 820T synthesizer tuning step model

// the TCXO calibration offset is a holdover from the Funcube dongle and doesn't
// really fit the Airspy with its internal factory calibration
// All this really works correctly only with a gpsdo, forcing the calibration offset to 0
static double set_correct_freq(struct sdrstate *sdr,double freq){
  struct frontend *frontend = sdr->frontend;
  // sdr->converter refers to an upconverter, so it's added to the frequency we request
  int64_t intfreq = round((freq + sdr->converter)/ (1 + frontend->sdr.calibrate));
  int ret __attribute__((unused)) = AIRSPY_SUCCESS; // Won't be used when asserts are disabled
  ret = airspy_set_freq(sdr->device,intfreq - sdr->offset);
  assert(ret == AIRSPY_SUCCESS);
  double const tf = true_freq(intfreq);
  frontend->sdr.frequency = tf * (1 + frontend->sdr.calibrate) - sdr->converter;
  FILE *fp = fopen(sdr->frequency_file,"w");
  if(fp){
    if(fprintf(fp,"%lf\n",frontend->sdr.frequency) < 0)
      fprintf(stdout,"Can't write to tuner state file %s: %s\n",sdr->frequency_file,strerror(errno));
    fclose(fp);
    fp = NULL;
  }
  return frontend->sdr.frequency;
}
double airspy_tune(struct frontend *frontend,double f){
  struct sdrstate *sdr = frontend->sdr.context;
  return set_correct_freq(sdr,f);
}


static void set_gain(struct sdrstate *sdr,int gainstep){
  struct frontend *frontend = sdr->frontend;
  if(gainstep < 0)
    gainstep = 0;
  else if(gainstep >= GAIN_COUNT)
    gainstep = GAIN_COUNT-1;

  if(gainstep != sdr->gainstep){
    sdr->gainstep = gainstep;
    int const tab = GAIN_COUNT - 1 - sdr->gainstep;
    if(sdr->linearity){
      int ret __attribute__((unused)) = AIRSPY_SUCCESS; // Won't be used when asserts are disabled
      ret = airspy_set_linearity_gain(sdr->device,sdr->gainstep);
      assert(ret == AIRSPY_SUCCESS);
      frontend->sdr.if_gain = airspy_linearity_vga_gains[tab];
      frontend->sdr.mixer_gain = airspy_linearity_mixer_gains[tab];
      frontend->sdr.lna_gain = airspy_linearity_lna_gains[tab];
    } else {
      int ret __attribute__((unused)) = AIRSPY_SUCCESS; // Won't be used when asserts are disabled
      ret = airspy_set_sensitivity_gain(sdr->device,sdr->gainstep);
      assert(ret == AIRSPY_SUCCESS);
      frontend->sdr.if_gain = airspy_sensitivity_vga_gains[tab];
      frontend->sdr.mixer_gain = airspy_sensitivity_mixer_gains[tab];
      frontend->sdr.lna_gain = airspy_sensitivity_lna_gains[tab];
    }
#if 0
    send_airspy_status(sdr,1);
#endif
    if(Verbose)
      printf("New gainstep %d: LNA = %d, mixer = %d, vga = %d\n",gainstep,
	     frontend->sdr.lna_gain,frontend->sdr.mixer_gain,frontend->sdr.if_gain);
  }
}


