// linked-in module for rx888 Mk ii for ka9q-radio's radiod
// Accept control commands from UDP socket
//
// Copyright (c)  2021 Ruslan Migirov <trapi78@gmail.com>
// Credit: https://github.com/rhgndf/rx888_stream
// Copyright (c)  2023 Franco Venturi K4VZ
// Copyright (c)  2023 Phil Karn KA9Q

// VHF tuner support by K4VZ July 2024
// Note: VHF tuner does not work yet -- KA9Q, 17 Aug 2024
#include <assert.h>
#include <pthread.h>
#include <libusb-1.0/libusb.h>
#include <iniparser/iniparser.h>
#include <string.h>
#include <sysexits.h>
#include <unistd.h>
#include <strings.h>
#include <stdatomic.h>
#include <stdint.h>
#include <inttypes.h>

#include "misc.h"
#include "status.h"
#include "config.h"
#include "radio.h"
#include "rx888.h"
#include "si5351.h"
#include "r828d.h"
#include "rx888_vhf_gain.h"
#include "rx888_vhf_vga_mode.h"
#include "r828d_bandwidth_profiles.h"
#include "ezusb.h"
#include "sched.h"
#include "timing.h"
#include "defaults.h"

static uint16_t const Vendor_id = 0x04b4;
static uint16_t const Unloaded_product_id = 0x00f3;
static uint16_t const Loaded_product_id = 0x00f1;

static double const MIN_SAMPRATE =      1e6; // 1 MHz, in ltc2208 spec
static double const MAX_SAMPRATE =    130e6; // 130 MHz, in ltc2208 spec
static double const DEFAULT_SAMPRATE = 64.8e6; // Synthesizes cleanly from 27 MHz reference
static float const NYQUIST = 0.47;  // Upper end of usable bandwidth, relative to 1/2 sample rate
static float const AGC_UPPER_LIMIT = -15.0;   // Reduce RF gain if A/D level exceeds this in dBFS
static float const AGC_LOWER_LIMIT = -26.0;   // Increase RF gain if level is below this in dBFS
static int const AGC_INTERVAL = 1;           // Seconds between runs of AGC loop
static float const START_GAIN = 10.0;         // Initial VGA gain, dB
static float const PTC  = 0.1; // 100 ms time constant for computing Power_smooth
static float const DEFAULT_GAINCAL = +1.4;

// Reference frequency for Si5351 clock generator
static double const DEFAULT_REFERENCE = 27e6;
// Max allowable error on reference; 1e-4 = 100 ppm. Mainly to catch entry scaling errors

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
  float high_threshold;
  float low_threshold;

  double reference;
  int blank;
  int blank_count;
  bool randomizer;
  bool dither;
  uint32_t gpios;
  uint64_t last_sample_count; // Used to verify sample rate
  int64_t last_count_time;
  bool message_posted; // Clock rate error posted last time around
  // RTP<->GPS offset / sample-loss monitor (agc_rx888, once per AGC_INTERVAL).
  // The ADC clock is GPSDO-disciplined (external ref into the Si5351) and
  // GPS_TIME is the host's GPS-disciplined clock, so the offset between host
  // wallclock and the received-sample count must be CONSTANT.  Any step means
  // samples were lost (USB transfer drop / xhci reset) or, if failures==0, a
  // measurement artifact — never clock drift.  Logged here so the loss is
  // visible and attributable instead of only surfacing as a downstream fault.
  int64_t monitor_last_gps;            // gps_time_ns at last check
  uint64_t monitor_last_samples;       // frontend->samples at last check
  unsigned long monitor_last_failures; // failure_count at last check
  bool monitor_init;                   // baseline captured
  double cumulative_loss_sec;          // running total of offset movement
  bool clock_step_logging;             // master enable for the RX888 loss monitor; config (default off)
  double clock_step_threshold;         // |move| (sec) over an interval to log; config
  bool clock_rate_log;                 // always log measured rate each minute; config
  float scale;        // Scale samples for #bits and front end gain
  int undersample;     // Use undersample aliasing on baseband input for VHF/UHF. n = 1 => no undersampling
  float dc_offset;    // A/D offset, units, used only to adjust power reading. It just goes into the FFT DC bin
  float power_smooth; // Arbitrary exponential smoothing factor for front end power estimate
  pthread_t cmd_thread;
  pthread_t proc_thread;
  pthread_t agc_thread;
  _Atomic enum state state;
  bool reset;
  // Set this to cause proc_rx888 to use regular caching writes to the FFT input ring buffer
  // the default uses non-temporal writes that bypass cache because the 20ms FFT working sets are too big
  // at high sample rates to fit in typical L3 caches and they get evicted before the FFT can read them
  // Worse, the cached stores push out the FFT's working set, slowing it down
  // But you might experiment with this on CPUs with very large caches
  bool cached_stores;
  _Atomic bool device_gone;
};

static uint8_t R828D_shadow[32];
static uint8_t R828D_status[5]; // status registers 0-4 are apparently distinct from write registers 0-4, which would have to be write-only

static void load_rx888s(char const *firmware);
static void rx_callback(struct libusb_transfer *transfer);
static int rx888_usb_init(struct sdrstate *sdr,const char *firmware,unsigned int queuedepth,unsigned int reqsize);
static void rx888_set_dither_and_randomizer(struct sdrstate *sdr,bool dither,bool randomizer);
static void rx888_set_att(struct sdrstate *sdr,float att,bool vhf);
static void rx888_set_gain(struct sdrstate *sdr,float gain,bool vhf);
static int rx888_apply_vhf_manual_gains(struct sdrstate *sdr,float rf,float if_gain);
static int rx888_apply_vhf_bandwidth(struct sdrstate *sdr,const r828d_bw_profile *profile);
static double rx888_set_samprate(struct sdrstate *sdr,double samprate);
static double rx888_set_tuner_ref(struct sdrstate *sdr, double const reference, double const f);
static void rx888_set_hf_mode(struct sdrstate *sdr);
static int rx888_start_rx(struct sdrstate *sdr,libusb_transfer_cb_fn callback);
static void rx888_stop_rx(struct sdrstate const *sdr);
static void rx888_close(struct sdrstate *sdr);
static void free_transfer_buffers(unsigned char **databuffers,struct libusb_transfer **transfers,unsigned int queuedepth);
static float val2gain(int g);
static int gain2val(float gain);
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
  if(r != 5){
    fprintf(stderr,"RX888 R828D short/failed status read: %d (expected 5)\n",r);
    return -1;
  }
  for(int i=0; i<r; i++)
    R828D_status[i] = bitrev(buf[i]);
  return r;
}
static inline int r828_read(struct sdrstate *sdr, uint8_t reg, uint8_t *val){
  // Device reads LSB first, but writes MSB first (!)
  reg &= 31;
  int r = control_recv(sdr->dev_handle, I2CRFX3, R828D_ADDR, reg, val, sizeof *val);
  if(r != 1){
    fprintf(stderr,"RX888 R828D short/failed reg %02x read: %d\n",reg,r);
    return -1;
  }
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
  int const r = control_send_byte(sdr->dev_handle, I2CWFX3, R828D_ADDR, reg, arg);
  if(r != 0){
    fprintf(stderr,"RX888 R828D register %02x write failed: %d\n",reg,r);
    return -1;
  }
  R828D_shadow[reg] = arg;  // Commit shadow only after hardware write succeeds
  return 0;
}
static inline int r828_write_byte_mask(struct sdrstate *sdr, uint8_t reg, uint8_t arg, uint8_t mask){
  reg &= 31;
  arg = (arg & mask) | (R828D_shadow[reg] & ~mask);
  return r828_write_byte(sdr,reg,arg);
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
  "att",                    // synonym for atten
  "atten",                  // fixed attenuator gain, dB. Either -10 or +10 is interprepted as 10 dB of attenuation
  "blank",                  // ad-hoc impulse noise blanker for DG8NGN
  "cached",
  "clock-rate-log",         // Log measured sample rate every minute even when within tolerance
  "clock-step-logging",     // Master enable for the RX888 sample-loss/offset-step monitor (default off)
  "clock-step-threshold",   // |RTP<->GPS offset move| (sec) over an interval to log as sample loss; default 0.05
  "description",
  "device",
  "dither",                 // Dither A/D LSB, not very useful with noisy antenna signals
  "featten",                // synonym for atten
  "fegain",                 // synonym for gain
  "firmware",
  "frequency",              // VHF tuner LO (not RF centre); RF centre = LO - profile IF
  "vhf-rf-gain",            // R828D manual LNA and mixer nominal dB
  "vhf-if-gain",            // R828D fixed VGA nominal dB
  "vhf-bandwidth-mhz",      // R828D analogue IF profile (default 8 MHz)
  "vhf-auto-gain",          // Reserved; true rejected until hardware AGC validated
  "gain",                   // fixed VGA gain, dB
  "gaincal",
  "gainmode",               // Obsolete
  "library",
  "queuedepth",
  "rand",                   // Hardware randomizer, probably doesn't help reduce spurs but it should be tested
  "reference",              // Clock reference, default 27 MHz
  "reqsize",
  "rfatten",                // synonym for atten
  "rfgain",                 // synonym for gain
  "rxgain",                 // synomym for gain
  "samprate",               // 129.6/64.8 MHz are good choices with 27 MHz reference. No fractional N, and good FFT factors
  "serial",
  "undersample",            // Use higher Nyquist zones. Requires removal of internal LPF and use of proper bandpass
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
  sdr->reset = config_getboolean(dictionary, section, "reset", sdr->reset);
  sdr->cached_stores = config_getboolean(dictionary, section, "cached", true); // use ordinary cached stores by default, false for NT
  // GPIOs
  sdr->gpios = 0;
  // Enable/disable dithering
  sdr->dither = config_getboolean(dictionary,section, "dither", false);
  // Enable/output output randomization
  sdr->randomizer = config_getboolean(dictionary,section, "rand", false);
  rx888_set_dither_and_randomizer(sdr, sdr->dither, sdr->randomizer);
  // RTP<->GPS offset / sample-loss monitor (opt-in; OFF by default so stock
  // radiod behaviour is unchanged for legacy users — see agc_rx888)
  sdr->clock_step_logging = config_getboolean(dictionary, section, "clock-step-logging", false);
  sdr->clock_step_threshold = config_getdouble(dictionary, section, "clock-step-threshold", 0.05); // seconds
  sdr->clock_rate_log = config_getboolean(dictionary, section, "clock-rate-log", false);
  sdr->blank = config_getint(dictionary, section, "blank", 0);

  // RF Gain calibration
  // WA2ZKD measured several rx888s with very consistent results
  // e.g., -90 dBm gives -91.4 dBFS with 0 dB VGA gain and 0 dB attenuation
  // If you use a preamp or converter, add its gain to gaincal
  // Note: sign convention has flipped dec 2025 to have units of dBm/FS vs FS/dbm
  // ie. an input of +1.4 dBm gives 0 dBFS with atten == rfgain == 0
  frontend->rf_level_cal = config_getfloat(dictionary,section,"gaincal",DEFAULT_GAINCAL);

  // Attenuation, default 0
  float att = fabsf(config_getfloat(dictionary,section,"att",9999));
  att = fabsf(config_getfloat(dictionary,section,"atten",att));
  att = fabsf(config_getfloat(dictionary,section,"featten",att));
  att = fabsf(config_getfloat(dictionary,section,"rfatten",att));
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
  float gain = config_getfloat(dictionary,section,"gain",9999);
  gain = config_getfloat(dictionary,section,"rfgain",gain);
  gain = config_getfloat(dictionary,section,"rxgain",gain);
  gain = config_getfloat(dictionary,section,"fegain",gain);
  if(gain == 9999){
    gain = START_GAIN; // Default
  } else {
    // Explicitly specifed, turn off AGC
    // should there be limits?
    frontend->rf_agc = false;
  }
  rx888_set_gain(sdr,gain,false);

  double reference = DEFAULT_REFERENCE;
  {
    char const *p = config_getstring(dictionary,section,"reference",NULL);
    if(p != NULL)
      reference = parse_frequency(p,false);
  }
  double samprate = DEFAULT_SAMPRATE;
  {
    char const *p = config_getstring(dictionary,section,"samprate",NULL);
    if(p != NULL)
      samprate = parse_frequency(p,false);
  }
  if(samprate < MIN_SAMPRATE || samprate > MAX_SAMPRATE){
    fprintf(stderr,"Invalid sample rate %'lf ",samprate);
    samprate = samprate < MIN_SAMPRATE ? MIN_SAMPRATE : MAX_SAMPRATE; // must be one or the other
    fprintf(stderr,"forcing %'lf\n",samprate);
  }
  sdr->reference = reference;
  samprate = rx888_set_samprate(sdr,samprate); // Update to actual samprate, if different
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
  sdr->low_threshold = config_getfloat(dictionary,section,"agc-low-threshold",AGC_LOWER_LIMIT);
  sdr->high_threshold = config_getfloat(dictionary,section,"agc-high-threshold",AGC_UPPER_LIMIT);

  float xfer_time = (float)(sdr->reqsize * sdr->pktsize) / (sizeof(int16_t) * frontend->samprate);
  // Compute exponential smoothing constant
  // value is 1 - exp(-blocktime/tc), but use expm1() function to save precision
  sdr->power_smooth = -expm1f(-xfer_time/PTC);

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

#if defined(__x86_64__)
  if(sdr->cached_stores){
    if(__builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512bw") &&
       __builtin_cpu_supports("avx2") && __builtin_cpu_supports("popcnt"))
      fprintf(stderr,"RX888 using avx512 vectorized callback with cached stores\n");
    else if(__builtin_cpu_supports("avx2") &&  __builtin_cpu_supports("popcnt"))
      fprintf(stderr,"RX888 using avx2 vectorized callback with cached stores\n");
  } else {
    if(__builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512bw") &&
       __builtin_cpu_supports("avx2") && __builtin_cpu_supports("popcnt"))
      fprintf(stderr,"RX888 using avx512 vectorized callback with non-temporal stores\n");
    else if(__builtin_cpu_supports("avx2") &&  __builtin_cpu_supports("popcnt"))
      fprintf(stderr,"RX888 using avx2 vectorized callback with non-temporal stores\n");
  }
#endif

  // VHF-UHF tuning
  {
    char const *p = config_getstring(dictionary,section,"frequency",NULL);
    if(p != NULL){
      if(sdr->undersample > 1){
	fprintf(stderr,"frequency = ignored in undersample mode\n");
      } else {
	double frequency = parse_frequency(p,false);
        if(!isfinite(frequency) || frequency < MIN_FREQUENCY || frequency > MAX_FREQUENCY){
          fprintf(stderr,"Invalid VHF/UHF tuner LO %'lf\n",frequency);
          return -1;
        } else {
          /* 'frequency' is the tuner LO, not an RF-channel centre.
           * For the default 8 MHz IF profile, LO 163.570 MHz yields
           * RF centre 159.000 MHz (RF = LO - nominal IF).
           * Other IF profiles require the operator to adjust LO if the
           * desired RF centre is to remain constant.
           */
          double const bandwidth_mhz =
            config_getdouble(dictionary,section,"vhf-bandwidth-mhz",8.0);
          if(!isfinite(bandwidth_mhz) || bandwidth_mhz < .29 ||
             bandwidth_mhz > 8.0){
            fprintf(stderr,"RX888 invalid VHF analogue bandwidth: %.6f MHz\n",
                    bandwidth_mhz);
            return -1;
          }
          uint32_t const bandwidth_hz=(uint32_t)llround(bandwidth_mhz*1e6);
          const r828d_bw_profile * const profile=r828d_bw_find(bandwidth_hz);
          if(!profile || fabs(bandwidth_mhz*1e6-(double)profile->bandwidth_hz)>1){
            fprintf(stderr,"RX888 unsupported VHF bandwidth %.6f MHz (21 exact profiles available)\n",
                    bandwidth_mhz);
            return -1;
          }
          if(config_getboolean(dictionary,section,"vhf-auto-gain",false)){
            fprintf(stderr,"RX888 VHF tuner hardware AGC not validated: use vhf-auto-gain = false\n");
            return -1;
          }
          float const vhf_rf=config_getfloat(dictionary,section,"vhf-rf-gain",37.2f);
          float const vhf_if=config_getfloat(dictionary,section,"vhf-if-gain",16.3f);
          double const actual_frequency=rx888_set_tuner_frequency(sdr,frequency);
          if(actual_frequency<=0 || !isfinite(actual_frequency) ||
             rx888_apply_vhf_bandwidth(sdr,profile)!=0 ||
             rx888_apply_vhf_manual_gains(sdr,vhf_rf,vhf_if)!=0){
            frontend->lock=false;
            fprintf(stderr,"RX888 VHF startup incomplete; refusing receiver-ready state\n");
            return -1;
          }
          frontend->frequency=actual_frequency;
          frontend->lock=true;
          fprintf(stderr,
                  "RX888 VHF marine LO %.3f MHz; nominal IF %.3f MHz; RF centre %.3f MHz; nominal span %.3f..%.3f MHz\n",
                  actual_frequency/1e6,(double)profile->if_hz/1e6,
                  (actual_frequency-profile->if_hz)/1e6,
                  (actual_frequency-profile->if_hz-profile->bandwidth_hz/2.0)/1e6,
                  (actual_frequency-profile->if_hz+profile->bandwidth_hz/2.0)/1e6);
        }
      }
    }
  }
  if(frontend->frequency == 0)
    rx888_set_hf_mode(sdr);

  // Experimental spur notching, works on coherent spurs only
  // What generates 1/8, 2/8, 3/8? And probably 4/8 too, though that's the Nyquist freq
  // and we don't use it
  frontend->spurs[0] = sdr->reference;
  frontend->spurs[1] = samprate / 8;
  frontend->spurs[2] = samprate / 4;  // 2/8
  frontend->spurs[3] = (3 * samprate) / 8;
  frontend->spurs[4] = 2 * sdr->reference;
  if(samprate - 3 * 2 * sdr->reference > 0)
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
      return 0; // Already stopped
    usleep(10000); // 10 ms
  }
  pthread_join(sdr->proc_thread, NULL);
  pthread_join(sdr->agc_thread, NULL);
  atomic_store(&sdr->state,STOPPED);
  fprintf(stderr,"rx888 stopped\n");
  return 0;
}

// command to set analog gain. Turn off AGC if it was on
float rx888_gain(struct frontend * const frontend, float gain){
  struct sdrstate * const sdr = (struct sdrstate *)frontend->context;
  if(frontend->rf_agc)
    fprintf(stderr,"manual gain setting, turning off AGC\n");
  frontend->rf_agc = false;
  rx888_set_gain(sdr,gain,sdr->undersample == 1 && frontend->frequency != 0);
  return frontend->rf_gain;
}

// command to set analog attenuation. Turn off AGC if it was on
float rx888_atten(struct frontend * const frontend, float atten){
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

  stick_core();
  {
    sdr->last_count_time = sdr->last_callback_time = gps_time_ns();
    int ret = rx888_start_rx(sdr,rx_callback);
    (void)ret;
    assert(ret == 0);
  }
  enum state s;
  while((s = atomic_load(&sdr->state)) == RUNNING || s == STARTING) {
    if(sdr->device_gone){
      // Device actually disappeared, exit immediately in case
      // it gets quickly plugged back in before 5 seconds
      fprintf(stderr,"RX888 device disappeared, exiting\n");
      rx888_close(sdr);
      exit(EX_NOINPUT);
    }
    // But also check for a silent hang with libusb_handle_events_timeout_completed()
    // Check more directly how long it's been since we last got data
    // sdr->last_callback_time is set in rx_callback()
    int const maxtime = 5;
    if(gps_time_ns() > sdr->last_callback_time + maxtime * BILLION){
      fprintf(stderr,"No rx888 data for %d seconds, quitting\n",maxtime);
      //      rx888_close(sdr);
      exit(EX_TEMPFAIL);
    }
    struct timeval tv = {
      .tv_sec = 1,
      .tv_usec = 0
    };
    int const ret = libusb_handle_events_timeout_completed(NULL,&tv,NULL);
    if(ret != 0){
      fprintf(stderr,"libusb_handle_events_timeout returned %s (%d)\n", libusb_error_name(ret), ret);
      switch(ret){
      case LIBUSB_ERROR_INTERRUPTED:
	continue;               /* signal interruption; not a device failure */
      case LIBUSB_ERROR_NO_DEVICE:
	rx888_close(sdr);
	exit(EX_NOINPUT);
      default:
	exit(EX_TEMPFAIL);
      }
    }
  }
  // probably in STOPPING state
  rx888_stop_rx(sdr);
  return NULL;
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
    // ── RTP<->GPS offset-step / sample-loss monitor (every AGC_INTERVAL) ──
    // The ADC clock is GPSDO-disciplined and GPS_TIME is the host's
    // GPS-disciplined clock, so over any interval the host time should
    // advance by exactly received_samples / nominal_rate.  A shortfall means
    // samples were LOST (the host clock ran on while the ADC count didn't) —
    // a USB transfer drop or xhci reset — which steps the published
    // (GPS_TIME, RTP_TIMESNAP) offset by the lost duration.  Catching it at
    // ~1 s resolution attributes the downstream timing fault to its cause;
    // pairing it with the USB failure count separates real loss (failures>0)
    // from a measurement artifact (failures==0).  Positive move == loss.
    // Opt-in via clock-step-logging (default off) so stock radiod is unchanged.
    if(sdr->clock_step_logging){
      uint64_t const samples_now = frontend->samples;       // 64-bit aligned: atomic read
      unsigned long const failures_now = sdr->failure_count;
      if(sdr->monitor_init){
	int64_t const d_gps = now - sdr->monitor_last_gps;
	int64_t const d_samp = (int64_t)(samples_now - sdr->monitor_last_samples);
	if(d_gps > 0 && frontend->samprate > 0){
	  double const accounted_ns = (double)d_samp * BILLION / (double)frontend->samprate;
	  double const move_sec = (double)(d_gps - (int64_t)accounted_ns) / BILLION;
	  if(fabs(move_sec) > sdr->clock_step_threshold){
	    sdr->cumulative_loss_sec += move_sec;
	    unsigned long const d_fail = failures_now - sdr->monitor_last_failures;
	    double const lost_samples = (double)d_gps * frontend->samprate / BILLION - (double)d_samp;
	    fprintf(stderr,"RX888 RTP<->GPS offset STEPPED %+.3f s over %.1f s "
		    "(~%'.0f samples %s; USB xfer failures +%lu; cumulative %+.3f s) — %s\n",
		    move_sec, (double)d_gps/BILLION, fabs(lost_samples),
		    lost_samples >= 0 ? "missing" : "extra", d_fail, sdr->cumulative_loss_sec,
		    move_sec > 0 ? (d_fail > 0 ? "SAMPLE LOSS (USB transfer drop)"
				    : "SAMPLE LOSS (no USB failure flagged — investigate)")
		    : "host-clock step / extra samples");
	  }
	}
      } else
	sdr->monitor_init = true;
      sdr->monitor_last_gps = now;
      sdr->monitor_last_samples = samples_now;
      sdr->monitor_last_failures = failures_now;
    }
    if(now >= sdr->last_count_time + 60 * BILLION){
      // Verify sample rate once per minute.  GPSDO-locked, so this should be
      // ~0 ppm; a steady deficit indicates ongoing sample loss.
      int64_t const sampcount = frontend->samples - sdr->last_sample_count;
      double const rate = BILLION * (double)sampcount / (now - sdr->last_count_time);
      double const error = (rate - frontend->samprate) / (double)frontend->samprate;
      sdr->last_count_time = now;
      sdr->last_sample_count = frontend->samples;
      bool const log_rate = ((sdr->clock_step_logging && sdr->clock_rate_log)
			     || fabs(error) > 0.01 || sdr->message_posted);
      if(log_rate){
	if(sdr->clock_step_logging)
	  // Enhanced: every minute if clock-rate-log, else >=1% error / return to nominal.
	  fprintf(stderr,"RX888 measured sample rate: %'.1lf Hz vs nominal %'lf Hz (%+.2f ppm)\n",
		  rate,frontend->samprate,1e6*error);
	else
	  // Stock behaviour: only on >=1% error or return to nominal.
	  fprintf(stderr,"RX888 measured sample rate error: %'.1lf Hz vs nominal %'lf Hz\n",
		  rate,frontend->samprate);
	sdr->message_posted = (fabs(error) > 0.01);
      }
    }
    if(frontend->if_power == 0)
      continue; // avoid -Inf dB
    float scaled_new_power = frontend->if_power * scale_ADpower2FS(frontend);
    float new_dBFS = power2dB(scaled_new_power);

    if(frontend->if_power > frontend->if_power_max){
      if(Verbose){
	// Don't print a message unless the increase is > 0.1 dB, the precision of the printf
	float scaled_old_power = frontend->if_power_max * scale_ADpower2FS(frontend);
	float old_dBFS = power2dB(scaled_old_power);
	if(new_dBFS >= old_dBFS + 0.1)
	  fprintf(stderr,"New input power high watermark: %.1f dBFS\n",new_dBFS);
      }
      frontend->if_power_max = frontend->if_power;
    }
    if(frontend->rf_agc && (new_dBFS > sdr->high_threshold || new_dBFS < sdr->low_threshold)){
      float const target_level = (sdr->high_threshold + sdr->low_threshold)/2;
      float new_gain = frontend->rf_gain - (new_dBFS - target_level);
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
#if defined(__x86_64__)
#include <immintrin.h>

// AVX2 vector version
// Horizontal sum of four uint64_t in 256 bits
__attribute__((target("avx2")))
static inline uint64_t hsum_u64x4(__m256i x){
    __m128i lo = _mm256_castsi256_si128(x);
    __m128i hi = _mm256_extracti128_si256(x, 1);
    __m128i sum = _mm_add_epi64(lo, hi);

    return (uint64_t)_mm_cvtsi128_si64(sum) +
           (uint64_t)_mm_extract_epi64(sum, 1);
}
__attribute__((target("avx2")))
static inline int64_t hsum_i64x4(__m256i x){
  __m128i lo = _mm256_castsi256_si128(x);
  __m128i hi = _mm256_extracti128_si256(x, 1);
  __m128i sum = _mm_add_epi64(lo, hi);

  return (int64_t)_mm_cvtsi128_si64(sum) +
         (int64_t)_mm_extract_epi64(sum, 1);
}
__attribute__((target("avx2")))
static int convert_avx2(struct sdrstate *sdr, float *restrict wptr,int16_t const *restrict samples, int sampcount, float scale, uint64_t *energy, int64_t *sum, bool const randomize){
  assert(((uintptr_t)wptr & 31) == 0);
  assert((sampcount & 15) == 0);

  __m256i energy0 = _mm256_setzero_si256();
  __m256i energy1 = _mm256_setzero_si256();
  __m256 const vscale = _mm256_set1_ps(scale);
  __m256i const upper = _mm256_set1_epi16(32766);
  __m256i const lower = _mm256_set1_epi16(-32766);
  __m256i const ones = _mm256_set1_epi16(1);
  __m256i sample_sums = _mm256_setzero_si256();
  int clip_count = 0;
  bool const cached = sdr->cached_stores;

  for(int i = 0; i < sampcount; i += 16){
    __m256i x16 =  _mm256_loadu_si256((__m256i const *)(samples + i)); // load 16 int16_t samples
    if(randomize){
      // derandomize ADC 2208 samples: if lsb == 1, flip all other bits
      __m256i mask = _mm256_slli_epi16(x16, 15);
      mask = _mm256_srai_epi16(mask, 14);
      x16 = _mm256_xor_si256(x16, mask);
    }
    sample_sums = _mm256_add_epi32(sample_sums, _mm256_madd_epi16(x16, ones)); // this won't overflow for sane blocksizes

    // Compute energy: sums of squares, in two halves
    __m256i const pair_energy = _mm256_madd_epi16(x16, x16); // combines pairs, produces 8 int32_t
    __m128i const energy_lo = _mm256_castsi256_si128(pair_energy);           // lower 4 int32_t
    __m128i const energy_hi = _mm256_extracti128_si256(pair_energy, 1);      // upper 4 int32_t
    energy0 = _mm256_add_epi64(energy0,_mm256_cvtepu32_epi64(energy_lo));    // unsigned extend to 4 uint64_t in 256bit vector
    energy1 = _mm256_add_epi64(energy1,_mm256_cvtepu32_epi64(energy_hi));    // unsigned extend to 4 uint64_t in 256bit vector

    // Check for overranges: samples > 32766 or < -32766
    __m256i const high = _mm256_cmpgt_epi16(x16, upper);   // groups of 16 0's or 1s for each comparison
    __m256i const low =  _mm256_cmpgt_epi16(lower, x16);
    __m256i const clipped = _mm256_or_si256(high, low);    // 0xffff if either limit
    uint32_t const mask = (uint32_t)_mm256_movemask_epi8(clipped); // Extract two bits (0x11) from each group of 16, pack into 32 bits
    clip_count += (unsigned)__builtin_popcount(mask) / 2; // because each 0xffff in clipped produces 2 bits
    if(mask != 0)
      sdr->blank_count = sdr->blank >> 4; // divide by 16 samples per operation
    if(sdr->blank_count > 0){
      sdr->blank_count--;
      x16 = _mm256_setzero_si256();
    }
    // convert to int32_t, convert to float, scale (in two groups of 8 samples each)
    __m128i const lo16 = _mm256_castsi256_si128(x16);
    __m128i const hi16 = _mm256_extracti128_si256(x16, 1);
    __m256 const lo = _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_cvtepi16_epi32(lo16)),vscale);
    __m256 const hi = _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_cvtepi16_epi32(hi16)),vscale);

    // write one complete 64-byte cache line (16 samples)
    if(cached){
      _mm256_store_ps(wptr + i, lo);
      _mm256_store_ps(wptr + i + 8, hi);
    } else {
      // non-temporal store (bypass the cache) because it's not going to stick around anyway until the FFT reads it
      // up to 20 ms from now
      _mm256_stream_ps(wptr + i, lo);
      _mm256_stream_ps(wptr + i + 8, hi);
    }
  }
  // must precede publication of the new write pointer
  if(!cached)
    _mm_sfence();

  __m128i const sum_lo = _mm256_castsi256_si128(sample_sums);
  __m128i const sum_hi = _mm256_extracti128_si256(sample_sums, 1);
  __m256i const sum_lo64 = _mm256_cvtepi32_epi64(sum_lo);
  __m256i const sum_hi64 = _mm256_cvtepi32_epi64(sum_hi);
  *sum += hsum_i64x4(sum_lo64) + hsum_i64x4(sum_hi64);
  *energy += hsum_u64x4(energy0) + hsum_u64x4(energy1);
  return clip_count;
}

// Process 32 ADC samples into two 512-bit float vectors. Keep blank_count
// in 16-sample units so blanking agrees with convert_avx2, including tails.
__attribute__((target("avx512f,avx512bw,avx2,popcnt")))
static int convert_avx512(struct sdrstate *sdr, float *restrict wptr, int16_t const *restrict samples, int sampcount, float scale, uint64_t *energy, int64_t *sum, bool const randomize){
  assert(((uintptr_t)wptr & 63) == 0);
  assert((sampcount & 15) == 0);
  // Each lane sums two int16_t values per 32-sample iteration.
  assert(sampcount <= 1048576);
  __m512i energy0 = _mm512_setzero_si512();
  __m512i energy1 = _mm512_setzero_si512();
  __m512i sample_sums = _mm512_setzero_si512();
  __m512 const vscale = _mm512_set1_ps(scale);
  __m512i const upper = _mm512_set1_epi16(32766);
  __m512i const lower = _mm512_set1_epi16(-32766);
  __m512i const ones = _mm512_set1_epi16(1);
  int clip_count = 0;
  bool const cached = sdr->cached_stores;

  int i = 0;
  for(; i + 31 < sampcount; i += 32){
    __m512i x16 = _mm512_loadu_si512((void const *)(samples + i));
    if(randomize){
      __m512i mask = _mm512_slli_epi16(x16, 15);
      mask = _mm512_srai_epi16(mask, 14);
      x16 = _mm512_xor_si512(x16, mask);
    }
    // Accumulate in 32-bit lanes, like AVX2; widen once after the loop.
    sample_sums = _mm512_add_epi32(sample_sums, _mm512_madd_epi16(x16, ones));
    // A pair of -32768 samples squares to 0x80000000: unsigned widen.
    __m512i const power = _mm512_madd_epi16(x16, x16);
    energy0 = _mm512_add_epi64(energy0, _mm512_cvtepu32_epi64(_mm512_castsi512_si256(power)));
    energy1 = _mm512_add_epi64(energy1, _mm512_cvtepu32_epi64(_mm512_extracti64x4_epi64(power, 1)));
    uint32_t const clipped = (uint32_t)(_mm512_cmpgt_epi16_mask(x16, upper) |
                                                    _mm512_cmpgt_epi16_mask(lower, x16));
    clip_count += __builtin_popcount(clipped);
    __mmask32 keep = (__mmask32)UINT32_MAX;
    for(int half = 0; half < 2; half++){
      if((clipped >> (16 * half)) & 0xffffU)
        sdr->blank_count = sdr->blank >> 4;
      if(sdr->blank_count > 0){
        sdr->blank_count--;
        keep &= ~((__mmask32)0xffffU << (16 * half));
      }
    }
    x16 = _mm512_maskz_mov_epi16(keep, x16);
    __m512 const lo = _mm512_mul_ps(_mm512_cvtepi32_ps(_mm512_cvtepi16_epi32(_mm512_castsi512_si256(x16))), vscale);
    __m512 const hi = _mm512_mul_ps(_mm512_cvtepi32_ps(_mm512_cvtepi16_epi32(_mm512_extracti64x4_epi64(x16, 1))), vscale);
    if(cached){
      _mm512_store_ps(wptr + i, lo);
      _mm512_store_ps(wptr + i + 16, hi);
    } else {
      _mm512_stream_ps(wptr + i, lo);
      _mm512_stream_ps(wptr + i + 16, hi);
    }
  }
  __m512i const sums0 = _mm512_cvtepi32_epi64(_mm512_castsi512_si256(sample_sums));
  __m512i const sums1 = _mm512_cvtepi32_epi64(_mm512_extracti64x4_epi64(sample_sums, 1));
  *sum += _mm512_reduce_add_epi64(sums0) + _mm512_reduce_add_epi64(sums1);
  *energy += (uint64_t)_mm512_reduce_add_epi64(energy0) + (uint64_t)_mm512_reduce_add_epi64(energy1);
  // Existing callback contract allows a final group of 16 samples.
  if(i < sampcount)
    clip_count += convert_avx2(sdr, wptr + i, samples + i, sampcount - i, scale, energy, sum, randomize);
  if(!cached)
    _mm_sfence(); // Complete streaming stores before publishing the write pointer.
  return clip_count;
}

#endif

// Portable C version
static int convert(struct sdrstate *sdr, float *restrict wptr, int16_t const *restrict samples, int sampcount, float scale, uint64_t *energy, int64_t *sum, bool randomize){
  int clip_count = 0;

  for(int i = 0; i < sampcount; i ++){
    int16_t x = samples[i];
    if(randomize)
      x ^= (x << 15) >> 14;
    *sum += x;
    *energy += (uint64_t) ((int32_t)x * (int32_t)x);
    if(x > 32766 || x < -32766){
      clip_count++;
      sdr->blank_count = sdr->blank;
    }
    if(sdr->blank_count > 0){
      sdr->blank_count--;
      x = 0;
    }
    wptr[i] = (float)x * scale;
  }
  return clip_count;
}

// Callback called with incoming receiver data from A/D
//static void rx_callback(struct libusb_transfer * const transfer){
void rx_callback(struct libusb_transfer * const transfer){
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
  uint64_t in_energy = 0; // A/D energy accumulator
  int64_t sum = 0;
  int16_t const * const restrict samples = (int16_t *)transfer->buffer;
  float * const restrict wptr = frontend->in.input_write_pointer.r;
  int const sampcount = size / sizeof(int16_t);
  float const scale = (float)sdr->scale;
  int overloads;
#if defined(__x86_64__)
  if(__builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512bw") &&
     __builtin_cpu_supports("avx2") && __builtin_cpu_supports("popcnt") &&
     ((uintptr_t)wptr & 63) == 0)
    overloads = convert_avx512(sdr, wptr,samples,sampcount,scale,&in_energy,&sum,sdr->randomizer);
  else if(__builtin_cpu_supports("avx2") && __builtin_cpu_supports("popcnt"))
    overloads = convert_avx2(sdr, wptr,samples,sampcount,scale,&in_energy,&sum,sdr->randomizer);
  else
#endif
    overloads = convert(sdr, wptr,samples,sampcount,scale,&in_energy,&sum,sdr->randomizer);
  if(overloads){
    frontend->overranges += overloads;
    frontend->samp_since_over = 0;
  } else
    frontend->samp_since_over += sampcount;

  // These blocks are kinda small, so exponentially smooth the power readings
  if(sampcount != 0){
    float const dc = (float)sum / sampcount;
    float const net_power = (float)in_energy / sampcount - dc * dc; // subtract DC's contribution to total RF power
    frontend->if_power += sdr->power_smooth * (net_power - frontend->if_power);
  }
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
  load_rx888s(firmware);
  // Look for a loaded RX888 with the right numbers
  if(Serial != NULL){ // specified on command line, overrides config file
    // Specified on command line, convert and compare
    sdr->serial = strtoll(Serial,NULL,16); // always hex
  }
  if(sdr->serial != 0)
    fprintf(stderr,"Looking for rx888 serial %016llx\n",(long long)sdr->serial);
  else if(USB_busnum != -1 && USB_devnum != -1)
    fprintf(stderr,"Looking for rx888 at %d:%d\n",USB_busnum,USB_devnum);

  libusb_device *device = NULL;
  libusb_device **device_list = NULL;
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

    flockfile(stderr);
    fprintf(stderr,"found rx888 vendor %04x, device %04x",desc.idVendor,desc.idProduct);
    libusb_device_handle *handle = NULL;
    int rc = libusb_open(device,&handle);
    if(rc != 0 || handle == NULL){
      fprintf(stderr," libusb_open() failed: %s\n",libusb_strerror(rc));
      funlockfile(stderr);
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
      funlockfile(stderr);
      continue; // Keep looking, there just might be another
    }
    // Is this the droid we're looking for?
    uint64_t current_serialnum = strtoll(serial,NULL,16); // device serial
    uint8_t current_busnum = libusb_get_bus_number(device);
    uint8_t current_devnum = libusb_get_device_address(device);

    if(sdr->serial == 0 && USB_busnum == -1 && USB_devnum == -1){
      // No particular unit specified, so take it
      fprintf(stderr,", selected by default\n");
      funlockfile(stderr);
      sdr->dev_handle = handle;
      sdr->serial = current_serialnum;
      break;
    } else if(sdr->serial == current_serialnum){
      fprintf(stderr,", selected by cmd line serial number\n");
      funlockfile(stderr);
      sdr->dev_handle = handle;
      break;
    } else if(current_busnum == USB_busnum && current_devnum == USB_devnum){
      fprintf(stderr,", selected by USB device ID\n");
      funlockfile(stderr);
      sdr->serial = current_serialnum;
      sdr->dev_handle = handle;
      break;
    } else {
      fprintf(stderr,"\n"); // Not selected; close and keep looking
      funlockfile(stderr);
      libusb_close(handle);
      handle = NULL;
    }
  }
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
    int const rc = libusb_claim_interface(sdr->dev_handle, 0);
    if(rc != 0){
      fprintf(stderr, "Error claiming USB interface: %s (%d): %s\n",
	      libusb_error_name(rc), rc, libusb_strerror(rc));
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
  libusb_free_device_list(device_list,1);
  device_list = NULL;
  device = NULL;

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

  sdr->transfers = NULL;
  sdr->databuffers = NULL;

  if(sdr->dev_handle != NULL)
    libusb_release_interface(sdr->dev_handle,0);
  sdr->dev_handle = NULL;

  if(sdr->config)
    libusb_free_config_descriptor(sdr->config);
  sdr->config = NULL;

  if(sdr->dev_handle)
    libusb_close(sdr->dev_handle);
  sdr->dev_handle = NULL;

  if(device_list != NULL)
    libusb_free_device_list(device_list,1);
  device_list = NULL;

  libusb_exit(NULL);
  return -1;
}
// Load any Rx888s found in bootloader mode
// We can't be selective since the serial numbers aren't visible until the firmware is running
// With the rx888 bootloader launched by udev, this section will usually not find anything
// but a race is possible at boot time when systemd starts both at the same time
// So it's recommended to let rx888_boot handle unloaded devices
static void load_rx888s(char const *firmware){
  if(firmware == NULL || strlen(firmware) == 0)
    return;

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
    if(desc.idVendor != Vendor_id || desc.idProduct != Unloaded_product_id)
      continue;

    flockfile(stderr);
    fprintf(stderr,"found unloaded rx888 vendor %04x, device %04x",desc.idVendor,desc.idProduct);
    libusb_device_handle *handle = NULL;
    rc = libusb_open(device,&handle);
    if(rc != 0 || handle == NULL){
      fprintf(stderr,", libusb_open() failed: %s\n",libusb_strerror(rc));
      funlockfile(stderr);
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
    char full_firmware_file[PATH_MAX] = {0};
    dist_path(full_firmware_file,sizeof(full_firmware_file),firmware);
    fprintf(stderr,", loading rx888 firmware file %s\n",full_firmware_file);
    funlockfile(stderr);
    if(ezusb_load_ram(handle,full_firmware_file,FX_TYPE_FX3,IMG_TYPE_IMG,1) == 0){
      fprintf(stderr,"rx888 loaded\n");
      sleep(1); // how long should this be?
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

/* Explicit hardware control: RF LNA/mixer and fixed IF VGA use index
 * tables, not nominal dB arguments to FX3 gain commands. All gain writes
 * complete before reported frontend values are published.
 */
static int rx888_apply_vhf_manual_gains(struct sdrstate *sdr,float rf,float if_gain){
  struct frontend *frontend=sdr->frontend;
  int const rf_idx=rx888_vhf_nearest_rf(rf);
  int const if_idx=rx888_vhf_nearest_if(if_gain);
  if(r828_write_byte_mask(sdr,0x05,0x10,0x10)!=0 ||
     r828_write_byte_mask(sdr,0x07,0x00,0x10)!=0 ||
     r828_write_byte_mask(sdr,0x05,rx888_vhf_lna[rf_idx],0x0f)!=0 ||
     r828_write_byte_mask(sdr,0x07,rx888_vhf_mixer[rf_idx],0x0f)!=0 ||
     r828_write_byte_mask(sdr,0x0c,(uint8_t)if_idx,RX888_R828D_FIXED_VGA_MASK)!=0){
    fprintf(stderr,"RX888 VHF manual gain setup failed\n");
    return -1;
  }
  frontend->rf_atten=49.6f-rx888_vhf_rf_db[rf_idx];
  frontend->rf_gain=rx888_vhf_if_db[if_idx];
  frontend->rf_agc=false; /* Not the unrelated HF software gain loop */
  sdr->scale=scale_AD(frontend);
  fprintf(stderr,"RX888 fixed IF VGA R0C=%02x VGA_MODE=%u (expected 0); RF %.1f dB IF %.1f dB\n",
          R828D_shadow[0x0c],(unsigned)((R828D_shadow[0x0c]>>4)&1),
          rx888_vhf_rf_db[rf_idx],rx888_vhf_if_db[if_idx]);
  return 0;
}

static int rx888_apply_vhf_bandwidth(struct sdrstate *sdr,
                                     const r828d_bw_profile *profile){
  if(!profile)return -1;
  /* R0A only its filter code nibble; R0B never overrides calibration bit 4.
   * Preserve the existing high bits, which affect the ADC signal path.
   */
  if(r828_write_byte_mask(sdr,0x0a,profile->r0a,0x0f)!=0 ||
     r828_write_byte_mask(sdr,0x0b,profile->r0b,0xef)!=0 ||
     r828_write_byte_mask(sdr,0x1e,profile->r1e,0x40)!=0){
    fprintf(stderr,"RX888 VHF analogue bandwidth programming failed: %u Hz\n",
            profile->bandwidth_hz);
    return -1;
  }
  fprintf(stderr,"RX888 VHF bandwidth %u Hz; nominal IF +%u Hz; R0A=%02x R0B=%02x R1E=%02x (software shadow)\n",
          profile->bandwidth_hz,profile->if_hz,
          R828D_shadow[0x0a],R828D_shadow[0x0b],R828D_shadow[0x1e]);
  return 0;
}

static void rx888_set_att(struct sdrstate *sdr,float att,bool vhf){
  assert(sdr != NULL);
  struct frontend *frontend = sdr->frontend;
  assert(frontend != NULL);
  if(!vhf){
    frontend->rf_atten=att;
    sdr->scale=scale_AD(frontend);
    int const arg=(int)(att*2);
    argument_send(sdr->dev_handle,DAT31_ATT,arg);
  } else {
    int const idx=rx888_vhf_nearest_rf(isfinite(att)?49.6f-att:37.2f);
    if(r828_write_byte_mask(sdr,0x05,0x10,0x10)!=0 ||
       r828_write_byte_mask(sdr,0x07,0x00,0x10)!=0 ||
       r828_write_byte_mask(sdr,0x05,rx888_vhf_lna[idx],0x0f)!=0 ||
       r828_write_byte_mask(sdr,0x07,rx888_vhf_mixer[idx],0x0f)!=0){
      fprintf(stderr,"RX888 VHF RF gain runtime update failed\n");
      return;
    }
    frontend->rf_atten=49.6f-rx888_vhf_rf_db[idx];
    sdr->scale=scale_AD(frontend);
  }
}

static void rx888_set_gain(struct sdrstate *sdr,float gain,bool vhf){
  assert(sdr != NULL);
  struct frontend *frontend = sdr->frontend;
  assert(frontend != NULL);
  if(!vhf){
    int const arg=gain2val(gain);
    argument_send(sdr->dev_handle,AD8340_VGA,arg);
    frontend->rf_gain=val2gain(arg);
  } else {
    int const idx=rx888_vhf_nearest_if(gain);
    /* Mask 0x1f clears R0C[4] VGA_MODE as well as setting code[3:0]. */
    if(r828_write_byte_mask(sdr,0x0c,(uint8_t)idx,RX888_R828D_FIXED_VGA_MASK)!=0){
      fprintf(stderr,"RX888 VHF IF VGA runtime update failed\n");
      return;
    }
    frontend->rf_gain=rx888_vhf_if_db[idx];
  }
  sdr->scale=scale_AD(frontend);
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
static int rx888_set_vhf_mode(struct sdrstate *sdr){
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
  if(rx888_set_tuner_ref(sdr, sdr->reference, R828D_REF) <= 0){
    fprintf(stderr,"RX888 VHF CLK2 reference failed to start\n");
    return -1;
  }
  // set up tuner
  if(r828_status(sdr)!=5)return -1;
  fprintf(stderr, "R820/828 chip ID 0x%x\n",R828D_status[0]);

#if 1
  /* Those initial values start from REG_SHADOW_START */
  static uint8_t r82xx_init_array[] = {
    0x80,	//Reg 0x05
    0x12, 	//Reg 0x06
    0x70,	//Reg 0x07
    0xc0, 	//Reg 0x08
    0x40, 	//Reg 0x09
    0xdb, 	//Reg 0x0a
    0x6b,	//Reg 0x0b
    RX888_R828D_VGA_INIT, //Reg 0x0c; fixed VGA, mode bit 4 cleared
    0x53, 	//Reg 0x0d
    0x53, 	//Reg 0x0e
    0x68,	//Reg 0x0f
    0x64, 	//Reg 0x10
    0xbb, 	//Reg 0x11
    0x80, 	//Reg 0x12
    0x00,	//Reg 0x13
    0x0f, 	//Reg 0x14
    0x00, 	//Reg 0x15
    0xc0, 	//Reg 0x16
    0x30,	//Reg 0x17
    0x48, 	//Reg 0x18
    0xec, 	//Reg 0x19
    0x60, 	//Reg 0x1a
    0x00,	//Reg 0x1b
    0x24,	//Reg 0x1c
    0xdd, 	//Reg 0x1d
    0x0e, 	//Reg 0x1e
    0x40	//Reg 0x1f
  };
  for(int i=5; i < 32; i++){
    if(r828_write_byte(sdr,i,r82xx_init_array[i-5])!=0){
      fprintf(stderr,"RX888 VHF register initialization failed at %02x\n",i);
      return -1;
    }
  }

#else

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
#endif
  return 0;
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

  if(frontend->frequency == 0 && rx888_set_vhf_mode(sdr)!=0)
    return 0;

  // Diagnostic: RF tracking table; preserve clock and IF settings.
  {
    unsigned band = 0;
    unsigned const count = sizeof(Freq_ranges) / sizeof(Freq_ranges[0]);
    while(band + 1 < count && f >= 1e6 * Freq_ranges[band + 1][0])
      band++;
    if(r828_write_byte_mask(sdr,0x17,Freq_ranges[band][1],0x08)!=0 ||
       r828_write_byte_mask(sdr,0x1a,Freq_ranges[band][2],0xc3)!=0 ||
       r828_write_byte(sdr,0x1b,Freq_ranges[band][3])!=0)
      return 0;
    fprintf(stderr,"RX888 tracking test: band=%d MHz, R17=%02x R1A=%02x R1B=%02x\n",
            Freq_ranges[band][0],R828D_shadow[0x17],
            R828D_shadow[0x1a],R828D_shadow[0x1b]);
  }

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

  if(r828_write_byte_mask(sdr,26,0,R828D_R26_PLL_AUTO_CLK)!=0 ||
     r828_write_byte_mask(sdr,18,4<<5,R828D_R18_VCOC)!=0)
    return 0;
  {
    // Mystery code Returns 1 anyway
    if(r828_status(sdr)!=5)return 0;
    uint8_t val = R828D_status[4];
    int vco_fine_tune = (val & R828D_R4_VCO_FINE_TUNE) >> 4;
    fprintf(stderr,"vco fine tune %d\n",vco_fine_tune);
    if(vco_fine_tune > 1)
      div_num--;
    else if(vco_fine_tune < 1)
      div_num++;
  }
  if(div_num<0 || div_num>5 ||
     r828_write_byte(sdr,16,(uint8_t)((div_num<<5)|R828D_R16_FIXED))!=0)
    return 0;
  double const vco_div = 0.5 + 65536 * exact_vco / (2 * R828D_REF);
  int const nint = floor(vco_div / 65536);
  int const sdm = floor(vco_div - nint * 65536);
  if(nint<13 || sdm<0 || sdm>=65536){
    fprintf(stderr,"RX888 R828D PLL divisor invalid\n");
    return 0;
  }
  int const ni = (nint-13) >> 2;
  int const si = nint - ((ni << 2) + 13);
  if(r828_write_byte(sdr,20,(uint8_t)(ni+(si<<6)))!=0)return 0;
  if(sdm == 0) {
    // Divisor is an exact integer, disable the fractional PLL probably to lower phase noise
    if(r828_write_byte_mask(sdr,18,R828D_R18_PW_SDM,R828D_R18_PW_SDM)!=0)
      return 0;
  } else {
    if(r828_write_byte(sdr,21,(uint8_t)(sdm&0xff))!=0 ||
       r828_write_byte(sdr,22,(uint8_t)(sdm>>8))!=0 ||
       r828_write_byte_mask(sdr,18,0,R828D_R18_PW_SDM)!=0)
      return 0;
  }
  int i;
  for(i=0; i < 50; i++){
    if(r828_status(sdr)!=5)return 0;
    uint8_t val = R828D_status[2];
    if(val & R828D_R2_VCO_INDICATOR) // vco locked?
      break;
    usleep(1000);
  }
  if(i == 50){
    fprintf(stdout,"R828D PLL didn't lock\n");
    if(r828_write_byte_mask(sdr,18,0x60,R828D_R18_VCOC)!=0)
      return 0;
    for(i=0; i < 50; i++){
      if(r828_status(sdr)!=5)return 0;
      uint8_t val = R828D_status[2];
      if(val & R828D_R2_VCO_INDICATOR) // vco locked?
	break;
      usleep(1000);
    }
    if(i == 50){
      fprintf(stderr,"still didn't lock, regs =");
      for(int j=0; j < 5; j++)
	fprintf(stderr," %0x",R828D_status[j]);
      fprintf(stderr,"\n");
      return 0; // Never publish a non-locked tuner as ready
    }
  }
  if(r828_write_byte_mask(sdr,26,0x08,R828D_R26_PLL_AUTO_CLK)!=0)
    return 0;

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

static void rx888_stop_rx(struct sdrstate const * const sdr){
  assert(sdr != NULL);

  while(sdr->xfers_in_progress != 0){
    if(Verbose)
      fprintf(stderr,"%d transfers are pending\n",sdr->xfers_in_progress);
    struct timeval tv = {
      .tv_sec = 1,
      .tv_usec = 0
    };
    long long const stime = gps_time_ns();
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
static float const VERNIER = 0.055744;
static float const PREGAIN = 7.079458;

static float val2gain(int g){
  int const msb = g & 128 ? true : false;
  int const gaincode = g & 127;
  float av = gaincode * VERNIER * (1 + (PREGAIN - 1) * msb);
  return voltage2dB(av); // decibels
}

static int gain2val(float gain){
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

static double rx888_set_samprate(struct sdrstate *sdr, double const samprate){
  assert(sdr != NULL);
  assert(isfinite(samprate) && samprate > 0);

  si5351_solution_t best = {0};
  if(!si5351_solve(sdr->reference,samprate,&best)){
    fprintf(stderr,"si5351_solve(reference=%'lf, samprate=%'lf) failed\n", sdr->reference, samprate);
    return 0;
  }
  bool ms_int = false;
  if(best.E == 0)
    ms_int = true;

  si5351_pvals_t pll = {0};
  si5351_get_pll_pvals(&best,&pll);
  // doubles ref and vco are for display only
  double const vco = sdr->reference * (best.A + (double)best.B / best.C);
  flockfile(stderr);
  fprintf(stderr,"RX888 Si5351 PLL: vco = %'lf * (%'u + %'u/%'u) = %'lf Hz;",
	  sdr->reference, best.A, best.B, best.C, vco);
  fprintf(stderr," P1=%u, P2=%u, P3=%u\n",pll.P1, pll.P2, pll.P3);
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
  // Calculate actual output sample rate
  double const fout = vco / (best.R * (best.D + (double)best.E / best.F));
  fprintf(stderr,"RX888 Si5351 output divider: samprate = vco / (%'u*(%'u + %'u/%'u)) = %'lf Hz",
	  best.R, best.D, best.E, best.F, fout);
  fprintf(stderr,"; P1=%u, P2=%u, P3=%u\n", ms.P1, ms.P2, ms.P3);
  funlockfile(stderr);
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
  // clkin input divide by 1, both PLLs sourced from Xtal (is there any other choice?)
  si5351_write_byte(sdr, SI5351_REGISTER_PLL_SOURCE, 0);
  // start clock
  si5351_write_byte(sdr, SI5351_REGISTER_PLL_RESET, SI5351_VALUE_PLLA_RESET);
  // power on clock 0
  uint8_t clock_control = SI5351_VALUE_CLK_SRC_MS | SI5351_VALUE_CLK_DRV_8MA | SI5351_VALUE_MS_SRC_PLLA;

  // The SI5351_VALUE_MS_INT can be set only if the output divisor is an integer. It is in the original code, which sets it to 6.
  if(ms_int)
    clock_control |= SI5351_VALUE_MS_INT;

  si5351_write_byte(sdr, SI5351_REGISTER_CLK_BASE+0, clock_control);

  // Ensure clk0 output is not disabled
  uint8_t enb;
  si5351_read(sdr, SI5351_REGISTER_OUTPUT_CTL,&enb);
  enb &= ~(1<<0);
  si5351_write_byte(sdr, SI5351_REGISTER_OUTPUT_CTL,enb);
  si5351_write_byte(sdr, SI5351_REGISTER_OUTPUT_MASK, 0); // disable OEB pin (doesn't exist anyway)


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
  return fout;
}
// Set CLK2 output, the R828D tuner reference (usually 16 MHz)
static double rx888_set_tuner_ref(struct sdrstate *sdr, double const reference, double const f){
  assert(sdr != NULL);
  assert(reference != 0);

  si5351_solution_t best = {0};
  bool ms_int = false;
  if(!si5351_solve(reference, f, &best)){
    fprintf(stderr,"si5351_solve(%'lf, %'lf) failed\n", reference, f);
    return 0;
  }
  if(best.E == 0)
    ms_int = true;

  si5351_pvals_t pll = {0};
  si5351_get_pll_pvals(&best, &pll);
  double const vco = reference * (best.A + (double)best.B / best.C);
  fprintf(stderr,"RX888 Si5351 PLLB: vco=%.6f Hz; P1=%u P2=%u P3=%u\n",
          vco,pll.P1,pll.P2,pll.P3);
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
  double const fout = vco / (best.R * (best.D + (double)best.E / best.F));
  fprintf(stderr,"RX888 Si5351 CLK2 tuner reference=%.6f Hz; P1=%u P2=%u P3=%u\n",
          fout,ms.P1,ms.P2,ms.P3);
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
  // clkin input divide by 1, both PLLs sourced from Xtal (is there any other choice?)
  si5351_write_byte(sdr, SI5351_REGISTER_PLL_SOURCE, 0);

  si5351_write_byte(sdr, SI5351_REGISTER_PLL_RESET, SI5351_VALUE_PLLB_RESET);
  // power on CLK2, ref clock to R828D/R828T tuner
  uint8_t clock_control = SI5351_VALUE_CLK_SRC_MS | SI5351_VALUE_CLK_DRV_8MA | SI5351_VALUE_MS_SRC_PLLB;
  // The SI5351_VALUE_MS_INT can be set only if the output divisor is an integer.
  if(ms_int)
    clock_control |= SI5351_VALUE_MS_INT;

  si5351_write_byte(sdr, SI5351_REGISTER_CLK_BASE+2, clock_control); // turn on CLK2
  // Ensure clk2 output is not disabled
  uint8_t enb;
  si5351_read(sdr, SI5351_REGISTER_OUTPUT_CTL,&enb);
  enb &= ~(1<<2);
  si5351_write_byte(sdr, SI5351_REGISTER_OUTPUT_CTL,enb);
  si5351_write_byte(sdr, SI5351_REGISTER_OUTPUT_MASK, 0); // disable OEB pin (doesn't exist anyway)


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
  if(!clock_ok){
    fprintf(stderr,"RX888 tuner ref clock not locked/running; aborting VHF setup\n");
    return 0;
  }
  return fout;
}
