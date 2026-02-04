
// linked-in module for rx888 Mk ii for ka9q-radio's radiod
// Accept control commands from UDP socket
//
// Copyright (c)  2021 Ruslan Migirov <trapi78@gmail.com>
// Credit: https://github.com/rhgndf/rx888_stream
// Copyright (c)  2023 Franco Venturi K4VZ
// Copyright (c)  2023 Phil Karn KA9Q

// VHF tuner support by K4VZ July 2024
// Note: VHF tuner does not work yet -- KA9Q, 17 Aug 2024

#define _GNU_SOURCE 1
#include <assert.h>
#include <pthread.h>
#include <libusb-1.0/libusb.h>
#include <iniparser/iniparser.h>
#if defined(linux)
#include <bsd/string.h>
#endif
#include <sysexits.h>
#include <unistd.h>
#include <strings.h>
#include <assert.h>

#include "misc.h"
#include "status.h"
#include "config.h"
#include "radio.h"
#include "rx888.h"
#include "ezusb.h"

#define INPUT_PRIORITY 95

static int64_t const MIN_SAMPRATE =      1000000; // 1 MHz, in ltc2208 spec
static int64_t const MAX_SAMPRATE =    130000000; // 130 MHz, in ltc2208 spec
static int64_t const DEFAULT_SAMPRATE = 64800000; // Synthesizes cleanly from 27 MHz reference
static double const NYQUIST = 0.47;  // Upper end of usable bandwidth, relative to 1/2 sample rate
static double const AGC_UPPER_LIMIT = -15.0;   // Reduce RF gain if A/D level exceeds this in dBFS
static double const AGC_LOWER_LIMIT = -26.0;   // Increase RF gain if level is below this in dBFS
static int const AGC_INTERVAL = 1;           // Seconds between runs of AGC loop
static double const START_GAIN = 10.0;         // Initial VGA gain, dB
static double const PTC  = 0.1; // 100 ms time constant for computing Power_smooth
static double const DEFAULT_GAINCAL = +1.4;
// smoothing alpha to block DC. Don't make too large (too quick) because the corrections are applied once per
// (256K) block to minimize loss of precision, and the lag can cause instability
static double const DC_ALPHA = 4e-7;

// Reference frequency for Si5351 clock generator
static int64_t const MIN_REFERENCE = 10e6;  //  10 MHz
static int64_t const MAX_REFERENCE = 100e6; // 100 MHz
static int64_t const DEFAULT_REFERENCE = 27e6;
// Max allowable error on reference; 1e-4 = 100 ppm. Mainly to catch entry scaling errors
static double const MAX_CALIBRATE = 1e-4;

#if 0
// Min and Max frequency for VHF/UHF tuner
static int64_t const MIN_FREQUENCY = 50e6;   //  50 MHz ?
static int64_t const MAX_FREQUENCY = 2000e6; // 2000 MHz
static uint32_t const R828D_FREQ = 16000000;     // R820T reference frequency
static int64_t const R828D_IF_CARRIER = 4570000;
#endif

static double Power_smooth; // Arbitrary exponential smoothing factor for front end power estimate
int Ezusb_verbose = 0; // Used by ezusb.c
// Global variables set by config file options in main.c
extern int Verbose;
extern volatile bool Stop_transfers; // Flag to stop receive thread upcalls; defined in main.c
extern char const *Description;

// Hardware-specific stuff.
// Anything generic should be moved to 'struct frontend' under sdr in radio.h
struct sdrstate {
  struct frontend *frontend;  // Avoid references to external globals

  // USB stuff
  struct libusb_device_handle *dev_handle;
  uint64_t serial;             // Device serial number
  struct libusb_config_descriptor *config;
  unsigned int pktsize;
  unsigned int transfer_size;  // Size of data transfers performed so far (unused)
  unsigned int transfer_index; // Write index into the transfer_size array (unused)
  struct libusb_transfer **transfers; // List of transfer structures.
  unsigned char **databuffers;        // List of data buffers.
  long long last_callback_time;

  // USB transfer
  int xfers_in_progress;
  unsigned int queuedepth; // Number of requests to queue
  unsigned int reqsize;    // Request size in number of packets
  unsigned long success_count;  // Number of successful transfers
  unsigned long failure_count;  // Number of failed transfers

  // RF Hardware
  double high_threshold;
  double low_threshold;

  int64_t reference;
  bool randomizer;
  bool dither;
  uint32_t gpios;
  uint64_t last_sample_count; // Used to verify sample rate
  int64_t last_count_time;
  bool message_posted; // Clock rate error posted last time around
  double scale;        // Scale samples for #bits and front end gain
  int undersample;     // Use undersample aliasing on baseband input for VHF/UHF. n = 1 => no undersampling
  double dc_offset;    // A/D offset, units, used only to adjust power reading. It just goes into the FFT DC bin
  pthread_t cmd_thread;
  pthread_t proc_thread;
  pthread_t agc_thread;
  bool ms_int;
};

static bool hack_no_usb_reset = false;

static void rx_callback(struct libusb_transfer *transfer);
static int rx888_usb_init(struct sdrstate *sdr,const char *firmware,unsigned int queuedepth,unsigned int reqsize);
static void rx888_set_dither_and_randomizer(struct sdrstate *sdr,bool dither,bool randomizer);
static void rx888_set_att(struct sdrstate *sdr,double att,bool vhf);
static void rx888_set_gain(struct sdrstate *sdr,double gain,bool vhf);
static double rx888_set_samprate(struct sdrstate *sdr,int64_t reference,int64_t samprate);
static void rx888_set_hf_mode(struct sdrstate *sdr);
static int rx888_start_rx(struct sdrstate *sdr,libusb_transfer_cb_fn callback);
static void rx888_stop_rx(struct sdrstate *sdr);
static void rx888_close(struct sdrstate *sdr);
static void free_transfer_buffers(unsigned char **databuffers,struct libusb_transfer **transfers,unsigned int queuedepth);
static double val2gain(int g);
static int gain2val(double gain);
static void *proc_rx888(void *arg);
static void *agc_rx888(void *arg);
#if 0
static double rx888_set_tuner_frequency(struct sdrstate *sdr,double frequency);
static double actual_freq(double frequency);
#endif



#define N_USB_SPEEDS 6
static char const *usb_speeds[N_USB_SPEEDS] = {
  "unknown",
  "Low (1.5 Mb/s)",
  "Full (12 Mb/s)",
  "High (480 Mb/s)",
  "Super (5 Gb/s)",
  "Super+ (10Gb/s)"
};

static char const *Rx888_keys[] = {
  "agc-high-threshold",
  "agc-low-threshold",
  "att", // synonym for atten
  "atten", // fixed attenuator gain, dB. Either -10 or +10 is interprepted as 10 dB of attenuation
  "calibrate", // Set to zero when an external GPSDO or Rb/Cs reference is used
  "description",
  "device",
  "dither",  // Dither A/D LSB, not very useful with noisy antenna signals
  "featten", // synonym for atten
  "fegain",  // synonym for gain
  "firmware",
  "frequency", // Used only in VHF mode (not yet implemented)
  "gain",    // fixed VGA gain, dB
  "gaincal",
  "gainmode", // Obsolete
  "library",
  "queuedepth",
  "rand",    // Hardware randomizer, probably doesn't help reduce spurs but it should be tested
  "reference", // Clock reference, default 27 MHz (unlike 10 MHz, nobody cares if that gets into your receiver)
  "reqsize",
  "rfatten", // synonym for atten
  "rfgain", // synonym for gain
  "rxgain", // synomym for gain
  "samprate", // 129.6/64.8 MHz are good choices with 27 MHz reference. No fractional N, and good FFT factors
  "serial",
  "undersample", // Use higher Nyquist zones. Requires removal of internal LPF and use of proper bandpass
  "hack_no_usb_reset", // Don't reset USB device after sending the STOPFX3 command--the reset seems to break alternate FX3 firmware
  NULL
};


int rx888_setup(struct frontend * const frontend,dictionary const * const dictionary,char const * const section){
  assert(dictionary != NULL);
  // Hardware-dependent setup
  {
    char const *device = config_getstring(dictionary,section,"device",section);
    if(strcasecmp(device,"rx888") != 0)
      return -1; // Not for us
  }
  config_validate_section(stderr,dictionary,section,Rx888_keys,NULL);
  struct sdrstate * const sdr = calloc(1,sizeof(struct sdrstate));
  assert(sdr != NULL);
  // Cross-link generic and hardware-specific control structures
  sdr->frontend = frontend;
  frontend->context = sdr;
  frontend->isreal = true; // Make sure the right kind of filter gets created!
  frontend->bitspersample = 16; // For gain scaling
  frontend->rf_agc = true; // On by default unless gain or atten is specified
  {
    char const *p = config_getstring(dictionary,section,"serial",NULL); // is serial specified?
    if(p != NULL){
      sdr->serial = strtoll(p,NULL,16);
    }
  }
  // Firmware file
  char const *firmware = config_getstring(dictionary,section,"firmware","SDDC_FX3.img");
  // Queue depth, default 16; 32 sometimes overflows
  int queuedepth = config_getint(dictionary,section,"queuedepth",16);
  if(queuedepth < 1 || queuedepth > 64) {
    fprintf(stderr,"Invalid queue depth %d, using 16\n",queuedepth);
    queuedepth = 16;
  }
  // Packets per transfer request, default 32
  int reqsize = config_getint(dictionary,section,"reqsize",32);
  if(reqsize < 1 || reqsize > 64) {
    fprintf(stderr,"Invalid request size %d, using 32\n",reqsize);
    reqsize = 32;
  }
  hack_no_usb_reset=config_getboolean(dictionary,section,"hack_no_usb_reset",false);
  {
    int ret;
    if((ret = rx888_usb_init(sdr,firmware,queuedepth,reqsize)) != 0){
      fprintf(stderr,"rx888_usb_init() failed\n");
      return -1;
    }
  }
  // GPIOs
  sdr->gpios = 0;
  // Enable/disable dithering
  sdr->dither = config_getboolean(dictionary,section,"dither",false);
  // Enable/output output randomization
  sdr->randomizer = config_getboolean(dictionary,section,"rand",false);
  rx888_set_dither_and_randomizer(sdr,sdr->dither,sdr->randomizer);

  // RF Gain calibration
  // WA2ZKD measured several rx888s with very consistent results
  // e.g., -90 dBm gives -91.4 dBFS with 0 dB VGA gain and 0 dB attenuation
  // If you use a preamp or converter, add its gain to gaincal
  // Note: sign convention has flipped dec 2025 to have units of dBm/FS vs FS/dbm
  // ie. an input of +1.4 dBm gives 0 dBFS with atten == rfgain == 0
  frontend->rf_level_cal = config_getdouble(dictionary,section,"gaincal",DEFAULT_GAINCAL);

  // Attenuation, default 0
  double att = fabs(config_getdouble(dictionary,section,"att",9999));
  att = fabs(config_getdouble(dictionary,section,"atten",att));
  att = fabs(config_getdouble(dictionary,section,"featten",att));
  att = fabs(config_getdouble(dictionary,section,"rfatten",att));
  if(att == 9999){
    att = 0; // AGC still on, default attenuation 0 dB (not very useful anyway)
  } else {
    // Explicitly specified, turn off AGC
    if(att > 31.5)
      att = 31.5;
    frontend->rf_agc = false;
  }
  rx888_set_att(sdr,att,false);

  // Gain Mode now automatically set by gain; gain < 0 dB -> low, gain >= 0 dB -> high
  char const *gainmode = config_getstring(dictionary,section,"gainmode",NULL);
  if(gainmode != NULL)
    fprintf(stderr,"gainmode parameter is obsolete, now set automatically\n");

  // Gain value
  double gain = config_getdouble(dictionary,section,"gain",9999);
  gain = config_getdouble(dictionary,section,"rfgain",gain);
  gain = config_getdouble(dictionary,section,"rxgain",gain);
  gain = config_getdouble(dictionary,section,"fegain",gain);
  if(gain == 9999){
    gain = START_GAIN; // Default
  } else {
    // Explicitly specifed, turn off AGC
    // should there be limits?
    frontend->rf_agc = false;
  }
  rx888_set_gain(sdr,gain,false);

  int64_t reference = DEFAULT_REFERENCE;
  {
    char const *p = config_getstring(dictionary,section,"reference",NULL);
    if(p != NULL)
      reference = llrint(parse_frequency(p,false));
  }
  if(reference < MIN_REFERENCE || reference > MAX_REFERENCE){
    fprintf(stderr,"Invalid reference frequency %'lld, forcing %'lld\n",(long long)reference,(long long)DEFAULT_REFERENCE);
    reference = DEFAULT_REFERENCE;
  }
  double calibrate = config_getdouble(dictionary,section,"calibrate",0);
  if(fabsl(calibrate) >= MAX_CALIBRATE){
    fprintf(stderr,"Unreasonable frequency calibration %.3g, setting to 0\n",calibrate);
    calibrate = 0;
  }
  int64_t samprate = DEFAULT_SAMPRATE;
  {
    char const *p = config_getstring(dictionary,section,"samprate",NULL);
    if(p != NULL)
      samprate = llrint(parse_frequency(p,false));
  }
  if(samprate < MIN_SAMPRATE){
    fprintf(stderr,"Invalid sample rate %'lld, forcing %'lld\n",(long long)samprate,(long long)MIN_SAMPRATE);
    samprate = MIN_SAMPRATE;
  }
  if(samprate > MAX_SAMPRATE){
    fprintf(stderr,"Invalid sample rate %'lld, forcing %'lld\n",(long long)samprate,(long long)MAX_SAMPRATE);
    samprate = MAX_SAMPRATE;
  }
  sdr->reference = reference * (1 + calibrate);
  usleep(5000);
  samprate = rx888_set_samprate(sdr,sdr->reference,samprate); // Update to actual samprate, if different
  frontend->samprate = samprate;

  sdr->undersample = config_getint(dictionary,section,"undersample",1);
  if(sdr->undersample < 1){
    fprintf(stderr,"rx888 undersample must be >= 1, ignoring\n");
    sdr->undersample = 1;
  }
  int mult = sdr->undersample / 2;
  frontend->frequency = frontend->samprate * mult;
  if(sdr->undersample & 1){
    // Somewhat arbitrary. See https://ka7oei.blogspot.com/2024/12/frequency-response-of-rx-888-sdr-at.html
    frontend->min_IF = 15000;
    frontend->max_IF = NYQUIST * samprate;
  } else {
    frontend->min_IF = -NYQUIST * samprate;
    frontend->max_IF = -15000;
  }
  // start clock
  control_send_byte(sdr->dev_handle,I2CWFX3,SI5351_ADDR,SI5351_REGISTER_PLL_RESET,SI5351_VALUE_PLLA_RESET);
  // power on clock 0

  uint8_t clock_control = SI5351_VALUE_CLK_SRC_MS | SI5351_VALUE_CLK_DRV_8MA | SI5351_VALUE_MS_SRC_PLLA;

  // The SI5351_VALUE_MS_INT can be set only if the output divisor is an integer. It is in the original code, which sets it to 6.
  if(sdr->ms_int)
    clock_control |= SI5351_VALUE_MS_INT;

  control_send_byte(sdr->dev_handle,I2CWFX3,SI5351_ADDR,SI5351_REGISTER_CLK_BASE+0,clock_control);
  {
    char const *p = config_getstring(dictionary,section,"description",Description ? Description : "rx888");
    if(p != NULL){
      strlcpy(frontend->description,p,sizeof(frontend->description));
      Description = p;
    }
  }
  sdr->low_threshold = config_getdouble(dictionary,section,"agc-low-threshold",AGC_LOWER_LIMIT);
  sdr->high_threshold = config_getdouble(dictionary,section,"agc-high-threshold",AGC_UPPER_LIMIT);

  double xfer_time = (double)(sdr->reqsize * sdr->pktsize) / (sizeof(int16_t) * frontend->samprate);
  // Compute exponential smoothing constant
  // Use double to avoid denormalized addition
  // value is 1 - exp(-blocktime/tc), but use expm1() function to save precision

  Power_smooth = -expm1(-xfer_time/PTC);

  fprintf(stderr,"RX888 AGC %s, nominal gain %.1f dB, actual gain %.1f dB, atten %.1f dB, gain cal %.1f dB, dither %d, randomizer %d, USB queue depth %d, USB request size %'d * pktsize %'d = %'d bytes (%g sec)\n",
	  frontend->rf_agc ? "on" : "off",
	  gain,frontend->rf_gain,frontend->rf_atten,frontend->rf_level_cal,
sdr->dither,sdr->randomizer,sdr->queuedepth,sdr->reqsize,sdr->pktsize,sdr->reqsize * sdr->pktsize,
	  xfer_time);

#if 0
  // VHF-UHF tuning
  {
    char const *p = config_getstring(dictionary,section,"frequency",NULL);
    if(p != NULL){
      if(sdr->undersample > 1){
	fprintf(stderr,"frequency = ignored in undersample mode\n");
      } else {
	double frequency = parse_frequency(p,false);
	if(frequency < MIN_FREQUENCY || frequency > MAX_FREQUENCY){
	  fprintf(stderr,"Invalid VHF/UHF frequency %'lf, ignoring\n",frequency);
	} else {
	  // VHF/UHF mode
	  double actual_frequency = rx888_set_tuner_frequency(sdr,frequency);
	  fprintf(stderr,"Actual VHF/UHF tuner frequency %'lf\n",actual_frequency);
	  frontend->frequency = actual_frequency;
	  rx888_set_att(sdr,att,true);
	  rx888_set_gain(sdr,gain,true);
	  frontend->lock = true;
	}
      }
    }
  }
#else
  frontend->frequency = 0;
#endif
  if(frontend->frequency == 0)
    rx888_set_hf_mode(sdr);
  usleep(1000000); // 1s - see SDDC_FX3 firmware

  // Experimental spur notching, works on coherent spurs only
  // What generates 1/8, 2/8, 3/8? And probably 4/8 too, though that's the Nyquist freq
  // and we don't use it
  frontend->spurs[0] = sdr->reference; // reference clock
  frontend->spurs[1] = samprate / 8;
  frontend->spurs[2] = samprate / 4;  // 2/8
  frontend->spurs[3] = (3 * samprate) / 8;
  frontend->spurs[4] = 2 * sdr->reference;
  if(samprate - 3 * sdr->reference > 0)
    frontend->spurs[5] = samprate - 3 * sdr->reference; // 3rd harmonic of reference aliased back down
  return 0;
}

// Come back here after common stuff has been set up (filters, etc)
int rx888_startup(struct frontend * const frontend){
  struct sdrstate * const sdr = (struct sdrstate *)frontend->context;
  // Start processing A/D data
  sdr->scale = scale_AD(frontend); // set scaling now that we know the forward FFT size
  pthread_create(&sdr->proc_thread,NULL,proc_rx888,sdr);
  pthread_create(&sdr->agc_thread,NULL,agc_rx888,sdr);
  fprintf(stderr,"rx888 running\n");
  return 0;
}

// command to set analog gain. Turn off AGC if it was on
double rx888_gain(struct frontend * const frontend, double gain){
  struct sdrstate * const sdr = (struct sdrstate *)frontend->context;
  if(frontend->rf_agc)
    fprintf(stderr,"manual gain setting, turning off AGC\n");
  frontend->rf_agc = false;
  rx888_set_gain(sdr,gain,sdr->undersample == 1 && frontend->frequency != 0);
  return frontend->rf_gain;
}

// command to set analog attenuation. Turn off AGC if it was on
double rx888_atten(struct frontend * const frontend, double atten){
  struct sdrstate * const sdr = (struct sdrstate *)frontend->context;
  if(frontend->rf_agc)
    fprintf(stderr,"manual atten setting, turning off AGC\n");
  frontend->rf_agc = false;
  rx888_set_att(sdr,atten,sdr->undersample == 1 && frontend->frequency != 0);
  return frontend->rf_atten;
}

// Process incoming A/D samples
static void *proc_rx888(void *arg){
  struct sdrstate * const sdr = (struct sdrstate *)arg;
  assert(sdr != NULL);
  pthread_setname("proc_rx888");

  realtime(INPUT_PRIORITY);
  stick_core();
  {
    int64_t const now = gps_time_ns();
    sdr->last_callback_time = now;
    sdr->last_count_time = now;

    int ret __attribute__ ((unused));
    ret = rx888_start_rx(sdr,rx_callback);
    assert(ret == 0);
  }
  do {
    // If the USB cable is pulled, libusb_handle_events() simply hangs
    // so use libusb_handle_events_timeout_completed() instead
    // Unfortunately it doesn't give any indication that it has timed out
    // so we check more directly how long it's been since we last got data
    // sdr->last_callback_time is set in rx_callback()
    int const maxtime = 5;
    if(gps_time_ns() > sdr->last_callback_time + maxtime * BILLION){
      Stop_transfers = true;
      fprintf(stderr,"No rx888 data for %d seconds, quitting\n",maxtime);
      break;
    }
    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    int const ret = libusb_handle_events_timeout_completed(NULL,&tv,NULL);
    if(ret != 0){
      // Apparent failure
      fprintf(stderr,"handle_events returned %d\n",ret);
      Stop_transfers = true;
    }
  } while (!Stop_transfers);

  rx888_stop_rx(sdr);
  rx888_close(sdr);
  // Can't do anything without the front end; quit entirely
  fprintf(stderr,"rx888 has aborted, exiting radiod\n");
  exit(EX_NOINPUT);
}

// Monitor power levels, record new watermarks, adjust AGC if enabled
// Also perform coarse check on sample rate, compared to system clock
static void *agc_rx888(void *arg){
  struct sdrstate * const sdr = (struct sdrstate *)arg;
  assert(sdr != NULL);
  pthread_setname("agc_rx888");
  struct frontend *frontend = sdr->frontend;
  while(1){
    sleep(AGC_INTERVAL);
    int64_t now = gps_time_ns();
    if(now >= sdr->last_count_time + 60 * BILLION){
      // Verify approximate sample rate once per minute
      int64_t const sampcount = frontend->samples - sdr->last_sample_count;
      double const rate = BILLION * (double)sampcount / (now - sdr->last_count_time);
      double const error = fabs((rate - frontend->samprate) / (double)frontend->samprate);
      sdr->last_count_time = now;
      sdr->last_sample_count = frontend->samples;
      if(error > 0.01 || sdr->message_posted){
	// Post message every time the clock is off by 1% or more, or if it has just returned to nominal
	fprintf(stderr,"RX888 measured sample rate error: %'.1lf Hz vs nominal %'lf Hz\n",
		rate,frontend->samprate);
	sdr->message_posted = (error > 0.01);
      }
    }
    double scaled_new_power = frontend->if_power * scale_ADpower2FS(frontend);
    double new_dBFS = power2dB(scaled_new_power);

    if(frontend->if_power > frontend->if_power_max){
      if(Verbose){
	// Don't print a message unless the increase is > 0.1 dB, the precision of the printf
	double scaled_old_power = frontend->if_power_max * scale_ADpower2FS(frontend);
	double old_dBFS = power2dB(scaled_old_power);
	if(new_dBFS >= old_dBFS + 0.1)
	  fprintf(stderr,"New input power high watermark: %.1f dBFS\n",new_dBFS);
      }
      frontend->if_power_max = frontend->if_power;
    }
    if(frontend->rf_agc && (new_dBFS > sdr->high_threshold || new_dBFS < sdr->low_threshold)){
      double const target_level = (sdr->high_threshold + sdr->low_threshold)/2;
      double new_gain = frontend->rf_gain - (new_dBFS - target_level);
      if(new_gain > 34)
	new_gain = 34;
      if(gain2val(new_gain) != gain2val(frontend->rf_gain)){ // only if it'll actually change
	if(Verbose)
	  fprintf(stderr,"Front end gain change from %.1f dB to %.1f dB\n",frontend->rf_gain,new_gain);
	rx888_set_gain(sdr,new_gain,false);
	// Change averaged value to speed convergence
	frontend->if_power *= dB2power(target_level - new_dBFS);
	// Unlatch high water mark
	frontend->if_power_max = 0;
      }
    }
  }
  return NULL;
}


// Callback called with incoming receiver data from A/D
static void rx_callback(struct libusb_transfer * const transfer){
  assert(transfer != NULL);
  struct sdrstate * const restrict sdr = (struct sdrstate *)transfer->user_data;
  struct frontend * const restrict frontend = sdr->frontend;
  int64_t const now = gps_time_ns();

  sdr->xfers_in_progress--;
  sdr->last_callback_time = now;

  if(transfer->status != LIBUSB_TRANSFER_COMPLETED) {
    sdr->failure_count++;
    if(Verbose > 1)
      fprintf(stderr,"Transfer %p callback status %s received %d bytes.\n",transfer,
	      libusb_error_name(transfer->status), transfer->actual_length);
    if(!Stop_transfers) {
      if(libusb_submit_transfer(transfer) == 0)
        sdr->xfers_in_progress++;
    }
    return;
  }

  // successful USB transfer
  int const size = transfer->actual_length;
  sdr->success_count++;

  // Feed directly into FFT input buffer, accumulate energy
  double in_energy = 0; // A/D energy accumulator
  int16_t const * const restrict samples = (int16_t *)transfer->buffer;
  float * const restrict wptr = frontend->in.input_write_pointer.r;
  int const sampcount = size / sizeof(int16_t);
  double delta_sum = 0;
  if(sdr->randomizer){
    for(int i=0; i < sampcount; i++){
      int32_t s = samples[i];
      s ^= (s << 31) >> 30; // Put LSB in sign bit, then shift back by one less bit to make ..ffffe or 0
      if(abs(s) >= 32767){
	frontend->overranges++;
	frontend->samp_since_over = 0;
      } else {
	frontend->samp_since_over++;
      }
      // Remove DC offset
      // Use double precision to avoid denormals
      double const e  = s - sdr->dc_offset;
      delta_sum += e;
      in_energy += e * e;
      wptr[i] = (float)(e * sdr->scale);
    }
  } else {
    for(int i=0; i < sampcount; i++){
      if(abs(samples[i]) >= 32767){
	frontend->overranges++;
	frontend->samp_since_over = 0;
      } else {
	frontend->samp_since_over++;
      }
      // Remove DC offset
      // Use double precision to avoid denormals
      double const s = samples[i] - sdr->dc_offset;
      delta_sum += s;
      in_energy += s * s;
      wptr[i] = (float)(s * sdr->scale);
    }
  }
  sdr->dc_offset += DC_ALPHA * delta_sum;
  // These blocks are kinda small, so exponentially smooth the power readings
  frontend->if_power += Power_smooth * (in_energy / sampcount - frontend->if_power);
  frontend->samples += sampcount; // Count original samples
  if(!Stop_transfers) {
    if(libusb_submit_transfer(transfer) == 0)
      sdr->xfers_in_progress++;
  }
  write_rfilter(&frontend->in,NULL,sampcount); // Update write pointer, invoke FFT if block is complete
}

static int rx888_usb_init(struct sdrstate *const sdr,const char * const firmware,unsigned int const queuedepth,unsigned int const reqsize){
  if(firmware == NULL){
    fprintf(stderr,"Firmware not loaded and not available\n");
    return -1;
  }
  char full_firmware_file[PATH_MAX] = {0};
  dist_path(full_firmware_file,sizeof(full_firmware_file),firmware);

  {
    int ret = libusb_init(NULL);
    if(ret != 0){
      fprintf(stderr,"Error initializing libusb: %s\n",
	      libusb_error_name(ret));
      return -1;
    }
  }
  uint16_t const vendor_id = 0x04b4;
  uint16_t const unloaded_product_id = 0x00f3;
  uint16_t const loaded_product_id = 0x00f1;

  if(sdr->serial != 0)
    fprintf(stderr,"Looking for rx888 serial %016llx\n",(long long)sdr->serial);

  // Search for unloaded rx888s (0x04b4:0x00f3) with the desired serial, or all such devices if no serial specified
  // and load with firmware
  libusb_device **device_list;
  ssize_t dev_count = libusb_get_device_list(NULL,&device_list);
  for(ssize_t i=0; i < dev_count; i++){
    libusb_device *device = device_list[i];
    if(device == NULL)
      break; // End of list

    struct libusb_device_descriptor desc = {0};
    int rc = libusb_get_device_descriptor(device,&desc);
    if(rc != 0){
      fprintf(stderr," libusb_get_device_descriptor() failed: %s\n",libusb_strerror(rc));
      continue;
    }
    if(desc.idVendor != vendor_id || desc.idProduct != unloaded_product_id)
      continue;

    fprintf(stderr,"found rx888 vendor %04x, device %04x",desc.idVendor,desc.idProduct);
    libusb_device_handle *handle = NULL;
    rc = libusb_open(device,&handle);
    if(rc != 0 || handle == NULL){
      fprintf(stderr,", libusb_open() failed: %s\n",libusb_strerror(rc));
      continue;
    }
    if(desc.iManufacturer){
      char manufacturer[100] = {0};
      int ret = libusb_get_string_descriptor_ascii(handle,desc.iManufacturer,(unsigned char *)manufacturer,sizeof(manufacturer));
      if(ret > 0)
	fprintf(stderr,", manufacturer '%s'",manufacturer);
    }
    if(desc.iProduct){
      char product[100] = {0};
      int ret = libusb_get_string_descriptor_ascii(handle,desc.iProduct,(unsigned char *)product,sizeof(product));
      if(ret > 0)
	fprintf(stderr,", product '%s'",product);
    }
    char serial[100] = {0};
    if(desc.iSerialNumber){
      int ret = libusb_get_string_descriptor_ascii(handle,desc.iSerialNumber,(unsigned char *)serial,sizeof(serial));
      if(ret > 0){
	fprintf(stderr,", serial '%s'",serial);
      }
    }
    // The proper serial number doesn't appear until the device is loaded with firmware, so load all we find
    fprintf(stderr,", loading rx888 firmware file %s",full_firmware_file);
    if(ezusb_load_ram(handle,full_firmware_file,FX_TYPE_FX3,IMG_TYPE_IMG,1) == 0){
      fprintf(stderr,", done\n");
      sleep(1); // how long should this be?
    } else {
      fprintf(stderr,", failed for device %d.%d (logical)\n",
	      libusb_get_bus_number(device),libusb_get_device_address(device));
    }
    libusb_close(handle);
    handle = NULL;
  }
  libusb_free_device_list(device_list,1);
  device_list = NULL;

  // Scan list again, looking for a loaded device
  libusb_device *device = NULL;
  dev_count = libusb_get_device_list(NULL,&device_list);
  for(int i=0; i < dev_count; i++){
    device = device_list[i];
    if(device == NULL)
      break; // End of list

    struct libusb_device_descriptor desc = {0};
    int rc = libusb_get_device_descriptor(device,&desc);
    if(rc != 0){
      fprintf(stderr," libusb_get_device_descriptor() failed: %s\n",libusb_strerror(rc));
      continue;
    }
    if(desc.idVendor != vendor_id || desc.idProduct != loaded_product_id)
      continue;

    fprintf(stderr,"found rx888 vendor %04x, device %04x",desc.idVendor,desc.idProduct);
    libusb_device_handle *handle = NULL;
    rc = libusb_open(device,&handle);
    if(rc != 0 || handle == NULL){
      fprintf(stderr," libusb_open() failed: %s\n",libusb_strerror(rc));
      continue;
    }
    if(desc.iManufacturer){
      char manufacturer[100] = {0};
      int ret = libusb_get_string_descriptor_ascii(handle,desc.iManufacturer,(unsigned char *)manufacturer,sizeof(manufacturer));
      if(ret > 0)
	fprintf(stderr,", manufacturer '%s'",manufacturer);
    }
    if(desc.iProduct){
      char product[100] = {0};
      int ret = libusb_get_string_descriptor_ascii(handle,desc.iProduct,(unsigned char *)product,sizeof(product));
      if(ret > 0)
	fprintf(stderr,", product '%s'",product);
    }
    char serial[100] = {0};
    if(desc.iSerialNumber){
      int ret = libusb_get_string_descriptor_ascii(handle,desc.iSerialNumber,(unsigned char *)serial,sizeof(serial));
      if(ret > 0){
	fprintf(stderr,", serial '%s'",serial);
      }
    }
    enum libusb_speed usb_speed = libusb_get_device_speed(device);
    if(usb_speed < N_USB_SPEEDS)
      fprintf(stderr,", USB speed: %s",usb_speeds[usb_speed]);
    else
      fprintf(stderr,", unknown rx888 USB speed index %d",usb_speed);

    if(usb_speed < LIBUSB_SPEED_SUPER){
      fprintf(stderr,": not at least SuperSpeed; is it plugged into a blue USB jack?\n");
      continue; // Keep looking, there just might be another
    }
    // Is this the droid we're looking for?
    uint64_t serialnum = strtoll(serial,NULL,16);
    if(sdr->serial == 0 || sdr->serial == serialnum){
      // Either the user didn't specify a serial, or this is the one he did; use it
      fprintf(stderr,", selected\n");
      sdr->dev_handle = handle;
      break;
    }
    fprintf(stderr,"\n"); // Not selected; close and keep looking
    libusb_close(handle);
    handle = NULL;
  }
  libusb_free_device_list(device_list,1);
  device_list = NULL;

  // If a device has been found, device and dev_handle will be non-NULL
  if(device == NULL || sdr->dev_handle == NULL){
    fprintf(stderr,"Error or device could not be found\n");
    goto end;
  }
  // Stop and reopen in case it was left running - KA9Q
  usleep(5000);
  command_send(sdr->dev_handle,STOPFX3,0);
  if (!hack_no_usb_reset)
  {
    int r = libusb_reset_device(sdr->dev_handle);
    if(r != 0){
      fprintf(stderr,"reset failed, %d\n",r);
    }
  }
  {
    int ret = libusb_kernel_driver_active(sdr->dev_handle,0);
    if(ret != 0){
      fprintf(stderr,"Kernel driver active. Trying to detach kernel driver\n");
      ret = libusb_detach_kernel_driver(sdr->dev_handle,0);
      if(ret != 0){
	fprintf(stderr,"Could not detach kernel driver from an interface\n");
	goto end;
      }
    }
  }
  libusb_get_config_descriptor(device, 0, &sdr->config);
  {
    int const ret = libusb_claim_interface(sdr->dev_handle, 0);
    if(ret != 0){
      fprintf(stderr, "Error claiming USB interface\n");
      goto end;
    }
  }
  {
    // All this just to get sdr->pktsize?
    struct libusb_interface_descriptor const *interfaceDesc = &(sdr->config->interface[0].altsetting[0]);
    assert(interfaceDesc != NULL);
    struct libusb_endpoint_descriptor const *endpointDesc = &interfaceDesc->endpoint[0];
    assert(endpointDesc != NULL);

    struct libusb_ss_endpoint_companion_descriptor *ep_comp = NULL;
    int const rc = libusb_get_ss_endpoint_companion_descriptor(NULL,endpointDesc,&ep_comp);
    if(rc != 0){
      fprintf(stderr,"libusb_get_ss_endpoint_companion_descriptor returned: %s (%d)\n",libusb_error_name(rc),rc);
      goto end;
    }
    assert(ep_comp != NULL);
    sdr->pktsize = endpointDesc->wMaxPacketSize * (ep_comp->bMaxBurst + 1);
    libusb_free_ss_endpoint_companion_descriptor(ep_comp);
  }
  sdr->databuffers = (u_char **)calloc(queuedepth,sizeof(u_char *));
  if(sdr->databuffers == NULL){
    fprintf(stderr,"Failed to allocate data buffers\n");
    goto end;
  }
  sdr->transfers = (struct libusb_transfer **)calloc(queuedepth,sizeof(struct libusb_transfer *));
  if(sdr->transfers == NULL){
    fprintf(stderr,"Failed to allocate transfer buffers\n");
    goto end;
  }
  for(unsigned int i = 0; i < queuedepth; i++){
    sdr->databuffers[i] = (u_char *)malloc(reqsize * sdr->pktsize);
    if(sdr->databuffers[i] == NULL)
      goto end;
    sdr->transfers[i] = libusb_alloc_transfer(0);
  }
  sdr->queuedepth = queuedepth;
  sdr->reqsize = reqsize;
  return 0;

end:;
  free_transfer_buffers(sdr->databuffers,sdr->transfers,sdr->queuedepth);

  FREE(sdr->transfers);
  FREE(sdr->databuffers);

  if(sdr->dev_handle != NULL)
    libusb_release_interface(sdr->dev_handle,0);
  sdr->dev_handle = NULL;

  if(sdr->config)
    libusb_free_config_descriptor(sdr->config);
  sdr->config = NULL;

  if(sdr->dev_handle)
    libusb_close(sdr->dev_handle);
  sdr->dev_handle = NULL;

  libusb_exit(NULL);
  return -1;
}

static void rx888_set_dither_and_randomizer(struct sdrstate *sdr,bool dither,bool randomizer){
  assert(sdr != NULL);
  if(dither)
    sdr->gpios |= DITH;

  if(randomizer)
    sdr->gpios |= RANDO;

  usleep(5000);
  command_send(sdr->dev_handle,GPIOFX3,sdr->gpios);
  sdr->dither = dither;
  sdr->randomizer = randomizer;
}

static void rx888_set_att(struct sdrstate *sdr,double att,bool vhf){
  assert(sdr != NULL);
  struct frontend *frontend = sdr->frontend;
  assert(frontend != NULL);
  usleep(5000);

  frontend->rf_atten = att;
  sdr->scale = scale_AD(frontend);
  if(!vhf){
    int const arg = (int)(att * 2);
    argument_send(sdr->dev_handle,DAT31_ATT,arg);
  } else {
    int const arg = (int)att;
    argument_send(sdr->dev_handle,R82XX_ATTENUATOR,arg);
  }
}

static void rx888_set_gain(struct sdrstate *sdr,double gain,bool vhf){
  assert(sdr != NULL);
  struct frontend *frontend = sdr->frontend;
  assert(frontend != NULL);
  usleep(5000);

  if(!vhf){
    int const arg = gain2val(gain);
    argument_send(sdr->dev_handle,AD8340_VGA,arg);
    frontend->rf_gain = val2gain(arg); // Store actual nearest value
  } else {
    int const arg = (int)gain;
    argument_send(sdr->dev_handle,R82XX_VGA,arg);
  }
  sdr->scale = scale_AD(frontend);
}

static void rx888_set_hf_mode(struct sdrstate *sdr){
  command_send(sdr->dev_handle,TUNERSTDBY,0); // Stop Tuner
  // switch to HF Antenna
  usleep(5000);
  sdr->gpios &= ~VHF_EN;
  command_send(sdr->dev_handle,GPIOFX3,sdr->gpios);
}

#if 0
// Pretty sure this is broken
static double rx888_set_tuner_frequency(struct sdrstate *sdr,double frequency){
  assert(sdr != NULL);
  struct frontend *frontend = sdr->frontend;
  if(frequency == frontend->frequency)
    return frequency;

  if(frequency != 0.0){
    // disable HF by set max ATT
    rx888_set_att(sdr,31.5,false);  // max att 31.5 dB
    // switch to VHF Antenna
    usleep(5000);
    sdr->gpios |= VHF_EN;
    command_send(sdr->dev_handle,GPIOFX3,sdr->gpios);

    // high gain, 0db
    uint8_t gain = 0x80 | 3;
    argument_send(sdr->dev_handle,AD8340_VGA,gain);

    // Enable Tuner reference clock
    uint32_t ref = R828D_FREQ;
    command_send(sdr->dev_handle,TUNERINIT,ref); // Initialize Tuner
    command_send(sdr->dev_handle,TUNERTUNE,(uint64_t)frequency);
  } else {
    rx888_set_hf_mode(sdr);
  }
  frontend->frequency = frequency;
  return frequency;
}
#endif

static int rx888_start_rx(struct sdrstate *sdr,libusb_transfer_cb_fn callback){
  assert(sdr != NULL);
  assert(callback != NULL);

  unsigned char ep = 1 | LIBUSB_ENDPOINT_IN;
  for(unsigned int i = 0; i < sdr->queuedepth; i++){
    assert(sdr->transfers[i] != NULL);
    assert(sdr->databuffers[i] != NULL);
    assert(sdr->dev_handle != NULL);

    libusb_fill_bulk_transfer(sdr->transfers[i],sdr->dev_handle,ep,sdr->databuffers[i],
                  sdr->reqsize * sdr->pktsize,callback,(void *)sdr,0);
    int const rStatus = libusb_submit_transfer(sdr->transfers[i]);
    assert(rStatus == 0);
    if(rStatus == 0)
      sdr->xfers_in_progress++;
  }

  usleep(5000);
  command_send(sdr->dev_handle,STARTFX3,0);
  usleep(5000);
  command_send(sdr->dev_handle,TUNERSTDBY,0);

  return 0;
}

static void rx888_stop_rx(struct sdrstate *sdr){
  assert(sdr != NULL);

  while(sdr->xfers_in_progress != 0){
    if(Verbose)
      fprintf(stderr,"%d transfers are pending\n",sdr->xfers_in_progress);
    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    long long stime = gps_time_ns();
    int const ret = libusb_handle_events_timeout_completed(NULL,&tv,NULL);
    if(ret != 0)
      fprintf(stderr,"libusb error %d while stopping\n",ret);

    if(gps_time_ns() > stime + BILLION/2){ // taken more than half a second, too slow
      // Apparently triggers an assertion failure inside libusb but we don't care, we're already aborting
      // Probably should release a lock or something though
      fprintf(stderr,"libusb_handle_events_timeout_completed() timed out while stopping rx888\n");
      break;
    }
    usleep(100000);
  }

  fprintf(stderr,"Transfers completed\n");
  free_transfer_buffers(sdr->databuffers,sdr->transfers,sdr->queuedepth);
  sdr->databuffers = NULL;
  sdr->transfers = NULL;

  command_send(sdr->dev_handle,STOPFX3,0);
}

static void rx888_close(struct sdrstate *sdr){
  assert(sdr != NULL);

  if(sdr->dev_handle)
    libusb_release_interface(sdr->dev_handle,0);

  if(sdr->config)
    libusb_free_config_descriptor(sdr->config);
  sdr->config = NULL;

  if(sdr->dev_handle)
    libusb_close(sdr->dev_handle);

  sdr->dev_handle = NULL;
  libusb_exit(NULL);
}

// Function to free data buffers and transfer structures
static void free_transfer_buffers(unsigned char **databuffers,
                                  struct libusb_transfer **transfers,
                                  unsigned int queuedepth){
  // Free up any allocated data buffers
  if(databuffers != NULL){
    for(unsigned int i = 0; i < queuedepth; i++)
      FREE(databuffers[i]);

    free(databuffers); // caller will have to nail the pointer
  }
  // Free up any allocated transfer structures
  if(transfers != NULL){
    for(unsigned int i = 0; i < queuedepth; i++){
      if(transfers[i] != NULL)
        libusb_free_transfer(transfers[i]);

      transfers[i] = NULL;
    }
    free(transfers); // caller will have to nail the pointer
  }
}

// gain computation for AD8370 variable gain amplifier
static double const VERNIER = 0.055744;
static double const PREGAIN = 7.079458;

static double val2gain(int g){
  int const msb = g & 128 ? true : false;
  int const gaincode = g & 127;
  double av = gaincode * VERNIER * (1 + (PREGAIN - 1) * msb);
  return voltage2dB(av); // decibels
}

static int gain2val(double gain){
  int highgain = gain < 0 ? 0 : 1;
  gain = gain > 34 ? 34 : gain;
  int g = lrint(dB2voltage(gain) / (VERNIER * (1 + (PREGAIN - 1)* highgain)));

  if(g > 127)
    g = 127;
  if(g < 0)
    g = 0;
  g |= (highgain << 7);
  return g;
}
double rx888_tune(struct frontend *frontend,double freq){
#if 1
  (void)frontend;
  (void)freq;
  return 0; // temp
#else
  struct sdrstate * const sdr = (struct sdrstate *)frontend->context;
  if(frontend->lock || sdr->undersample != 1)
    return frontend->frequency;
  if(freq == 0.0){
    frontend->frequency = 0;
    rx888_set_hf_mode(sdr);
    return 0;
  } else {
    return rx888_set_tuner_frequency(sdr,freq);
  }
#endif
}

// New chat GPT version that works only in integers, avoiding floating point precision problems

// si5351_solver.c
// Integer/rational Si5351 divider solver (PLL + MultiSynth) with spur-aware scoring.
// No floating-point search; continued fractions operate on exact rationals.

#include <stdint.h>
#include <stdbool.h>

#ifndef U128
#define U128 __uint128_t
#endif

// ----------------- utilities -----------------

static uint64_t gcd_u64(uint64_t a, uint64_t b){
    while(b){
        uint64_t t = a % b;
        a = b;
        b = t;
    }
    return a;
}

#if 0
static uint64_t clamp_u64(uint64_t x, uint64_t lo, uint64_t hi){
    return (x < lo) ? lo : (x > hi) ? hi : x;
}
#endif

// Best rational approximation to p/q with denominator <= max_den.
// Uses continued fractions + semiconvergent step.
static void best_rational_approx(uint64_t p, uint64_t q, uint64_t max_den,
                                 uint64_t *out_num, uint64_t *out_den)
{
    // Convergents: h[-2]=0,h[-1]=1 ; k[-2]=1,k[-1]=0
    uint64_t h_m2 = 0, h_m1 = 1;
    uint64_t k_m2 = 1, k_m1 = 0;

    uint64_t pp = p, qq = q;

    // track last within limit
    uint64_t last_h = 0, last_k = 1;

    while(qq != 0){
        uint64_t a0 = pp / qq;
        uint64_t r  = pp % qq;

        U128 h = (U128)a0 * h_m1 + h_m2;
        U128 k = (U128)a0 * k_m1 + k_m2;

        if(k > max_den){
            // semiconvergent: k_m2 + t*k_m1 <= max_den
            if(k_m1 == 0){
                *out_num = last_h;
                *out_den = last_k;
                return;
            }
            uint64_t t = (max_den - k_m2) / k_m1;
            U128 hs = (U128)t * h_m1 + h_m2;
            U128 ks = (U128)t * k_m1 + k_m2;
            *out_num = (uint64_t)hs;
            *out_den = (uint64_t)ks;
            return;
        }

        last_h = (uint64_t)h;
        last_k = (uint64_t)k;

        h_m2 = h_m1; h_m1 = (uint64_t)h;
        k_m2 = k_m1; k_m1 = (uint64_t)k;

        pp = qq;
        qq = r;
    }

    // exact convergent fits
    *out_num = last_h;
    *out_den = last_k;
}

// Absolute difference of two rationals a/b and c/d, returned as numerator over common denom in u128.
// |a/b - c/d| = |ad - bc| / bd
static U128 abs_diff_num_u128(uint64_t a, uint64_t b, uint64_t c, uint64_t d){
    U128 left  = (U128)a * d;
    U128 right = (U128)c * b;
    return (left > right) ? (left - right) : (right - left);
}

// ----------------- Si5351 packing -----------------
// For a+b/c, compute P1,P2,P3 per AN619 style formulas:
// P1 = 128*a + floor(128*b/c) - 512
// P2 = 128*b - c*floor(128*b/c)
// P3 = c
// (Same structure for PLL and MultiSynth fractional dividers.)  (See AN619) :contentReference[oaicite:5]{index=5}
typedef struct {
    uint32_t P1, P2, P3;
} si5351_pvals_t;

static si5351_pvals_t pack_abc(uint64_t a, uint64_t b, uint64_t c){
    si5351_pvals_t v;
    if(b == 0){
        // integer mode
        v.P1 = (uint32_t)(128u*a - 512u);
        v.P2 = 0;
        v.P3 = 1;
        return v;
    }
    // t = floor(128*b/c)
    uint64_t t = (U128)128u * b / c;
    v.P1 = (uint32_t)(128u*a + t - 512u);
    v.P2 = (uint32_t)((U128)128u*b - (U128)c*t);
    v.P3 = (uint32_t)c;
    return v;
}

// ----------------- configuration + scoring -----------------

static uint64_t SI5351_DEN_MAX = 1048575u;

typedef struct {
    // PLL: A + B/C
    uint32_t A, B, C;

    // MultiSynth: D + E/F
    uint32_t D, E, F;

    // Output R divider (power of two: 1,2,4,...,128)
    uint32_t R;

    // Achieved frequency as rational: f_ref * (A+B/C)/(D+E/F) / R
    // represented as num/den in Hz
    uint64_t fout_num;
    uint64_t fout_den;

    // Scoring
    U128 err_num;        // |fout - target| expressed as numerator over common denom
    uint8_t prefer_rank; // lower is better
} si5351_solution_t;

static bool multisynth_is_legal(uint32_t D, uint32_t E, uint32_t F){
    if(F == 0) return false;
    if(E >= F) return false;

    // Special integer-only cases: 4,6,8 are allowed.
    if(E == 0 && (D == 4 || D == 6 || D == 8)) return true;

    // Fractional MS valid range: >= 8 + 1/den ... 2048 (AN619/AN1234) :contentReference[oaicite:6]{index=6}
    // That means:
    // - D must be >= 8
    // - if D == 8 then E must be > 0
    if(D < 8) return false;
    if(D == 8 && E == 0) return false; // but ok in integer-only case above
    if(D > 2048) return false;
    return true;
}

static bool pll_is_legal(uint32_t A, uint32_t B, uint32_t C){
    if(C == 0) return false;
    if(B >= C) return false;
    // Typical legal A range for PLL multiplier is ~15..90 (app notes) :contentReference[oaicite:7]{index=7}
    if(A < 15 || A > 90) return false;
    if(C > SI5351_DEN_MAX) return false;
    return true;
}

static bool pll_freq_in_range(uint64_t fref_hz, uint32_t A, uint32_t B, uint32_t C){
    // fpll = fref * (A + B/C)
    // compare as rational: fpll = fref*(A*C + B)/C
    U128 num = (U128)fref_hz * ((U128)A*C + B);
    U128 den = C;

    // 600..900 MHz typical VCO range :contentReference[oaicite:8]{index=8}
    const U128 lo = (U128)600000000;
    const U128 hi = (U128)900000000;

    // num/den within [lo,hi]  <=> lo*den <= num <= hi*den
    return (lo*den <= num) && (num <= hi*den);
}

// Compute achieved fout as reduced rational (num/den) in Hz.
static void compute_fout_rational(uint64_t fref_hz,
                                  uint32_t A,uint32_t B,uint32_t C,
                                  uint32_t D,uint32_t E,uint32_t F,
                                  uint32_t R,
                                  uint64_t *out_num, uint64_t *out_den)
{
    // fout = fref * ( (A*C+B)/C ) / ( (D*F+E)/F ) / R
    //      = fref * (A*C+B) * F / ( C*(D*F+E)*R )
    U128 num = (U128)fref_hz * ((U128)A*C + B) * F;
    U128 den = (U128)C * ((U128)D*F + E) * R;

    // Reduce to u64 by gcd if possible (fits in u128 first)
    // Compute gcd on 64-bit only if both fit, else do a small reduction pass.
    // Here we do a conservative reduction using 64-bit gcd when possible.
    // (For typical Hz ranges, these will fit in 64-bit after reduction.)
    uint64_t n64 = (uint64_t)num;
    uint64_t d64 = (uint64_t)den;
    uint64_t g = gcd_u64(n64, d64);
    n64 /= g; d64 /= g;
    *out_num = n64;
    *out_den = d64;
}

// Spur-aware preference rank: 0 best.
static uint8_t preference_rank(uint32_t D,uint32_t E,uint32_t F, uint32_t C){
    // Prefer integer MS (E==0, D in {4,6,8}) most.
    if(E == 0 && (D == 4 || D == 6 || D == 8)) return 0;

    // Next: fractional MS but small denominators
    // Penalize larger denominators loosely.
    uint8_t r = 1;
    if(F > 1000) r++;
    if(F > 10000) r++;
    if(C > 1000) r++;
    if(C > 10000) r++;
    return r;
}

// ----------------- main solver -----------------

// Solve for one output: given fref and desired fout (Hz), return best solution.
// Strategy:
//  1) Reduce r = fout/fref to P/Q exactly.
//  2) Enumerate R divider (1..128 power-of-two).
//  3) Try integer MS modes first (4,6,8), then fractional MS (D=8..2048),
//     picking E/F by approximating a target MS value derived from a target PLL freq.
//  4) For each candidate MS, compute required PLL ratio X = r*Y and approximate its fractional part by B/C (C<=1,048,575).
//  5) Score by absolute frequency error + preference rank.
bool si5351_solve(uint64_t fref_hz, uint64_t fout_hz, si5351_solution_t *best)
{
    if(!best || fref_hz == 0 || fout_hz == 0) return false;

    // Exact ratio r = P/Q
    uint64_t P = fout_hz;
    uint64_t Q = fref_hz;
    uint64_t g = gcd_u64(P, Q);
    P /= g; Q /= g;

    // Initialize best as "infinite error"
    best->err_num = (U128)(~(U128)0);
    best->prefer_rank = 255;

    // Candidate PLL targets (Hz) to bias MS approximation.
    const uint64_t pll_targets[] = { 900000000ULL, 864000000ULL, 800000000ULL, 750000000ULL, 600000000ULL };
    const unsigned n_pll_targets = sizeof(pll_targets)/sizeof(pll_targets[0]);

    // R divider values: 1,2,4,...,128
    for(uint32_t R = 1; R <= 128; R <<= 1){

        // ---- 3a) integer MS first: D in {4,6,8}, E=0,F=1
        const uint32_t D_ints[] = {4,6,8};
        for(unsigned i=0;i<3;i++){
            uint32_t D = D_ints[i], E = 0, F = 1;
            if(!multisynth_is_legal(D,E,F)) continue;

            // Required PLL freq = fout * D * R
            // Ensure within VCO band via PLL ratio later.
#if 0
            U128 fpll = (U128)fout_hz * D * R;
#endif
            // Compute PLL ratio X = fpll / fref = (fout/fref) * (D*R)
            // X = (P/Q) * (D*R) = (P*(D*R))/Q
            U128 Xn = (U128)P * (D*R);
            U128 Xd = (U128)Q;

            uint64_t A = (uint64_t)(Xn / Xd);
            U128 rem = Xn % Xd;

            // fractional part rem/Xd approximated by B/C
            uint64_t B=0,C=1;
            if(rem != 0){
                // Reduce rem/Xd first for smaller numbers
                uint64_t rem64 = (uint64_t)rem;
                uint64_t xd64  = (uint64_t)Xd;
                uint64_t gg = gcd_u64(rem64, xd64);
                rem64 /= gg; xd64 /= gg;
                best_rational_approx(rem64, xd64, SI5351_DEN_MAX, &B, &C);
            }

            if(!pll_is_legal((uint32_t)A,(uint32_t)B,(uint32_t)C)
	       || !pll_freq_in_range(fref_hz,(uint32_t)A,(uint32_t)B,(uint32_t)C))
	      continue;

            // achieved fout as rational:
            uint64_t fn, fd;
            compute_fout_rational(fref_hz,(uint32_t)A,(uint32_t)B,(uint32_t)C,D,E,F,R,&fn,&fd);

            // error = |fn/fd - fout_hz|
            // compute |fn - fout*fd| / fd
            U128 err = abs_diff_num_u128(fn, fd, fout_hz, 1);

            uint8_t pref = preference_rank(D,E,F,(uint32_t)C);

            // Select best: smallest err, then pref
            if(err < best->err_num || (err == best->err_num && pref < best->prefer_rank)){
                best->A=(uint32_t)A; best->B=(uint32_t)B; best->C=(uint32_t)C;
                best->D=D; best->E=E; best->F=F;
                best->R=R;
                best->fout_num=fn; best->fout_den=fd;
                best->err_num=err;
                best->prefer_rank=pref;
            }
        }

        // ---- 3b) fractional MS: D=8..2048, E/F chosen by approximating target MS ratio
        for(uint32_t D=8; D<=2048; D++){
            for(unsigned t=0;t<n_pll_targets;t++){
                uint64_t fpll_target = pll_targets[t];

                // target multisynth value Ytarget = fpll_target / (fout * R)
                // Ytarget in [D, D+1)
                // Compute fractional part exactly: frac = Ytarget - D
                // frac = (fpll_target) / (fout*R) - D = (fpll_target - D*fout*R) / (fout*R)
                U128 denom = (U128)fout_hz * R;
                U128 numer = (U128)fpll_target;

                // Skip if D band doesn't straddle target
                U128 Dden = (U128)D * denom;
                U128 D1den = (U128)(D+1) * denom;
                if(numer < Dden || numer >= D1den) continue;

                U128 frac_num = numer - Dden;   // in [0, denom)
                uint64_t E=0,F=1;

                // Approx frac_num/denom by E/F with F<=DEN_MAX, and require E>0 if D==8
                if(frac_num != 0){
                    uint64_t fn = (uint64_t)frac_num;
                    uint64_t fd = (uint64_t)denom;
                    uint64_t gg = gcd_u64(fn, fd);
                    fn/=gg; fd/=gg;
                    best_rational_approx(fn, fd, SI5351_DEN_MAX, &E, &F);
                } else {
                    // exactly integer D; but fractional mode requires D==8 must be >8, and for 8 integer is legal only via integer-mode case (handled above)
                    E = 0; F = 1;
                }

                if(!multisynth_is_legal(D,(uint32_t)E,(uint32_t)F)) continue;

                // Now Y = (D*F+E)/F exactly.
                // Compute required PLL ratio X = r * Y = (P/Q)*((D*F+E)/F)
                U128 Yn = (U128)D*F + E;
                U128 Yd = (U128)F;

                U128 Xn = (U128)P * Yn;
                U128 Xd = (U128)Q * Yd;

                uint64_t A = (uint64_t)(Xn / Xd);
                U128 rem = Xn % Xd;

                uint64_t B=0,C=1;
                if(rem != 0){
                    uint64_t rem64 = (uint64_t)rem;
                    uint64_t xd64  = (uint64_t)Xd;
                    uint64_t gg2 = gcd_u64(rem64, xd64);
                    rem64/=gg2; xd64/=gg2;
                    best_rational_approx(rem64, xd64, SI5351_DEN_MAX, &B, &C);
                }

                if(!pll_is_legal((uint32_t)A,(uint32_t)B,(uint32_t)C)
		   || !pll_freq_in_range(fref_hz,(uint32_t)A,(uint32_t)B,(uint32_t)C))
		  continue;

                uint64_t fn, fd;
                compute_fout_rational(fref_hz,(uint32_t)A,(uint32_t)B,(uint32_t)C,
                                      D,(uint32_t)E,(uint32_t)F,R,&fn,&fd);

                U128 err = abs_diff_num_u128(fn, fd, fout_hz, 1);
                uint8_t pref = preference_rank(D,(uint32_t)E,(uint32_t)F,(uint32_t)C);

                if(err < best->err_num || (err == best->err_num && pref < best->prefer_rank)){
                    best->A=(uint32_t)A; best->B=(uint32_t)B; best->C=(uint32_t)C;
                    best->D=D; best->E=(uint32_t)E; best->F=(uint32_t)F;
                    best->R=R;
                    best->fout_num=fn; best->fout_den=fd;
                    best->err_num=err;
                    best->prefer_rank=pref;
                }
            }
        }
    }

    return best->err_num != (U128)(~(U128)0);
}

// Helpers to get packed register parameters:
void si5351_get_pll_pvals(const si5351_solution_t *s, si5351_pvals_t *pll){
    *pll = pack_abc(s->A, s->B, s->C);
}
void si5351_get_ms_pvals(const si5351_solution_t *s, si5351_pvals_t *ms){
    *ms = pack_abc(s->D, s->E, s->F);
}
double rx888_set_samprate(struct sdrstate *sdr,int64_t const reference,int64_t const samprate){
  assert(sdr != NULL);
  assert(reference != 0);
  assert(samprate != 0);

  si5351_solution_t best = {0};

  bool result = si5351_solve(reference,samprate,&best);
  if(!result){
    fprintf(stderr,"si5351_solve(%'lld, %'lld) failed\n", (long long)reference, (long long)samprate);
    return 0;
  }
  if(best.E == 0)
    sdr->ms_int = true;

  double const pll_ratio = (double)best.A + (double)best.B / (double)best.C;
  double const vco = reference * pll_ratio;
  si5351_pvals_t pll = {0};
  si5351_get_pll_pvals(&best,&pll);

  fprintf(stderr,"RX888 Si5351 PLL: vco = %'lld * (%'d + %'d/%'d) = %'lf; P1=%d, P2=%d, P3=%d\n",
	  (long long)reference,
	  best.A, best.B, best.C,
	  vco,
	  pll.P1, pll.P2, pll.P3);

  uint8_t data_clkin[] = {
    (pll.P3 & 0x0000ff00) >>  8,
    (pll.P3 & 0x000000ff) >>  0,
    (pll.P1 & 0x00030000) >> 16,
    (pll.P1 & 0x0000ff00) >>  8,
    (pll.P1 & 0x000000ff) >>  0,
    ((pll.P3 & 0x000f0000) >> 12) | ((pll.P2 & 0x000f0000) >> 16),
    (pll.P2 & 0x0000ff00) >>  8,
    (pll.P2 & 0x000000ff) >>  0
  };
  control_send(sdr->dev_handle,I2CWFX3,SI5351_ADDR,SI5351_REGISTER_MSNA_BASE,data_clkin,sizeof(data_clkin));

  double const output_ratio = (double)best.D + (double)best.E / (double)best.F;
  si5351_pvals_t ms = {0};
  si5351_get_ms_pvals(&best,&ms);
  fprintf(stderr,"RX888 Si5351 output divider: samprate = vco / (%'d*(%'d + %'d/%'d)) = %'lf/%'lf = %'lld/%'lld = %'lf (error = %'lg); P1=%d, P2=%d, P3=%d\n",
	  best.R,
	  best.D, best.E, best.F,
	  vco,
	  output_ratio,
	  (long long)best.fout_num, (long long)best.fout_den,
	  vco / output_ratio,
	  vco/output_ratio - samprate,
	  ms.P1, ms.P2, ms.P3);

  uint8_t data_clkout[] = {
    (ms.P3 & 0x0000ff00) >>  8,
    (ms.P3 & 0x000000ff) >>  0,
    (uint8_t)((__builtin_ctz(best.R) << 4) | ((ms.P1 & 0x00030000) >> 16)), // __builtin_ctz() is essentially log2(int) for powers of 2
    (ms.P1 & 0x0000ff00) >>  8,
    (ms.P1 & 0x000000ff) >>  0,
    (uint8_t)(((ms.P3 & 0x000f0000) >> 12) | ((ms.P2 & 0x000f0000) >> 16)),
    (ms.P2 & 0x0000ff00) >>  8,
    (ms.P2 & 0x000000ff) >>  0
  };
  control_send(sdr->dev_handle,I2CWFX3,SI5351_ADDR,SI5351_REGISTER_MS0_BASE,data_clkout,sizeof(data_clkout));
  return (double)best.fout_num / best.fout_den;
}
