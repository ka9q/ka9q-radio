// Front end driver for Airspy R2 and Airspy Mini, linked into ka9q-radio's radiod
// Copyright 2023, Phil Karn, KA9Q

#undef DEBUG_AGC
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <libairspy/airspy.h>
#include <errno.h>
#include <iniparser/iniparser.h>
#include <string.h>
#include <sysexits.h>
#include <unistd.h>
#include <strings.h>
#include <stdatomic.h>

#include "misc.h"
#include "status.h"
#include "radio.h"
#include "config.h"
#include "airspy.h"
#include "sched.h"
#include "defaults.h"

// Global variables set by config file options
extern int Verbose;
extern char const *Description;
extern char const *Serial;


// Anything generic should be in 'struct frontend' section 'sdr' in radio.h
enum state {
  STOPPED,
  STARTING,
  STOPPING,
  RUNNING
};
struct sdrstate {
  struct frontend *frontend;  // Avoid references to external globals
  struct airspy_device *device;    // Opaque pointer

  uint32_t sample_rates[20];
  uint64_t SN; // Serial number

  bool antenna_bias; // Bias tee on/off

  // Tuning
  double converter;   // Upconverter base frequency (usually 120 MHz)
  double offset; // 1/4 of sample rate in real mode; 0 in complex mode

  // AGC
  bool software_agc;
  bool linearity; // Use linearity gain tables; default is sensitivity
  int gainstep; // Airspy gain table steps (0-21), higher numbers == higher gain
  float agc_energy; // Integrated energy
  int agc_samples; // Samples represented in energy
  float high_threshold;
  float low_threshold;
  float scale;         // Scale samples for #bits and front end gain

  // Settings from the config file, kept so they can be restored after a USB failure
  bool lna_agc;
  bool mixer_agc;
  int lna_gain;   // -1 if not set
  int mixer_gain; // -1 if not set
  int vga_gain;   // -1 if not set
  double tuned;   // Last frequency asked of the tuner, 0 if none yet

  // Recovery from USB failures (cable pulled or disturbed, device reset)
  pthread_mutex_t lock; // Protects device and the hardware settings; held by the sample callback
  _Atomic uint64_t callbacks; // Sample callbacks so far, to see a stalled stream
  bool close_ok;  // libairspy can close a handle whose stream failed (1.0.11 and later)
  int abandoned;  // Failed handles left open because closing them is unsafe (libairspy 1.0.10)

  pthread_t cmd_thread;
  pthread_t monitor_thread;
  _Atomic enum state state;
};

// Taken from Airspy library driver source
#define GAIN_COUNT (22)
uint8_t airspy_linearity_vga_gains[GAIN_COUNT] = {     13, 12, 11, 11, 11, 11, 11, 10, 10, 10, 10, 10, 10, 10, 10, 10, 9, 8, 7, 6, 5, 4 };
uint8_t airspy_linearity_mixer_gains[GAIN_COUNT] = {   12, 12, 11,  9,  8,  7,  6,  6,  5,  0,  0,  1,  0,  0,  2,  2, 1, 1, 1, 1, 0, 0 };
uint8_t airspy_linearity_lna_gains[GAIN_COUNT] = {     14, 14, 14, 13, 12, 10,  9,  9,  8,  9,  8,  6,  5,  3,  1,  0, 0, 0, 0, 0, 0, 0 };
uint8_t airspy_sensitivity_vga_gains[GAIN_COUNT] = {   13, 12, 11, 10,  9,  8,  7,  6,  5,  5,  5,  5,  5,  4,  4,  4, 4, 4, 4, 4, 4, 4 };
uint8_t airspy_sensitivity_mixer_gains[GAIN_COUNT] = { 12, 12, 12, 12, 11, 10, 10,  9,  9,  8,  7,  4,  4,  4,  3,  2, 2, 1, 0, 0, 0, 0 };
uint8_t airspy_sensitivity_lna_gains[GAIN_COUNT] = {   14, 14, 14, 14, 14, 14, 14, 14, 14, 13, 12, 12,  9,  9,  8,  7, 6, 5, 3, 2, 1, 0 };


static char const *Airspy_keys[] = {
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

static bool Name_set = false;
static float Power_alpha = 0.05; // Calculate this properly someday
static int set_correct_freq(struct sdrstate *sdr,double freq);
static int rx_callback(airspy_transfer *transfer);
static void *airspy_monitor(void *p);
static double true_freq(uint64_t freq);
static int set_gain(struct sdrstate *sdr,int gainstep);
static int apply_settings(struct sdrstate *sdr,int gainstep);
static void drop_device(struct sdrstate *sdr,bool started);
static void reopen(struct sdrstate *sdr);

// Recovery timing, in the monitor's 100 ms polls
#define STALL_POLLS 20     // No samples for 2 s: the stream has stalled
#define MAX_DELAY_POLLS 50 // Retry the open at least every 5 s
#define GIVEUP_POLLS 100   // Attached for 10 s but will not open: give up and exit
#define MAX_ABANDONED 16   // Each failed handle left open keeps about 4 MB of libairspy buffers

int airspy_setup(struct frontend * const frontend,dictionary * const Dictionary,char const * const section){
  assert(Dictionary != NULL);

  struct sdrstate * const sdr = calloc(1,sizeof(struct sdrstate));
  assert(sdr != NULL);
  // Cross-link generic and hardware-specific control structures
  sdr->frontend = frontend;
  frontend->context = sdr;
  {
    char const *device = config_getstring(Dictionary,section,"device",section);
    if(strcasecmp(device,"airspy") != 0)
      return -1; // Not for us
  }
  config_validate_section(stderr,Dictionary,section,Airspy_keys,NULL);
  {
    int ret;
    if((ret = airspy_init()) != AIRSPY_SUCCESS){
      fprintf(stderr,"airspy_init() failed: %s\n",airspy_error_name(ret));
      return -1;
    }
  }
  {
    // Determine the serial number we'll look for
    sdr->SN = 0;
    extern char const *Serial; // Defined in main.c, set from -s on command line
    char const *sn = NULL;
    if(Serial != NULL && strlen(Serial) > 0){
      fprintf(stderr,"Serial '%s' specified on command line\n",Serial);
      sn = Serial;
      char const *colon = strchr(sn,':');
      if(colon != NULL)
	sn = colon+1;
      char *endptr = NULL;
      sdr->SN = strtoull(sn,&endptr,16);
      if(endptr == NULL || *endptr != '\0')
	fprintf(stderr,"Invalid serial number %s in section %s\n",sn,section);
    }
    // If not successfully set from command line, use keyword in config file, if any
    if(sdr->SN == 0 && (sn = config_getstring(Dictionary,section,"serial",NULL)) != NULL){
      char *endptr = NULL;
      sdr->SN = strtoull(sn,&endptr,16);
      if(endptr == NULL || *endptr != '\0')
	fprintf(stderr,"Invalid serial number %s in section %s\n",sn,section);
    }
    // If it still hasn't been set, enumerate and pick one
    if(sdr->SN == 0){
      // Specific serial number not specified, enumerate and pick the first one
      int n_serials = 100; // ridiculously large
      uint64_t serials[n_serials];

      n_serials = airspy_list_devices(serials,n_serials); // Return actual number
      if(n_serials <= 0){
	fprintf(stderr,"No airspy devices found\n");
	return -1;
      }
      flockfile(stderr);
      fprintf(stderr,"Discovered airspy device serial%s:",n_serials > 1 ? "s" : "");
      for(int i = 0; i < n_serials; i++){
	fprintf(stderr," %llx",(long long)serials[i]);
      }
      fprintf(stderr,"\n");
      funlockfile(stderr);
      fprintf(stderr,"Selecting %llx by default; to select another, add -snn to command line or 'serial = ' to config file\n",(long long)serials[0]);
      sdr->SN = serials[0];
    }
  }
  {
    int const ret = airspy_open_sn(&sdr->device,sdr->SN);
    if(ret != AIRSPY_SUCCESS){
      fprintf(stderr,"airspy_open(%llx) failed: %s\n",(long long)sdr->SN,airspy_error_name(ret));
      return -1;
    }
  }
  {
    airspy_lib_version_t version;
    airspy_lib_version(&version);

    // Library doesn't define, but says should be >= 128
#define VERSION_LOCAL_SIZE (128)
    char hw_version[VERSION_LOCAL_SIZE];
    airspy_version_string_read(sdr->device,hw_version,sizeof(hw_version));

    fprintf(stderr,"Airspy serial %llx, hw version %s, library version %d.%d.%d\n",
	    (long long unsigned)sdr->SN,
	    hw_version,
	    version.major_version,version.minor_version,version.revision);

    // airspy_close() in libairspy 1.0.10 calls libusb_exit() before it frees the transfers, so
    // freeing any transfer that was ever submitted reads the freed libusb context; whether that crashes
    // depends on what has reused the memory. And when the library itself stops the stream on a USB error,
    // 1.0.10 skips stopping its threads, so airspy_close() also frees the device under a thread still
    // waiting on it. Both fixed in 1.0.11 (Debian packages a 1.0.11 snapshot as 1.0.10-2 and later)
    sdr->close_ok = version.major_version > 1
      || (version.major_version == 1 && (version.minor_version > 0 || version.revision >= 11));
  }
  pthread_mutex_init(&sdr->lock,NULL);
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
      fprintf(stderr,"error, no valid sample rates!\n");
      return -1;
    }
    flockfile(stderr);
    fprintf(stderr,"%'d sample rate%s:",number_sample_rates,number_sample_rates > 1 ? "s":"");
    ret = airspy_get_samplerates(sdr->device,sdr->sample_rates,number_sample_rates);
    assert(ret == AIRSPY_SUCCESS);
    for(int n = 0; n < number_sample_rates; n++){
      fprintf(stderr," %'d",sdr->sample_rates[n]);
      if(sdr->sample_rates[n] < 1)
	break;
    }
    fprintf(stderr,"\n");
    funlockfile(stderr);
  }
  {
    frontend->samprate = sdr->sample_rates[0];  // Default to first (highest) sample rate on list
    char const *p = config_getstring(Dictionary,section,"samprate",NULL);
    if(p != NULL)
      frontend->samprate = parse_frequency(p,false);
  }
  frontend->isreal = true;
  frontend->bitspersample = 12;
  sdr->offset = frontend->samprate/4;
  sdr->converter = config_getdouble(Dictionary,section,"converter",0);
  frontend->calibrate = config_getdouble(Dictionary,section,"calibrate",0);
  frontend->rf_level_cal = NAN; // varies wildly with frequency; uncalibrated

  fprintf(stderr,"Set sample rate %'lf Hz, offset %'lf Hz\n",frontend->samprate,sdr->offset);
  frontend->max_IF = -600000;
  frontend->min_IF = -0.47 * frontend->samprate;

  sdr->gainstep = -1; // Force update first time

  // Hardware device settings, sent to the device by apply_settings() below
  sdr->linearity = config_getboolean(Dictionary,section,"linearity",false);
  sdr->software_agc = true; // On by default unless one of the hardware AGCs is turned on
  sdr->lna_agc = config_getboolean(Dictionary,section,"lna-agc",false); // default off
  if(sdr->lna_agc)
    sdr->software_agc = false;

  sdr->mixer_agc = config_getboolean(Dictionary,section,"mixer-agc",false); // default off
  if(sdr->mixer_agc)
    sdr->software_agc = false;

  sdr->lna_gain = config_getint(Dictionary,section,"lna-gain",-1);
  if(sdr->lna_gain != -1){
    frontend->lna_gain = sdr->lna_gain;
    sdr->software_agc = false;
  }
  sdr->mixer_gain = config_getint(Dictionary,section,"mixer-gain",-1);
  if(sdr->mixer_gain != -1){
    frontend->mixer_gain = sdr->mixer_gain;
    sdr->software_agc = false;
  }
  sdr->vga_gain = config_getint(Dictionary,section,"vga-gain",-1);
  if(sdr->vga_gain != -1){
    frontend->if_gain = sdr->vga_gain;
    sdr->software_agc = false;
  }
  int gainstep = config_getint(Dictionary,section,"gainstep",-1);
  if(gainstep >= 0){
    if(gainstep > GAIN_COUNT-1)
      gainstep = GAIN_COUNT-1;
  } else if(sdr->software_agc){
    gainstep = GAIN_COUNT-1; // Start AGC with max gain step
  }
  sdr->antenna_bias = config_getboolean(Dictionary,section,"bias",false);
  {
    char const * const p = config_getstring(Dictionary,section,"description",Description ? Description : "airspy");
    if(p != NULL){
      strlcpy(frontend->description,p,sizeof(frontend->description));
      Description = p;
    }
  }
  double init_frequency = 0;
  {
    char const *p = config_getstring(Dictionary,section,"frequency",NULL);
    if(p != NULL)
      init_frequency = parse_frequency(p,false);
  }
  sdr->tuned = init_frequency; // 0: leave it untuned until a channel asks
  {
    int const ret = apply_settings(sdr,gainstep);
    if(ret != AIRSPY_SUCCESS){
      fprintf(stderr,"airspy %llx: setting up the device failed: %s\n",(long long unsigned)sdr->SN,airspy_error_name(ret));
      return -1;
    }
  }
  frontend->rf_gain = frontend->lna_gain + frontend->mixer_gain + frontend->if_gain;
  fprintf(stderr,"Software AGC %d; linearity %d, LNA AGC %d, Mix AGC %d, LNA gain %d, Mix gain %d, VGA gain %d, gainstep %d, bias tee %d\n",
	  sdr->software_agc,sdr->linearity,sdr->lna_agc,sdr->mixer_agc,frontend->lna_gain,frontend->mixer_gain,frontend->if_gain,gainstep,sdr->antenna_bias);

  if(sdr->software_agc){
    float const dh = config_getfloat(Dictionary,section,"agc-high-threshold",-10.0);
    sdr->high_threshold = dB2power(-fabsf(dh));
    float const dl = config_getfloat(Dictionary,section,"agc-low-threshold",-40.0);
    sdr->low_threshold = dB2power(-fabsf(dl));
    fprintf(stderr,"AGC thresholds: high %.1f dBFS, low %.1f dBFS\n",dh,dl);
  }
  if(init_frequency != 0){
    frontend->lock = true;
    fprintf(stderr,"Locked tuner frequency %'.3lf Hz\n",init_frequency);
  }
  return 0;
}
int airspy_startup(struct frontend * const frontend){
  struct sdrstate * const sdr = (struct sdrstate *)frontend->context;
  while(true){
    enum state s = STOPPED;
    if(atomic_compare_exchange_strong(&sdr->state,&s,STARTING))
      break;
    if(s == RUNNING)
      return 0; // Already running
    usleep(10000); // 10 ms
  }
  sdr->scale = scale_AD(frontend); // set scaling now that we know the forward FFT size
  pthread_create(&sdr->monitor_thread,NULL,airspy_monitor,sdr); // prio gets set in first callback
  atomic_store(&sdr->state,RUNNING);
  fprintf(stderr,"airspy running\n");
  return 0;
}

int airspy_shutdown(struct frontend * const frontend){
  struct sdrstate * const sdr = (struct sdrstate *)frontend->context;
  while(true){
    enum state s = RUNNING;
    if(atomic_compare_exchange_strong(&sdr->state,&s,STOPPING))
      break;
    if(s == STOPPED)
      return 0;
    usleep(10000);
  }
  pthread_join(sdr->cmd_thread,NULL);
  pthread_join(sdr->monitor_thread,NULL);
  atomic_store(&sdr->state,STOPPED);
  fprintf(stderr,"airspy stopped\n");
  return 0;
}

static bool running(struct sdrstate *sdr){
  enum state const s = atomic_load(&sdr->state);
  return s == RUNNING || s == STARTING;
}
// Sleep for some 100 ms polls; false if the front end is being shut down
static bool wait_polls(struct sdrstate *sdr,int polls){
  for(int i = 0; i < polls && running(sdr); i++)
    usleep(100000);
  return running(sdr);
}

// Starts the stream, then watches it. When it stops on its own (USB error, device gone) or stalls,
// the device is dropped and opened again by serial number with all its settings.
// Only this thread changes sdr->device after setup
static void *airspy_monitor(void *p){
  struct sdrstate * const sdr = (struct sdrstate *)p;
  assert(sdr != NULL);
  pthread_setname("airspy-mon");
  stick_core();

  if(sdr->device == NULL)
    reopen(sdr); // Lost while the front end was stopped
  else {
    int const ret = airspy_start_rx(sdr->device,rx_callback,sdr);
    if(ret != AIRSPY_SUCCESS){
      fprintf(stderr,"airspy %llx: start failed: %s\n",(long long unsigned)sdr->SN,airspy_error_name(ret));
      drop_device(sdr,true);
      reopen(sdr);
    }
  }
  uint64_t last = atomic_load(&sdr->callbacks);
  int idle = 0;
  bool restarted = false; // Stream restarted on the same handle after a stall, no samples since
  while(running(sdr)){
    usleep(100000); // 10 hz
    if(sdr->device == NULL)
      break; // reopen() gave up because we're stopping
    long long unsigned const sn = (long long unsigned)sdr->SN;
    uint64_t const n = atomic_load(&sdr->callbacks);
    if(airspy_is_streaming(sdr->device)){
      if(n != last){
	last = n;
	idle = 0;
	restarted = false;
	continue;
      }
      if(++idle < STALL_POLLS)
	continue;
      idle = 0;
      if(!restarted){
	// libairspy still thinks it's streaming: first try a stop and start on the same handle,
	// as when the last channel closes and a new one opens
	fprintf(stderr,"airspy %llx: no samples for 2 s, restarting the stream\n",sn);
	restarted = true;
	airspy_stop_rx(sdr->device);
	int const ret = airspy_start_rx(sdr->device,rx_callback,sdr);
	if(ret == AIRSPY_SUCCESS)
	  continue;
	fprintf(stderr,"airspy %llx: restart failed: %s, reopening\n",sn,airspy_error_name(ret));
      } else
	fprintf(stderr,"airspy %llx: still no samples after a restart, reopening\n",sn);
    } else
      fprintf(stderr,"airspy %llx: stream stopped by libairspy (USB error or device gone), reopening\n",sn);

    drop_device(sdr,true);
    reopen(sdr);
    last = atomic_load(&sdr->callbacks);
    idle = 0;
    restarted = false;
  }
  if(sdr->device != NULL)
    airspy_stop_rx(sdr->device);
  return 0;
}

// Stop using the device after a failure. 'started' if airspy_start_rx() was ever called on it
static void drop_device(struct sdrstate * const sdr,bool const started){
  struct airspy_device * const dev = sdr->device;
  if(dev == NULL)
    return;
  bool const lib_stopped = !airspy_is_streaming(dev);
  pthread_mutex_lock(&sdr->lock);
  sdr->device = NULL; // Callbacks still coming from it are ignored; tuning is kept for the new one
  pthread_mutex_unlock(&sdr->lock);

  if(sdr->close_ok || !started){
    airspy_close(dev); // Also stops the stream
    return;
  }
  // libairspy 1.0.10 (see airspy_setup): never airspy_close() a handle that has streamed.
  // While it still thinks it's streaming (a stall), airspy_stop_rx() does stop and join its threads.
  // Leave the handle open. If the device stayed attached its interface stays claimed,
  // so it won't open again and reopen() eventually exits for a restart
  if(!lib_stopped)
    airspy_stop_rx(dev);
  if(++sdr->abandoned > MAX_ABANDONED){
    fprintf(stderr,"airspy %llx: %d failed handles left open, exiting radiod\n",(long long unsigned)sdr->SN,sdr->abandoned);
    exit(EX_NOINPUT);
  }
  fprintf(stderr,"airspy %llx: libairspy before 1.0.11 cannot close a failed handle safely; leaving it open (%d so far)\n",
	  (long long unsigned)sdr->SN,sdr->abandoned);
}

// Open the device again by serial number, restore every setting and restart the stream.
// Returns once it's streaming, or when the front end is being shut down.
// The samples missed meanwhile are not replaced: the input filter's blocks simply carry on, and
// with them every channel's RTP timestamps, so the outage is seen only against wall clock time.
// Zeros for the whole gap would push seconds of blocks through every channel at once
static void reopen(struct sdrstate * const sdr){
  long long unsigned const sn = (long long unsigned)sdr->SN;
  int delay = 5;    // Polls before the next attempt, doubling up to MAX_DELAY_POLLS
  int waited = 0;   // Polls since the failure
  int attached = 0; // Polls the device has been attached but would not open
  for(int attempt = 1; wait_polls(sdr,delay); attempt++){
    waited += delay;
    struct airspy_device *dev = NULL;
    int ret = airspy_open_sn(&dev,sdr->SN);
    if(ret == AIRSPY_SUCCESS){
      pthread_mutex_lock(&sdr->lock);
      sdr->device = dev;
      sdr->agc_energy = 0;
      sdr->agc_samples = 0;
      ret = apply_settings(sdr,sdr->gainstep);
      pthread_mutex_unlock(&sdr->lock);
      if(ret != AIRSPY_SUCCESS){
	fprintf(stderr,"airspy %llx: reopened, but setting it up failed: %s\n",sn,airspy_error_name(ret));
	drop_device(sdr,false);
      } else if((ret = airspy_start_rx(dev,rx_callback,sdr)) != AIRSPY_SUCCESS){
	fprintf(stderr,"airspy %llx: reopened, but starting it failed: %s\n",sn,airspy_error_name(ret));
	drop_device(sdr,true);
      } else {
	fprintf(stderr,"airspy %llx: streaming again after %.1f s (attempt %d)\n",sn,waited / 10.,attempt);
	return;
      }
    } else if(attempt == 1 || attempt % 10 == 0)
      fprintf(stderr,"airspy %llx: not back yet (%s), still trying\n",sn,airspy_error_name(ret));

    // Attached, yet it won't open: something else holds it, maybe a handle we had to leave open
    uint64_t serials[20];
    int const n = airspy_list_devices(serials,20);
    bool present = false;
    for(int i = 0; i < n; i++)
      if(serials[i] == sdr->SN)
	present = true;
    if(!present)
      attached = 0;
    else if((attached += delay) >= GIVEUP_POLLS){
      fprintf(stderr,"airspy %llx: attached but would not open for %d s, exiting radiod\n",sn,GIVEUP_POLLS/10);
      exit(EX_NOINPUT);
    }
    delay *= 2;
    if(delay > MAX_DELAY_POLLS)
      delay = MAX_DELAY_POLLS;
  }
}

static int rx_callback(airspy_transfer *transfer){
  assert(transfer != NULL);
  struct sdrstate * const sdr = (struct sdrstate *)transfer->ctx;
  assert(sdr != NULL);
  struct frontend * const frontend = sdr->frontend;
  assert(frontend != NULL);

  // Shouldn't have to set real-time priority in the callback, but it doesn't
  // seem to get inherited by pthread_create() despite the documentation
  // At least this works
  if(!Name_set){
    pthread_setname("airspy-cb");
    Name_set = true;
  }
  pthread_mutex_lock(&sdr->lock);
  if(transfer->device != sdr->device){
    // From a handle dropped after a failure: keep its samples out, ask it to stop
    pthread_mutex_unlock(&sdr->lock);
    return -1;
  }
  atomic_fetch_add_explicit(&sdr->callbacks,1,memory_order_relaxed);
  if(transfer->dropped_samples)
    fprintf(stderr,"dropped %'lld\n",(long long)transfer->dropped_samples);

  assert(transfer->sample_type == AIRSPY_SAMPLE_RAW);
  int const sampcount = transfer->sample_count;
  float * wptr = frontend->in.input_write_pointer.r;
  uint32_t const *up = (uint32_t *)transfer->samples;
  assert(wptr != NULL);
  assert(up != NULL);
  uint64_t in_energy = 0;
  int over;
#if defined(__x86_64__)
  if(__builtin_cpu_supports("avx2") &&  __builtin_cpu_supports("popcnt"))
    over = airspy_unpack_avx2(wptr,up,sampcount,(float)sdr->scale,&in_energy);
  else
#endif
    over = airspy_unpack(wptr,up,sampcount,(float)sdr->scale,&in_energy);
  if(over){
    frontend->overranges += over;
    frontend->samp_since_over = 0;
  } else
    frontend->samp_since_over += sampcount;

  frontend->samples += sampcount;
  write_rfilter(&frontend->in,NULL,sampcount); // Update write pointer, invoke FFT
  if(sampcount != 0)
    frontend->if_power += Power_alpha * ((float)in_energy / sampcount - frontend->if_power);
  if(sdr->software_agc){
    // Integrate A/D energy over A/D averaging period
    sdr->agc_energy += in_energy;
    sdr->agc_samples += sampcount;
    if(sdr->agc_samples >= frontend->samprate/10){ // Time to re-evaluate after 100 ms
      float avg_agc_power = scale_ADpower2FS(frontend) * sdr->agc_energy / sdr->agc_samples;
      int ret = AIRSPY_SUCCESS;
      if(avg_agc_power < sdr->low_threshold){
	if(Verbose)
	  fprintf(stderr,"AGC power %.1f dBFS\n",power2dB(avg_agc_power));
	ret = set_gain(sdr,sdr->gainstep + 1);
      } else if(avg_agc_power > sdr->high_threshold){
	if(Verbose)
	  fprintf(stderr,"AGC power %.1f dBFS\n",power2dB(avg_agc_power));
	ret = set_gain(sdr,sdr->gainstep - 1);
      }
      if(ret != AIRSPY_SUCCESS)
	fprintf(stderr,"airspy %llx: AGC gain change failed: %s\n",(long long unsigned)sdr->SN,airspy_error_name(ret));
      // Reset integrator
      sdr->agc_energy = 0;
      sdr->agc_samples = 0;
    }
  }
  pthread_mutex_unlock(&sdr->lock);
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

// set the airspy tuner to the requested frequency, applying:
// Spyverter converter offset (120 MHz, or 0 if not in use)
// TCXO calibration offset
// Fs/4 = 5 MHz offset (firmware assumes library real->complex conversion, which we don't use)
// Apply 820T synthesizer tuning step model

// the TCXO calibration offset is a holdover from the Funcube dongle and doesn't
// really fit the Airspy with its internal factory calibration
// All this really works correctly only with a gpsdo, forcing the calibration offset to 0
static int set_correct_freq(struct sdrstate * const sdr,double const freq){
  struct frontend * const frontend = sdr->frontend;
  sdr->tuned = freq;
  // sdr->converter refers to an upconverter, so it's added to the frequency we request
  int64_t const intfreq = lrint((freq + sdr->converter)/ (1 + frontend->calibrate));
  int ret = AIRSPY_SUCCESS;
  if(sdr->device != NULL) // Otherwise the device is being reopened, and gets it then
    ret = airspy_set_freq(sdr->device,(uint32_t)(intfreq - sdr->offset));
  double const tf = true_freq(intfreq);
  frontend->frequency = tf * (1 + frontend->calibrate) - sdr->converter;
  return ret;
}
double airspy_tune(struct frontend * const frontend,double const f){
  if(frontend->lock)
    return frontend->frequency;
  struct sdrstate * const sdr = frontend->context;
  pthread_mutex_lock(&sdr->lock);
  int const ret = set_correct_freq(sdr,f);
  double const actual = frontend->frequency;
  pthread_mutex_unlock(&sdr->lock);
  if(ret != AIRSPY_SUCCESS)
    fprintf(stderr,"airspy %llx: tuning to %'.3lf Hz failed: %s\n",(long long unsigned)sdr->SN,f,airspy_error_name(ret));
  return actual;
}

// Send every setting to the device: at setup, and to a new handle after a USB failure
// Caller holds sdr->lock (not needed during setup)
static int apply_settings(struct sdrstate * const sdr,int const gainstep){
  struct frontend * const frontend = sdr->frontend;
  struct airspy_device * const dev = sdr->device;
  int ret;
  if((ret = airspy_set_packing(dev,1)) != AIRSPY_SUCCESS
     || (ret = airspy_set_sample_type(dev,AIRSPY_SAMPLE_RAW)) != AIRSPY_SUCCESS
     || (ret = airspy_set_samplerate(dev,(uint32_t)frontend->samprate)) != AIRSPY_SUCCESS
     || (ret = airspy_set_lna_agc(dev,sdr->lna_agc)) != AIRSPY_SUCCESS
     || (ret = airspy_set_mixer_agc(dev,sdr->mixer_agc)) != AIRSPY_SUCCESS)
    return ret;
  if(sdr->lna_gain != -1 && (ret = airspy_set_lna_gain(dev,(uint8_t)sdr->lna_gain)) != AIRSPY_SUCCESS)
    return ret;
  if(sdr->mixer_gain != -1 && (ret = airspy_set_mixer_gain(dev,(uint8_t)sdr->mixer_gain)) != AIRSPY_SUCCESS)
    return ret;
  if(sdr->vga_gain != -1 && (ret = airspy_set_vga_gain(dev,(uint8_t)sdr->vga_gain)) != AIRSPY_SUCCESS)
    return ret;
  if(gainstep >= 0){
    sdr->gainstep = -1; // Force the update
    if((ret = set_gain(sdr,gainstep)) != AIRSPY_SUCCESS)
      return ret;
  }
  if((ret = airspy_set_rf_bias(dev,sdr->antenna_bias)) != AIRSPY_SUCCESS)
    return ret;
  if(sdr->tuned != 0 && (ret = set_correct_freq(sdr,sdr->tuned)) != AIRSPY_SUCCESS)
    return ret;
  return AIRSPY_SUCCESS;
}

static int set_gain(struct sdrstate * const sdr,int gainstep){
  struct frontend * const frontend = sdr->frontend;
  if(gainstep < 0)
    gainstep = 0;
  else if(gainstep >= GAIN_COUNT)
    gainstep = GAIN_COUNT-1;

  int ret = AIRSPY_SUCCESS;
  if(gainstep != sdr->gainstep){
    sdr->gainstep = gainstep; // Kept even if the device fails, so it's restored when reopened
    int const tab = GAIN_COUNT - 1 - sdr->gainstep;
    if(sdr->linearity){
      ret = airspy_set_linearity_gain(sdr->device,(uint8_t)sdr->gainstep);
      frontend->if_gain = airspy_linearity_vga_gains[tab];
      frontend->mixer_gain = airspy_linearity_mixer_gains[tab];
      frontend->lna_gain = airspy_linearity_lna_gains[tab];
    } else {
      ret = airspy_set_sensitivity_gain(sdr->device,(uint8_t)sdr->gainstep);
      frontend->if_gain = airspy_sensitivity_vga_gains[tab];
      frontend->mixer_gain = airspy_sensitivity_mixer_gains[tab];
      frontend->lna_gain = airspy_sensitivity_lna_gains[tab];
    }
    frontend->rf_gain = frontend->lna_gain + frontend->mixer_gain + frontend->if_gain;
    sdr->scale = scale_AD(frontend);
    if(Verbose)
      fprintf(stderr,"New gainstep %d: LNA = %d, mixer = %d, vga = %d\n",gainstep,
	     frontend->lna_gain,frontend->mixer_gain,frontend->if_gain);
  }
  return ret;
}
