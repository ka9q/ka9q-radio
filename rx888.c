// linked-in module for rx888 Mk ii for ka9q-radio's radiod
// Accept control commands from UDP socket
//
// Copyright (c)  2021 Ruslan Migirov <trapi78@gmail.com>
// Credit: https://github.com/rhgndf/rx888_stream
// Copyright (c)  2023 Franco Venturi
// Copyright (c)  2023 Phil Karn

#define _GNU_SOURCE 1
#include <assert.h>
#include <pthread.h>
#include <libusb-1.0/libusb.h>
#include <iniparser/iniparser.h>
#if defined(linux)
#include <bsd/string.h>
#endif
#include <sysexits.h>

#include "misc.h"
#include "status.h"
#include "config.h"
#include "radio.h"
#include "rx888.h"
#include "ezusb.h"

static float const power_smooth = 0.05;

int Ezusb_verbose = 0; // Used by ezusb.c
// Global variables set by config file options in main.c
extern int Verbose;
extern int Overlap; // Forward FFT overlap factor, samples
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

  // State for sample rate conversion by interpolation
  float last;
  double sample_phase; // must be double precision - phase increments are small

  // USB transfer
  int xfers_in_progress;
  unsigned int queuedepth; // Number of requests to queue
  unsigned int reqsize;    // Request size in number of packets
  unsigned long success_count;  // Number of successful transfers
  unsigned long failure_count;  // Number of failed transfers

  // RF Hardware
  bool randomizer;
  bool dither;
  bool highgain;

  pthread_t cmd_thread;
  pthread_t proc_thread;  
};

static void rx_callback(struct libusb_transfer *transfer);
static int rx888_usb_init(struct sdrstate *sdr,const char *firmware,unsigned int queuedepth,unsigned int reqsize);
static void rx888_set_dither_and_randomizer(struct sdrstate *sdr,bool dither,bool randomizer);
static void rx888_set_att(struct sdrstate *sdr,float att);
static void rx888_set_gain(struct sdrstate *sdr,float gain);
static double rx888_set_samprate(struct sdrstate *sdr,unsigned int samprate);
static int rx888_start_rx(struct sdrstate *sdr,libusb_transfer_cb_fn callback);
static void rx888_stop_rx(struct sdrstate *sdr);
static void rx888_close(struct sdrstate *sdr);
static void free_transfer_buffers(unsigned char **databuffers,struct libusb_transfer **transfers,unsigned int queuedepth);
static double val2gain(int g);
static int gain2val(bool highgain, double gain);
static void *proc_rx888(void *arg);
static double actual_freq(double frequency);


#define N_USB_SPEEDS 6
static char const *usb_speeds[N_USB_SPEEDS] = {
  "unknown",
  "Low (1.5 Mb/s)",
  "Full (12 Mb/s)",
  "High (480 Mb/s)",
  "Super (5 Gb/s)",
  "Super+ (10Gb/s)"
};


int rx888_setup(struct frontend * const frontend,dictionary const * const dictionary,char const * const section){
  assert(dictionary != NULL);

  struct sdrstate * const sdr = calloc(1,sizeof(struct sdrstate));
  // Cross-link generic and hardware-specific control structures
  sdr->frontend = frontend;
  frontend->context = sdr;

  {
    char const *device = config_getstring(dictionary,section,"device",NULL);
    if(strcasecmp(device,"rx888") != 0)
      return -1; // Not for us
  }
  // Hardware-dependent setup
  {
    char const *p = config_getstring(dictionary,section,"serial",NULL); // is serial specified?
    if(p != NULL){
      sdr->serial = strtoll(p,NULL,16);
    }
  }

  // Firmware file
  char const *firmware = config_getstring(dictionary,section,"firmware","SDDC_FX3.img");
  // Queue depth, default 16; 32 sometimes overflows
  int const queuedepth = config_getint(dictionary,section,"queuedepth",16);
  if(queuedepth < 1 || queuedepth > 64) {
    fprintf(stdout,"Invalid queue depth %d\n",queuedepth);
    return -1;
  }
  // Packets per transfer request, default 32
  int const reqsize = config_getint(dictionary,section,"reqsize",32);
  if(reqsize < 1 || reqsize > 64) {
    fprintf(stdout,"Invalid request size %d\n",reqsize);
    return -1;
  }
  {
    int ret;
    if((ret = rx888_usb_init(sdr,firmware,queuedepth,reqsize)) != 0){
      fprintf(stdout,"rx888_usb_init() failed\n");
      return -1;
    }
  }
  // Enable/disable dithering
  sdr->dither = config_getboolean(dictionary,section,"dither",false);
  // Enable/output output randomization
  sdr->randomizer = config_getboolean(dictionary,section,"rand",false);
  rx888_set_dither_and_randomizer(sdr,sdr->dither,sdr->randomizer);

  // Attenuation, default 0
  float att = fabsf(config_getfloat(dictionary,section,"att",0));
  if(att > 31.5)
    att = 31.5;
  rx888_set_att(sdr,att);
  
  // Gain Mode low/high, default high
  char const *gainmode = config_getstring(dictionary,section,"gainmode","high");
  if(strcmp(gainmode, "high") == 0)
    sdr->highgain = true;
  else if(strcmp(gainmode, "low") == 0)
    sdr->highgain = false;
  else {
    fprintf(stdout,"Invalid gain mode %s, default high\n",gainmode);
    sdr->highgain = true;
  }
  // Gain value, default +1.5 dB
  float gain = config_getfloat(dictionary,section,"gain",1.5);
  rx888_set_gain(sdr,gain);
  
  // Sample Rate, default 64.8
  unsigned int samprate = 64800000;
  {
    char const *p = config_getstring(dictionary,section,"samprate",NULL);
    if(p != NULL)
      samprate = parse_frequency(p,false);
  }

  if(samprate < 1000000){
    int const minsamprate = 1000000; // 1 MHz?
    fprintf(stdout,"Invalid sample rate %'d, forcing %'d\n",samprate,minsamprate);
    samprate = minsamprate;
  }
  double actual = rx888_set_samprate(sdr,samprate);
  frontend->samprate = samprate;
  frontend->min_IF = 0;
  frontend->max_IF = 0.47 * frontend->samprate; // Just an estimate - get the real number somewhere
  frontend->isreal = true; // Make sure the right kind of filter gets created!
  frontend->bitspersample = 16;
  frontend->calibrate = config_getdouble(dictionary,section,"calibrate",0);
  if(fabsl(frontend->calibrate) >= 1e-4){
    fprintf(stdout,"Unreasonable frequency calibration %.3g, setting to 0\n",frontend->calibrate);
    frontend->calibrate = 0;
  }
  frontend->lock = true; // Doesn't tune in direct conversion mode
  {
    char const *p = config_getstring(dictionary,section,"description","rx888");
    FREE(frontend->description);
    frontend->description = strdup(p);
    fprintf(stdout,"%s: ",frontend->description);
  }
  double ferror = actual - frontend->samprate;
  fprintf(stdout,"rx888 samprate requested %'d Hz, actual %'.3lf Hz (err %.3lf Hz; %.3lf ppm), calibrate %.3g, gain mode %s, requested gain %.1f dB, actual gain %.1f dB, atten %.1f dB, dither %d, randomizer %d, USB queue depth %d, USB request size %'d * pktsize %'d = %'d bytes (%g sec)\n",
	  frontend->samprate,actual,ferror, 1e6 * ferror / frontend->samprate,
	  frontend->calibrate,sdr->highgain ? "high" : "low",
	  gain,frontend->rf_gain,frontend->rf_atten,sdr->dither,sdr->randomizer,sdr->queuedepth,sdr->reqsize,sdr->pktsize,sdr->reqsize * sdr->pktsize,
	  (float)(sdr->reqsize * sdr->pktsize) / (sizeof(int16_t) * frontend->samprate));

  return 0;
}

// Come back here after common stuff has been set up (filters, etc)
int rx888_startup(struct frontend * const frontend){
  struct sdrstate * const sdr = (struct sdrstate *)frontend->context;

  // Start processing A/D data
  pthread_create(&sdr->proc_thread,NULL,proc_rx888,sdr);
  fprintf(stdout,"rx888 running\n");
  return 0;
}

static void *proc_rx888(void *arg){
  struct sdrstate * const sdr = (struct sdrstate *)arg;
  assert(sdr != NULL);
  pthread_setname("proc_rx888");

  realtime();
  {
    int ret __attribute__ ((unused));
    ret = rx888_start_rx(sdr,rx_callback);
    assert(ret == 0);
    sdr->last_callback_time = gps_time_ns();
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

// Callback called with incoming receiver data from A/D
static void rx_callback(struct libusb_transfer * const transfer){
  assert(transfer != NULL);
  struct sdrstate * const sdr = (struct sdrstate *)transfer->user_data;
  struct frontend *frontend = sdr->frontend;

  sdr->xfers_in_progress--;
  sdr->last_callback_time = gps_time_ns();

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
  float * const wptr = frontend->in->input_write_pointer.r;
  int const sampcount = size / sizeof(int16_t);
  int output_count = 0;
  if(frontend->calibrate == 0){
    if(sdr->randomizer){
      for(int i=0; i < sampcount; i++){
	int32_t s = samples[i];
	s ^= (s << 31) >> 30; // Put LSB in sign bit, then shift back by one less bit to make ..ffffe or 0
	wptr[i] = s;
	in_energy += wptr[i] * wptr[i];
      }
    } else {
#if 1
      for(int i=0; i < sampcount; i++){
	wptr[i] = samples[i];
	in_energy += wptr[i] * wptr[i];
      }
#else
      // Apparently no faster, at least on i7
      typedef float v8sf __attribute__ ((vector_size (32)));
      typedef short v8si __attribute__ ((vector_size (16)));

      v8si *in = (v8si *)samples;
      v8sf *out = (v8sf *)wptr; // 8 32-bit floats

      v8sf prod = {0,0,0,0,0,0,0,0};
      int cnt = sampcount / 8;
      for(int i=0; i < cnt; i++){
	out[i] = __builtin_convertvector(in[i],v8sf);
	prod += out[i] * out[i];
      }
      for(int i = 0; i < 8; i++)
	in_energy += prod[i];
#endif
    }
    output_count = sampcount;
  } else {
    /* Correct sample rate by linear interpolation. Creates "chugging"
     artifacts in noise at low levels and consumes a lot of CPU. So I
     don't recommend using unless you need more precision and you
     can't use an external GPSDO
    */
    for(int i=0; i < sampcount; i++){
      int32_t s = samples[i];
      if(sdr->randomizer)
	s ^= (s << 31) >> 30; // Put LSB in sign bit, then shift back by one less bit to make ..ffffe or 0

      float const f = s;
      in_energy += f * f;
      if(sdr->sample_phase < 1.0){
	// Usual case
	assert(sdr->sample_phase >= 0 && sdr->sample_phase < 1);
	wptr[output_count++] = sdr->last * (1 - sdr->sample_phase) + f * sdr->sample_phase;
      } else {
	// Sample clock is fast, don't generate an output sample this time
	sdr->sample_phase -= 1.0;
      }
      sdr->sample_phase += frontend->calibrate;
      if(sdr->sample_phase < 0){
	// Clock is slow; generate another from the same pair
	sdr->sample_phase += 1.0;
	assert(sdr->sample_phase >= 0 && sdr->sample_phase < 1);
	wptr[output_count++] = sdr->last * (1 - sdr->sample_phase) + f * sdr->sample_phase;	  
      }
      sdr->last = f;
    }
  }

  write_rfilter(frontend->in,NULL,output_count); // Update write pointer, invoke FFT if block is complete

  // temp fix for a previous bug: was resetting integrator each time it was read, so more than one reader
  // would cause premature resets. Go back to a single exponential smoother
  {
    float power  = (float)in_energy / output_count;
    frontend->if_power += power_smooth * (power - frontend->if_power);
    if(power > frontend->if_power_max){
      if(Verbose){
	float scaled_old_power = frontend->if_power_max * scale_ADpower2FS(frontend);
	float scaled_new_power = power * scale_ADpower2FS(frontend);

	// Don't print a message unless the increase is > 0.1 dB, the precision of the printf
	float new_dBFS = power2dB(scaled_new_power);
	float old_dBFS = power2dB(scaled_old_power);
	if(new_dBFS >= old_dBFS + 0.1)
	  fprintf(stdout,"New input power high watermark: %.1f dBFS\n",new_dBFS);
      }
      frontend->if_power_max = power;
    }
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
    int rc __attribute__ ((unused));
    rc = libusb_get_device_descriptor(device,&desc);
    assert(rc == 0);
    if(desc.idVendor != vendor_id || desc.idProduct != unloaded_product_id)
      continue;

    fprintf(stdout,"found rx888 vendor %04x, device %04x",desc.idVendor,desc.idProduct);
    libusb_device_handle *handle = NULL;
    libusb_open(device,&handle);
    if(handle == NULL){
      fprintf(stdout,", libusb_open() failed\n");
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
    int rc __attribute__ ((unused));
    rc = libusb_get_device_descriptor(device,&desc);
    assert(rc == 0);
    if(desc.idVendor != vendor_id || desc.idProduct != loaded_product_id)
      continue;

    fprintf(stdout,"found rx888 vendor %04x, device %04x",desc.idVendor,desc.idProduct);
    libusb_device_handle *handle = NULL;
    libusb_open(device,&handle);
    if(handle == NULL){
      fprintf(stdout," libusb_open() failed\n");
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
  uint32_t gpio = 0;
  if(dither)
    gpio |= DITH;

  if(randomizer)
    gpio |= RANDO;

  usleep(5000);
  command_send(sdr->dev_handle,GPIOFX3,gpio);
  sdr->dither = dither;
  sdr->randomizer = randomizer;
}

static void rx888_set_att(struct sdrstate *sdr,float att){
  assert(sdr != NULL);
  struct frontend *frontend = sdr->frontend;
  assert(frontend != NULL);
  usleep(5000);

  frontend->rf_atten = att;
  int const arg = (int)(att * 2);
  argument_send(sdr->dev_handle,DAT31_ATT,arg);
}

static void rx888_set_gain(struct sdrstate *sdr,float gain){
  assert(sdr != NULL);
  struct frontend *frontend = sdr->frontend;
  assert(frontend != NULL);
  usleep(5000);

  int const arg = gain2val(sdr->highgain,gain);
  argument_send(sdr->dev_handle,AD8340_VGA,arg);
  frontend->rf_gain = val2gain(arg); // Store actual nearest value
}

static double rx888_set_samprate(struct sdrstate *sdr,unsigned int samprate){
  assert(sdr != NULL);
  struct frontend *frontend = sdr->frontend;
  usleep(5000);
  command_send(sdr->dev_handle,STARTADC,samprate);
  frontend->samprate = samprate;
  return actual_freq((double)samprate);
}

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

static int gain2val(bool highgain, double gain){
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
  if(frontend->lock)
    return frontend->frequency;
  return 0; // No tuning implemented (direct sampling only)
}


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

