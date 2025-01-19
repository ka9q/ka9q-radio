// Read from SDRplay SDR using SDRplay API version 3.x
// Accept control commands from UDP socket
// Written by K4VZ August 2023, adapted from existing KA9Q SDR handler programs
// Version linked into radiod
#define _GNU_SOURCE 1
#include <assert.h>
#include <pthread.h>
#include <sdrplay_api.h>
#include <errno.h>
#include <unistd.h>
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
extern int Overlap;
extern const char *App_path;
extern int Verbose;

static float Power_smooth = 0.05; // Calculate this properly someday

// SDRplay device status
enum sdrplay_status {
  NOT_INITIALIZED = 0,
  SDRPLAY_API_OPEN = 1,
  DEVICE_API_LOCKED = 2,
  DEVICE_SELECTED = 4,
  DEVICE_STREAMING = 8
};

// Anything generic should be in 'struct frontend' section 'sdr' in radio.h
struct sdrstate {
  struct frontend *frontend;  // Avoid references to external globals
  sdrplay_api_DeviceT device;
  sdrplay_api_DeviceParamsT *device_params;
  sdrplay_api_RxChannelParamsT *rx_channel_params;
  float scale;
  enum sdrplay_status device_status;

  // Statistics and other auxiliary data
  uint64_t events;
  unsigned int next_sample_num;

  pthread_t cmd_thread;
  pthread_t monitor_thread;
};

// SDRplay specific constants, data structures, and functions
static const sdrplay_api_DbgLvl_t dbgLvl = sdrplay_api_DbgLvl_Disable;
//static const sdrplay_api_DbgLvl_t dbgLvl = sdrplay_api_DbgLvl_Verbose;

static const double MIN_SAMPLE_RATE = 2e6;
static const double MAX_SAMPLE_RATE = 10.66e6;
static const int MAX_DECIMATION = 32;

// Taken from SDRplay API Specification Guide (Gain Reduction Tables)
uint8_t const rsp1_0_420_lna_states[] = { 0, 24, 19, 43 };
uint8_t const rsp1_420_1000_lna_states[] = { 0, 7, 19, 26 };
uint8_t const rsp1_1000_2000_lna_states[] = { 0, 5, 19, 24 };

uint8_t const rsp1a_0_60_lna_states[] = { 0, 6, 12, 18, 37, 42, 61 };
uint8_t const rsp1a_60_420_lna_states[] = { 0, 6, 12, 18, 20, 26, 32, 38, 57, 62 };
uint8_t const rsp1a_420_1000_lna_states[] = { 0, 7, 13, 19, 20, 27, 33, 39, 45, 64 };
uint8_t const rsp1a_1000_2000_lna_states[] = { 0, 6, 12, 20, 26, 32, 38, 43, 62 };

uint8_t const rsp1b_0_50_lna_states[] = { 0, 6, 12, 18, 37, 42, 61 };
uint8_t const rsp1b_50_60_lna_states[] = { 0, 6, 12, 18, 20, 26, 32, 38, 57, 62 };
uint8_t const rsp1b_60_420_lna_states[] = { 0, 6, 12, 18, 20, 26, 32, 38, 57, 62 };
uint8_t const rsp1b_420_1000_lna_states[] = { 0, 7, 13, 19, 20, 27, 33, 39, 45, 64 };
uint8_t const rsp1b_1000_2000_lna_states[] = { 0, 6, 12, 20, 26, 32, 38, 43, 62 };

uint8_t const rsp2_0_420_lna_states[] = { 0, 10, 15, 21, 24, 34, 39, 45, 64 };
uint8_t const rsp2_420_1000_lna_states[] = { 0, 7, 10, 17, 22, 41 };
uint8_t const rsp2_1000_2000_lna_states[] = { 0, 5, 21, 15, 15, 34 };
uint8_t const rsp2_0_60_hiz_lna_states[] = { 0, 6, 12, 18, 37 };

uint8_t const rspduo_0_60_lna_states[] = { 0, 6, 12, 18, 37, 42, 61 };
uint8_t const rspduo_60_420_lna_states[] = { 0, 6, 12, 18, 20, 26, 32, 38, 57, 62 };
uint8_t const rspduo_420_1000_lna_states[] = { 0, 7, 13, 19, 20, 27, 33, 39, 45, 64 };
uint8_t const rspduo_1000_2000_lna_states[] = { 0, 6, 12, 20, 26, 32, 38, 43, 62 };
uint8_t const rspduo_0_60_hiz_lna_states[] = { 0, 6, 12, 18, 37 };

uint8_t const rspdx_0_2_hdr_lna_states[] = { 0, 3, 6, 9, 12, 15, 18, 21, 24, 25, 27, 30, 33, 36, 39, 42, 45, 48, 51, 54, 57, 60 };
uint8_t const rspdx_0_12_lna_states[] = { 0, 3, 6, 9, 12, 15, 24, 27, 30, 33, 36, 39, 42, 45, 48, 51, 54, 57, 60 };
uint8_t const rspdx_12_50_lna_states[] = { 0, 3, 6, 9, 12, 15, 18, 24, 27, 30, 33, 36, 39, 42, 45, 48, 51, 54, 57, 60 };
uint8_t const rspdx_50_60_lna_states[] = { 0, 3, 6, 9, 12, 20, 23, 26, 29, 32, 35, 38, 44, 47, 50, 53, 56, 59, 62, 65, 68, 71, 74, 77, 80 };
uint8_t const rspdx_60_250_lna_states[] = { 0, 3, 6, 9, 12, 15, 24, 27, 30, 33, 36, 39, 42, 45, 48, 51, 54, 57, 60, 63, 66, 69, 72, 75, 78, 81, 84 };
uint8_t const rspdx_250_420_lna_states[] = { 0, 3, 6, 9, 12, 15, 18, 24, 27, 30, 33, 36, 39, 42, 45, 48, 51, 54, 57, 60, 63, 66, 69, 72, 75, 78, 81, 84 };
uint8_t const rspdx_420_1000_lna_states[] = { 0, 7, 10, 13, 16, 19, 22, 25, 31, 34, 37, 40, 43, 46, 49, 52, 55, 58, 61, 64, 67 };
uint8_t const rspdx_1000_2000_lna_states[] = { 0, 5, 8, 11, 14, 17, 20, 32, 35, 38, 41, 44, 47, 50, 53, 56, 59, 62, 65 };

uint8_t const rspdxr2_0_2_hdr_lna_states[] = { 0, 3, 6, 9, 12, 15, 18, 21, 24, 25, 27, 30, 33, 36, 39, 42, 45, 48, 51, 54, 57, 60 };
uint8_t const rspdxr2_0_12_lna_states[] = { 0, 3, 6, 9, 12, 15, 24, 27, 30, 33, 36, 39, 42, 45, 48, 51, 54, 57, 60 };
uint8_t const rspdxr2_12_50_lna_states[] = { 0, 3, 6, 9, 12, 15, 18, 24, 27, 30, 33, 36, 39, 42, 45, 48, 51, 54, 57, 60 };
uint8_t const rspdxr2_50_60_lna_states[] = { 0, 3, 6, 9, 12, 20, 23, 26, 29, 32, 35, 38, 44, 47, 50, 53, 56, 59, 62, 65, 68, 71, 74, 77, 80 };
uint8_t const rspdxr2_60_250_lna_states[] = { 0, 3, 6, 9, 12, 15, 24, 27, 30, 33, 36, 39, 42, 45, 48, 51, 54, 57, 60, 63, 66, 69, 72, 75, 78, 81, 84 };
uint8_t const rspdxr2_250_420_lna_states[] = { 0, 3, 6, 9, 12, 15, 18, 24, 27, 30, 33, 36, 39, 42, 45, 48, 51, 54, 57, 60, 63, 66, 69, 72, 75, 78, 81, 84 };
uint8_t const rspdxr2_420_1000_lna_states[] = { 0, 7, 10, 13, 16, 19, 22, 25, 31, 34, 37, 40, 43, 46, 49, 52, 55, 58, 61, 64, 67 };
uint8_t const rspdxr2_1000_2000_lna_states[] = { 0, 5, 8, 11, 14, 17, 20, 32, 35, 38, 41, 44, 47, 50, 53, 56, 59, 62, 65 };


// SDRplay specific functions
static int open_sdrplay(struct sdrstate *sdr);
static int close_sdrplay(struct sdrstate *sdr);
static int find_rsp(struct sdrstate *sdr,char const *sn);
static int set_rspduo_mode(struct sdrstate *sdr,char const *mode,char const *antenna);
static int select_device(struct sdrstate *sdr);
static double set_center_freq(struct sdrstate *sdr,double const frequency);
static int set_ifreq(struct sdrstate *sdr,int const ifreq);
static int set_bandwidth(struct sdrstate *sdr,int const bandwidth,double const samprate);
static int set_samplerate(struct sdrstate *sdr,double const samprate);
static double get_samplerate(struct sdrstate *sdr);
static int set_antenna(struct sdrstate *sdr,char const *antenna);
static uint8_t const * get_lna_states(struct sdrstate *sdr,double const frequency,int *lna_state_count);
static int set_rf_gain(struct sdrstate *sdr,int const lna_state,int const rf_att,int const rf_gr,double const frequency);
static float get_rf_atten(struct sdrstate *sdr,double const frequency);
static int set_if_gain(struct sdrstate *sdr,int const if_att,int const if_gr,int const if_agc,int const if_agc_rate,int const if_agc_setPoint_dBfs,int const if_agc_attack_ms,int const if_agc_decay_ms,int const if_agc_decay_delay_ms,int const if_agc_decay_threshold_dB);
static int set_dc_offset_iq_imbalance_correction(struct sdrstate *sdr,int const dc_offset_corr,int const iq_imbalance_corr);
static int set_bulk_transfer_mode(struct sdrstate *sdr,int const transfer_mode_bulk);
static int set_notch_filters(struct sdrstate *sdr,bool const rf_notch,bool const dab_notch,bool const am_notch);
static void *sdrplay_monitor(void *p);
static int set_biasT(struct sdrstate *sdr,bool const biasT);
static int start_rx(struct sdrstate *sdr,sdrplay_api_StreamCallback_t rx_callback,sdrplay_api_EventCallback_t event_callback);
static void rx_callback(int16_t *xi,int16_t *xq,sdrplay_api_StreamCbParamsT *params,unsigned int numSamples,unsigned int reset,void *cbContext);
static void event_callback(sdrplay_api_EventT eventId,sdrplay_api_TunerSelectT tuner,sdrplay_api_EventParamsT *params,void *cbContext);
static void show_device_params(struct sdrstate *sdr);

static char const *Sdrplay_keys[] = {
  "device",
  "library",
  "serial",
  "frequency",
  "rspduo-mode",
  "antenna",
  "bandwidth",
  "samprate",
  "calibrate",
  "lna-state",
  "rf-att",
  "rf-gr",
  "if-att",
  "if-gr",
  "if-att",
  "if-agc",
  "if-agc-rate",
  "if-agc-setpoint-dbfs",
  "if-agc-attack-ms",
  "if-agc-decay-ms",
  "if-agc-decay-delay-ms",
  "if-agc-decay-threshold-db",
  "dc-offset-corr",
  "iq-imbalance-corr",
  "bulk-transfer-mode",
  "rf-notch",
  "dab-notch",
  "am-notch",
  "bias-t",
  "description",
  NULL
};


int sdrplay_setup(struct frontend * const frontend,dictionary * const Dictionary,char const * const section){
  assert(Dictionary != NULL);

  struct sdrstate * const sdr = calloc(1,sizeof(struct sdrstate));
  // Cross-link generic and hardware-specific control structures
  sdr->frontend = frontend;
  frontend->context = sdr;
  {
    char const *device = config_getstring(Dictionary,section,"device",section);
    if(strcasecmp(device,"sdrplay") != 0)
      return -1; // Not for us
  }
  {
    if((open_sdrplay(sdr)) != 0){
      fprintf(stdout,"open_sdrplay() failed\n");
      close_sdrplay(sdr);
      return -1;
    }
  }
  config_validate_section(stdout,Dictionary,section,Sdrplay_keys,NULL);
  {
    char const * const sn = config_getstring(Dictionary,section,"serial",NULL);
    if(find_rsp(sdr,sn) == -1){
      close_sdrplay(sdr);
      return -1;
    }
  }
  if(sdr->device.hwVer == SDRPLAY_RSPduo_ID){
    char const * const mode = config_getstring(Dictionary,section,"rspduo-mode",NULL);
    char const * const antenna = config_getstring(Dictionary,section,"antenna",NULL);
    if(set_rspduo_mode(sdr,mode,antenna) == -1){
      close_sdrplay(sdr);
      return -1;
    }
  }
  if(select_device(sdr) == -1){
    close_sdrplay(sdr);
    return -1;
  }
  fprintf(stdout,"SDRplay RSP serial %s, hw model %d, API version %.2f\n",
          sdr->device.SerNo,
          sdr->device.hwVer,
          SDRPLAY_API_VERSION);

  // Initialize hardware first
  int const ifreq = config_getint(Dictionary,section,"ifreq",-1);
  if(set_ifreq(sdr,ifreq) == -1){
    close_sdrplay(sdr);
    return -1;
  }

  // Default sample rate to 2Msps
  int const bandwidth = config_getint(Dictionary,section,"bandwidth",-1);
  double const samprate = config_getdouble(Dictionary,section,"samprate",MIN_SAMPLE_RATE);
  if(set_bandwidth(sdr,bandwidth,samprate) == -1){
    close_sdrplay(sdr);
    return -1;
  }

  fprintf(stdout,"Set sample rate %'f Hz\n",samprate);
  if(set_samplerate(sdr,samprate) == -1){
    close_sdrplay(sdr);
    return -1;
  }
  frontend->samprate = get_samplerate(sdr);
  frontend->isreal = false;
  frontend->bitspersample = 16;
  frontend->calibrate = config_getdouble(Dictionary,section,"calibrate",0);
  frontend->min_IF = -0.46 * frontend->samprate;
  frontend->max_IF = +0.46 * frontend->samprate;

  // Need to know the initial frequency beforehand because of RF att/LNA state
  double init_frequency = 0;
  {
    char const *p = config_getstring(Dictionary,section,"frequency",NULL);
    if(p != NULL)
      init_frequency = parse_frequency(p,false);
  }
  // Hardware device settings
  {
    char const *antenna = config_getstring(Dictionary,section,"antenna",NULL);
    if(set_antenna(sdr,antenna) == -1){
      close_sdrplay(sdr);
      return -1;
    }

    int const lna_state = config_getint(Dictionary,section,"lna-state",-1);
    int const rf_att = config_getint(Dictionary,section,"rf-att",-1);
    int const rf_gr = config_getint(Dictionary,section,"rf-gr",-1);
    // if no init frequency, use a default frequency of 200MHz to set RF gains
    double const rfgr_frequency = init_frequency > 0 ? init_frequency : 200e6;
    if(set_rf_gain(sdr,lna_state,rf_att,rf_gr,rfgr_frequency) == -1){
      close_sdrplay(sdr);
      return -1;
    }
    frontend->rf_atten = get_rf_atten(sdr,rfgr_frequency);

    int const if_att = config_getint(Dictionary,section,"if-att",-1);
    int const if_gr = config_getint(Dictionary,section,"if-gr",-1);
    bool const if_agc = config_getboolean(Dictionary,section,"if-agc",false); // default off
    int const if_agc_rate = config_getint(Dictionary,section,"if-agc-rate",-1);
    int const if_agc_setPoint_dBfs = config_getint(Dictionary,section,"if-agc-setpoint-dbfs",-60);
    int const if_agc_attack_ms = config_getint(Dictionary,section,"if-agc-attack-ms",0);
    int const if_agc_decay_ms = config_getint(Dictionary,section,"if-agc-decay-ms",0);
    int const if_agc_decay_delay_ms = config_getint(Dictionary,section,"if-agc-decay-delay-ms",0);
    int const if_agc_decay_threshold_dB = config_getint(Dictionary,section,"if-agc-decay-threshold-db",0);
    if(set_if_gain(sdr,if_att,if_gr,if_agc,if_agc_rate,if_agc_setPoint_dBfs,if_agc_attack_ms,if_agc_decay_ms,if_agc_decay_delay_ms,if_agc_decay_threshold_dB) == -1){
      close_sdrplay(sdr);
      return -1;
    }

    bool const dc_offset_corr = config_getboolean(Dictionary,section,"dc-offset-corr",true); // default on
    bool const iq_imbalance_corr = config_getboolean(Dictionary,section,"iq-imbalance-corr",true); // default on
    if(set_dc_offset_iq_imbalance_correction(sdr,dc_offset_corr,iq_imbalance_corr) == -1) {
      close_sdrplay(sdr);
      return -1;
    }

    bool const transfer_mode_bulk = config_getboolean(Dictionary,section,"bulk-transfer-mode",false); // default isochronous
    if(set_bulk_transfer_mode(sdr,transfer_mode_bulk) == -1){
      close_sdrplay(sdr);
      return -1;
    }

    bool const rf_notch = config_getboolean(Dictionary,section,"rf-notch",false);
    bool const dab_notch = config_getboolean(Dictionary,section,"dab-notch",false);
    bool const am_notch = config_getboolean(Dictionary,section,"am-notch",false);
    if(set_notch_filters(sdr,rf_notch,dab_notch,am_notch) == -1){
      close_sdrplay(sdr);
      return -1;
    }

    bool const biasT = config_getboolean(Dictionary,section,"bias-t",false);
    if(set_biasT(sdr,biasT) == -1){
      close_sdrplay(sdr);
      return -1;
    }
  }
  {
    char const * const p = config_getstring(Dictionary,section,"description","SDRplay RSP");
    FREE(frontend->description);
    frontend->description = strdup(p);
    fprintf(stdout,"%s: ",frontend->description);
  }

  fprintf(stdout,"RF LNA state %d, IF att %d, IF AGC %d, IF AGC setPoint %d, DC offset corr %d, IQ imbalance corr %d\n",
          (int)(sdr->rx_channel_params->tunerParams.gain.LNAstate),
          sdr->rx_channel_params->tunerParams.gain.gRdB,
          sdr->rx_channel_params->ctrlParams.agc.enable,
          sdr->rx_channel_params->ctrlParams.agc.setPoint_dBfs,
          sdr->rx_channel_params->ctrlParams.dcOffset.DCenable,
          sdr->rx_channel_params->ctrlParams.dcOffset.IQenable);

  if(init_frequency != 0){
    set_center_freq(sdr,init_frequency);
    frontend->lock = true;
    fprintf(stdout,"Locked tuner frequency %'.3lf Hz\n",init_frequency);
  }
  return 0;
}

int sdrplay_startup(struct frontend * const frontend){
  struct sdrstate * const sdr = (struct sdrstate *)frontend->context;
  pthread_create(&sdr->monitor_thread,NULL,sdrplay_monitor,sdr);
  return 0;
}

static void *sdrplay_monitor(void *p){
  struct sdrstate * const sdr = (struct sdrstate *)p;
  assert(sdr != NULL);
  struct frontend * const frontend = sdr->frontend;
  assert(frontend != NULL);
  pthread_setname("sdrplay-mon");

  realtime();
  int ret __attribute__ ((unused));
  ret = start_rx(sdr,rx_callback,event_callback);
  assert(ret == 0);
  fprintf(stdout,"SDRplay RSP running\n");
  // Periodically poll status to ensure device hasn't reset
  uint64_t prev_samples = 0;
  while(true){
    sleep(1);
    uint64_t curr_samples = frontend->samples;
    if(!(curr_samples > prev_samples))
      break; // Device seems to have bombed. Exit and let systemd restart us
    prev_samples = curr_samples;
  }
  fprintf(stdout,"Device is no longer streaming, exiting\n");
  close_sdrplay(sdr);
  exit(1); // Let systemd restart us
}

double sdrplay_tune(struct frontend * const frontend,double const f){
  if(frontend->lock)
    return frontend->frequency;
  struct sdrstate * const sdr = frontend->context;
  return set_center_freq(sdr,f);
}


// SDRplay specific functions
static int open_sdrplay(struct sdrstate *sdr){
  sdrplay_api_ErrT err;
  err = sdrplay_api_Open();
  if(err != sdrplay_api_Success){
    fprintf(stdout,"sdrplay_api_Open() failed: %s\n",sdrplay_api_GetErrorString(err));
    return -1;
  }
  sdr->device_status |= SDRPLAY_API_OPEN;
  float ver;
  err = sdrplay_api_ApiVersion(&ver);
  if(err != sdrplay_api_Success){
    fprintf(stdout,"sdrplay_api_ApiVersion() failed: %s\n",sdrplay_api_GetErrorString(err));
    return -1;
  }
  if(ver != SDRPLAY_API_VERSION){
    fprintf(stdout,"SDRplay API version mismatch: found %.2f, expecting %.2f\n",ver,SDRPLAY_API_VERSION);
    return -1;
  }
  err = sdrplay_api_DebugEnable(NULL,dbgLvl);
  if(err != sdrplay_api_Success){
    fprintf(stdout,"sdrplay_api_DebugEnable() failed: %s\n",sdrplay_api_GetErrorString(err));
    return -1;
  }
  return 0;
}

static int close_sdrplay(struct sdrstate *sdr){
  struct frontend * const frontend = sdr->frontend;
  assert(frontend != NULL);
  int ret = 0;
  if(sdr->device_status & DEVICE_STREAMING){
    sdrplay_api_ErrT err = sdrplay_api_Uninit(sdr->device.dev);
    if(err != sdrplay_api_Success){
      fprintf(stdout,"sdrplay_api_Uninit() failed: %s\n",sdrplay_api_GetErrorString(err));
      ret = -1;
    }
    sdr->device_status &= ~DEVICE_STREAMING;
    fprintf(stdout,"sdrplay done streaming - samples=%llu - events=%llu\n",
	    (long long unsigned)frontend->samples,(long long unsigned)sdr->events);
  }
  if(sdr->device_status & DEVICE_SELECTED){
    sdrplay_api_LockDeviceApi();
    sdrplay_api_ErrT err = sdrplay_api_ReleaseDevice(&sdr->device);
    if(err != sdrplay_api_Success){
      fprintf(stdout,"sdrplay_api_ReleaseDevice() failed: %s\n",sdrplay_api_GetErrorString(err));
      ret = -1;
    }
    sdrplay_api_UnlockDeviceApi();
    sdr->device_status &= ~DEVICE_SELECTED;
  }
  if(sdr->device_status & DEVICE_API_LOCKED){
    sdrplay_api_ErrT err = sdrplay_api_UnlockDeviceApi();
    if(err != sdrplay_api_Success){
      fprintf(stdout,"sdrplay_api_UnlockDeviceApi() failed: %s\n",sdrplay_api_GetErrorString(err));
      ret = -1;
    }
    sdr->device_status &= ~DEVICE_API_LOCKED;
  }
  if(sdr->device_status & SDRPLAY_API_OPEN){
    sdrplay_api_ErrT err = sdrplay_api_Close();
    if(err != sdrplay_api_Success){
      fprintf(stdout,"sdrplay_api_Close() failed: %s\n",sdrplay_api_GetErrorString(err));
      ret = -1;
    }
    sdr->device_status &= ~SDRPLAY_API_OPEN;
  }
  return ret;
}

static int find_rsp(struct sdrstate *sdr,char const *sn){
  sdrplay_api_ErrT err;
  err = sdrplay_api_LockDeviceApi();
  if(err != sdrplay_api_Success){
    fprintf(stdout,"sdrplay_api_LockDeviceApi() failed: %s\n",sdrplay_api_GetErrorString(err));
    return -1;
  }
  sdr->device_status |= DEVICE_API_LOCKED;
  unsigned int ndevices = SDRPLAY_MAX_DEVICES;
  sdrplay_api_DeviceT devices[SDRPLAY_MAX_DEVICES];
  err = sdrplay_api_GetDevices(devices,&ndevices,ndevices);
  if(err != sdrplay_api_Success){
    fprintf(stdout,"sdrplay_api_GetDevices() failed: %s\n",sdrplay_api_GetErrorString(err));
    return -1;
  }

  if(ndevices == 0){
    fprintf(stdout,"No SDRplay RSP devices found\n");
    return -1;
  }
  if(sn == NULL){
    fprintf(stdout,"Discovered SDRplay RSP device serial%s:",ndevices > 1 ? "s" : "");
    int firstvalid = -1;
    for(unsigned int i = 0; i < ndevices; i++){
      if(devices[i].valid){
        fprintf(stdout," %s",devices[i].SerNo);
        if(firstvalid == -1)
          firstvalid = i;
      }
    }
    fprintf(stdout,"\n");
    fprintf(stdout,"Selecting %s; to select another, add 'serial = ' to config file\n",devices[firstvalid].SerNo);
  }

  int found = 0;
  for(unsigned int i = 0; i < ndevices; i++){
    if(devices[i].valid){
      if(sn == NULL || strcmp(devices[i].SerNo,sn) == 0){
        sdr->device = devices[i];
        found = 1;
        break;
      }
    }
  }
  if(!found){
    fprintf(stdout,"sdrplay device %s not found or unavailable\n",sn);
    return -1;
  }
  return 0;
}

static int set_rspduo_mode(struct sdrstate *sdr,char const *mode,char const *antenna){
  // RSPduo mode
  int valid_mode = 1;
  if(mode == NULL){
    if(sdr->device.rspDuoMode & sdrplay_api_RspDuoMode_Single_Tuner)
      sdr->device.rspDuoMode = sdrplay_api_RspDuoMode_Single_Tuner;
  } else if(strcmp(mode,"single-tuner") == 0 || strcmp(mode,"Single Tuner") == 0){
    if(sdr->device.rspDuoMode & sdrplay_api_RspDuoMode_Single_Tuner)
      sdr->device.rspDuoMode = sdrplay_api_RspDuoMode_Single_Tuner;
    else
      valid_mode = 0;
  } else if(strcmp(mode,"dual-tuner") == 0 || strcmp(mode,"Dual Tuner") == 0){
    if(sdr->device.rspDuoMode & sdrplay_api_RspDuoMode_Dual_Tuner){
      sdr->device.rspDuoMode = sdrplay_api_RspDuoMode_Dual_Tuner;
      sdr->device.rspDuoSampleFreq = 6e6;
    } else
      valid_mode = 0;
  } else if(strcmp(mode,"master") == 0 || strcmp(mode,"Master") == 0){
    if(sdr->device.rspDuoMode & sdrplay_api_RspDuoMode_Master){
      sdr->device.rspDuoMode = sdrplay_api_RspDuoMode_Master;
      sdr->device.rspDuoSampleFreq = 6e6;
    } else
      valid_mode = 0;
  } else if(strcmp(mode,"master-8msps") == 0 || strcmp(mode,"Master (SR=8MHz)") == 0){
    if(sdr->device.rspDuoMode & sdrplay_api_RspDuoMode_Master){
      sdr->device.rspDuoMode = sdrplay_api_RspDuoMode_Master;
      sdr->device.rspDuoSampleFreq = 8e6;
    } else
      valid_mode = 0;
  } else if(strcmp(mode,"slave") == 0 || strcmp(mode,"Slave") == 0){
    if(!(sdr->device.rspDuoMode == sdrplay_api_RspDuoMode_Slave))
      valid_mode = 0;
  } else
    valid_mode = 0;
  if(!valid_mode){
    fprintf(stdout,"sdrplay - RSPduo mode %s is invalid or not available\n",mode);
    return -1;
  }

  // RSPduo tuner
  int valid_tuner = 1;
  if(antenna == NULL){
    if(sdr->device.rspDuoMode == sdrplay_api_RspDuoMode_Single_Tuner ||
       sdr->device.rspDuoMode == sdrplay_api_RspDuoMode_Master)
      sdr->device.tuner = sdrplay_api_Tuner_A;
  } else if(strcmp(antenna,"tuner1-50ohm") == 0 || strcmp(antenna,"Tuner 1 50ohm") == 0 || strcmp(antenna,"high-z") == 0 || strcmp(antenna,"High Z") == 0){
    if(sdr->device.rspDuoMode != sdrplay_api_RspDuoMode_Dual_Tuner && sdr->device.tuner & sdrplay_api_Tuner_A)
      sdr->device.tuner = sdrplay_api_Tuner_A;
    else
      valid_tuner = 0;
  } else if(strcmp(antenna,"tuner2-50ohm") == 0 || strcmp(antenna,"Tuner 2 50ohm") == 0){
    if(sdr->device.rspDuoMode != sdrplay_api_RspDuoMode_Dual_Tuner && sdr->device.tuner & sdrplay_api_Tuner_B)
      sdr->device.tuner = sdrplay_api_Tuner_B;
    else
      valid_tuner = 0;
  } else
    valid_tuner = 0;
  if(!valid_tuner){
    fprintf(stdout,"sdrplay - antenna %s is invalid or not available\n",antenna);
    return -1;
  }

  return 0;
}

static int select_device(struct sdrstate *sdr){
  sdrplay_api_ErrT err;
  err = sdrplay_api_SelectDevice(&sdr->device);
  if(err != sdrplay_api_Success){
    fprintf(stdout,"sdrplay_api_SelectDevice() failed: %s\n",sdrplay_api_GetErrorString(err));
    return -1;
  }
  sdr->device_status |= DEVICE_SELECTED;
  err = sdrplay_api_UnlockDeviceApi();
  sdr->device_status &= ~DEVICE_API_LOCKED;
  if(err != sdrplay_api_Success){
    fprintf(stdout,"sdrplay_api_UnlockDeviceApi() failed: %s\n",sdrplay_api_GetErrorString(err));
    return -1;
  }
  err = sdrplay_api_DebugEnable(sdr->device.dev,dbgLvl);
  if(err != sdrplay_api_Success){
    fprintf(stdout,"sdrplay_api_DebugEnable() failed: %s\n",sdrplay_api_GetErrorString(err));
    return -1;
  }
  err = sdrplay_api_GetDeviceParams(sdr->device.dev,&sdr->device_params);
  if(err != sdrplay_api_Success){
    fprintf(stdout,"sdrplay_api_GetDeviceParams() failed: %s\n",sdrplay_api_GetErrorString(err));
    return -1;
  }
  if(sdr->device.tuner == sdrplay_api_Tuner_A){
    sdr->rx_channel_params = sdr->device_params->rxChannelA;
  } else if(sdr->device.tuner == sdrplay_api_Tuner_B){
    sdr->rx_channel_params = sdr->device_params->rxChannelB;
  } else {
    fprintf(stdout,"sdrplay - invalid tuner: %d\n",sdr->device.tuner);
    return -1;
  }
  return 0;
}

static double set_center_freq(struct sdrstate *sdr,double const frequency){
  struct frontend * const frontend = sdr->frontend;
  double const calibrated_frequency = frequency * (1 + frontend->calibrate);
  sdr->rx_channel_params->tunerParams.rfFreq.rfHz = calibrated_frequency;
  if(sdr->device_status & DEVICE_STREAMING){
    sdrplay_api_ErrT err;
    err = sdrplay_api_Update(sdr->device.dev,sdr->device.tuner,sdrplay_api_Update_Tuner_Frf,sdrplay_api_Update_Ext1_None);
    if(err != sdrplay_api_Success){
      fprintf(stdout,"sdrplay_api_Update(Tuner_Frf) failed: %s\n",sdrplay_api_GetErrorString(err));
      return -1;
    }
  }
  frontend->frequency = calibrated_frequency;
  return frontend->frequency;
}

static int set_ifreq(struct sdrstate *sdr,int const ifreq){
  int valid_if = 1;
  if(sdr->device.hwVer == SDRPLAY_RSPduo_ID && (sdr->device.rspDuoMode == sdrplay_api_RspDuoMode_Dual_Tuner || sdr->device.rspDuoMode == sdrplay_api_RspDuoMode_Master || sdr->device.rspDuoMode == sdrplay_api_RspDuoMode_Slave)){
    if(sdr->device.rspDuoSampleFreq == 6e6 && (ifreq == -1 || ifreq == 1620))
      sdr->rx_channel_params->tunerParams.ifType = sdrplay_api_IF_1_620;
    else if(sdr->device.rspDuoSampleFreq == 8e6 && (ifreq == -1 || ifreq == 2048))
      sdr->rx_channel_params->tunerParams.ifType = sdrplay_api_IF_2_048;
    else
      valid_if = 0;
  } else {
    if(ifreq == -1 || ifreq == 0){
      sdr->rx_channel_params->tunerParams.ifType = sdrplay_api_IF_Zero;
    } else if(ifreq == 450){
      sdr->rx_channel_params->tunerParams.ifType = sdrplay_api_IF_0_450;
    } else if(ifreq == 1620){
      sdr->rx_channel_params->tunerParams.ifType = sdrplay_api_IF_1_620;
    } else if(ifreq == 2048){
      sdr->rx_channel_params->tunerParams.ifType = sdrplay_api_IF_2_048;
    } else
      valid_if = 0;
  }
  if(!valid_if){
    fprintf(stdout,"sdrplay - IF=%d is invalid\n",ifreq);
    return -1;
  }
  return 0;
}

static int set_bandwidth(struct sdrstate *sdr,int const bandwidth,double const samprate){
  double samprate_kHz = samprate / 1000.0;
  int valid_bandwidth = 1;
  if(bandwidth == sdrplay_api_BW_0_200 || (bandwidth == -1 && samprate_kHz < sdrplay_api_BW_0_300)){
    sdr->rx_channel_params->tunerParams.bwType = sdrplay_api_BW_0_200;
  } else if(bandwidth == sdrplay_api_BW_0_300 || (bandwidth == -1 && samprate_kHz < sdrplay_api_BW_0_600)){
    sdr->rx_channel_params->tunerParams.bwType = sdrplay_api_BW_0_300;
  } else if(bandwidth == sdrplay_api_BW_0_600 || (bandwidth == -1 && samprate_kHz < sdrplay_api_BW_1_536)){
    sdr->rx_channel_params->tunerParams.bwType = sdrplay_api_BW_0_600;
  } else if(bandwidth == sdrplay_api_BW_1_536 || (bandwidth == -1 && samprate_kHz < sdrplay_api_BW_5_000)){
    sdr->rx_channel_params->tunerParams.bwType = sdrplay_api_BW_1_536;
  } else if(bandwidth == sdrplay_api_BW_5_000 || (bandwidth == -1 && samprate_kHz < sdrplay_api_BW_6_000)){
    sdr->rx_channel_params->tunerParams.bwType = sdrplay_api_BW_5_000;
  } else if(bandwidth == sdrplay_api_BW_6_000 || (bandwidth == -1 && samprate_kHz < sdrplay_api_BW_7_000)){
    sdr->rx_channel_params->tunerParams.bwType = sdrplay_api_BW_6_000;
  } else if(bandwidth == sdrplay_api_BW_7_000 || (bandwidth == -1 && samprate_kHz < sdrplay_api_BW_8_000)){
    sdr->rx_channel_params->tunerParams.bwType = sdrplay_api_BW_7_000;
  } else if(bandwidth == sdrplay_api_BW_8_000 || (bandwidth == -1 && samprate_kHz >= sdrplay_api_BW_8_000)){
    sdr->rx_channel_params->tunerParams.bwType = sdrplay_api_BW_8_000;
  } else
    valid_bandwidth = 0;
  if(!valid_bandwidth){
    fprintf(stdout,"sdrplay - Bandwidth=%d is invalid\n",bandwidth);
    return -1;
  }
  return 0;
}

static int set_samplerate(struct sdrstate *sdr,double const samprate){
  // get actual sample rate and decimation
  double actual_sample_rate;
  int decimation;
  for(decimation = 1; decimation <= MAX_DECIMATION; decimation *= 2){
    actual_sample_rate = samprate * decimation;
    if(actual_sample_rate >= MIN_SAMPLE_RATE)
      break;
  }
  if(!(actual_sample_rate >= MIN_SAMPLE_RATE && actual_sample_rate <= MAX_SAMPLE_RATE)){
    fprintf(stdout,"sdrplay - sample_rate=%f is invalid\n",samprate);
    return -1;
  }
  if(sdr->device.hwVer == SDRPLAY_RSPduo_ID && (sdr->device.rspDuoMode == sdrplay_api_RspDuoMode_Dual_Tuner || sdr->device.rspDuoMode == sdrplay_api_RspDuoMode_Master || sdr->device.rspDuoMode == sdrplay_api_RspDuoMode_Slave)){
    if(actual_sample_rate == MIN_SAMPLE_RATE){
      if(sdr->device_params->devParams)
        sdr->device_params->devParams->fsFreq.fsHz = sdr->device.rspDuoSampleFreq;
    } else {
      fprintf(stdout,"sdrplay - sample_rate=%f is invalid\n",samprate);
      return -1;
    }
  } else {
    sdr->device_params->devParams->fsFreq.fsHz = actual_sample_rate;
  }
  if(decimation > 1){
    sdr->rx_channel_params->ctrlParams.decimation.enable = 1;
    sdr->rx_channel_params->ctrlParams.decimation.decimationFactor = decimation;
  } else {
    sdr->rx_channel_params->ctrlParams.decimation.enable = 0;
    sdr->rx_channel_params->ctrlParams.decimation.decimationFactor = 1;
  }

  return 0;
}

static double get_samplerate(struct sdrstate *sdr){
  double samprate = 0.0;
  if(sdr->rx_channel_params->tunerParams.ifType == sdrplay_api_IF_Zero){
    samprate = sdr->device_params->devParams->fsFreq.fsHz;
  } else if(sdr->rx_channel_params->tunerParams.ifType == sdrplay_api_IF_1_620){
    samprate = 2e6;
  } else if(sdr->rx_channel_params->tunerParams.ifType == sdrplay_api_IF_2_048){
    samprate = 2e6;
  }
  if(sdr->rx_channel_params->ctrlParams.decimation.enable)
    samprate /= sdr->rx_channel_params->ctrlParams.decimation.decimationFactor;
  return samprate;
}

static int set_antenna(struct sdrstate *sdr,char const *antenna){
  int valid_antenna = 1;
  if(sdr->device.hwVer == SDRPLAY_RSP2_ID){
    if(antenna == NULL || strcmp(antenna, "antenna-a") == 0 || strcmp(antenna, "Antenna A") == 0){
      sdr->rx_channel_params->rsp2TunerParams.antennaSel = sdrplay_api_Rsp2_ANTENNA_A;
      sdr->rx_channel_params->rsp2TunerParams.amPortSel = sdrplay_api_Rsp2_AMPORT_2;
    } else if(strcmp(antenna, "antenna-b") == 0 || strcmp(antenna, "Antenna B") == 0){
      sdr->rx_channel_params->rsp2TunerParams.antennaSel = sdrplay_api_Rsp2_ANTENNA_B;
      sdr->rx_channel_params->rsp2TunerParams.amPortSel = sdrplay_api_Rsp2_AMPORT_2;
    } else if(strcmp(antenna, "hi-z") == 0 || strcmp(antenna, "Hi-Z") == 0){
      sdr->rx_channel_params->rsp2TunerParams.antennaSel = sdrplay_api_Rsp2_ANTENNA_A;
      sdr->rx_channel_params->rsp2TunerParams.amPortSel = sdrplay_api_Rsp2_AMPORT_1;
    } else
      valid_antenna = 0;
  } else if(sdr->device.hwVer == SDRPLAY_RSPduo_ID){
    if(antenna == NULL){
      sdr->rx_channel_params->rspDuoTunerParams.tuner1AmPortSel = sdrplay_api_RspDuo_AMPORT_2;
    } else if(strcmp(antenna,"tuner1-50ohm") == 0 || strcmp(antenna,"Tuner 1 50ohm") == 0){
      if(sdr->device.tuner & sdrplay_api_Tuner_A)
        sdr->rx_channel_params->rspDuoTunerParams.tuner1AmPortSel = sdrplay_api_RspDuo_AMPORT_2;
      else
        valid_antenna = 0;
    } else if(strcmp(antenna,"tuner2-50ohm") == 0 || strcmp(antenna,"Tuner 2 50ohm") == 0){
      if(sdr->device.tuner & sdrplay_api_Tuner_B)
        sdr->rx_channel_params->rspDuoTunerParams.tuner1AmPortSel = sdrplay_api_RspDuo_AMPORT_2;
      else
        valid_antenna = 0;
    } else if(strcmp(antenna,"high-z") == 0 || strcmp(antenna,"High Z") == 0){
      if(sdr->device.tuner & sdrplay_api_Tuner_A)
        sdr->rx_channel_params->rspDuoTunerParams.tuner1AmPortSel = sdrplay_api_RspDuo_AMPORT_1;
      else
        valid_antenna = 0;
    } else
      valid_antenna = 0;
  } else if(sdr->device.hwVer == SDRPLAY_RSPdx_ID){
    if(antenna == NULL || strcmp(antenna, "antenna-a") == 0 || strcmp(antenna, "Antenna A") == 0){
      sdr->device_params->devParams->rspDxParams.antennaSel = sdrplay_api_RspDx_ANTENNA_A;
    } else if(strcmp(antenna, "antenna-b") == 0 || strcmp(antenna, "Antenna B") == 0){
      sdr->device_params->devParams->rspDxParams.antennaSel = sdrplay_api_RspDx_ANTENNA_B;
    } else if(strcmp(antenna, "antenna-c") == 0 || strcmp(antenna, "Antenna C") == 0){
      sdr->device_params->devParams->rspDxParams.antennaSel = sdrplay_api_RspDx_ANTENNA_C;
    } else
      valid_antenna = 0;
  } else if(sdr->device.hwVer == SDRPLAY_RSPdxR2_ID){
    if(antenna == NULL || strcmp(antenna, "antenna-a") == 0 || strcmp(antenna, "Antenna A") == 0){
      sdr->device_params->devParams->rspDxParams.antennaSel = sdrplay_api_RspDx_ANTENNA_A;
    } else if(strcmp(antenna, "antenna-b") == 0 || strcmp(antenna, "Antenna B") == 0){
      sdr->device_params->devParams->rspDxParams.antennaSel = sdrplay_api_RspDx_ANTENNA_B;
    } else if(strcmp(antenna, "antenna-c") == 0 || strcmp(antenna, "Antenna C") == 0){
      sdr->device_params->devParams->rspDxParams.antennaSel = sdrplay_api_RspDx_ANTENNA_C;
    } else
      valid_antenna = 0;
  } else {
    if(antenna != NULL)
      valid_antenna = 0;
  }
  if(!valid_antenna){
    fprintf(stdout,"sdrplay - Antenna=%s is invalid (or not available)\n",antenna);
    return -1;
  }
  return 0;
}

static uint8_t const *get_lna_states(struct sdrstate *sdr,double const frequency,int *lna_state_count){
  if(sdr->device.hwVer == SDRPLAY_RSP1_ID){
    if(frequency < 420e6){
      *lna_state_count = sizeof(rsp1_0_420_lna_states) / sizeof(uint8_t);
      return rsp1_0_420_lna_states;
    } else if(frequency < 1000e6){
      *lna_state_count = sizeof(rsp1_420_1000_lna_states) / sizeof(uint8_t);
      return rsp1_420_1000_lna_states;
    } else {
      *lna_state_count = sizeof(rsp1_1000_2000_lna_states) / sizeof(uint8_t);
      return rsp1_1000_2000_lna_states;
    }
  } else if(sdr->device.hwVer == SDRPLAY_RSP1A_ID){
    if(frequency < 60e6){
      *lna_state_count = sizeof(rsp1a_0_60_lna_states) / sizeof(uint8_t);
      return rsp1a_0_60_lna_states;
    } else if(frequency < 420e6){
      *lna_state_count = sizeof(rsp1a_60_420_lna_states) / sizeof(uint8_t);
      return rsp1a_60_420_lna_states;
    } else if(frequency < 1000e6){
      *lna_state_count = sizeof(rsp1a_420_1000_lna_states) / sizeof(uint8_t);
      return rsp1a_420_1000_lna_states;
    } else {
      *lna_state_count = sizeof(rsp1a_1000_2000_lna_states) / sizeof(uint8_t);
      return rsp1a_1000_2000_lna_states;
    }
  } else if(sdr->device.hwVer == SDRPLAY_RSP1B_ID){
    if(frequency < 50e6){
      *lna_state_count = sizeof(rsp1b_0_50_lna_states) / sizeof(uint8_t);
      return rsp1b_0_50_lna_states;
    } else if(frequency < 60e6){
      *lna_state_count = sizeof(rsp1b_50_60_lna_states) / sizeof(uint8_t);
      return rsp1b_50_60_lna_states;
    } else if(frequency < 420e6){
      *lna_state_count = sizeof(rsp1b_60_420_lna_states) / sizeof(uint8_t);
      return rsp1b_60_420_lna_states;
    } else if(frequency < 1000e6){
      *lna_state_count = sizeof(rsp1b_420_1000_lna_states) / sizeof(uint8_t);
      return rsp1b_420_1000_lna_states;
    } else {
      *lna_state_count = sizeof(rsp1b_1000_2000_lna_states) / sizeof(uint8_t);
      return rsp1b_1000_2000_lna_states;
    }
  } else if(sdr->device.hwVer == SDRPLAY_RSP2_ID){
    if(frequency < 60e6 && sdr->rx_channel_params->rsp2TunerParams.amPortSel == sdrplay_api_Rsp2_AMPORT_1){
      *lna_state_count = sizeof(rsp2_0_60_hiz_lna_states) / sizeof(uint8_t);
      return rsp2_0_60_hiz_lna_states;
    } else if(frequency < 420e6){
      *lna_state_count = sizeof(rsp2_0_420_lna_states) / sizeof(uint8_t);
      return rsp2_0_420_lna_states;
    } else if(frequency < 1000e6){
      *lna_state_count = sizeof(rsp2_420_1000_lna_states) / sizeof(uint8_t);
      return rsp2_420_1000_lna_states;
    } else {
      *lna_state_count = sizeof(rsp2_1000_2000_lna_states) / sizeof(uint8_t);
      return rsp2_1000_2000_lna_states;
    }
  } else if(sdr->device.hwVer == SDRPLAY_RSPduo_ID){
    if(frequency < 60e6 && sdr->rx_channel_params->rspDuoTunerParams.tuner1AmPortSel == sdrplay_api_RspDuo_AMPORT_1){
      *lna_state_count = sizeof(rspduo_0_60_hiz_lna_states) / sizeof(uint8_t);
      return rspduo_0_60_hiz_lna_states;
    } else if(frequency < 60e6){
      *lna_state_count = sizeof(rspduo_0_60_lna_states) / sizeof(uint8_t);
      return rspduo_0_60_lna_states;
    } else if(frequency < 420e6){
      *lna_state_count = sizeof(rspduo_60_420_lna_states) / sizeof(uint8_t);
      return rspduo_60_420_lna_states;
    } else if(frequency < 1000e6){
      *lna_state_count = sizeof(rspduo_420_1000_lna_states) / sizeof(uint8_t);
      return rspduo_420_1000_lna_states;
    } else {
      *lna_state_count = sizeof(rspduo_1000_2000_lna_states) / sizeof(uint8_t);
      return rspduo_1000_2000_lna_states;
    }
  } else if(sdr->device.hwVer == SDRPLAY_RSPdx_ID){
    if(frequency < 2e6 && sdr->device_params->devParams->rspDxParams.hdrEnable == 1){
      *lna_state_count = sizeof(rspdx_0_2_hdr_lna_states) / sizeof(uint8_t);
      return rspdx_0_2_hdr_lna_states;
    } else if(frequency < 12e6){
      *lna_state_count = sizeof(rspdx_0_12_lna_states) / sizeof(uint8_t);
      return rspdx_0_12_lna_states;
    } else if(frequency < 50e6){
      *lna_state_count = sizeof(rspdx_12_50_lna_states) / sizeof(uint8_t);
      return rspdx_12_50_lna_states;
    } else if(frequency < 60e6){
      *lna_state_count = sizeof(rspdx_50_60_lna_states) / sizeof(uint8_t);
      return rspdx_50_60_lna_states;
    } else if(frequency < 250e6){
      *lna_state_count = sizeof(rspdx_60_250_lna_states) / sizeof(uint8_t);
      return rspdx_60_250_lna_states;
    } else if(frequency < 420e6){
      *lna_state_count = sizeof(rspdx_250_420_lna_states) / sizeof(uint8_t);
      return rspdx_250_420_lna_states;
    } else if(frequency < 1000e6){
      *lna_state_count = sizeof(rspdx_420_1000_lna_states) / sizeof(uint8_t);
      return rspdx_420_1000_lna_states;
    } else {
      *lna_state_count = sizeof(rspdx_1000_2000_lna_states) / sizeof(uint8_t);
      return rspdx_1000_2000_lna_states;
    }
  } else if(sdr->device.hwVer == SDRPLAY_RSPdxR2_ID){
    if(frequency < 2e6 && sdr->device_params->devParams->rspDxParams.hdrEnable == 1){
      *lna_state_count = sizeof(rspdxr2_0_2_hdr_lna_states) / sizeof(uint8_t);
      return rspdxr2_0_2_hdr_lna_states;
    } else if(frequency < 12e6){
      *lna_state_count = sizeof(rspdxr2_0_12_lna_states) / sizeof(uint8_t);
      return rspdxr2_0_12_lna_states;
    } else if(frequency < 50e6){
      *lna_state_count = sizeof(rspdxr2_12_50_lna_states) / sizeof(uint8_t);
      return rspdxr2_12_50_lna_states;
    } else if(frequency < 60e6){
      *lna_state_count = sizeof(rspdxr2_50_60_lna_states) / sizeof(uint8_t);
      return rspdxr2_50_60_lna_states;
    } else if(frequency < 250e6){
      *lna_state_count = sizeof(rspdxr2_60_250_lna_states) / sizeof(uint8_t);
      return rspdxr2_60_250_lna_states;
    } else if(frequency < 420e6){
      *lna_state_count = sizeof(rspdxr2_250_420_lna_states) / sizeof(uint8_t);
      return rspdxr2_250_420_lna_states;
    } else if(frequency < 1000e6){
      *lna_state_count = sizeof(rspdxr2_420_1000_lna_states) / sizeof(uint8_t);
      return rspdxr2_420_1000_lna_states;
    } else {
      *lna_state_count = sizeof(rspdxr2_1000_2000_lna_states) / sizeof(uint8_t);
      return rspdxr2_1000_2000_lna_states;
    }
  }
  *lna_state_count = 0;
  return NULL;
}

static int set_rf_gain(struct sdrstate *sdr,int const lna_state,int const rf_att,int const rf_gr,double const frequency){
  struct frontend *frontend = sdr->frontend;

  int lna_state_count = 0;
  uint8_t const * const lna_states = get_lna_states(sdr,frequency,&lna_state_count);
  assert(lna_states != NULL);
  assert(lna_state_count > 0);
  int valid_rf_gain = 1;
  if(lna_state != -1) {
    if(rf_att != -1 || rf_gr != -1){
      fprintf(stdout,"sdrplay - only one of lna-state, rf-att, or rf-gr is allowed\n");
      return -1;
    }
    if(lna_state >= 0 && lna_state < lna_state_count)
      sdr->rx_channel_params->tunerParams.gain.LNAstate = lna_state;
    else
      valid_rf_gain = 0;
  } else {
    if(rf_att != -1 && rf_gr != -1){
      fprintf(stdout,"sdrplay - only one of lna-state, rf-att, or rf-gr is allowed\n");
      return -1;
    }
    int rf_gRdB = rf_att;
    if(rf_gRdB == -1)
      rf_gRdB = rf_gr;
    if(rf_gRdB == -1)
      return 0;
    // find the closest LNA state
    int lna_state_min = -1;
    int delta_min = 1000;
    for(int i = 0; i < lna_state_count; i++){
      int delta = abs(lna_states[i] - rf_gRdB);
      if(delta < delta_min){
        lna_state_min = i;
        delta_min = delta;
      }
    }
    sdr->rx_channel_params->tunerParams.gain.LNAstate = lna_state_min;
  }
  if(!valid_rf_gain){
    fprintf(stdout,"sdrplay - RF gain reduction is invalid - lna_state=%d rf_att=%d rf_gr=%d\n",lna_state,rf_att,rf_gr);
    return -1;
  }

  if(sdr->device_status & DEVICE_STREAMING){
    sdrplay_api_ErrT err;
    err = sdrplay_api_Update(sdr->device.dev,sdr->device.tuner,sdrplay_api_Update_Tuner_Gr,sdrplay_api_Update_Ext1_None);
    if(err != sdrplay_api_Success){
      fprintf(stdout,"sdrplay_api_Update(Tuner_Gr) failed: %s\n",sdrplay_api_GetErrorString(err));
      return -1;
    }
  }
  sdr->scale = scale_AD(frontend);
  return 0;
}

static float get_rf_atten(struct sdrstate *sdr,double const frequency){
  int lna_state_count = 0;
  uint8_t const * const lna_states = get_lna_states(sdr,frequency,&lna_state_count);
  assert(lna_states != NULL);
  assert(lna_state_count > 0);
  int const lna_state = sdr->rx_channel_params->tunerParams.gain.LNAstate;
  if(lna_state < 0 || lna_state >= lna_state_count){
    fprintf(stdout,"LNA state out of range: %d - range=[%d,%d(\n",lna_state,0,lna_state_count);
    return NAN;
  }
  return (float)lna_states[lna_state];
}

static int set_if_gain(struct sdrstate *sdr,int const if_att,int const if_gr,int const if_agc,int const if_agc_rate,int const if_agc_setPoint_dBfs,int const if_agc_attack_ms,int const if_agc_decay_ms,int const if_agc_decay_delay_ms,int const if_agc_decay_threshold_dB){
  if(!if_agc){
    int if_gRdB = if_att;
    if(if_gRdB == -1)
      if_gRdB = if_gr;
    if(if_gRdB != -1){
      if(if_gRdB >= sdrplay_api_NORMAL_MIN_GR && if_gRdB <= MAX_BB_GR){
        sdr->rx_channel_params->ctrlParams.agc.enable = sdrplay_api_AGC_DISABLE;
        sdr->rx_channel_params->tunerParams.gain.gRdB = if_gRdB;
      } else {
        fprintf(stdout,"sdrplay - IF gain reduction is out of range - if_att/if_gr=%d\n",if_gRdB);
        return -1;
      }
    }

    if(sdr->device_status & DEVICE_STREAMING){
      sdrplay_api_ErrT err;
      err = sdrplay_api_Update(sdr->device.dev,sdr->device.tuner,sdrplay_api_Update_Ctrl_Agc | sdrplay_api_Update_Tuner_Gr,sdrplay_api_Update_Ext1_None);
      if(err != sdrplay_api_Success){
        fprintf(stdout,"sdrplay_api_Update(Ctrl_Agc | Tuner_Gr) failed: %s\n",sdrplay_api_GetErrorString(err));
        return -1;
      }
    }
    return 0;
  }

  // AGC
  if(if_gr != -1){
    fprintf(stdout,"sdrplay - cannot select both IF gain reduction (if-gr) and AGC (if-agc)\n");
    return -1;
  }

  if(if_agc_rate == -1){   // default: sdrplay_api_AGC_50HZ
    sdr->rx_channel_params->ctrlParams.agc.enable = sdrplay_api_AGC_50HZ;
  } else if(if_agc_rate == 5){
    sdr->rx_channel_params->ctrlParams.agc.enable = sdrplay_api_AGC_5HZ;
  } else if(if_agc_rate == 50){
    sdr->rx_channel_params->ctrlParams.agc.enable = sdrplay_api_AGC_50HZ;
  } else if(if_agc_rate == 100){
    sdr->rx_channel_params->ctrlParams.agc.enable = sdrplay_api_AGC_100HZ;
  } else if(if_agc_rate == 0){  // use AGC scheme
    sdr->rx_channel_params->ctrlParams.agc.enable = sdrplay_api_AGC_CTRL_EN;
    sdr->rx_channel_params->ctrlParams.agc.setPoint_dBfs = if_agc_setPoint_dBfs;
    sdr->rx_channel_params->ctrlParams.agc.attack_ms = if_agc_attack_ms;
    sdr->rx_channel_params->ctrlParams.agc.decay_ms = if_agc_decay_ms;
    sdr->rx_channel_params->ctrlParams.agc.decay_delay_ms = if_agc_decay_delay_ms;
    sdr->rx_channel_params->ctrlParams.agc.decay_threshold_dB = if_agc_decay_threshold_dB;
  }

  if(sdr->device_status & DEVICE_STREAMING){
    sdrplay_api_ErrT err;
    err = sdrplay_api_Update(sdr->device.dev,sdr->device.tuner,sdrplay_api_Update_Ctrl_Agc | sdrplay_api_Update_Tuner_Gr,sdrplay_api_Update_Ext1_None);
    if(err != sdrplay_api_Success){
      fprintf(stdout,"sdrplay_api_Update(Ctrl_Agc | Tuner_Gr) failed: %s\n",sdrplay_api_GetErrorString(err));
      return -1;
    }
  }

  return 0;
}

static int set_dc_offset_iq_imbalance_correction(struct sdrstate *sdr,int const dc_offset_corr,int const iq_imbalance_corr){
  if(dc_offset_corr){
    sdr->rx_channel_params->ctrlParams.dcOffset.DCenable = 1;
  } else {
    sdr->rx_channel_params->ctrlParams.dcOffset.DCenable = 0;
  }
  if(iq_imbalance_corr){
    sdr->rx_channel_params->ctrlParams.dcOffset.DCenable = 1;
    sdr->rx_channel_params->ctrlParams.dcOffset.IQenable = 1;
  } else {
    sdr->rx_channel_params->ctrlParams.dcOffset.IQenable = 0;
  }

  return 0;
}

static int set_bulk_transfer_mode(struct sdrstate *sdr,int const transfer_mode_bulk){
  if(transfer_mode_bulk)
    sdr->device_params->devParams->mode = sdrplay_api_BULK;
  else
    sdr->device_params->devParams->mode = sdrplay_api_ISOCH;
  return 0;
}

static int set_notch_filters(struct sdrstate *sdr,bool const rf_notch,bool const dab_notch,bool const am_notch){
  if(sdr->device.hwVer == SDRPLAY_RSP1A_ID){
    sdr->device_params->devParams->rsp1aParams.rfNotchEnable = rf_notch;
    sdr->device_params->devParams->rsp1aParams.rfDabNotchEnable = dab_notch;
  } else if(sdr->device.hwVer == SDRPLAY_RSP1B_ID){
    sdr->device_params->devParams->rsp1aParams.rfNotchEnable = rf_notch;
    sdr->device_params->devParams->rsp1aParams.rfDabNotchEnable = dab_notch;
  } else if(sdr->device.hwVer == SDRPLAY_RSP2_ID){
    sdr->rx_channel_params->rsp2TunerParams.rfNotchEnable = rf_notch;
  } else if(sdr->device.hwVer == SDRPLAY_RSPduo_ID){
    sdr->rx_channel_params->rspDuoTunerParams.rfNotchEnable = rf_notch;
    sdr->rx_channel_params->rspDuoTunerParams.rfDabNotchEnable = dab_notch;
    if(sdr->device.tuner == sdrplay_api_Tuner_A)
      sdr->rx_channel_params->rspDuoTunerParams.tuner1AmNotchEnable = am_notch;
  } else if(sdr->device.hwVer == SDRPLAY_RSPdx_ID){
    sdr->device_params->devParams->rspDxParams.rfNotchEnable = rf_notch;
    sdr->device_params->devParams->rspDxParams.rfDabNotchEnable = dab_notch;
  } else if(sdr->device.hwVer == SDRPLAY_RSPdxR2_ID){
    sdr->device_params->devParams->rspDxParams.rfNotchEnable = rf_notch;
    sdr->device_params->devParams->rspDxParams.rfDabNotchEnable = dab_notch;
  }
  return 0;
}

static int set_biasT(struct sdrstate *sdr,bool const biasT){
  if(sdr->device.hwVer == SDRPLAY_RSP1A_ID){
    sdr->rx_channel_params->rsp1aTunerParams.biasTEnable = biasT;
  } else if(sdr->device.hwVer == SDRPLAY_RSP1B_ID){
    sdr->rx_channel_params->rsp1aTunerParams.biasTEnable = biasT;
  } else if(sdr->device.hwVer == SDRPLAY_RSP2_ID){
    sdr->rx_channel_params->rsp2TunerParams.biasTEnable = biasT;
  } else if(sdr->device.hwVer == SDRPLAY_RSPduo_ID){
    sdr->rx_channel_params->rspDuoTunerParams.biasTEnable = biasT;
  } else if(sdr->device.hwVer == SDRPLAY_RSPdx_ID){
    sdr->device_params->devParams->rspDxParams.biasTEnable = biasT;
  } else if(sdr->device.hwVer == SDRPLAY_RSPdxR2_ID){
    sdr->device_params->devParams->rspDxParams.biasTEnable = biasT;
  }
  return 0;
}

static int start_rx(struct sdrstate *sdr,sdrplay_api_StreamCallback_t rx_callback,sdrplay_api_EventCallback_t event_callback){
  sdrplay_api_ErrT err;
  sdrplay_api_CallbackFnsT callbacks;
  callbacks.StreamACbFn = rx_callback;
  callbacks.StreamBCbFn = NULL;
  callbacks.EventCbFn = event_callback;
  sdr->events = 0L;
  if(Verbose)
    show_device_params(sdr);
  err = sdrplay_api_Init(sdr->device.dev,&callbacks,sdr);
  if(err != sdrplay_api_Success){
    fprintf(stdout,"sdrplay_api_Init() failed: %s\n",sdrplay_api_GetErrorString(err));
    return -1;
  }
  sdr->device_status |= DEVICE_STREAMING;
  return 0;
}


static bool Name_set = false;
// Callback called with incoming receiver data from A/D
static void rx_callback(int16_t *xi,int16_t *xq,sdrplay_api_StreamCbParamsT *params,unsigned int numSamples,unsigned int reset,void *cbContext){
  struct sdrstate * const sdr = (struct sdrstate *)cbContext;
  assert(sdr != NULL);
  struct frontend * const frontend = sdr->frontend;
  assert(frontend != NULL);

  (void)reset;

  if(!Name_set){
    pthread_setname("sdrplay-cb");
    Name_set = true;
  }

  if(sdr->next_sample_num && params->firstSampleNum != sdr->next_sample_num){
    unsigned int dropped_samples;
    if(sdr->next_sample_num < params->firstSampleNum)
      dropped_samples = params->firstSampleNum - sdr->next_sample_num;
    else
      dropped_samples = UINT_MAX - (params->firstSampleNum - sdr->next_sample_num) + 1;
    sdr->next_sample_num = params->firstSampleNum + numSamples;
    fprintf(stdout,"dropped %'d\n",dropped_samples);
  }

  int const sampcount = numSamples;
  float complex * const wptr = frontend->in.input_write_pointer.c;
  assert(wptr != NULL);
  float in_energy = 0;
  for(int i=0; i < sampcount; i++){
    float complex samp;
    __real__ samp = (int)xi[i];
    __imag__ samp = (int)xq[i];
    in_energy += cnrmf(samp);
    wptr[i] = samp * sdr->scale;
  }
  frontend->samples += sampcount;
  frontend->timestamp = gps_time_ns();
  write_cfilter(&frontend->in,NULL,sampcount); // Update write pointer, invoke FFT
  if(isfinite(in_energy)){
    frontend->if_power_instant = in_energy / sampcount;
    frontend->if_power += Power_smooth * (frontend->if_power_instant - frontend->if_power);
  }
  return;
}

static void event_callback(sdrplay_api_EventT eventId,sdrplay_api_TunerSelectT tuner,sdrplay_api_EventParamsT *params,void *cbContext){
  (void)tuner;

  struct sdrstate *sdr = (struct sdrstate *)cbContext;
  int64_t event_timestamp;
  char event_timestamp_formatted[1024];
  sdrplay_api_ErrT err;

  static int64_t power_overload_detected = -1;

  switch(eventId){
  case sdrplay_api_GainChange:
#if 0
    // this type of event could get very chatty
    sdrplay_api_GainCbParamT *gainParams = &params->gainParams;
    fprintf(stdout,"gain change - gRdB=%d lnaGRdB=%d currGain=%.2f\n",gainParams->gRdB,gainParams->lnaGRdB,gainParams->currGain);
#endif
    break;
  case sdrplay_api_PowerOverloadChange:
    event_timestamp = gps_time_ns();
    format_gpstime(event_timestamp_formatted,sizeof(event_timestamp_formatted),event_timestamp);
    switch(params->powerOverloadParams.powerOverloadChangeType){
    case sdrplay_api_Overload_Detected:
      power_overload_detected = event_timestamp;
      fprintf(stdout,"%s - overload detected\n",event_timestamp_formatted);
      break;
    case sdrplay_api_Overload_Corrected:
      if(power_overload_detected >= 0){
        fprintf(stdout,"%s - overload corrected - duration=%lldns\n",
		event_timestamp_formatted, (long long)event_timestamp - power_overload_detected);
      } else {
        fprintf(stdout,"%s - overload corrected\n",event_timestamp_formatted);
      }
      power_overload_detected = -1;
      break;
    }
    // send ack back for overload events
    err = sdrplay_api_Update(sdr->device.dev,sdr->device.tuner,sdrplay_api_Update_Ctrl_OverloadMsgAck,sdrplay_api_Update_Ext1_None);
    if(err != sdrplay_api_Success){
      fprintf(stdout,"sdrplay_api_Update(Ctrl_OverloadMsgAck) failed: %s\n",sdrplay_api_GetErrorString(err));
    }
    break;
  case sdrplay_api_DeviceRemoved:
    fprintf(stdout,"device removed\n");
    break;
  case sdrplay_api_RspDuoModeChange:
    fprintf(stdout,"PSPduo mode change\n");
    break;
  case sdrplay_api_DeviceFailure:
    fprintf(stdout,"device failure\n");
    break;
  }
  sdr->events++;
  return;
}

static void show_device_params(struct sdrstate *sdr){
  fprintf(stdout,"\n");
  fprintf(stdout,"# Device parameters:\n");
  sdrplay_api_RxChannelParamsT *rx_channels[] = {sdr->device_params->rxChannelA,sdr->device_params->rxChannelB};
  for(int i = 0; i < 2; i++){
    sdrplay_api_RxChannelParamsT* rx_channel = rx_channels[i];
    if(rx_channel == NULL)
      continue;
    fprintf(stdout,"RX channel=%s\n",rx_channel == sdr->device_params->rxChannelA ? "A" : (rx_channel == sdr->device_params->rxChannelB ? "B" : "?"));
    sdrplay_api_TunerParamsT *tunerParams = &rx_channel->tunerParams;
    fprintf(stdout,"    rfHz=%lf\n",tunerParams->rfFreq.rfHz);
    fprintf(stdout,"    bwType=%d\n",tunerParams->bwType);
    fprintf(stdout,"    ifType=%d\n",tunerParams->ifType);
    sdrplay_api_DecimationT *decimation = &rx_channel->ctrlParams.decimation;
    fprintf(stdout,"    decimationFactor=%d\n",(int)(decimation->decimationFactor));
    fprintf(stdout,"    decimation.enable=%d\n",(int)(decimation->enable));
    fprintf(stdout,"    gain.gRdB=%d\n",tunerParams->gain.gRdB);
    fprintf(stdout,"    gain.LNAstate=%d\n",(int)(tunerParams->gain.LNAstate));
    sdrplay_api_AgcT *agc = &rx_channel->ctrlParams.agc;
    fprintf(stdout,"    agc.enable=%d\n",agc->enable);
    fprintf(stdout,"    agc.setPoint_dBfs=%d\n",agc->setPoint_dBfs);
    fprintf(stdout,"    agc.attack_ms=%hd\n",agc->attack_ms);
    fprintf(stdout,"    agc.decay_ms=%hd\n",agc->decay_ms);
    fprintf(stdout,"    agc.decay_delay_ms=%hd\n",agc->decay_delay_ms);
    fprintf(stdout,"    agc.decay_threashold_dB=%hd\n",agc->decay_threshold_dB);
    fprintf(stdout,"    agc.syncUpdate=%d\n",agc->syncUpdate);
    fprintf(stdout,"    dcOffset.DCenable=%d\n",(int)(rx_channel->ctrlParams.dcOffset.DCenable));
    fprintf(stdout,"    dcOffsetTuner.dcCale=%d\n",(int)(tunerParams->dcOffsetTuner.dcCal));
    fprintf(stdout,"    dcOffsetTuner.speedUp=%d\n",(int)(tunerParams->dcOffsetTuner.speedUp));
    fprintf(stdout,"    dcOffsetTuner.trackTime=%d\n",(int)(tunerParams->dcOffsetTuner.trackTime));
    fprintf(stdout,"    dcOffset.IQenable=%d\n",(int)(rx_channel->ctrlParams.dcOffset.IQenable));
  }
  fprintf(stdout,"\n");
  if (sdr->device_params->devParams) {
    fprintf(stdout,"fsHz=%lf\n",sdr->device_params->devParams->fsFreq.fsHz);
    fprintf(stdout,"ppm=%lf\n",sdr->device_params->devParams->ppm);
  }
  fprintf(stdout,"\n");
  if(sdr->device.hwVer == SDRPLAY_RSP2_ID){
    fprintf(stdout,"antennaSel=%d\n",sdr->rx_channel_params->rsp2TunerParams.antennaSel);
    fprintf(stdout,"amPortSel=%d\n",sdr->rx_channel_params->rsp2TunerParams.amPortSel);
    fprintf(stdout,"\n");
  } else if(sdr->device.hwVer == SDRPLAY_RSPduo_ID){
    fprintf(stdout,"tuner=%d\n",sdr->device.tuner);
    fprintf(stdout,"tuner1AmPortSel=%d\n",sdr->rx_channel_params->rspDuoTunerParams.tuner1AmPortSel);
    fprintf(stdout,"\n");
  } else if(sdr->device.hwVer == SDRPLAY_RSPdx_ID){
    fprintf(stdout,"antennaSel=%d\n",sdr->device_params->devParams->rspDxParams.antennaSel);
    fprintf(stdout,"\n");
  } else if(sdr->device.hwVer == SDRPLAY_RSPdxR2_ID){
    fprintf(stdout,"antennaSel=%d\n",sdr->device_params->devParams->rspDxParams.antennaSel);
    fprintf(stdout,"\n");
  }
  fprintf(stdout,"transferMode=%d\n",sdr->device_params->devParams->mode);
  fprintf(stdout,"\n");
  return;
}
