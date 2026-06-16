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
#include <stdatomic.h>

#include "misc.h"
#include "status.h"
#include "config.h"
#include "radio.h"
#include "rx888.h"
#include "si5351.h"
#include "r828d.h"
#include "ezusb.h"

static uint16_t const Vendor_id = 0x04b4;
static uint16_t const Unloaded_product_id = 0x00f3;
static uint16_t const Loaded_product_id = 0x00f1;

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
static long long const MIN_REFERENCE = 10e6;  //  10 MHz
static long long const MAX_REFERENCE = 100e6; // 100 MHz
static long long const DEFAULT_REFERENCE = 27e6;
// Max allowable error on reference; 1e-4 = 100 ppm. Mainly to catch entry scaling errors
static double const MAX_CALIBRATE = 1e-4;

int Ezusb_verbose = 0; // Used by ezusb.c
// Global variables set by config file options in main.c
extern int Verbose;
extern char const *Description;
extern int USB_busnum;
extern int USB_devnum;
extern char const *Serial;

// Hardware-specific stuff.
// Anything generic should be moved to 'struct frontend' under sdr in radio.h
enum state {
  STOPPED,
  STARTING,
  STOPPING,
  RUNNING
};
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

  uint8_t fw_major,fw_minor;

  // USB transfer
  int xfers_in_progress;
  unsigned int queuedepth; // Number of requests to queue
  unsigned int reqsize;    // Request size in number of packets
  unsigned long success_count;  // Number of successful transfers
  unsigned long failure_count;  // Number of failed transfers

  // RF Hardware
  double high_threshold;
  double low_threshold;

  long long reference;
  bool randomizer;
  bool dither;
  uint32_t gpios;
  uint64_t last_sample_count; // Used to verify sample rate
  int64_t last_count_time;
  bool message_posted; // Clock rate error posted last time around
  double scale;        // Scale samples for #bits and front end gain
  int undersample;     // Use undersample aliasing on baseband input for VHF/UHF. n = 1 => no undersampling
  double dc_offset;    // A/D offset, units, used only to adjust power reading. It just goes into the FFT DC bin
  double power_smooth; // Arbitrary exponential smoothing factor for front end power estimate
  pthread_t cmd_thread;
  pthread_t proc_thread;
  pthread_t agc_thread;
  _Atomic enum state state;
  bool reset;
  _Atomic bool device_gone;
};

static uint8_t R828D_shadow[32];

static void rx_callback(struct libusb_transfer *transfer);
static int rx888_usb_init(struct sdrstate *sdr,const char *firmware,unsigned int queuedepth,unsigned int reqsize);
static void rx888_set_dither_and_randomizer(struct sdrstate *sdr,bool dither,bool randomizer);
static void rx888_set_att(struct sdrstate *sdr,double att,bool vhf);
static void rx888_set_gain(struct sdrstate *sdr,double gain,bool vhf);
static double rx888_set_samprate(struct sdrstate *sdr,long long reference,long long samprate);
static double rx888_set_tuner_ref(struct sdrstate *sdr, long long const reference, long long const f);
static void rx888_set_hf_mode(struct sdrstate *sdr);
static int rx888_start_rx(struct sdrstate *sdr,libusb_transfer_cb_fn callback);
static void rx888_stop_rx(struct sdrstate *sdr);
static void rx888_close(struct sdrstate *sdr);
static void free_transfer_buffers(unsigned char **databuffers,struct libusb_transfer **transfers,unsigned int queuedepth);
static double val2gain(int g);
static int gain2val(double gain);
static void *proc_rx888(void *arg);
static void *agc_rx888(void *arg);

static double rx888_set_tuner_frequency(struct sdrstate *sdr,double frequency);
// Read one Si5351 register over I2C (reg# is silicon-defined → version-proof).
static inline int si5351_read(struct sdrstate *sdr, uint8_t reg, uint8_t *val){
  return control_recv(sdr->dev_handle, I2CRFX3, SI5351_ADDR, reg, val, sizeof *val);
}
static inline int si5351_write(struct sdrstate *sdr, uint8_t reg, uint8_t *arg, int len){
  return control_send(sdr->dev_handle, I2CWFX3, SI5351_ADDR, reg, arg, len);
}
static inline int si5351_write_byte(struct sdrstate *sdr, uint8_t reg, uint8_t arg){
  return control_send_byte(sdr->dev_handle, I2CWFX3, SI5351_ADDR, reg, arg);
}
// Reads are bit reversed for some strange reason
static inline uint8_t bitrev(uint8_t b){
  b = ((b & 0xf0) >> 4) | ((b & 0x0f) << 4);
  b = ((b & 0xcc) >> 2) | ((b & 0x33) << 2);
  b = ((b & 0xaa) >> 1) | ((b & 0x55) << 1);
  return b;
}

// hack test to read first 5 bytes (status registers)
static inline int r828_status(struct sdrstate *sdr){
  uint8_t buf[5];
  int r = control_recv(sdr->dev_handle, I2CRFX3, R828D_ADDR, 0, buf, sizeof buf);
  assert(r == 5);
  for(int i=0; i<r; i++)
    R828D_shadow[i] = bitrev(buf[i]);
  return r;
}
static inline int r828_read(struct sdrstate *sdr, uint8_t reg, uint8_t *val){
  // Device reads LSB first, but writes MSB first (!)
  reg &= 31;
  int r = control_recv(sdr->dev_handle, I2CRFX3, R828D_ADDR, reg, val, sizeof *val);
  assert(r == 1);
  *val = bitrev(*val);
  R828D_shadow[reg] = *val;
  return r;
}
#if 0
static inline int r828_write(struct sdrstate *sdr, uint8_t reg, uint8_t *arg, int len){
  return control_send(sdr->dev_handle, I2CWFX3, R828D_ADDR, reg, arg, len);
}
#endif

static inline int r828_write_byte(struct sdrstate *sdr, uint8_t reg, uint8_t arg){
  reg &= 31;
  R828D_shadow[reg] = arg;
  int r = control_send_byte(sdr->dev_handle, I2CWFX3, R828D_ADDR, reg, arg);
  assert(r == 0);
  return r;
}
static inline int r828_write_byte_mask(struct sdrstate *sdr, uint8_t reg, uint8_t arg, uint8_t mask){
  reg &= 31;
  R828D_shadow[reg] = arg = (arg & mask) | (R828D_shadow[reg] & ~mask);
  int r = control_send_byte(sdr->dev_handle, I2CWFX3, R828D_ADDR, reg, arg);
  assert(r == 0);
  return r;
}

// stolen from github.com/ringof/rx888-firmware/blob/claude/return-vhf-tuner/rx888_vhf.py
//R828D tracking-filter bands: (LO_start_MHz, open_d, rf_mux_ploy, tf_c)
int Freq_ranges[][4] = {
    {  0,0x08,0x02,0xDF},{ 50,0x08,0x02,0xBE},{ 55,0x08,0x02,0x8B},{ 60,0x08,0x02,0x7B},
    { 65,0x08,0x02,0x69},{ 70,0x08,0x02,0x58},{ 75,0x00,0x02,0x44},{ 80,0x00,0x02,0x44},
    { 90,0x00,0x02,0x34},{100,0x00,0x02,0x34},{110,0x00,0x02,0x24},{120,0x00,0x02,0x24},
    {140,0x00,0x02,0x14},{180,0x00,0x02,0x13},{220,0x00,0x02,0x13},{250,0x00,0x02,0x11},
    {280,0x00,0x02,0x00},{310,0x00,0x41,0x00},{450,0x00,0x41,0x00},{588,0x00,0x40,0x00},
    {650,0x00,0x40,0x00},
};
// Bandwidth presets: (reg_0x0A_val, reg_0x0B_val, reg_0x1E_val, if_center_hz)
//  Wide presets from hardcoded top of set_bandwidth; narrow from IFi[] table
// (tuner_r82xx_explained.md §5). Keys are MHz (float for sub-MHz entries).
int Bw_presets[][4] = {
  {0x10, 0x0B, 0x60, 4570000}, // 8 MHz
  {0x10, 0x2A, 0x60, 4570000}, // 7 MHz
  {0x10, 0x6B, 0x00, 3570000}, // 6 MHz
  {0x0B, 0x6B, 0x00, 3570000}, // 5 MHz
  {0x04, 0x8F, 0x00, 2000000}, // 3 MHz
  {0x0F, 0x8B, 0x00, 1900000}, // 1.6 MHz
  {0x0F, 0xEA, 0x00, 1706000}, // 0.6 MHz
  {0x0F, 0xE7, 0x00, 1925000}, // 0.29 MHz
};
double Bw_cycle[] = {8, 7, 6, 5, 3, 1.6, 0.6, 0.29};

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
  "reset",
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
  sdr->serial = 0;
  {
    char const *p = config_getstring(dictionary,section,"serial",NULL); // is serial specified?
    if(p != NULL)
      sdr->serial = strtoll(p,NULL,16);
  }
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
  // Firmware file is now empty by default. We ignore unloaded devices and
  // wait for rx888_boot to load one so it appears as 0xf1
  char const *firmware = config_getstring(dictionary,section,"firmware","");
  int ret;
  if((ret = rx888_usb_init(sdr,firmware,queuedepth,reqsize)) != 0){
    fprintf(stderr,"rx888_usb_init() failed\n");
    return -1;
  }
  sdr->reset = config_getboolean(dictionary,section,"reset",sdr->reset);
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
      att = 31.5; // Max attenuation is 31.5 dB
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

  long long reference = DEFAULT_REFERENCE;
  {
    char const *p = config_getstring(dictionary,section,"reference",NULL);
    if(p != NULL)
      reference = llrint(parse_frequency(p,false));
  }
  if(reference < MIN_REFERENCE || reference > MAX_REFERENCE){
    fprintf(stderr,"Invalid reference frequency %'lld, forcing %'lld\n", reference, DEFAULT_REFERENCE);
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
  if(samprate < MIN_SAMPRATE || samprate > MAX_SAMPRATE){
    fprintf(stderr,"Invalid sample rate %'lld, ",(long long)samprate);
    samprate = samprate < MIN_SAMPRATE ? MIN_SAMPRATE : MAX_SAMPRATE; // must be one or the other
    fprintf(stderr,"forcing %'lld\n",(long long)samprate);
  }
  sdr->reference = reference * (1 + calibrate);
  samprate = rx888_set_samprate(sdr,sdr->reference,samprate); // Update to actual samprate, if different
  frontend->samprate = samprate;

  sdr->undersample = config_getint(dictionary,section,"undersample",1);
  if(sdr->undersample < 1){
    fprintf(stderr,"rx888 undersample must be >= 1, ignoring\n");
    sdr->undersample = 1;
  }
  // note intentional integer truncation, ie undersample = 1 -> frequency = 0
  frontend->frequency = frontend->samprate * (sdr->undersample / 2);
  if(sdr->undersample & 1){
    // Somewhat arbitrary. See https://ka7oei.blogspot.com/2024/12/frequency-response-of-rx-888-sdr-at.html
    frontend->min_IF = 15000;
    frontend->max_IF = NYQUIST * samprate;
  } else {
    frontend->min_IF = -NYQUIST * samprate;
    frontend->max_IF = -15000;
  }
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

  sdr->power_smooth = -expm1(-xfer_time/PTC);

  fprintf(stderr,"RX888 AGC %s, nominal gain %.1f dB, actual gain %.1f dB, atten %.1f dB, gain cal %.1f dBm, dither %s, randomizer %s, USB queue depth %d, USB request size %'d * pktsize %'d = %'d bytes (%g sec)\n",
	  frontend->rf_agc ? "on" : "off",
	  gain,
	  frontend->rf_gain,
	  frontend->rf_atten,
	  frontend->rf_level_cal,
	  sdr->dither ? "on" : "off",
	  sdr->randomizer ? "on" : "off",
	  sdr->queuedepth,
	  sdr->reqsize,
	  sdr->pktsize,
	  sdr->reqsize * sdr->pktsize,
	  xfer_time);

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
  if(frontend->frequency == 0)
    rx888_set_hf_mode(sdr);

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
  while(true){
    enum state s = STOPPED;
    if(atomic_compare_exchange_strong(&sdr->state,&s,STARTING))
      break;
    if(s == RUNNING)
      return 0; // Already running
    usleep(10000); // 10 ms
  }
  // Start processing A/D data only if no already running
  sdr->scale = scale_AD(frontend); // set scaling now that we know the forward FFT size
  pthread_create(&sdr->proc_thread,NULL,proc_rx888,sdr);
  pthread_create(&sdr->agc_thread,NULL,agc_rx888,sdr);
  atomic_store(&sdr->state,RUNNING);
  fprintf(stderr,"rx888 running\n");
  return 0;
}

int rx888_shutdown(struct frontend * const frontend){
  struct sdrstate * const sdr = (struct sdrstate *)frontend->context;
  while(true){
    enum state s = RUNNING;
    if(atomic_compare_exchange_strong(&sdr->state,&s,STOPPING))
      break;
    if(s == STOPPED)
      return 0; // Already running
    usleep(10000); // 10 ms
  }
  pthread_join(sdr->proc_thread, NULL);
  pthread_join(sdr->agc_thread, NULL);
  atomic_store(&sdr->state,STOPPED);
  fprintf(stderr,"rx888 stopped\n");
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

  realtime(2 + default_prio());
  stick_core();
  {
    sdr->last_count_time = sdr->last_callback_time = gps_time_ns();
    int ret __attribute__ ((unused));
    ret = rx888_start_rx(sdr,rx_callback);
    assert(ret == 0);
  }
  enum state s;
  while((s = atomic_load(&sdr->state)) == RUNNING || s == STARTING) {
    if(sdr->device_gone){
      // Device actually disappeared, exit immediately in case
      // it gets quickly plugged back in before 5 seconds
      fprintf(stderr,"RX888 device disappeared, exiting\n");
      goto abort;
   }
    // But also check for a silent hang with libusb_handle_events_timeout_completed()
    // Check more directly how long it's been since we last got data
    // sdr->last_callback_time is set in rx_callback()
    int const maxtime = 5;
    if(gps_time_ns() > sdr->last_callback_time + maxtime * BILLION){
      fprintf(stderr,"No RX888 data for %d seconds, quitting\n",maxtime);
      goto abort;
    }
    struct timeval tv = {
      .tv_sec = 1,
      .tv_usec = 0
    };
    int const ret = libusb_handle_events_timeout_completed(NULL,&tv,NULL);
    if(ret != 0){
      // Apparent failure
      fprintf(stderr,"RX888 libusb_handle_events returned %d\n",ret);
      goto abort;
    }
  }
  rx888_stop_rx(sdr); // we were told to stop (eg, no users), return gracefully
  return NULL;
 abort:;
  rx888_stop_rx(sdr);
  rx888_close(sdr);
  exit(EX_NOINPUT);
}

// Monitor power levels, record new watermarks, adjust AGC if enabled
// Also perform coarse check on sample rate, compared to system clock
static void *agc_rx888(void *arg){
  struct sdrstate * const sdr = (struct sdrstate *)arg;
  assert(sdr != NULL);
  pthread_setname("agc_rx888");
  struct frontend *frontend = sdr->frontend;
  enum state s;
  while((s = atomic_load(&sdr->state)) == RUNNING || s == STARTING){
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

  sdr->xfers_in_progress--;

  if(transfer->status == LIBUSB_TRANSFER_NO_DEVICE){
    sdr->device_gone = true;
    return;
  }
  if(transfer->status != LIBUSB_TRANSFER_COMPLETED) {
    sdr->failure_count++;
    if(Verbose > 1)
      fprintf(stderr,"Transfer %p callback status %s received %d bytes.\n",transfer,
	      libusb_error_name(transfer->status), transfer->actual_length);
    int const s = atomic_load(&sdr->state);
    if(s == STARTING || s == RUNNING) {
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
      // Assumes arithmetic right shift of signed integers
      // Put LSB in sign bit, then shift back by one less bit to make ..ffffe or 0
      s ^= (s << 31) >> 30;
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
  if(sampcount != 0 && !isnan(in_energy) && isfinite(in_energy))
    frontend->if_power += sdr->power_smooth * (in_energy / sampcount - frontend->if_power);
  frontend->samples += sampcount; // Count original samples
  if(atomic_load(&sdr->state) == RUNNING) {
    if(libusb_submit_transfer(transfer) == 0)
      sdr->xfers_in_progress++;
  }
  sdr->last_callback_time = gps_time_ns();  // Reset watchdog only after read has succeeded
  write_rfilter(&frontend->in,NULL,sampcount); // Update write pointer, invoke FFT if block is complete
}

static int rx888_usb_init(struct sdrstate *const sdr,const char * const firmware,unsigned int const queuedepth,unsigned int const reqsize){
  {
    int ret = libusb_init(NULL);
    if(ret != 0){
      fprintf(stderr,"Error initializing libusb: %s\n", libusb_strerror(ret));
      return -1;
    }
  }
  // If firmware file is not set (default), ignore unloaded rx888 devices, let rx888_boot get them
  if(firmware != NULL && strlen(firmware) > 0){
    // Search for unloaded rx888s (0x04b4:0x00f3) with the desired serial, or all such devices if no serial specified
    // and load with firmware
    // With the rx888 bootloader launched by udev, this section will usually not find anything
    // but a race is possible at boot time when systemd starts both at the same time
    // So it's recommended to let rx888_boot handle unloaded devices
    libusb_device **device_list;
    ssize_t dev_count = libusb_get_device_list(NULL,&device_list);
    for(ssize_t i=0; i < dev_count; i++){
      libusb_device *device = device_list[i];
      if(device == NULL)
	break; // End of list

      struct libusb_device_descriptor desc = {0};
      int ret = libusb_get_device_descriptor(device,&desc);
      if(ret != 0){
	fprintf(stderr," libusb_get_device_descriptor() failed: %s\n",libusb_strerror(ret));
	continue;
      }
      if(desc.idVendor != Vendor_id || desc.idProduct != Unloaded_product_id)
	continue;

      fprintf(stderr,"found unloaded rx888 vendor %04x, device %04x",desc.idVendor,desc.idProduct);
      libusb_device_handle *handle = NULL;
      ret = libusb_open(device,&handle);
      if(ret != 0 || handle == NULL){
	fprintf(stderr,", libusb_open() failed: %s\n",libusb_strerror(ret));
	continue;
      }
      if(desc.iManufacturer){
	char manufacturer[100] = {0};
	if(libusb_get_string_descriptor_ascii(handle,desc.iManufacturer,(unsigned char *)manufacturer,sizeof manufacturer) > 0)
	  fprintf(stderr,", manufacturer '%s'",manufacturer);
      }
      if(desc.iProduct){
	char product[100] = {0};
	if(libusb_get_string_descriptor_ascii(handle,desc.iProduct,(unsigned char *)product,sizeof product) > 0)
	  fprintf(stderr,", product '%s'",product);
      }
      char serial[100] = {0};
      if(desc.iSerialNumber){
	if(libusb_get_string_descriptor_ascii(handle,desc.iSerialNumber,(unsigned char *)serial,sizeof serial) > 0)
	  fprintf(stderr,", serial '%s'",serial);
      }
      // The proper serial number doesn't appear until the device is loaded with firmware, so load all we find
      char full_firmware_file[PATH_MAX] = {0};
      dist_path(full_firmware_file,sizeof(full_firmware_file),firmware);
      fprintf(stderr,", loading rx888 firmware file %s\n",full_firmware_file);
      if(ezusb_load_ram(handle,full_firmware_file,FX_TYPE_FX3,IMG_TYPE_IMG,1) == 0){
	fprintf(stderr,"rx888 loaded\n");
	sleep(1); // how long should we wait for the unit to boot and reappear on the bus?
      } else {
	fprintf(stderr,"rx888 load failed for device %d.%d (logical)\n",
		libusb_get_bus_number(device),libusb_get_device_address(device));
      }
      libusb_close(handle);
      handle = NULL;
    }
    libusb_free_device_list(device_list,1);
    device_list = NULL;
  }
  // Scan list again, looking for a loaded RX888 with the right numbers
  if(Serial != NULL){ // specified on command line, overrides config file
    // Specified on command line, convert and compare
    sdr->serial = strtoll(Serial,NULL,16); // always hex
  }
  if(sdr->serial != 0)
    fprintf(stderr,"Looking for rx888 serial %016llx\n",(long long)sdr->serial);
  else if(USB_busnum != -1 && USB_devnum != -1)
    fprintf(stderr,"Looking for rx888 at %d:%d\n",USB_busnum,USB_devnum);

  libusb_device *device = NULL;
  libusb_device **device_list;
  ssize_t dev_count = libusb_get_device_list(NULL,&device_list);
  for(int i=0; i < dev_count; i++){
    device = device_list[i];
    if(device == NULL)
      break; // End of list

    struct libusb_device_descriptor desc = {0};
    int ret = libusb_get_device_descriptor(device,&desc);
    if(ret != 0){
      fprintf(stderr," libusb_get_device_descriptor() failed: %s\n",libusb_strerror(ret));
      continue;
    }
    if(desc.idVendor != Vendor_id || desc.idProduct != Loaded_product_id)
      continue;

    fprintf(stderr,"found rx888 vendor %04x, device %04x",desc.idVendor,desc.idProduct);
    libusb_device_handle *handle = NULL;
    ret = libusb_open(device,&handle);
    if(ret != 0 || handle == NULL){
      fprintf(stderr," libusb_open() failed: %s\n",libusb_strerror(ret));
      continue;
    }
    if(desc.iManufacturer){
      char manufacturer[100] = {0};
      if(libusb_get_string_descriptor_ascii(handle,desc.iManufacturer,(unsigned char *)manufacturer,sizeof(manufacturer)) > 0)
	fprintf(stderr,", manufacturer '%s'",manufacturer);
    }
    if(desc.iProduct){
      char product[100] = {0};
      if(libusb_get_string_descriptor_ascii(handle,desc.iProduct,(unsigned char *)product,sizeof(product)) > 0)
	fprintf(stderr,", product '%s'",product);
    }
    char serial[100] = {0};
    if(desc.iSerialNumber){
      if(libusb_get_string_descriptor_ascii(handle,desc.iSerialNumber,(unsigned char *)serial,sizeof(serial)) > 0)
	fprintf(stderr,", serial '%s'",serial);
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
    uint64_t current_serialnum = strtoll(serial,NULL,16); // device serial
    uint8_t current_busnum = libusb_get_bus_number(device);
    uint8_t current_devnum = libusb_get_device_address(device);

    if(sdr->serial == 0 && USB_busnum == -1 && USB_devnum == -1){
      // No particular unit specified, so take it
      fprintf(stderr,", selected by default\n");
      sdr->dev_handle = handle;
      sdr->serial = current_serialnum;
      break;
    } else if(sdr->serial == current_serialnum){
      fprintf(stderr,", selected by line serial number\n");
      sdr->dev_handle = handle;
      break;
    } else if(current_busnum == USB_busnum && current_devnum == USB_devnum){
      fprintf(stderr,", selected by USB device ID\n");
      sdr->serial = current_serialnum;
      sdr->dev_handle = handle;
      break;
    } else {
      fprintf(stderr,"\n"); // Not selected; close and keep looking
      libusb_close(handle);
      handle = NULL;
    }
  }
  libusb_free_device_list(device_list,1);
  device_list = NULL;

  // If a device has been found, device and dev_handle will be non-NULL
  if(device == NULL || sdr->dev_handle == NULL){
    fprintf(stderr,"Error or device could not be found\n");
    goto quit;
  }
  // Stop and reopen in case it was left running - KA9Q
  command_send(sdr->dev_handle,STOPFX3,0);
  if(sdr->reset){
    int ret = libusb_reset_device(sdr->dev_handle);
    if(ret != 0)
      fprintf(stderr,"reset failed, %s\n",libusb_strerror(ret));
  }
  {
    int ret = libusb_kernel_driver_active(sdr->dev_handle,0);
    if(ret != 0){
      fprintf(stderr,"Kernel driver active. Trying to detach kernel driver: %s\n",libusb_strerror(ret));
      if((ret = libusb_detach_kernel_driver(sdr->dev_handle,0)) != 0) {
	fprintf(stderr,"Could not detach kernel driver from an interface: %s\n",libusb_strerror(ret));
	goto quit;
      }
    }
  }
  libusb_get_config_descriptor(device, 0, &sdr->config);
  {
    int const ret = libusb_claim_interface(sdr->dev_handle, 0);
    if(ret != 0){
      fprintf(stderr, "Error claiming USB interface: %s\n",libusb_strerror(ret));
      goto quit;
    }
  }
  {
    // Query firmware identity: [hw model, fw major, fw minor, req count]
    unsigned char info[4];
    int ret = control_recv(sdr->dev_handle, TESTFX3, 0, 0, info, sizeof(info));
    if(ret < (int)sizeof(info)){
      fprintf(stderr,"TESTFX3 firmware query failed\n");
    } else {
      sdr->fw_major = info[1];
      sdr->fw_minor = info[2];
      fprintf(stderr,"RX888 hardware 0x%02x, firmware %u.%u\n", info[0], info[1], info[2]);
    }
  }
  {
    // All this just to get sdr->pktsize?
    struct libusb_interface_descriptor const *interfaceDesc = &(sdr->config->interface[0].altsetting[0]);
    assert(interfaceDesc != NULL);
    struct libusb_endpoint_descriptor const *endpointDesc = &interfaceDesc->endpoint[0];
    assert(endpointDesc != NULL);

    struct libusb_ss_endpoint_companion_descriptor *ep_comp = NULL;
    int const ret = libusb_get_ss_endpoint_companion_descriptor(NULL,endpointDesc,&ep_comp);
    if(ret != 0){
      fprintf(stderr,"libusb_get_ss_endpoint_companion_descriptor failed: %s\n",libusb_error_name(ret));
      goto quit;
    }
    assert(ep_comp != NULL);
    sdr->pktsize = endpointDesc->wMaxPacketSize * (ep_comp->bMaxBurst + 1);
    libusb_free_ss_endpoint_companion_descriptor(ep_comp);
  }
  sdr->databuffers = (u_char **)calloc(queuedepth,sizeof(u_char *));
  if(sdr->databuffers == NULL){
    fprintf(stderr,"Failed to allocate data buffers\n");
    goto quit;
  }
  sdr->transfers = (struct libusb_transfer **)calloc(queuedepth,sizeof(struct libusb_transfer *));
  if(sdr->transfers == NULL){
    fprintf(stderr,"Failed to allocate transfer buffers\n");
    goto quit;
  }
  for(unsigned int i = 0; i < queuedepth; i++){
    sdr->databuffers[i] = (u_char *)malloc(reqsize * sdr->pktsize);
    if(sdr->databuffers[i] == NULL)
      goto quit;
    sdr->transfers[i] = libusb_alloc_transfer(0);
  }
  sdr->queuedepth = queuedepth;
  sdr->reqsize = reqsize;
  return 0;

 quit:
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

  command_send(sdr->dev_handle,GPIOFX3,sdr->gpios);
  sdr->dither = dither;
  sdr->randomizer = randomizer;
}

static void rx888_set_att(struct sdrstate *sdr,double att,bool vhf){
  assert(sdr != NULL);
  struct frontend *frontend = sdr->frontend;
  assert(frontend != NULL);

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
  // switch to HF Antenna
  sdr->gpios &= ~VHF_EN;
  command_send(sdr->dev_handle,GPIOFX3,sdr->gpios);
  // HF AGC? gain?
  // Shut down tuner
  r828_write_byte(sdr, 6, R828D_R6_PWD_PDET1 | R828D_R6_FILT_3DB | (R828D_R6_PW_LNA & 1));
  r828_write_byte(sdr, 5, R828D_R5_PWD_LT | R828D_R5_PWD_LNA1);
  r828_write_byte(sdr, 7, R828D_R7_PW0_MIX| R828D_R7_MIXGAIN_MODE | (10 & R828D_R7_MIX_GAIN));
  r828_write_byte(sdr, 8, R828D_R8_PW0_AMP);
  r828_write_byte(sdr, 9, R828D_R9_PWD_IFFILT | R828D_R9_PW1_IFFILT);
  r828_write_byte(sdr, 10, (R828D_R10_PW_FILT & (1 << 5)) | (R828D_R10_FILT_CODE & 6));
  r828_write_byte(sdr, 12, R828D_R12_VGA_MODE | (R828D_R12_VGA_CODE & 5));
  r828_write_byte(sdr, 15, R828D_R15_FIXED | R828D_R15_CLK_OUT_ENB);
  r828_write_byte(sdr, 17, R828D_R17_FIXED);
  r828_write_byte(sdr, 23, R828D_R23_PW_LDO_D | R828D_R23_DIV_BUF_DUR | R828D_R23_FIXED);
  r828_write_byte(sdr, 25, R828D_R25_FIXED);

  // Shut down Si5351 CLK2 (reference for tuner)
  si5351_write_byte(sdr, SI5351_REGISTER_CLK_BASE+2, SI5351_VALUE_CLK_PDN); // CLK1
}

// Set VHF mode: enable ref clock to tuner, switch to VHF
// change sample rate?
static void rx888_set_vhf_mode(struct sdrstate *sdr){
  struct frontend *frontend = sdr->frontend;

  frontend->min_IF = -NYQUIST * frontend->samprate;
  frontend->max_IF = -15000;

  // disable HF by set max ATT
  rx888_set_att(sdr,31.5,false);  // max att 31.5 dB
  // switch to VHF Antenna
  sdr->gpios |= VHF_EN;
  command_send(sdr->dev_handle,GPIOFX3,sdr->gpios);

  // high gain, 0db
  uint8_t gain = 0x80 | 3;
  argument_send(sdr->dev_handle,AD8340_VGA,gain);

  // Configure Si5351 CLK2 output (R828D tuner reference input)
  rx888_set_tuner_ref(sdr, (long long)sdr->reference, (long long)R828D_REF);
  // set up tuner
  r828_status(sdr);
  fprintf(stderr, "R820/828 chip ID 0x%x\n",R828D_shadow[0]);

  // r5 = 0x80: loop-through OFF, LNA1 power on, LNA gain mode switch auto, LNA_GAIN = minimum
  r828_write_byte(sdr, 5, R828D_R5_PWD_LT);

  // r6 = 0x13: Power detector 1 on, power detector 3 off, filter gain 0 dB, LNA power max-3
  r828_write_byte(sdr, 6, R828D_R6_FIXED | (R828D_R6_PW_LNA & 3));

  // r7 = 0x70: Mixer on, mixer curent normal, mixer gain auto, mix gain minimum
  r828_write_byte(sdr, 7, R828D_R7_PWD_MIX | R828D_R7_PW0_MIX | R828D_R7_MIXGAIN_MODE);

  // r8 = 0xc0: mixer buffer power on, mixer buffer current low, image gain min
  r828_write_byte(sdr, 8, R828D_R8_PWD_AMP | R828D_R8_PW0_AMP);

  // r9 = 0x40: IF filter power on, if filter current low, image phase min
  r828_write_byte(sdr, 9, R828D_R9_PW1_IFFILT);

  // r10 (0xa) = 0xdb: channel filter off, filter power 2nd lowest,
  r828_write_byte(sdr, 10, R828D_R10_PWD_FILT | (R828D_R10_PW_FILT & (2 << 5)) | R828D_R10_FIXED
		  | (R828D_R10_FILT_CODE & 11));

  // r11 (0xb) = 0x6b: FLT_BW = narrowest, high pass filter corner 11
  r828_write_byte(sdr, 11, R828D_R11_FILT_BW | (R828D_R11_HPF & 11));

  // r12 (0xc) = 0xeb: VGA power on, VGA gain controlled by code 11
  r828_write_byte(sdr, 12, R828D_R12_SW_ADC | R828D_R12_PWD_VGA | R828D_R12_FIXED | (R828D_R12_VGA_CODE & 11));

  // r13 (0xd) = 0x53: LNA agc power detector high threshold = 5/15, low threshld = 3/15
  r828_write_byte(sdr, 13, (5<<4) | 3);

  // r14 (0xe) = 0x75: mixer agc threshold high = 7/15, low threshold = 5/15
  r828_write_byte(sdr, 14, (7<<4) | 5);

  // r15 (0xf) = 0x68: filter extension widest = off, clock out off, internal agc clock on, disable ring clock
  r828_write_byte(sdr, 15, (1<<5) | R828D_R15_CLK_OUT_ENB | R828D_R15_RING_CLK);

  // r16 (0x10) = 0x6c: SEL_DIV = 3, REFDIV=0, XTAL=1 ?
  r828_write_byte(sdr, 16, (3<<5) | R828D_R16_XTAL | R828D_R16_FIXED);

  // r17 (0x11) = 0xbb: PLL analog LDO 2.0 V, charge-pump = auto
  r828_write_byte(sdr, 17, (2 << 6) | (5 << 3) | R828D_R17_FIXED);

  // r18 (0x12) = 0x80: vco current = 4
  r828_write_byte(sdr, 18, 4 << 5);

  // r19 (0x13) = 0x31: VCO auto mode; low 6 bits are a VERSION TAG (ignored in auto)
  r828_write_byte(sdr, 19, 49);

  // r20 (0x14) = 0x0f: si = 0, ni = 15
  r828_write_byte(sdr, 20, 0x0f);

  // r21 (0x15) = 00
  r828_write_byte(sdr, 21, 0);

  // r22 (0x16) = 0xc0: high order divider fraction
  r828_write_byte(sdr, 22, 0xc0);

  // r23 (0x17) = 0x30: DIV_BUF_DUR = 3
  r828_write_byte(sdr, 23, R828D_R23_DIV_BUF_DUR);

  // r24 (0x18) = 0x48: ring oscillator OFF, nring = 8 (less than valid range 9-14)
  r828_write_byte(sdr, 24, R828D_R24_FIXED | 8);

  // r25 (0x19) = 0xec: RF tracking filter ON, poly-filter current = max, agc = agc_in
  r828_write_byte(sdr, 25, R828D_R25_PWD_RFFILT | R828D_R25_POLFIL_CUR | R828D_R25_FIXED);

  // r26 (0x1a) = 0x60: RFMUX = 1, AGC_CLK=1
  r828_write_byte(sdr, 26, 0x60);

  // r27 (0x1b) = 0
  r828_write_byte(sdr, 27, 0);

  // r28 (0x1c) = 0x24: mixer power-detector TOP 3rd highest?
  r828_write_byte(sdr, 28, (2 << 4) | R828D_R28_FIXED);

  // r29 (0x1d) = 0xdd: LNA_TOP = PDET2_GAIN = 5
  r828_write_byte(sdr, 29, R828D_R29_FIXED | (5 << 3) | 5);

  // r30 (0x1e) = 0x4a: FILTER_EXT = 1, PDET_CLK=10
  r828_write_byte(sdr, 30, (1<<6) | 10);

  // r31 (0x1f) = 0x40: LOOP THRU ATT enable, ring-osc power −5 dBaf
  r828_write_byte(sdr, 31, R828D_R31_FIXED);
}


// Rewritten to directly configure SI5351 CLK1 as tuner ref clock, configure R828D tuner with frequency
static double rx888_set_tuner_frequency(struct sdrstate *sdr,double f){
  assert(sdr != NULL);
  struct frontend *frontend = sdr->frontend;
  if(f == frontend->frequency)
    return f; // also catches 0->0

  if(f == 0){
    // Switching from VHF to HF
    rx888_set_hf_mode(sdr);
    // Sample rate? gain? AGC?
    frontend->frequency = 0;
    return 0;
  }
  if(f < MIN_FREQUENCY || f > MAX_FREQUENCY)
    return frontend->frequency; // invalid VHF frequency, ignore

  if(frontend->frequency == 0)
    rx888_set_vhf_mode(sdr);

  // code to program tuner pll
  // adapted from https://github.com/ik1xpv/ExtIO_sddc/blob/master/SDDC_FX3/driver/tuner_r82xx.c
  // Find nominal VCO freq
  double exact_vco = 2 * f; // VCO if we could use an exact divider
  int div_num;      // output = VCO / (2<<div_num)
  for(div_num = 0; div_num <= 5; div_num++){
    if(R828D_VCO_MIN <= exact_vco && exact_vco <= R828D_VCO_MAX)
      break;
    exact_vco *= 2;
  }
  if(div_num > 5)
    return frontend->frequency; // out of range

  r828_write_byte_mask(sdr, 26, 0, R828D_R26_PLL_AUTO_CLK); // pll tune (loop bw?) = 128k
  r828_write_byte_mask(sdr, 18, 4<<5, R828D_R18_VCOC); // vco current = 4 (100b)
  {
    // Mystery code Returns 1 anyway
    r828_status(sdr);
    uint8_t val = R828D_shadow[4];
    int vco_fine_tune = (val & R828D_R4_VCO_FINE_TUNE) >> 4;
    fprintf(stderr,"vco fine tune %d\n",vco_fine_tune);
    if(vco_fine_tune > 1)
      div_num--;
    else if(vco_fine_tune < 1)
      div_num++;
  }
  r828_write_byte(sdr, 16, div_num << 5 | R828D_R16_FIXED); // also set REFDIV low (no divider on xtal), no capacitor
  double const vco_div = 0.5 + 65536 * exact_vco / (2 * R828D_REF);
  int const nint = floor(vco_div / 65536);
  int const sdm = floor(vco_div - nint * 65536);
  assert(nint >= 13 && sdm >= 0 && sdm < 65536);
  int const ni = (nint-13) >> 2;
  int const si = nint - ((ni << 2) + 13);
  r828_write_byte(sdr, 20, ni + (si << 6)); // encoded integer divisor
  if(sdm == 0) {
    // Divisor is an exact integer, disable the fractional PLL probably to lower phase noise
    r828_write_byte_mask(sdr, 18, R828D_R18_PW_SDM, R828D_R18_PW_SDM); // disable fract pll
  } else {
    r828_write_byte(sdr, 21, sdm & 0xff);
    r828_write_byte(sdr, 22, sdm >> 8);
    r828_write_byte_mask(sdr, 18, 0, R828D_R18_PW_SDM); // enable frac pll (redundant?)
  }
  int i;
  for(i=0; i < 50; i++){
    r828_status(sdr);
    uint8_t val = R828D_shadow[2];
    if(val & R828D_R2_VCO_INDICATOR) // vco locked?
      break;
    usleep(1000);
  }
  if(i == 50){
    fprintf(stdout,"R828D PLL didn't lock\n");
    r828_write_byte_mask(sdr, 18, 0x60, R828D_R18_VCOC); // increase current
    for(i=0; i < 50; i++){
      r828_status(sdr);
      uint8_t val = R828D_shadow[2];
      if(val & R828D_R2_VCO_INDICATOR) // vco locked?
	break;
      usleep(1000);
    }
    if(i == 50)
      fprintf(stderr,"still didn't lock\n");
  }
  r828_write_byte_mask(sdr, 26, 0x08, R828D_R26_PLL_AUTO_CLK); // Drop loop bandwidth?

  double actual_vco = 2 * R828D_REF * (nint + ldexp((double)sdm,-16));
  double ff = ldexp(actual_vco, -(div_num+1)); // Actual synth frequency (important to know)
  fprintf(stderr,"nint = %d, sdm = %d, div_num = %d, ni = %d, si = %d, f=%'lf\n", nint, sdm, div_num, ni, si, ff);
  frontend->frequency = ff;
  return frontend->frequency;
}
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
  command_send(sdr->dev_handle,STARTFX3,0);
  return 0;
}

static void rx888_stop_rx(struct sdrstate *sdr){
  assert(sdr != NULL);

  while(sdr->xfers_in_progress != 0){
    if(Verbose)
      fprintf(stderr,"%d transfers are pending\n",sdr->xfers_in_progress);
    struct timeval tv = {
      .tv_sec = 1,
      .tv_usec = 0
    };
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
  }
  fprintf(stderr,"Transfers completed\n");
  command_send(sdr->dev_handle,STOPFX3,0);
}

static void rx888_close(struct sdrstate *sdr){
  assert(sdr != NULL);
  free_transfer_buffers(sdr->databuffers,sdr->transfers,sdr->queuedepth);
  sdr->databuffers = NULL;
  sdr->transfers = NULL;

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
  struct sdrstate * const sdr = (struct sdrstate *)frontend->context;
  if(frontend->lock || sdr->undersample != 1)
    return frontend->frequency;
  if(freq == 0.0){
    frontend->frequency = 0;
    rx888_set_hf_mode(sdr);
    return 0;
  } else {
    frontend->frequency = rx888_set_tuner_frequency(sdr,freq);
    return frontend->frequency;
  }
}

// Set CLK0 output, the A/D sample clock
// R15 is reset to 0, so the CLKIN divider is 1 (between 10-40 MHz)
// Both PLLs are taking their reference from the crystal (actually the external ref)
static double rx888_set_samprate(struct sdrstate *sdr, long long const reference, long long const samprate){
  assert(sdr != NULL);
  assert(reference != 0);
  assert(samprate != 0);

  si5351_solution_t best = {0};
  if(!si5351_solve(reference,samprate,&best)){
    fprintf(stderr,"si5351_solve(%'lld, %'lld) failed\n", reference, samprate);
    return 0;
  }
  bool ms_int = false;
  if(best.E == 0)
    ms_int = true;

  si5351_pvals_t pll = {0};
  si5351_get_pll_pvals(&best,&pll);
  {
    long long whole_hz,num,denom;
    whole_hz = reference * best.B / best.C;
    num = reference * best.B % best.C;
    denom = best.C;
    whole_hz += reference * best.A;
    fprintf(stderr,"RX888 Si5351 PLLA: vco = %'lld * (%'d + %'d/%'d) = %'lld",
	  reference,
	    best.A, best.B, best.C, whole_hz);
    if(num != 0)
      fprintf(stderr," + %'lld/%'lld", num, denom);
  }
  fprintf(stderr," Hz; P1=%d, P2=%d, P3=%d\n",pll.P1, pll.P2, pll.P3);
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
  si5351_write(sdr, SI5351_REGISTER_MSNA_BASE, data_clkin, sizeof data_clkin);

  si5351_pvals_t ms = {0};
  si5351_get_ms_pvals(&best,&ms);
  fprintf(stderr,"RX888 Si5351 CLK0 output divider: samprate = vco / (%'d*(%'d + %'d/%'d)) = %'lld",
	  best.R, best.D, best.E, best.F, best.fout_num);
  if(best.fout_den != 1){
    long long whole_hz = best.fout_num / best.fout_den;
    long long num = best.fout_num % best.fout_den;
    fprintf(stderr,"/%'lld = %'lld + %'lld/%'lld", best.fout_den, whole_hz, num, best.fout_den);
  }
  fprintf(stderr," Hz");
  if(best.err_num != 0)
    fprintf(stderr," (error = %'lld/%'lld)",(long long)best.err_num, best.fout_den);
  fprintf(stderr,"; P1=%d, P2=%d, P3=%d\n", ms.P1, ms.P2, ms.P3);
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
  si5351_write(sdr, SI5351_REGISTER_MS0_BASE, data_clkout, sizeof data_clkout);
  // start clock
  si5351_write_byte(sdr, SI5351_REGISTER_PLL_RESET, SI5351_VALUE_PLLA_RESET);
  // power on clock 0
  uint8_t clock_control = SI5351_VALUE_CLK_SRC_MS | SI5351_VALUE_CLK_DRV_8MA | SI5351_VALUE_MS_SRC_PLLA;

  // The SI5351_VALUE_MS_INT can be set only if the output divisor is an integer. It is in the original code, which sets it to 6.
  if(ms_int)
    clock_control |= SI5351_VALUE_MS_INT;

  si5351_write_byte(sdr, SI5351_REGISTER_CLK_BASE+0, clock_control);

  // Wait for sample clock PLL to lock
  bool clock_ok = false;
  for(int i = 0; i < 50; i++){              // ~50 ms; locks in a few ms
    uint8_t status = 0xFF, clk0 = 0xFF;
    si5351_read(sdr, SI5351_REGISTER_STATUS,  &status);          // reg 0:  bit5 LOL_A
    si5351_read(sdr, SI5351_REGISTER_CLK_BASE+0, &clk0);            // reg 16: bit7 CLK0_PDN
    if(!(status & SI5351_VALUE_LOL_A) && !(clk0 & SI5351_VALUE_CLK_PDN)){
      clock_ok = true;
      break;
    }
    usleep(1000);
  }
  if(!clock_ok)
    fprintf(stderr,"RX888 ADC clock not locked/running\n");
  return (double)best.fout_num / best.fout_den;
}
// Set CLK1 output, the R828D tuner reference (usually 16 MHz)
static double rx888_set_tuner_ref(struct sdrstate *sdr, long long const reference, long long const f){
  assert(sdr != NULL);
  assert(reference != 0);

  si5351_solution_t best = {0};
  bool ms_int = false;
  if(!si5351_solve(reference, f, &best)){
    fprintf(stderr,"si5351_solve(%'lld, %'lld) failed\n", reference, f);
    return 0;
  }
  if(best.E == 0)
    ms_int = true;

  si5351_pvals_t pll = {0};
  si5351_get_pll_pvals(&best, &pll);
  {
    long long whole_hz,num,denom;
    whole_hz = reference * best.B / best.C;
    num = reference * best.B % best.C;
    denom = best.C;
    whole_hz += reference * best.A;
    fprintf(stderr,"RX888 Si5351 PLLB: vco = %'lld * (%'d + %'d/%'d) = %'lld",
	  reference,
	    best.A, best.B, best.C, whole_hz);
    if(num != 0)
      fprintf(stderr," + %'lld/%'lld", num, denom);
  }
  fprintf(stderr," Hz; P1=%d, P2=%d, P3=%d\n",pll.P1, pll.P2, pll.P3);
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
  si5351_write(sdr, SI5351_REGISTER_MSNB_BASE, data_clkin, sizeof data_clkin);
  si5351_pvals_t ms = {0};
  si5351_get_ms_pvals(&best, &ms);
  fprintf(stderr,"RX888 Si5351 CLK2 output divider: tuner ref = vco / (%'d*(%'d + %'d/%'d)) = %'lld",
	  best.R, best.D, best.E, best.F, best.fout_num);
  if(best.fout_den != 1){
    long long whole_hz = best.fout_num / best.fout_den;
    long long num = best.fout_num % best.fout_den;
    fprintf(stderr,"/%'lld = %'lld + %'lld/%'lld", best.fout_den, whole_hz, num, best.fout_den);
  }
  fprintf(stderr," Hz");
  if(best.err_num != 0)
    fprintf(stderr," (error = %'lld/%'lld)",(long long)best.err_num, best.fout_den);
  fprintf(stderr,"; P1=%d, P2=%d, P3=%d\n", ms.P1, ms.P2, ms.P3);
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
  si5351_write(sdr, SI5351_REGISTER_MS2_BASE, data_clkout, sizeof data_clkout);
  si5351_write_byte(sdr, SI5351_REGISTER_PLL_RESET, SI5351_VALUE_PLLB_RESET);
  // power on CLK2, ref clock to R828D/R828T tuner
  uint8_t clock_control = SI5351_VALUE_CLK_SRC_MS | SI5351_VALUE_CLK_DRV_8MA | SI5351_VALUE_MS_SRC_PLLB;
  // The SI5351_VALUE_MS_INT can be set only if the output divisor is an integer.
  if(ms_int)
    clock_control |= SI5351_VALUE_MS_INT;

  si5351_write_byte(sdr, SI5351_REGISTER_CLK_BASE+2, clock_control); // turn on CLK2
  // Wait for PLLB to lock
  bool clock_ok = false;
  for(int i = 0; i < 50; i++){              // ~50 ms; locks in a few ms
    uint8_t status = 0xFF, clk1 = 0xFF;
    si5351_read(sdr, SI5351_REGISTER_STATUS,  &status);          // reg 0:  bit6 LOL_B
    si5351_read(sdr, SI5351_REGISTER_CLK_BASE+2, &clk1);         // reg 18: bit7 CLK2_PDN
    if(!(status & SI5351_VALUE_LOL_B) && !(clk1 & SI5351_VALUE_CLK_PDN)){
      clock_ok = true;
      break;
    }
    usleep(1000);
  }
  if(!clock_ok)
    fprintf(stderr,"RX888 tuner ref clock not locked/running\n");

  return (double)best.fout_num / best.fout_den;
}
