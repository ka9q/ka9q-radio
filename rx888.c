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

#include "misc.h"
#include "status.h"
#include "config.h"
#include "radio.h"
#include "rx888.h"
#include "ezusb.h"

static int const Min_samprate =      1000000; // 1 MHz, in ltc2208 spec
static int const Max_samprate =    130000000; // 130 MHz, in ltc2208 spec
static int const Default_samprate = 64800000; // Synthesizes cleanly from 27 MHz reference
static float const Nyquist = 0.47;  // Upper end of usable bandwidth, relative to 1/2 sample rate
static float const AGC_upper_limit = -15.0;   // Reduce RF gain if A/D level exceeds this in dBFS
static float const AGC_lower_limit = -22.0;   // Increase RF gain if level is below this in dBFS
static int const AGC_interval = 10;           // Seconds between runs of AGC loop
static float const Start_gain = 10.0;         // Initial VGA gain, dB
static float Power_smooth; // Arbitrary exponential smoothing factor for front end power estimate

// Reference frequency for Si5351 clock generator
static double const Min_reference = 10e6;  //  10 MHz
static double const Max_reference = 100e6; // 100 MHz
static double const Default_reference = 27e6;
// Max allowable error on reference; 1e-4 = 100 ppm. Mainly to catch entry scaling errors
static double const Max_calibrate = 1e-4;

#if 0
// Min and Max frequency for VHF/UHF tuner
static double const Min_frequency = 50e6;   //  50 MHz ?
static double const Max_frequency = 2000e6; // 2000 MHz
static uint32_t const R828D_FREQ = 16000000;     // R820T reference frequency
static double const R828D_IF_CARRIER = 4570000;
#endif

int Ezusb_verbose = 0; // Used by ezusb.c
// Global variables set by config file options in main.c
extern int Verbose;
extern volatile bool Stop_transfers; // Flag to stop receive thread upcalls

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
  double reference;
  bool randomizer;
  bool dither;
  uint32_t gpios;
  uint64_t last_sample_count; // Used to verify sample rate
  int64_t last_count_time;
  bool message_posted; // Clock rate error posted last time around
  float scale;         // Scale samples for #bits and front end gain
  int undersample;     // Use undersample aliasing on baseband input for VHF/UHF. n = 1 => no undersampling

  pthread_t cmd_thread;
  pthread_t proc_thread;
  pthread_t agc_thread;
};

static void rx_callback(struct libusb_transfer *transfer);
static int rx888_usb_init(struct sdrstate *sdr,const char *firmware,unsigned int queuedepth,unsigned int reqsize);
static void rx888_set_dither_and_randomizer(struct sdrstate *sdr,bool dither,bool randomizer);
static void rx888_set_att(struct sdrstate *sdr,float att,bool vhf);
static void rx888_set_gain(struct sdrstate *sdr,float gain,bool vhf);
static double rx888_set_samprate(struct sdrstate *sdr,double reference,unsigned int samprate);
static void rx888_set_hf_mode(struct sdrstate *sdr);
#if 0
static double rx888_set_tuner_frequency(struct sdrstate *sdr,double frequency);
#endif
static int rx888_start_rx(struct sdrstate *sdr,libusb_transfer_cb_fn callback);
static void rx888_stop_rx(struct sdrstate *sdr);
static void rx888_close(struct sdrstate *sdr);
static void free_transfer_buffers(unsigned char **databuffers,struct libusb_transfer **transfers,unsigned int queuedepth);
static double val2gain(int g);
static int gain2val(double gain);
static void *proc_rx888(void *arg);
static void *agc_rx888(void *arg);
#if 0
static double actual_freq(double frequency);
#endif
static void rational_approximation(double value, uint32_t max_denominator, uint32_t *a, uint32_t *b, uint32_t *c);


#define N_USB_SPEEDS 6
static char const *usb_speeds[N_USB_SPEEDS] = {
  "unknown",
  "Low (1.5 Mb/s)",
  "Full (12 Mb/s)",
  "High (480 Mb/s)",
  "Super (5 Gb/s)",
  "Super+ (10Gb/s)"
};

char const *Rx888_keys[] = {
  "device",
  "firmware",
  "serial",
  "queuedepth",
  "reqsize",
  "dither",
  "rand",
  "gaincal",
  "att",
  "atten",
  "featten",
  "rfatten",
  "gainmode",
  "gain",
  "rxgain",
  "fegain",
  "reference",
  "calibrate",
  "samprate",
  "undersample",
  "description",
  "frequency",
  NULL
};


int rx888_setup(struct frontend * const frontend,dictionary const * const dictionary,char const * const section){
  assert(dictionary != NULL);
  config_validate_section(stdout,dictionary,"hardware",Rx888_keys,NULL);

  // Hardware-dependent setup
  {
    char const *device = config_getstring(dictionary,section,"device",NULL);
    if(strcasecmp(device,"rx888") != 0)
      return -1; // Not for us
  }
  struct sdrstate * const sdr = calloc(1,sizeof(struct sdrstate));
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
    fprintf(stdout,"Invalid queue depth %d, using 16\n",queuedepth);
    queuedepth = 16;
  }
  // Packets per transfer request, default 32
  int reqsize = config_getint(dictionary,section,"reqsize",32);
  if(reqsize < 1 || reqsize > 64) {
    fprintf(stdout,"Invalid request size %d, using 32\n",reqsize);
    reqsize = 32;
  }
  {
    int ret;
    if((ret = rx888_usb_init(sdr,firmware,queuedepth,reqsize)) != 0){
      fprintf(stdout,"rx888_usb_init() failed\n");
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
  frontend->rf_level_cal = config_getfloat(dictionary,section,"gaincal",-1.4);

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
      att = 31.5;
    frontend->rf_agc = false;
  }
  rx888_set_att(sdr,att,false);

  // Gain Mode now automatically set by gain; gain < 0 dB -> low, gain >= 0 dB -> high
  char const *gainmode = config_getstring(dictionary,section,"gainmode",NULL);
  if(gainmode != NULL)
    fprintf(stdout,"gainmode parameter is obsolete, now set automatically\n");

  // Gain value
  float gain = config_getfloat(dictionary,section,"gain",9999);
  gain = config_getfloat(dictionary,section,"rxgain",gain);
  gain = config_getfloat(dictionary,section,"fegain",gain);
  if(gain == 9999){
    gain = Start_gain; // Default
  } else {
    // Explicitly specifed, turn off AGC
    // should there be limits?
    frontend->rf_agc = false;
  }
  rx888_set_gain(sdr,gain,false);

  double reference = Default_reference;
  {
    char const *p = config_getstring(dictionary,section,"reference",NULL);
    if(p != NULL)
      reference = parse_frequency(p,false);
  }
  if(reference < Min_reference || reference > Max_reference){
    fprintf(stdout,"Invalid reference frequency %'lf, forcing %'lf\n",reference,Default_reference);
    reference = Default_reference;
  }
  double calibrate = config_getdouble(dictionary,section,"calibrate",0);
  if(fabsl(calibrate) >= Max_calibrate){
    fprintf(stdout,"Unreasonable frequency calibration %.3g, setting to 0\n",calibrate);
    calibrate = 0;
  }
  int samprate = Default_samprate;
  {
    char const *p = config_getstring(dictionary,section,"samprate",NULL);
    if(p != NULL)
      samprate = parse_frequency(p,false);
  }
  if(samprate < Min_samprate){
    fprintf(stdout,"Invalid sample rate %'d, forcing %'d\n",samprate,Min_samprate);
    samprate = Min_samprate;
  }
  if(samprate > Max_samprate){
    fprintf(stdout,"Invalid sample rate %'d, forcing %'d\n",samprate,Max_samprate);
    samprate = Max_samprate;
  }
  sdr->reference = reference * (1 + calibrate);
  usleep(5000);
  double actual = rx888_set_samprate(sdr,sdr->reference,samprate);
  frontend->samprate = samprate;

  sdr->undersample = config_getint(dictionary,section,"undersample",1);
  if(sdr->undersample < 1){
    fprintf(stdout,"rx888 undersample must be >= 1, ignoring\n");
    sdr->undersample = 1;
  }
  int mult = sdr->undersample / 2;
  frontend->frequency = frontend->samprate * mult;
  if(sdr->undersample & 1){
    // Somewhat arbitrary. See https://ka7oei.blogspot.com/2024/12/frequency-response-of-rx-888-sdr-at.html
    frontend->min_IF = 15000;
    frontend->max_IF = Nyquist * samprate;
  } else {
    frontend->min_IF = -Nyquist * samprate;
    frontend->max_IF = -15000;
  }
  // start clock
  control_send_byte(sdr->dev_handle,I2CWFX3,SI5351_ADDR,SI5351_REGISTER_PLL_RESET,SI5351_VALUE_PLLA_RESET);
  // power on clock 0
  //  uint8_t const clock_control = SI5351_VALUE_MS_INT | SI5351_VALUE_CLK_SRC_MS | SI5351_VALUE_CLK_DRV_8MA | SI5351_VALUE_MS_SRC_PLLA;
  uint8_t const clock_control = SI5351_VALUE_CLK_SRC_MS | SI5351_VALUE_CLK_DRV_8MA | SI5351_VALUE_MS_SRC_PLLA;
  control_send_byte(sdr->dev_handle,I2CWFX3,SI5351_ADDR,SI5351_REGISTER_CLK_BASE+0,clock_control);
  {
    char const *p = config_getstring(dictionary,section,"description","rx888");
    FREE(frontend->description);
    frontend->description = strdup(p);
    fprintf(stdout,"%s: ",frontend->description);
  }
  double ferror = actual - samprate;
  float xfer_time = (float)(sdr->reqsize * sdr->pktsize) / (sizeof(int16_t) * frontend->samprate);
  // Compute exponential smoothing constant
  // value is 1 - exp(-blocktime/tc), but use expm1() function to save precision
  float const tc  = 1.0; // 1 second
  Power_smooth = -expm1f(-xfer_time/tc);

  fprintf(stdout,"rx888 reference %'.1lf Hz, nominal sample rate %'d Hz, actual %'.3lf Hz (synth err %.3lf Hz; %.3lf ppm), AGC %s, requested gain %.1f dB, actual gain %.1f dB, atten %.1f dB, gain cal %.1f dB, dither %d, randomizer %d, USB queue depth %d, USB request size %'d * pktsize %'d = %'d bytes (%g sec)\n",
	  sdr->reference,samprate,actual,ferror, 1e6 * ferror / samprate,
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
	fprintf(stdout,"frequency = ignored in undersample mode\n");
      } else {
	double frequency = parse_frequency(p,false);
	if(frequency < Min_frequency || frequency > Max_frequency){
	  fprintf(stdout,"Invalid VHF/UHF frequency %'lf, ignoring\n",frequency);
	} else {
	  // VHF/UHF mode
	  double actual_frequency = rx888_set_tuner_frequency(sdr,frequency);
	  fprintf(stdout,"Actual VHF/UHF tuner frequency %'lf\n",actual_frequency);
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
  return 0;
}

// Come back here after common stuff has been set up (filters, etc)
int rx888_startup(struct frontend * const frontend){
  struct sdrstate * const sdr = (struct sdrstate *)frontend->context;

  // Start processing A/D data
  pthread_create(&sdr->proc_thread,NULL,proc_rx888,sdr);
  pthread_create(&sdr->agc_thread,NULL,agc_rx888,sdr);
  fprintf(stdout,"rx888 running\n");
  return 0;
}

// command to set analog gain. Turn off AGC if it was on
float rx888_gain(struct frontend * const frontend, float gain){
  struct sdrstate * const sdr = (struct sdrstate *)frontend->context;
  if(frontend->rf_agc)
    fprintf(stdout,"manual gain setting, turning off AGC\n");
  frontend->rf_agc = false;
  rx888_set_gain(sdr,gain,sdr->undersample == 1 && frontend->frequency != 0);
  return frontend->rf_gain;
}

// command to set analog attenuation. Turn off AGC if it was on
float rx888_atten(struct frontend * const frontend, float atten){
  struct sdrstate * const sdr = (struct sdrstate *)frontend->context;
  if(frontend->rf_agc)
    fprintf(stdout,"manual atten setting, turning off AGC\n");
  frontend->rf_agc = false;
  rx888_set_att(sdr,atten,sdr->undersample == 1 && frontend->frequency != 0);
  return frontend->rf_atten;
}

// Process incoming A/D samples
static void *proc_rx888(void *arg){
  struct sdrstate * const sdr = (struct sdrstate *)arg;
  assert(sdr != NULL);
  pthread_setname("proc_rx888");

  realtime();
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
  fprintf(stdout,"rx888 has aborted, exiting radiod\n");
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
    sleep(AGC_interval);
    int64_t now = gps_time_ns();
    if(now >= sdr->last_count_time + 60 * BILLION){
      // Verify approximate sample rate once per minute
      int64_t const sampcount = frontend->samples - sdr->last_sample_count;
      double const rate = BILLION * (double)sampcount / (now - sdr->last_count_time);
      double const error = fabs((rate - frontend->samprate) / (double)frontend->samprate);
      if(error > 0.01 || sdr->message_posted){
	// Post message every time the clock is off by 1% or more, or if it has just returned to nominal
	fprintf(stdout,"RX888 measured sample rate error: %'.1lf Hz vs nominal %'d Hz\n",
		rate,frontend->samprate);
	sdr->message_posted = (error > 0.01);
      }
    }
    float scaled_new_power = frontend->if_power * scale_ADpower2FS(frontend);
    float new_dBFS = power2dB(scaled_new_power);

    if(frontend->if_power > frontend->if_power_max){
      if(Verbose){
	// Don't print a message unless the increase is > 0.1 dB, the precision of the printf
	float scaled_old_power = frontend->if_power_max * scale_ADpower2FS(frontend);
	float old_dBFS = power2dB(scaled_old_power);
	if(new_dBFS >= old_dBFS + 0.1)
	  fprintf(stdout,"New input power high watermark: %.1f dBFS\n",new_dBFS);
      }
      frontend->if_power_max = frontend->if_power;
    }
    if(frontend->rf_agc && (new_dBFS > AGC_upper_limit || new_dBFS < AGC_lower_limit)){
      float const target_level = (AGC_upper_limit + AGC_lower_limit)/2;
      float const new_gain = frontend->rf_gain - (new_dBFS - target_level);
      if(new_gain < 34){ // Don't try to go above max gain
	if(Verbose)
	  fprintf(stdout,"Front end gain change from %.1f dB to %.1f dB\n",frontend->rf_gain,new_gain);
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
  struct sdrstate * const sdr = (struct sdrstate *)transfer->user_data;
  struct frontend * const frontend = sdr->frontend;
  int64_t const now = gps_time_ns();

  sdr->xfers_in_progress--;
  sdr->last_callback_time = now;

  if(transfer->status != LIBUSB_TRANSFER_COMPLETED) {
    sdr->failure_count++;
    if(Verbose > 1)
      fprintf(stdout,"Transfer %p callback status %s received %d bytes.\n",transfer,
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
  float in_energy = 0; // A/D energy accumulator
  int16_t const * const samples = (int16_t *)transfer->buffer;
  float * const wptr = frontend->in.input_write_pointer.r;
  int const sampcount = size / sizeof(int16_t);
  if(sdr->randomizer){
    for(int i=0; i < sampcount; i++){
      int32_t s = samples[i];
      s ^= (s << 31) >> 30; // Put LSB in sign bit, then shift back by one less bit to make ..ffffe or 0
      if(s == 32767 || s <= -32767){
	frontend->overranges++;
	frontend->samp_since_over = 0;
      } else {
	frontend->samp_since_over++;
      }
      in_energy += s * s;
      wptr[i] = s * sdr->scale;
    }
  } else {
    for(int i=0; i < sampcount; i++){
      if(samples[i] == 32767 || samples[i] <= -32767){
	frontend->overranges++;
	frontend->samp_since_over = 0;
      } else {
	frontend->samp_since_over++;
      }
      wptr[i] = sdr->scale * samples[i];
      in_energy += samples[i] * samples[i];
    }
  }
  frontend->timestamp = now;
  write_rfilter(&frontend->in,NULL,sampcount); // Update write pointer, invoke FFT if block is complete

  // These blocks are kinda small, so exponentially smooth the power readings
  {
    frontend->if_power_instant  = (float)in_energy / sampcount;
    frontend->if_power += Power_smooth * (frontend->if_power_instant - frontend->if_power);
  }
  frontend->samples += sampcount; // Count original samples
  if(!Stop_transfers) {
    if(libusb_submit_transfer(transfer) == 0)
      sdr->xfers_in_progress++;
  }
}

static int rx888_usb_init(struct sdrstate *const sdr,const char * const firmware,unsigned int const queuedepth,unsigned int const reqsize){
  if(firmware == NULL){
    fprintf(stdout,"Firmware not loaded and not available\n");
    return -1;
  }
  char full_firmware_file[PATH_MAX];
  memset(full_firmware_file,0,sizeof(full_firmware_file));
  dist_path(full_firmware_file,sizeof(full_firmware_file),firmware);

  {
    int ret = libusb_init(NULL);
    if(ret != 0){
      fprintf(stdout,"Error initializing libusb: %s\n",
	      libusb_error_name(ret));
      return -1;
    }
  }
  uint16_t const vendor_id = 0x04b4;
  uint16_t const unloaded_product_id = 0x00f3;
  uint16_t const loaded_product_id = 0x00f1;

  if(sdr->serial != 0)
    fprintf(stdout,"Looking for rx888 serial %016llx\n",(long long)sdr->serial);

  // Search for unloaded rx888s (0x04b4:0x00f3) with the desired serial, or all such devices if no serial specified
  // and load with firmware
  libusb_device **device_list;
  int dev_count = libusb_get_device_list(NULL,&device_list);
  for(int i=0; i < dev_count; i++){
    libusb_device *device = device_list[i];
    if(device == NULL)
      break; // End of list

    struct libusb_device_descriptor desc = {0};
    int rc = libusb_get_device_descriptor(device,&desc);
    if(rc != 0){
      fprintf(stdout," libusb_get_device_descriptor() failed: %s\n",libusb_strerror(rc));
      continue;
    }
    if(desc.idVendor != vendor_id || desc.idProduct != unloaded_product_id)
      continue;

    fprintf(stdout,"found rx888 vendor %04x, device %04x",desc.idVendor,desc.idProduct);
    libusb_device_handle *handle = NULL;
    rc = libusb_open(device,&handle);
    if(rc != 0 || handle == NULL){
      fprintf(stdout,", libusb_open() failed: %s\n",libusb_strerror(rc));
      continue;
    }
    if(desc.iManufacturer){
      char manufacturer[100];
      memset(manufacturer,0,sizeof(manufacturer));
      int ret = libusb_get_string_descriptor_ascii(handle,desc.iManufacturer,(unsigned char *)manufacturer,sizeof(manufacturer));
      if(ret > 0)
	fprintf(stdout,", manufacturer '%s'",manufacturer);
    }
    if(desc.iProduct){
      char product[100];
      memset(product,0,sizeof(product));
      int ret = libusb_get_string_descriptor_ascii(handle,desc.iProduct,(unsigned char *)product,sizeof(product));
      if(ret > 0)
	fprintf(stdout,", product '%s'",product);
    }
    char serial[100];
    memset(serial,0,sizeof(serial));
    if(desc.iSerialNumber){
      int ret = libusb_get_string_descriptor_ascii(handle,desc.iSerialNumber,(unsigned char *)serial,sizeof(serial));
      if(ret > 0){
	fprintf(stdout,", serial '%s'",serial);
      }
    }
    // The proper serial number doesn't appear until the device is loaded with firmware, so load all we find
    fprintf(stdout,", loading rx888 firmware file %s",full_firmware_file);
    if(ezusb_load_ram(handle,full_firmware_file,FX_TYPE_FX3,IMG_TYPE_IMG,1) == 0){
      fprintf(stdout,", done\n");
      sleep(1); // how long should this be?
    } else {
      fprintf(stdout,", failed for device %d.%d (logical)\n",
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
      fprintf(stdout," libusb_get_device_descriptor() failed: %s\n",libusb_strerror(rc));
      continue;
    }
    if(desc.idVendor != vendor_id || desc.idProduct != loaded_product_id)
      continue;

    fprintf(stdout,"found rx888 vendor %04x, device %04x",desc.idVendor,desc.idProduct);
    libusb_device_handle *handle = NULL;
    rc = libusb_open(device,&handle);
    if(rc != 0 || handle == NULL){
      fprintf(stdout," libusb_open() failed: %s\n",libusb_strerror(rc));
      continue;
    }
    if(desc.iManufacturer){
      char manufacturer[100];
      memset(manufacturer,0,sizeof(manufacturer));
      int ret = libusb_get_string_descriptor_ascii(handle,desc.iManufacturer,(unsigned char *)manufacturer,sizeof(manufacturer));
      if(ret > 0)
	fprintf(stdout,", manufacturer '%s'",manufacturer);
    }
    if(desc.iProduct){
      char product[100];
      memset(product,0,sizeof(product));
      int ret = libusb_get_string_descriptor_ascii(handle,desc.iProduct,(unsigned char *)product,sizeof(product));
      if(ret > 0)
	fprintf(stdout,", product '%s'",product);
    }
    char serial[100];
    memset(serial,0,sizeof(serial));
    if(desc.iSerialNumber){
      int ret = libusb_get_string_descriptor_ascii(handle,desc.iSerialNumber,(unsigned char *)serial,sizeof(serial));
      if(ret > 0){
	fprintf(stdout,", serial '%s'",serial);
      }
    }
    // Is this the droid we're looking for?
    uint64_t serialnum = strtoll(serial,NULL,16);
    if(sdr->serial == 0 || sdr->serial == serialnum){
      // Either the user didn't specify a serial, or this is the one he did; use it
      fprintf(stdout,", selected\n");
      sdr->dev_handle = handle;
      break;
    }
    fprintf(stdout,"\n"); // Not selected; close and keep looking
    libusb_close(handle);
    handle = NULL;
  }
  libusb_free_device_list(device_list,1);
  device_list = NULL;

  // If a device has been found, device and dev_handle will be non-NULL
  if(device == NULL || sdr->dev_handle == NULL){
    fprintf(stdout,"Error or device could not be found\n");
    goto end;
  }
  enum libusb_speed usb_speed = libusb_get_device_speed(device);
  if(usb_speed < N_USB_SPEEDS)
    fprintf(stdout,"rx888 USB speed: %s\n",usb_speeds[usb_speed]);
  else
    fprintf(stdout,"Unknown rx888 USB speed index %d\n",usb_speed);

  if(usb_speed < LIBUSB_SPEED_SUPER){
    fprintf(stdout,"rx888 USB device is not at least SuperSpeed; is it plugged into a blue USB jack?\n");
    goto end;
  }

  // Stop and reopen in case it was left running - KA9Q
  usleep(5000);
  command_send(sdr->dev_handle,STOPFX3,0);
  {
    int r = libusb_reset_device(sdr->dev_handle);
    if(r != 0){
      fprintf(stdout,"reset failed, %d\n",r);
    }
  }
  {
    int ret = libusb_kernel_driver_active(sdr->dev_handle,0);
    if(ret != 0){
      fprintf(stdout,"Kernel driver active. Trying to detach kernel driver\n");
      ret = libusb_detach_kernel_driver(sdr->dev_handle,0);
      if(ret != 0){
	fprintf(stdout,"Could not detach kernel driver from an interface\n");
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
      fprintf(stdout,"libusb_get_ss_endpoint_companion_descriptor returned: %s (%d)\n",libusb_error_name(rc),rc);
      goto end;
    }
    assert(ep_comp != NULL);
    sdr->pktsize = endpointDesc->wMaxPacketSize * (ep_comp->bMaxBurst + 1);
    libusb_free_ss_endpoint_companion_descriptor(ep_comp);
  }
  sdr->databuffers = (u_char **)calloc(queuedepth,sizeof(u_char *));
  if(sdr->databuffers == NULL){
    fprintf(stdout,"Failed to allocate data buffers\n");
    goto end;
  }
  sdr->transfers = (struct libusb_transfer **)calloc(queuedepth,sizeof(struct libusb_transfer *));
  if(sdr->transfers == NULL){
    fprintf(stdout,"Failed to allocate transfer buffers\n");
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

static void rx888_set_att(struct sdrstate *sdr,float att,bool vhf){
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

static void rx888_set_gain(struct sdrstate *sdr,float gain,bool vhf){
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

// see: SiLabs Application Note AN619 - Manually Generating an Si5351 Register Map (https://www.silabs.com/documents/public/application-notes/AN619.pdf)
static double rx888_set_samprate(struct sdrstate *sdr,double const reference,unsigned int const samprate){
  assert(sdr != NULL);

#if 0
  // Should we always do it ourselves?
  if(reference == Default_reference){
    // Use firmware to set sample rate
    command_send(sdr->dev_handle,STARTADC,samprate);
    return actual_freq((double)samprate);
  }
  if(samprate == 0){
    // power off clock 0
    control_send_byte(sdr->dev_handle,I2CWFX3,SI5351_ADDR,SI5351_REGISTER_CLK_BASE+0,SI5351_VALUE_CLK_PDN);
    return 0;
  }
#endif
  // if the requested sample rate is below 1MHz, use an R divider right before the output pin
  double r_samprate = samprate;
  uint8_t rdiv = 0;
  while (r_samprate < 1e6 && rdiv <= 7) {
    r_samprate *= 2.0;
    rdiv += 1;
  }
  if (r_samprate < 1e6) {
    fprintf(stdout,"ERROR - requested sample rate is too low: %'d\n",samprate);
    return 0;
  }

  /* choose an even integer for the output MS */
  // if this is ever changed from an integer, make sure SI5351_VALUE_MS_INT isn't set later on
  uint32_t output_ms = ((uint32_t)(SI5351_MAX_VCO_FREQ / r_samprate));
  output_ms -= output_ms % 2;
  if (output_ms < 4 || output_ms > 900) {
    fprintf(stdout,"ERROR - invalid output MS: %d  (samprate=%'d)\n",output_ms,samprate);
    return 0;
  }
  // This sets the VCO frequency
  double const vco_frequency = r_samprate * output_ms;

  // Now pick a fractional divisor for the VCO synthesizer feedback loop to give us this VCO frequency from the reference
  double const feedback_ms = vco_frequency / reference;
  /* find a good rational approximation for feedback_ms */
  uint32_t a,b,c;
  rational_approximation(feedback_ms,SI5351_MAX_DENOMINATOR,&a,&b,&c);
  double const pll_ratio = a + (double)b / (double)c;
  double const vco = reference * pll_ratio;
  double output_samprate = vco / (output_ms * (1 << rdiv));

  fprintf(stdout,"Nominal samprate %'d, reference %'lf, feedback divisor %d + %d/%d, VCO %'lf, integer divisor %d * %d, output = %'lf\n",
	  samprate,
	  reference,
	  a,b,c,
	  vco,
	  output_ms,
	  1<<rdiv,
	  output_samprate);

  // Now fine-tune the output divider to get closer
  double output_divider = vco / (samprate * (1 << rdiv));
  uint32_t d,e,f;
  rational_approximation(output_divider,SI5351_MAX_DENOMINATOR,&d,&e,&f);
  output_divider = d + (double)e/f;
  output_samprate = vco / (output_divider * (1 << rdiv));

  fprintf(stdout,"Output divider %d + %d/%d, rdiv %d, actual samprate = %'lf\n",d,e,f,(1<<rdiv),output_samprate);

  /* configure clock input and PLL */
  uint32_t const b_over_c = 128 * b / c;
  uint32_t const msn_p1 = 128 * a + b_over_c - 512;
  uint32_t const msn_p2 = 128 * b  - c * b_over_c;
  uint32_t const msn_p3 = c;

  uint8_t data_clkin[] = {
    (msn_p3 & 0x0000ff00) >>  8,
    (msn_p3 & 0x000000ff) >>  0,
    (msn_p1 & 0x00030000) >> 16,
    (msn_p1 & 0x0000ff00) >>  8,
    (msn_p1 & 0x000000ff) >>  0,
    (msn_p3 & 0x000f0000) >> 12 | (msn_p2 & 0x000f0000) >> 16,
    (msn_p2 & 0x0000ff00) >>  8,
    (msn_p2 & 0x000000ff) >>  0
  };

  control_send(sdr->dev_handle,I2CWFX3,SI5351_ADDR,SI5351_REGISTER_MSNA_BASE,data_clkin,sizeof(data_clkin));

  /* configure clock output */
  /* since the output divider is an even integer a = output_ms, b = 0, c = 1 */
  uint32_t const e_over_f = 128 * e / f;
  uint32_t const ms_p1 = 128 * d + e_over_f - 512;
  uint32_t const ms_p2 = 128 * e - f * e_over_f;
  uint32_t const ms_p3 = f;

  uint8_t data_clkout[] = {
    (ms_p3 & 0x0000ff00) >>  8,
    (ms_p3 & 0x000000ff) >>  0,
    rdiv << 5 | (ms_p1 & 0x00030000) >> 16,
    (ms_p1 & 0x0000ff00) >>  8,
    (ms_p1 & 0x000000ff) >>  0,
    (ms_p3 & 0x000f0000) >> 12 | (ms_p2 & 0x000f0000) >> 16,
    (ms_p2 & 0x0000ff00) >>  8,
    (ms_p2 & 0x000000ff) >>  0
  };

  control_send(sdr->dev_handle,I2CWFX3,SI5351_ADDR,SI5351_REGISTER_MS0_BASE,data_clkout,sizeof(data_clkout));
  return output_samprate;
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

  unsigned int ep = 1 | LIBUSB_ENDPOINT_IN;
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
      fprintf(stdout,"%d transfers are pending\n",sdr->xfers_in_progress);
    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    long long stime = gps_time_ns();
    int const ret = libusb_handle_events_timeout_completed(NULL,&tv,NULL);
    if(ret != 0)
      fprintf(stdout,"libusb error %d while stopping\n",ret);

    if(gps_time_ns() > stime + BILLION/2){ // taken more than half a second, too slow
      // Apparently triggers an assertion failure inside libusb but we don't care, we're already aborting
      // Probably should release a lock or something though
      fprintf(stdout,"libusb_handle_events_timeout_completed() timed out while stopping rx888\n");
      break;
    }
    usleep(100000);
  }

  fprintf(stdout,"Transfers completed\n");
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
static double const Vernier = 0.055744;
static double const Pregain = 7.079458;

static double val2gain(int g){
  int const msb = g & 128 ? true : false;
  int const gaincode = g & 127;
  double av = gaincode * Vernier * (1 + (Pregain - 1) * msb);
  return voltage2dB(av); // decibels
}

static int gain2val(double gain){
  int highgain = gain < 0 ? 0 : 1;
  gain = gain > 34 ? 34 : gain;
  int g = round(dB2voltage(gain) / (Vernier * (1 + (Pregain - 1)* highgain)));

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

/* best rational approximation:
 *
 *     value ~= a + b/c     (where c <= max_denominator)
 *
 * References:
 * - https://en.wikipedia.org/wiki/Continued_fraction#Best_rational_approximations
 */
static void rational_approximation(double value, uint32_t max_denominator,
                                   uint32_t *a, uint32_t *b, uint32_t *c)
{
  const double epsilon = 1e-5;

  double af;
  double f0 = modf(value, &af);
  *a = (uint32_t) af;
  *b = 0;
  *c = 1;
  double f = f0;
  double delta = f0;
  /* we need to take into account that the fractional part has a_0 = 0 */
  uint32_t h[] = {1, 0};
  uint32_t k[] = {0, 1};
  for(int i = 0; i < 100; ++i){
    if(f <= epsilon){
      break;
    }
    double anf;
    f = modf(1.0 / f,&anf);
    uint32_t an = (uint32_t) anf;
    for(uint32_t m = (an + 1) / 2; m <= an; ++m){
      uint32_t hm = m * h[1] + h[0];
      uint32_t km = m * k[1] + k[0];
      if(km > max_denominator){
        break;
      }
      double d = fabs((double) hm / (double) km - f0);
      if(d < delta){
        delta = d;
        *b = hm;
        *c = km;
      }
    }
    uint32_t hn = an * h[1] + h[0];
    uint32_t kn = an * k[1] + k[0];
    h[0] = h[1]; h[1] = hn;
    k[0] = k[1]; k[1] = kn;
  }
  return;
}
#if 0

// Determine actual (vs requested) clock frequency
// Adapted from code by Franco Venturi, K4VZ

static const uint32_t xtalFreq = 27000000;

static double actual_freq(double frequency){
    while (frequency < 1000000)
        frequency = frequency * 2;

    // Calculate the division ratio. 900,000,000 is the maximum internal
    // PLL frequency: 900MHz
    uint32_t divider = 900000000UL / frequency;
    // Ensure an even integer division ratio
    if (divider % 2) divider--;

    // Calculate the pllFrequency: the divider * desired output frequency
    uint32_t pllFreq = divider * frequency;
#if 0
    fprintf(stderr, "pllA Freq %d\n", pllFreq);
#endif

    // Determine the multiplier to get to the required pllFrequency
    uint8_t mult = pllFreq / xtalFreq;
    // It has three parts:
    //    mult is an integer that must be in the range 15..90
    //    num and denom are the fractional parts, the numerator and denominator
    //    each is 20 bits (range 0..1048575)
    //    the actual multiplier is  mult + num / denom
    uint32_t l = pllFreq % xtalFreq;
    double f = (double)l;
    f *= 1048575;
    f /= xtalFreq;
    uint32_t num = (uint32_t)f;
    // For simplicity we set the denominator to the maximum 1048575
    uint32_t denom = 1048575;

    double actualPllFreq = (double) xtalFreq * (mult + (double) num / (double) denom);
#if 0
    fprintf(stderr, "actual PLL frequency: %d * (%d + %d / %d) = %lf\n", xtalFreq, mult, num, denom,actualPllFreq);
#endif

    double actualAdcFreq = actualPllFreq / (double) divider;
#if 0
    fprintf(stderr, "actual ADC frequency: %lf / %d = %lf\n", actualPllFreq, divider, actualAdcFreq);
#endif
    return actualAdcFreq;
}
#endif
