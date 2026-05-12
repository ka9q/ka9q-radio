// Front end driver for HydraSDR RFOne, linked into ka9q-radio's radiod
// Copyright 2023, Phil Karn, KA9Q

#undef DEBUG_AGC
#define _GNU_SOURCE 1
#include <assert.h>
#include <pthread.h>
#include <libhydrasdr/hydrasdr.h>
#include <errno.h>
#include <iniparser/iniparser.h>
#if defined(linux)
#include <bsd/string.h>
#endif
#include <sysexits.h>
#include <unistd.h>
#include <strings.h>

#include "misc.h"
#include "multicast.h"
#include "status.h"
#include "radio.h"
#include "config.h"

// Global variables set by config file options
extern int Verbose;
extern char const *Description;

// Anything generic should be in 'struct frontend' section 'sdr' in radio.h
struct sdrstate {
  struct frontend *frontend;  // Avoid references to external globals
  struct hydrasdr_device *device;    // Opaque pointer
  enum hydrasdr_sample_type sample_type;

  uint32_t sample_rates[20];
  uint64_t SN; // Serial number

  bool antenna_bias; // Bias tee on/off

  // Tuning
  double converter;   // Upconverter base frequency (usually 120 MHz)
  double offset; // 1/4 of sample rate in real mode; 0 in complex mode

  // AGC
  bool software_agc;
  bool linearity; // Use linearity gain tables; default is sensitivity
  int gainstep; // HydraSDR gain table steps (0-21), higher numbers == higher gain
  int mingainstep;
  int maxgainstep;
  double agc_energy; // Integrated energy
  int agc_samples; // Samples represented in energy
  double high_threshold;
  double low_threshold;
  double scale;         // Scale samples for #bits and front end gain

  pthread_t cmd_thread;
  pthread_t monitor_thread;
};

static const char *Sample_type_name[] = {
  "float32_iq",
  "float32_real",
  "int16_iq",
  "int16_real",
  "uint16_real",
  "raw",
  "int8_iq",
  "uint8_iq",
  "int8_real",
  "uint8_real",
};
#define HYDRASDR_SAMPTYPES (sizeof Sample_type_name / sizeof Sample_type_name[0])


static char const *Hydrasdr_keys[] = {
  "agc-high-threshold",
  "agc-low-threshold",
  "bias",
  "calibrate",
  "converter",
  "description",
  "device",
  "firmware",
  "frequency",
  "gainstep",
  "library",
  "linearity",
  "lna-agc",
  "lna-gain",
  "mixer-agc",
  "mixer-gain",
  "samprate",
  "serial",
  "vga-gain",
NULL
};


static double Power_alpha = 0.05; // Calculate this properly someday
static double set_correct_freq(struct sdrstate *sdr,double freq);
static int rx_callback(hydrasdr_transfer *transfer);
static void *hydrasdr_monitor(void *p);
static double true_freq(uint64_t freq);
static void set_gain(struct sdrstate *sdr,int gainstep);

int hydrasdr_setup(struct frontend * const frontend,dictionary * const Dictionary,char const * const section){
  assert(Dictionary != NULL);

  struct sdrstate * const sdr = calloc(1,sizeof(struct sdrstate));
  assert(sdr != NULL);
  // Cross-link generic and hardware-specific control structures
  sdr->frontend = frontend;
  frontend->context = sdr;
  {
    char const *device = config_getstring(Dictionary,section,"device",section);
    if(strcasecmp(device,"hydrasdr") != 0)
      return -1; // Not for us
  }
  config_validate_section(stderr,Dictionary,section,Hydrasdr_keys,NULL);
  {
    hydrasdr_lib_version_t lib_ver;
    hydrasdr_lib_version(&lib_ver);
    // Check library version compatibility using HYDRASDR_MAKE_VERSION macro
    uint32_t runtime_ver = HYDRASDR_MAKE_VERSION(lib_ver.major_version,
					       lib_ver.minor_version,
						 lib_ver.revision);
    // Minimum library version required (uses HYDRASDR_MAKE_VERSION macro)
#define MIN_LIB_VERSION HYDRASDR_MAKE_VERSION(1, 1, 0)
    if (runtime_ver < MIN_LIB_VERSION) {
      fprintf(stderr, "HydraSDR library version too old: need v1.1u.0+, got v%u.%u.%u\n",
	      lib_ver.major_version, lib_ver.minor_version, lib_ver.revision);
    } else {
      fprintf(stderr, "HydraSDR library v%u.%u.%u\n",
	       lib_ver.major_version, lib_ver.minor_version, lib_ver.revision);
    }
  }
  {
    char const * const sn = config_getstring(Dictionary,section,"serial",NULL);
    if(sn != NULL){
      char *endptr = NULL;
      sdr->SN = 0;
      sdr->SN = strtoull(sn,&endptr,16);
      if(endptr == NULL || *endptr != '\0'){
	fprintf(stderr,"Invalid serial number %s in section %s\n",sn,section);
	return -1;
      }
    } else {
      // Serial number not specified, enumerate and pick one
      int n_serials = 100; // ridiculously large
      uint64_t serials[n_serials];

      n_serials = hydrasdr_list_devices(serials,n_serials); // Return actual number
      if(n_serials <= 0){
	fprintf(stderr,"No HydraSDR devices found\n");
	return -1;
      }
      if(n_serials > 1){
	fprintf(stderr,"Discovered %d HydraSDR device serial%s:",n_serials,n_serials > 1 ? "s" : "");
	for(int i = 0; i < n_serials; i++){
	  fprintf(stderr," %llx",(long long)serials[i]);
	}
	fprintf(stderr,"\n");
      }
      sdr->SN = serials[0];
    }
  }
  {
    fprintf(stderr,"Selecting HydraSDR SN %llx: ",(long long)sdr->SN);

    int const ret = hydrasdr_open_sn(&sdr->device,sdr->SN);
    if(ret != HYDRASDR_SUCCESS){
      fprintf(stderr,"hydrasdr_open(SN %llx) failed: %s\n",(long long)sdr->SN,hydrasdr_error_name(ret));
      return -1;
    }
  }
  hydrasdr_device_info_t info = {0};
  {
    int ret = hydrasdr_get_device_info(sdr->device, &info);
    if(ret != HYDRASDR_SUCCESS){
      fprintf(stderr, "Cannot get HydraSDR information: %s\n", hydrasdr_error_name(ret));
      return -1;
    }
    fprintf(stderr,"hw version %s, firmware %s\n", info.board_name, info.firmware_version);

    fprintf(stderr,"Supported sample types:");
    for (unsigned int i = 0; i < HYDRASDR_SAMPTYPES; i++) {
      if(info.sample_types & (1 << i)) {
	fprintf(stderr," %s", Sample_type_name[i]);
      }
    }
    // Choose in descending order of preference
    // Packed raw mode is *by far* the most preferable; it minimizes both
    // CPU load (no real->complex conversion, minimum USB bit rate), but
    // as per Hydra this may not be supported in all future devices

    // The INT16 modes are normalized to 16 bits (-32768 to +32767) regardless of device, but
    // UINT16_REAL includes the A/D offset, which is device dependent
    // and not indicated in the info table. The SDROne is 12 bits so we'll assume offset=2048,
    // ie, values from 0 to 4095

    // Floats are assumed normalized to -/+ 1
    if(info.sample_types & (1 << HYDRASDR_SAMPLE_RAW)){
      sdr->sample_type = HYDRASDR_SAMPLE_RAW; // most efficient
      frontend->bitspersample = 12;
    } else if(info.sample_types & (1 << HYDRASDR_SAMPLE_UINT16_REAL)){
      sdr->sample_type = HYDRASDR_SAMPLE_UINT16_REAL;
      frontend->bitspersample = 16;
    } else if (info.sample_types & (1 << HYDRASDR_SAMPLE_INT16_REAL)){
      sdr->sample_type = HYDRASDR_SAMPLE_INT16_REAL;
      frontend->bitspersample = 16;
    } else if (info.sample_types & (1 << HYDRASDR_SAMPLE_FLOAT32_REAL)){
      sdr->sample_type = HYDRASDR_SAMPLE_FLOAT32_REAL;
      frontend->bitspersample = 1; // Floats are normalized
    } else if (info.sample_types & (1 << HYDRASDR_SAMPLE_INT16_IQ)){
      sdr->sample_type = HYDRASDR_SAMPLE_INT16_IQ;
      frontend->bitspersample = 16;
    } else if (info.sample_types & (1 << HYDRASDR_SAMPLE_FLOAT32_IQ)){
      sdr->sample_type = HYDRASDR_SAMPLE_FLOAT32_IQ;
      frontend->bitspersample = 1;
    } else if (info.sample_types & (1 << HYDRASDR_SAMPLE_UINT8_REAL)){
      sdr->sample_type = HYDRASDR_SAMPLE_UINT8_REAL;
      frontend->bitspersample = 8;
    } else if (info.sample_types & (1 << HYDRASDR_SAMPLE_INT8_REAL)){
      sdr->sample_type = HYDRASDR_SAMPLE_INT8_REAL;
      frontend->bitspersample = 8;
    } else if (info.sample_types & (1 << HYDRASDR_SAMPLE_UINT8_IQ)){
      sdr->sample_type = HYDRASDR_SAMPLE_UINT8_IQ;
      frontend->bitspersample = 8;
    } else if (info.sample_types & (1 << HYDRASDR_SAMPLE_INT8_IQ)){
      sdr->sample_type = HYDRASDR_SAMPLE_INT8_IQ;
      frontend->bitspersample = 8;
    } else {
      fprintf(stderr,"No supported sample formats\n");
      return -1;
    }
    fprintf(stderr,"; choosing %s\n",Sample_type_name[sdr->sample_type]);

    ret = hydrasdr_set_packing(sdr->device, true);
    assert(ret == HYDRASDR_SUCCESS); // should handle failure

    // Set this now, as it affects the list of supported sample rates
    ret = hydrasdr_set_sample_type(sdr->device,sdr->sample_type);
    assert(ret == HYDRASDR_SUCCESS);
    // Get and list sample rates
    ret = hydrasdr_get_samplerates(sdr->device,sdr->sample_rates,0);
    assert(ret == HYDRASDR_SUCCESS);
    int const number_sample_rates = sdr->sample_rates[0];
    if(number_sample_rates <= 0){
      fprintf(stderr,"error, no valid sample rates!\n");
      return -1;
    }
    fprintf(stderr,"%'d sample rate%s:",number_sample_rates,number_sample_rates > 1 ? "s":"");
    ret = hydrasdr_get_samplerates(sdr->device,sdr->sample_rates,number_sample_rates);
    assert(ret == HYDRASDR_SUCCESS);
    for(int n = 0; n < number_sample_rates; n++){
      fprintf(stderr," %'d",sdr->sample_rates[n]);
      if(sdr->sample_rates[n] < 1)
	break;
    }
    fprintf(stderr,"\n");
  }
  frontend->samprate = sdr->sample_rates[0];  // Default to first (highest) sample rate on list
  {
    char const *p = config_getstring(Dictionary,section,"samprate",NULL);
    if(p != NULL)
      frontend->samprate = parse_frequency(p,false);
  }
  // We already knew if an offset had to applied, but not how much until we knew the sample rate
  switch(sdr->sample_type){
  case HYDRASDR_SAMPLE_FLOAT32_IQ:
  case HYDRASDR_SAMPLE_INT16_IQ:
  case HYDRASDR_SAMPLE_INT8_IQ:
  case HYDRASDR_SAMPLE_UINT8_IQ:
    frontend->isreal = false;
    sdr->offset = 0;
    break;
  case HYDRASDR_SAMPLE_FLOAT32_REAL:
  case HYDRASDR_SAMPLE_INT16_REAL:
  case HYDRASDR_SAMPLE_UINT16_REAL:
  case HYDRASDR_SAMPLE_RAW:
  case HYDRASDR_SAMPLE_INT8_REAL:
  case HYDRASDR_SAMPLE_UINT8_REAL:
  default:
    frontend->isreal = true;
    sdr->offset = +frontend->samprate / 4; // Positive for high-side injection (assumed)
    break;
  }
  fprintf(stderr,"Set sample rate %'lf Hz, offset %'lf Hz\n",frontend->samprate,sdr->offset);
  {
    int ret __attribute__ ((unused));
    ret = hydrasdr_set_samplerate(sdr->device,(uint32_t)frontend->samprate);
    assert(ret == HYDRASDR_SUCCESS);
  }
  // Gain features
  sdr->linearity = config_getboolean(Dictionary,section,"linearity",false);

  fprintf(stderr,"Gain features:");
  if (info.features & HYDRASDR_CAP_LINEARITY_GAIN) {
    if(sdr->linearity){
      sdr->mingainstep = info.linearity_gain.min_value;
      sdr->maxgainstep = info.linearity_gain.max_value;
      sdr->gainstep = info.linearity_gain.default_value;
    }
    fprintf(stderr," linearity %u-%u %u,",
	    info.linearity_gain.min_value,
	    info.linearity_gain.max_value,
	    info.linearity_gain.default_value);
  }
  if (info.features & HYDRASDR_CAP_SENSITIVITY_GAIN) {
    if(!sdr->linearity){
      sdr->mingainstep = info.sensitivity_gain.min_value;
      sdr->maxgainstep = info.sensitivity_gain.max_value;
      sdr->gainstep = info.sensitivity_gain.default_value;
    }
    fprintf(stderr," sensitivity gain %u-%u %u,",
	    info.sensitivity_gain.min_value,
	    info.sensitivity_gain.max_value,
	    info.sensitivity_gain.default_value);
  }
  if (info.features & HYDRASDR_CAP_LNA_GAIN) {
    fprintf(stderr," LNA gain %u-%u %u,",
	    info.lna_gain.min_value,
	    info.lna_gain.max_value,
	    info.lna_gain.default_value);
  }
  if (info.features & HYDRASDR_CAP_RF_GAIN) {
    fprintf(stderr," RF gain %u-%u %u,",
	    info.rf_gain.min_value,
	    info.rf_gain.max_value,
	    info.rf_gain.default_value);
  }
  if (info.features & HYDRASDR_CAP_MIXER_GAIN) {
    fprintf(stderr," Mixer %u-%u %u,",
	    info.mixer_gain.min_value,
	    info.mixer_gain.max_value,
	    info.mixer_gain.default_value);
  }
  if (info.features & HYDRASDR_CAP_FILTER_GAIN) {
    fprintf(stderr," Filter %u-%u %u,",
	    info.filter_gain.min_value,
	    info.filter_gain.max_value,
	    info.filter_gain.default_value);
  }
  if (info.features & HYDRASDR_CAP_VGA_GAIN) {
    fprintf(stderr," VGA gain %u-%u %u",
	    info.vga_gain.min_value,
	    info.vga_gain.max_value,
	    info.vga_gain.default_value);
  }
  fputc('\n',stderr);

  fprintf(stderr,"features:");
  if (info.features & HYDRASDR_CAP_LNA_AGC)
    fprintf(stderr," LNA_AGC");
  if (info.features & HYDRASDR_CAP_MIXER_AGC)
    fprintf(stderr," Mixer_AGC");
  if (info.features & HYDRASDR_CAP_BIAS_TEE)
    fprintf(stderr," Bias-T");
  if (info.features & HYDRASDR_CAP_PACKING)
    fprintf(stderr," sample_packing");
  fputc('\n',stderr);

  sdr->converter = config_getdouble(Dictionary,section,"converter",0);
  frontend->calibrate = config_getdouble(Dictionary,section,"calibrate",0);
  // Set this from bandwidth info
  if(frontend->isreal){
    frontend->max_IF = -600000;
    frontend->min_IF = -0.47 * frontend->samprate;
  } else {
    // Complex, symmetrical
    frontend->max_IF = 0.47 * frontend->samprate;
    frontend->min_IF = -frontend->max_IF;
  }

  // Hardware device settings
  bool lna_agc = false;
  bool mixer_agc = false;
  int lna_gain = 0;
  int mixer_gain = 0;
  int vga_gain = 0;

  sdr->linearity = config_getboolean(Dictionary,section,"linearity",false);
  sdr->software_agc = true; // On by default unless one of the hardware AGCs is turned on
  if(info.features & HYDRASDR_CAP_LNA_AGC){
    lna_agc = config_getboolean(Dictionary,section,"lna-agc",false);
    hydrasdr_set_gain(sdr->device, HYDRASDR_GAIN_TYPE_LNA_AGC, (uint8_t)lna_agc);
    if(lna_agc)
      sdr->software_agc = false;
  }
  if(info.features & HYDRASDR_CAP_MIXER_AGC){
    mixer_agc = config_getboolean(Dictionary,section,"mixer-agc",false); // default off
    hydrasdr_set_gain(sdr->device, HYDRASDR_GAIN_TYPE_MIXER_AGC, (uint8_t)mixer_agc);
    if(mixer_agc)
      sdr->software_agc = false;
  }
  if (info.features & HYDRASDR_CAP_LNA_GAIN) {
    lna_gain = config_getint(Dictionary,section,"lna-gain",-1);
    if(lna_gain != -1){
      frontend->lna_gain = lna_gain;
      hydrasdr_set_gain(sdr->device, HYDRASDR_GAIN_TYPE_LNA, (uint8_t)lna_gain);
      sdr->software_agc = false;
    }
  }
  if (info.features & HYDRASDR_CAP_MIXER_GAIN) {
    mixer_gain = config_getint(Dictionary,section,"mixer-gain",-1);
    if(mixer_gain != -1){
      frontend->mixer_gain = mixer_gain;
      hydrasdr_set_gain(sdr->device, HYDRASDR_GAIN_TYPE_MIXER, (uint8_t)mixer_gain);
      sdr->software_agc = false;
    }
  }
  if (info.features & HYDRASDR_CAP_VGA_GAIN) {
    vga_gain = config_getint(Dictionary,section,"vga-gain",-1);
    if(vga_gain != -1){
      frontend->if_gain = vga_gain;
      hydrasdr_set_gain(sdr->device, HYDRASDR_GAIN_TYPE_VGA, (uint8_t)vga_gain);
      sdr->software_agc = false;
    }
  }
  int gainstep = config_getint(Dictionary,section,"gainstep",-1);
  if(gainstep >= 0){
    if(gainstep > sdr->maxgainstep)
      gainstep = sdr->maxgainstep;
    set_gain(sdr,gainstep); // Start AGC with max gain step
  } else if(sdr->software_agc){
    gainstep = sdr->maxgainstep;
    set_gain(sdr,gainstep); // Start AGC with max gain step
  }
  frontend->rf_gain = frontend->lna_gain + frontend->mixer_gain + frontend->if_gain;
  frontend->rf_level_cal = NAN;
  // sign convention flipped to make units dBm/FS, ie, input power in dBm for full scale

  //  frontend->rf_level_cal = config_getdouble(Dictionary,section,"gaincal",-4.8); // I don't think it's actually calibrated
  if (info.features & HYDRASDR_CAP_BIAS_TEE){
    sdr->antenna_bias = config_getboolean(Dictionary,section,"bias",false);
    int ret __attribute__ ((unused));
    ret = hydrasdr_set_rf_bias(sdr->device,sdr->antenna_bias);
    assert(ret == HYDRASDR_SUCCESS);
  }
  {
    char const * const p = config_getstring(Dictionary,section,"description",Description ? Description : "HydraSDR");
    if(p != NULL){
      strlcpy(frontend->description,p,sizeof(frontend->description));
      Description = p;
    }
  }
  fprintf(stderr,"Software AGC %d; linearity %d, LNA AGC %d, Mix AGC %d, LNA gain %d, Mix gain %d, VGA gain %d, gainstep %d, bias tee %d\n",
	  sdr->software_agc,sdr->linearity,lna_agc,mixer_agc,frontend->lna_gain,frontend->mixer_gain,frontend->if_gain,gainstep,sdr->antenna_bias);

  if(sdr->software_agc){
    double const dh = config_getdouble(Dictionary,section,"agc-high-threshold",-10.0);
    sdr->high_threshold = dB2power(-fabs(dh));
    double const dl = config_getdouble(Dictionary,section,"agc-low-threshold",-40.0);
    sdr->low_threshold = dB2power(-fabs(dl));
    fprintf(stderr,"AGC thresholds: high %.1f dBFS, low %.1lf dBFS\n",dh,dl);
  }
  double init_frequency = 0;
  {
    char const *p = config_getstring(Dictionary,section,"frequency",NULL);
    if(p != NULL)
      init_frequency = parse_frequency(p,false);
  }
  if(init_frequency != 0){
    set_correct_freq(sdr,init_frequency);
    frontend->lock = true;
    fprintf(stderr,"Locked tuner frequency %'.3lf Hz\n",init_frequency);
  }
  return 0;
}
int hydrasdr_startup(struct frontend * const frontend){
  struct sdrstate * const sdr = (struct sdrstate *)frontend->context;
  sdr->scale = scale_AD(frontend); // set scaling now that we know the forward FFT size
#if 0
  // This should work, but it doesn't
  // So we set it in the first callback
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
  pthread_attr_setschedpolicy(&attr, SCHED_FIFO);

  struct sched_param param = { 0 };
  param.sched_priority = (sched_get_priority_max(SCHED_FIFO) + sched_get_priority_min(SCHED_FIFO)) / 2; // midway?

  pthread_attr_setschedparam(&attr, &param);
  pthread_create(&sdr->monitor_thread,&attr,hydrasdr_monitor,sdr);
  pthread_attr_destroy(&attr);
#else
  pthread_create(&sdr->monitor_thread,NULL,hydrasdr_monitor,sdr);
#endif

  return 0;
}

static void *hydrasdr_monitor(void *p){
  struct sdrstate * const sdr = (struct sdrstate *)p;
  assert(sdr != NULL);
  pthread_setname("hydrasdr-mon");
  realtime(2 + default_prio()); // Doesn't seem to work
  stick_core();

  int ret = hydrasdr_start_rx(sdr->device,rx_callback,sdr);
  (void)ret;
  assert(ret == HYDRASDR_SUCCESS);
  fprintf(stderr,"hydrasdr running\n");
#if 0
  {
    hydrasdr_device_info_t info = {0};
    hydrasdr_get_device_info(sdr->device, &info);

  // Current streaming configuration
  fprintf(stderr,"Effective rate: %u Hz\n", info.current_samplerate);
  fprintf(stderr,"Hardware rate:  %u Hz\n", info.current_hw_samplerate);
  fprintf(stderr,"Decimation:     %ux (%s mode)\n",
	 info.current_decimation_factor,
	 info.current_decimation_mode ? "High Definition" : "Low Bandwidth");
  fprintf(stderr,"Bandwidth:      %u Hz (%s)\n",
	 info.current_bandwidth,
	 info.bandwidth_auto_selected ? "auto" : "manual");
  fprintf(stderr,"Sample type:    %u\n", info.current_sample_type);
  fprintf(stderr,"Packing:        %s\n", info.current_packing ? "12-bit" : "16-bit");
  }
#endif
  // Periodically poll status to ensure device hasn't reset
  while(true){
    sleep(1);
    if(!hydrasdr_is_streaming(sdr->device))
      break; // Device seems to have bombed. Exit and let systemd restart us
  }
  fprintf(stderr,"Device is no longer streaming, exiting\n");
  hydrasdr_close(sdr->device);
  exit(EX_NOINPUT); // Let systemd restart us
}


static bool Name_set = false;
// Callback called with incoming receiver data from A/D
static int rx_callback(hydrasdr_transfer *transfer){
  assert(transfer != NULL);
  struct sdrstate * const sdr = (struct sdrstate *)transfer->ctx;
  assert(sdr != NULL);
  struct frontend * const frontend = sdr->frontend;
  assert(frontend != NULL);

  // Shouldn't have to set real-time priority in the callback, but it doesn't
  // seem to get inherited by pthread_create() despite the documentation
  // At least this works
  if(!Name_set){
    pthread_setname("hydrasdr-cb");
    Name_set = true;
    realtime(2 + default_prio());
  }
  if(transfer->dropped_samples){
    fprintf(stderr,"dropped %'lld\n",(long long)transfer->dropped_samples);
  }
  int const sampcount = transfer->sample_count;
  double in_energy = 0;
  switch(sdr->sample_type){
  case HYDRASDR_SAMPLE_RAW:
    {
      // Libhydrasdr could do this for us, but this minimizes mem copies
      // This could probably be vectorized someday
      uint32_t const * restrict up = (uint32_t *)transfer->samples;
      float * restrict wptr = frontend->in.input_write_pointer.r;
      for(int i=0; i < sampcount; i+= 8){ // assumes multiple of 8
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
	  if(x == 2047 || x <= -2047){
	    frontend->overranges++;
	    frontend->samp_since_over = 0;
	  } else {
	    frontend->samp_since_over++;
	  }
	  wptr[j] = (float)(sdr->scale * x);
	  in_energy += (double)x * x;
	}
	wptr += 8;
	up += 3;
      }
    }
    break;
  case HYDRASDR_SAMPLE_UINT16_REAL:
    {
      // Not scaled from A/D width, which we're not given, so this isn't really a portable mode
      uint16_t const * restrict up = (uint16_t *)transfer->samples;
      float * restrict wptr = frontend->in.input_write_pointer.r;
      for(int i=0; i < sampcount; i++){
	int x = *up++; x -= 2048;
	if(x >= 2047 || x <= -2048){
	  frontend->overranges++;
	  frontend->samp_since_over = 0;
	} else {
	  frontend->samp_since_over++;
	}
	*wptr++ = sdr->scale * x;
	in_energy += (double)x * x;
      }
    }
    break;
  case HYDRASDR_SAMPLE_INT16_REAL:
    {
      int16_t const * restrict up = (int16_t *)transfer->samples;
      float * restrict wptr = frontend->in.input_write_pointer.r;
      for(int i=0; i < sampcount; i++){
	int const x = *up++;
	if(x >= 32767 || x <= -32768){
	  frontend->overranges++;
	  frontend->samp_since_over = 0;
	} else {
	  frontend->samp_since_over++;
	}
	*wptr++ = sdr->scale * x;
	in_energy += (double)x * x;
      }
    }
    break;
  case HYDRASDR_SAMPLE_FLOAT32_REAL:
    {
      // Could be a memcpy except we need energy
      float const * restrict up = (float *)transfer->samples;
      float * restrict wptr = frontend->in.input_write_pointer.r;
      for(int i=0; i < sampcount; i++){
	float const x = *up++;
	*wptr++ = sdr->scale * x;
	in_energy += (double)x * x;
      }
    }
    break;
  case HYDRASDR_SAMPLE_INT16_IQ:
    {
      int16_t const * restrict up = (int16_t *)transfer->samples;
      float complex * restrict wptr = frontend->in.input_write_pointer.c;
      for(int i=0; i < sampcount; i++){
	int const x = *up++;
	int const y = *up++;
	if(x >= 32767 || x <= -32768 || y >= 32767 || y <= -32768){
	  frontend->overranges++;
	  frontend->samp_since_over = 0;
	} else {
	  frontend->samp_since_over++;
	}
	double complex s = CMPLX((double)x,(double)y);
	*wptr++ = (float complex)(sdr->scale * s);
	in_energy += s * s;
      }
    }
    break;
  case HYDRASDR_SAMPLE_FLOAT32_IQ:
    {
      float complex const * restrict up = (float complex *)transfer->samples;
      float complex * restrict wptr = frontend->in.input_write_pointer.c;
      for(int i=0; i < sampcount; i++){
	double complex s = *up++;
	*wptr++ = (float complex)(sdr->scale * s);
	in_energy += s * s;
      }
    }
    break;
  case HYDRASDR_SAMPLE_UINT8_REAL:
    {
      uint8_t const * restrict up = (uint8_t *)transfer->samples;
      float * restrict wptr = frontend->in.input_write_pointer.r;
      for(int i=0; i < sampcount; i++){
	int x = *up++; x -= 128;
	if(x >= 127 || x <= -128){
	  frontend->overranges++;
	  frontend->samp_since_over = 0;
	} else {
	  frontend->samp_since_over++;
	}
	*wptr++ = sdr->scale * x;
	in_energy += (double)x * x;
      }
    }
    break;
  case HYDRASDR_SAMPLE_INT8_REAL:
    {
      int8_t const * restrict up = (int8_t *)transfer->samples;
      float * restrict wptr = frontend->in.input_write_pointer.r;
      for(int i=0; i < sampcount; i++){
	int const x = *up++;
	if(x >= 127 || x <= -128){
	  frontend->overranges++;
	  frontend->samp_since_over = 0;
	} else {
	  frontend->samp_since_over++;
	}
	*wptr++ = sdr->scale * x;
	in_energy += (double)x * x;
      }
    }
    break;
  case HYDRASDR_SAMPLE_UINT8_IQ:
    {
      uint8_t * restrict up = (uint8_t *)transfer->samples;
      float complex * restrict wptr = frontend->in.input_write_pointer.c;
      for(int i=0; i < sampcount; i++){
	int x = *up++; x -= 128;
	int y = *up++; y -= 128;
	if(x >= 127 || x <= -128 || y >= 127 || y <= -128){
	  frontend->overranges++;
	  frontend->samp_since_over = 0;
	} else {
	  frontend->samp_since_over++;
	}
	double complex s = CMPLX((double)x,(double)y);
	*wptr++ = sdr->scale * s;
	in_energy += s * s;
      }
    }
    break;
  case HYDRASDR_SAMPLE_INT8_IQ:
    {
      int8_t * restrict up = (int8_t *)transfer->samples;
      float complex * restrict wptr = frontend->in.input_write_pointer.c;
      for(int i=0; i < sampcount; i++){
	int const x = *up++;
	int const y = *up++;
	if(x >= 127 || x <= -128 || y >= 127 || y <= -128){
	  frontend->overranges++;
	  frontend->samp_since_over = 0;
	} else {
	  frontend->samp_since_over++;
	}
	double complex s = CMPLX((double)x,(double)y);
	*wptr++ = sdr->scale * s;
	in_energy += s * s;
      }
    }
    break;
  default:
    return -1; // Unsupported?
  }
  frontend->samples += sampcount;
  write_rfilter(&frontend->in,NULL,sampcount); // Update write pointer, invoke FFT
  frontend->if_power += Power_alpha * (in_energy / sampcount - frontend->if_power);
  if(sdr->software_agc){
    // Integrate A/D energy over A/D averaging period
    sdr->agc_energy += in_energy;
    sdr->agc_samples += sampcount;
    if(sdr->agc_samples >= frontend->samprate/10){ // Time to re-evaluate after 100 ms
      double avg_agc_power = scale_ADpower2FS(frontend) * sdr->agc_energy / sdr->agc_samples;
      if(avg_agc_power < sdr->low_threshold){
	if(Verbose)
	  fprintf(stderr,"AGC power %.1f dBFS\n",power2dB(avg_agc_power));
	set_gain(sdr,sdr->gainstep + 1);
      } else if(avg_agc_power > sdr->high_threshold){
	if(Verbose)
	  fprintf(stderr,"AGC power %.1f dBFS\n",power2dB(avg_agc_power));
	set_gain(sdr,sdr->gainstep - 1);
      }
      // Reset integrator
      sdr->agc_energy = 0;
      sdr->agc_samples = 0;
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
    uint64_t const vco = freq_hz << (div_num + 1);
    if (VCO_MIN <= vco && vco <= VCO_MAX)
      break;
  }
  if(div_num > MAX_DIV)
    return 0; // Frequency out of range

  // r = PLL programming bits: Nint in upper 16 bits, Nfract in lower 16 bits
  // Freq steps are pll_ref / 2^(16 + div_num) Hz
  // Note the '+ (pll_ref >> 1)' term simply rounds the division to the nearest integer
  uint64_t const r = ((freq_hz << (div_num + 16)) + (pll_ref >> 1)) / pll_ref;

  // This is a puzzle; is it related to spur suppression?
  double const offset = 0.25;
  // Compute true frequency
  return (((double)r + offset) * pll_ref) / (double)(1 << (div_num + 16));
}

// set the HydraSDR tuner to the requested frequency, applying:
// Spyverter converter offset (120 MHz, or 0 if not in use)
// TCXO calibration offset
// Fs/4 = 5 MHz offset (firmware assumes library real->complex conversion, which we don't use)
// Apply 820T synthesizer tuning step model

// the TCXO calibration offset is a holdover from the Funcube dongle and doesn't
// really fit the HydraSDR with its internal factory calibration
// All this really works correctly only with a gpsdo, forcing the calibration offset to 0
static double set_correct_freq(struct sdrstate * const sdr,double const freq){
  struct frontend * const frontend = sdr->frontend;
  // sdr->converter refers to an upconverter, so it's added to the frequency we request
  int64_t const intfreq = lrint((freq + sdr->converter)/ (1 + frontend->calibrate));
  int ret __attribute__((unused)) = HYDRASDR_SUCCESS; // Won't be used when asserts are disabled
  ret = hydrasdr_set_freq(sdr->device,(uint64_t)(intfreq - sdr->offset));
  assert(ret == HYDRASDR_SUCCESS);
  double const tf = true_freq(intfreq);
  frontend->frequency = tf * (1 + frontend->calibrate) - sdr->converter;
  return frontend->frequency;
}
double hydrasdr_tune(struct frontend * const frontend,double const f){
  if(frontend->lock)
    return frontend->frequency;
  struct sdrstate * const sdr = frontend->context;
  return set_correct_freq(sdr,f);
}


static void set_gain(struct sdrstate * const sdr,int gainstep){
  struct frontend * const frontend = sdr->frontend;
  if(gainstep < sdr->mingainstep)
    gainstep = sdr->mingainstep;
  else if(gainstep > sdr->maxgainstep)
    gainstep = sdr->maxgainstep;

  if(gainstep != sdr->gainstep){
    sdr->gainstep = gainstep;
    int ret __attribute__((unused)) = HYDRASDR_SUCCESS; // Won't be used when asserts are disabled
    ret = hydrasdr_set_gain(sdr->device,
			    sdr->linearity ? HYDRASDR_GAIN_TYPE_LINEARITY : HYDRASDR_GAIN_TYPE_SENSITIVITY,
			    (uint8_t)sdr->gainstep);
    assert(ret == HYDRASDR_SUCCESS);
    hydrasdr_gain_info_t info = {0};
    ret = hydrasdr_get_gain(sdr->device, HYDRASDR_GAIN_TYPE_LNA, &info);
    if(ret == HYDRASDR_SUCCESS)
      frontend->lna_gain = info.value;
    ret = hydrasdr_get_gain(sdr->device, HYDRASDR_GAIN_TYPE_MIXER, &info);
    if(ret == HYDRASDR_SUCCESS)
      frontend->mixer_gain = info.value;
    ret = hydrasdr_get_gain(sdr->device, HYDRASDR_GAIN_TYPE_VGA, &info);
    if(ret == HYDRASDR_SUCCESS)
      frontend->if_gain = info.value;

    frontend->rf_gain = frontend->lna_gain + frontend->mixer_gain + frontend->if_gain;
    sdr->scale = scale_AD(frontend);
    if(Verbose)
      fprintf(stderr,"New gainstep %d: LNA = %d, mixer = %d, vga = %d\n",gainstep,
	     frontend->lna_gain,frontend->mixer_gain,frontend->if_gain);
  }
}
