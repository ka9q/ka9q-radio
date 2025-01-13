// Front end driver for Airspy HF+ linked into ka9q-radio's radiod
// Copyright 2023, Phil Karn, KA9Q

#undef DEBUG_AGC
#define _GNU_SOURCE 1
#include <assert.h>
#include <pthread.h>
#include <libairspyhf/airspyhf.h>
#include <errno.h>
#include <iniparser/iniparser.h>
#if defined(linux)
#include <bsd/string.h>
#endif
#include <sysexits.h>
#include <unistd.h>
#include <strings.h>

#include "conf.h"
#include "misc.h"
#include "multicast.h"
#include "status.h"
#include "radio.h"
#include "config.h"

// Global variables set by config file options
extern int Verbose;
extern const char *App_path;

static float Power_smooth = 0.05; // Calculate this properly someday

static char const *Airspyhf_keys[] = {
  "device",
  "serial",
  "samprate",
  "calibrate",
  "hf-agc",
  "agc-thresh",
  "hf-att",
  "hf-lna",
  "lib-dsp",
  "description",
  "frequency",
  NULL
};

// Anything generic should be in 'struct frontend' section 'sdr' in radio.h
struct sdrstate {
  struct frontend *frontend;  // Avoid references to external globals
  struct airspyhf_device *device;    // Opaque pointer

  uint32_t sample_rates[20];
  uint64_t SN; // Serial number

  pthread_t cmd_thread;
  pthread_t monitor_thread;
};

static double set_correct_freq(struct sdrstate *sdr,double freq);
static int rx_callback(airspyhf_transfer_t *transfer);
static void *airspyhf_monitor(void *p);
static double true_freq(uint64_t freq);

int airspyhf_setup(struct frontend * const frontend,dictionary * const Dictionary,char const * const section){
  assert(Dictionary != NULL);

  struct sdrstate * const sdr = calloc(1,sizeof(struct sdrstate));
  // Cross-link generic and hardware-specific control structures
  sdr->frontend = frontend;
  frontend->context = sdr;
  {
    char const *device = config_getstring(Dictionary,section,"device",NULL);
    if(strcasecmp(device,"airspyhf") != 0)
      return -1; // Not for us
  }
  config_validate_section(stdout,Dictionary,section,Airspyhf_keys,NULL);
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

      n_serials = airspyhf_list_devices(serials,n_serials); // Return actual number
      if(n_serials == 0){
	fprintf(stdout,"No airspyhf devices found\n");
	return -1;
      }
      fprintf(stdout,"Discovered airspyhf device serials:");
      for(int i = 0; i < n_serials; i++){
	fprintf(stdout," %llx",(long long)serials[i]);
      }
      fprintf(stdout,"\n");
      fprintf(stdout,"Selecting %llx; to select another, add 'serial = ' to config file\n",(long long)serials[0]);
      sdr->SN = serials[0];
    }
  }
  {
    int ret = airspyhf_open_sn(&sdr->device,sdr->SN);
    if(ret != AIRSPYHF_SUCCESS){
      fprintf(stdout,"airspyhf_open(%llx) failed\n",(long long)sdr->SN);
      return -1;
    } 
  }
  {
    airspyhf_lib_version_t version;
    airspyhf_lib_version(&version);

    const int VERSION_LOCAL_SIZE = 128; // Library doesn't define, but says should be >= 128
    char hw_version[VERSION_LOCAL_SIZE];
    airspyhf_version_string_read(sdr->device,hw_version,sizeof(hw_version));

    fprintf(stdout,"Airspyhf serial %llx, hw version %s, library version %d.%d.%d\n",
	    (long long unsigned)sdr->SN,
	    hw_version,
	    version.major_version,version.minor_version,version.revision);
  }
  // Initialize hardware first
  {
    // Get and list sample rates
    int ret __attribute__ ((unused));
    ret = airspyhf_get_samplerates(sdr->device,sdr->sample_rates,0);
    assert(ret == AIRSPYHF_SUCCESS);
    int const number_sample_rates = sdr->sample_rates[0];
    fprintf(stdout,"%'d sample rates:",number_sample_rates);
    if(number_sample_rates == 0){
      fprintf(stdout,"error, no valid sample rates!\n");
      return -1;
    }
    ret = airspyhf_get_samplerates(sdr->device,sdr->sample_rates,number_sample_rates);
    assert(ret == AIRSPYHF_SUCCESS);
    for(int n = 0; n < number_sample_rates; n++){
      fprintf(stdout," %'d",sdr->sample_rates[n]);
      if(sdr->sample_rates[n] < 1)
	break;
    }
    fprintf(stdout,"\n");
  }
  // Default to first (highest) sample rate on list
  frontend->samprate = config_getint(Dictionary,section,"samprate",sdr->sample_rates[0]);
  {
    char const *p = config_getstring(Dictionary,section,"samprate",NULL);
    if(p != NULL)
      frontend->samprate = parse_frequency(p,false);
  }

  frontend->isreal = false;
  frontend->bitspersample = 1; // Causes gain scaling by unity
  frontend->calibrate = config_getdouble(Dictionary,section,"calibrate",0);

  fprintf(stdout,"Set sample rate %'u Hz\n",frontend->samprate);
  {
    int ret __attribute__ ((unused));
    ret = airspyhf_set_samplerate(sdr->device,(uint32_t)frontend->samprate);
    assert(ret == AIRSPYHF_SUCCESS);
  }
  frontend->min_IF = -0.43 * frontend->samprate;
  frontend->max_IF = +0.43 * frontend->samprate;

  {
    int const hf_agc = config_getboolean(Dictionary,section,"hf-agc",false); // default off
    airspyhf_set_hf_agc(sdr->device,hf_agc);

    int const agc_thresh = config_getboolean(Dictionary,section,"agc-thresh",false); // default off
    airspyhf_set_hf_agc_threshold(sdr->device,agc_thresh);

    int const hf_att = config_getboolean(Dictionary,section,"hf-att",false); // default off
    airspyhf_set_hf_att(sdr->device,hf_att);

    int const hf_lna = config_getboolean(Dictionary,section,"hf-lna",false); // default off
    airspyhf_set_hf_lna(sdr->device,hf_lna);

    int const lib_dsp = config_getboolean(Dictionary,section,"lib-dsp",true); // default on
    airspyhf_set_lib_dsp(sdr->device,lib_dsp);

    fprintf(stdout,"HF AGC %d, AGC thresh %d, hf att %d, hf-lna %d, lib-dsp %d\n",
	    hf_agc,agc_thresh,hf_att,hf_lna,lib_dsp);
  }
  {
    char const *p = config_getstring(Dictionary,section,"description",NULL);
    if(p != NULL){
      FREE(frontend->description);
      frontend->description = strdup(p);
      fprintf(stdout,"%s: ",frontend->description);
    }
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
    fprintf(stdout,"Locked tuner frequency %'.3lf Hz\n",init_frequency);
  }
  return 0;
}
int airspyhf_startup(struct frontend *frontend){
  struct sdrstate *sdr = (struct sdrstate *)frontend->context;
  pthread_create(&sdr->monitor_thread,NULL,airspyhf_monitor,sdr);
  return 0;
}

static void *airspyhf_monitor(void *p){
  struct sdrstate *sdr = (struct sdrstate *)p;
  assert(sdr != NULL);
  pthread_setname("airspyhf-mon");

  realtime();
  int ret __attribute__ ((unused));
  ret = airspyhf_start(sdr->device,rx_callback,sdr);
  assert(ret == AIRSPYHF_SUCCESS);
  fprintf(stdout,"airspyhf running\n");
  // Periodically poll status to ensure device hasn't reset
  while(true){
    sleep(1);
    if(!airspyhf_is_streaming(sdr->device))
      break; // Device seems to have bombed. Exit and let systemd restart us
  }
  fprintf(stdout,"Device is no longer streaming, exiting\n");
  // This can hang when the device locks up
  // This has been happening at KQ6RS
  //  airspyhf_close(sdr->device); 
  exit(EX_NOINPUT); // Let systemd restart us
}

static bool Name_set = false;
// Callback called with incoming receiver data from A/D
static int rx_callback(airspyhf_transfer_t *transfer){
  assert(transfer != NULL);
  struct sdrstate * const sdr = (struct sdrstate *)transfer->ctx;
  assert(sdr != NULL);
  struct frontend * const frontend = sdr->frontend;
  assert(frontend != NULL);

  if(!Name_set){
    pthread_setname("airspyhf-cb");
    Name_set = true;
  }
  if(transfer->dropped_samples){
    fprintf(stdout,"dropped %'lld\n",(long long)transfer->dropped_samples);
  }
  int const sampcount = transfer->sample_count;
  float complex * const wptr = frontend->in.input_write_pointer.c;
  float complex const * const up = (float complex *)transfer->samples;
  assert(wptr != NULL);
  assert(up != NULL);
  float in_energy = 0;
  for(int i=0; i < sampcount; i++){
    in_energy += cnrmf(up[i]);
    wptr[i] = up[i];
  }
  frontend->samples += sampcount;
  frontend->timestamp = gps_time_ns();
  write_cfilter(&frontend->in,NULL,sampcount); // Update write pointer, invoke FFT
  if(isfinite(in_energy)){
    frontend->if_power_instant = in_energy / sampcount;
    frontend->if_power += Power_smooth * (frontend->if_power_instant - frontend->if_power);
  }
  return 0;
}

static double true_freq(uint64_t freq_hz){
  return (double)freq_hz; // Placeholder; the library seems to do frequency correction
}

// set the airspyhf tuner to the requested frequency, applying:
// TCXO calibration offset
// the TCXO calibration offset is a holdover from the Funcube dongle and doesn't
// really fit the Airspyhf with its internal factory calibration
// All this really works correctly only with a gpsdo, forcing the calibration offset to 0
static double set_correct_freq(struct sdrstate *sdr,double freq){
  struct frontend *frontend = sdr->frontend;
  int64_t intfreq = round((freq)/ (1 + frontend->calibrate));
  int ret __attribute__((unused)) = AIRSPYHF_SUCCESS; // Won't be used when asserts are disabled
  ret = airspyhf_set_freq(sdr->device,intfreq);
  assert(ret == AIRSPYHF_SUCCESS);
  double const tf = true_freq(intfreq);
  frontend->frequency = tf * (1 + frontend->calibrate);
  return frontend->frequency;
}
double airspyhf_tune(struct frontend *frontend,double f){
  struct sdrstate *sdr = frontend->context;
  if(frontend->lock)
    return frontend->frequency; // Don't change it
  return set_correct_freq(sdr,f);
}
