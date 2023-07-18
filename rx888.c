// Linked-in module for rx888 Mk ii
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

#include "misc.h"
#include "status.h"
#include "config.h"
#include "radio.h"
#include "rx888.h"
#include "ezusb.h"

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
  int interface_number;
  struct libusb_config_descriptor *config;
  unsigned int pktsize;
  unsigned int transfer_size;  // Size of data transfers performed so far (unused)
  unsigned int transfer_index; // Write index into the transfer_size array (unused)
  struct libusb_transfer **transfers; // List of transfer structures.
  unsigned char **databuffers;        // List of data buffers.

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
static void rx888_set_samprate(struct sdrstate *sdr,unsigned int samprate);
static int rx888_start_rx(struct sdrstate *sdr,libusb_transfer_cb_fn callback);
static void rx888_stop_rx(struct sdrstate *sdr);
static void rx888_close(struct sdrstate *sdr);
static void free_transfer_buffers(unsigned char **databuffers,struct libusb_transfer **transfers,unsigned int queuedepth);
static double val2gain(int g);
static int gain2val(bool highgain, double gain);
static void *proc_rx888(void *arg);

int rx888_setup(struct frontend *frontend,dictionary *Dictionary,char const *section){
  assert(Dictionary != NULL);

  struct sdrstate * const sdr = calloc(1,sizeof(struct sdrstate));
  // Cross-link generic and hardware-specific control structures
  sdr->frontend = frontend;
  frontend->sdr.context = sdr;

  {
    char const *device = config_getstring(Dictionary,section,"device",NULL);
    if(strcasecmp(device,"rx888") != 0)
      return -1; // Not for us
  }
  // Hardware-dependent setup
  sdr->interface_number = config_getint(Dictionary,section,"number",0);

  // Firmware file
  char const *firmware = config_getstring(Dictionary,section,"firmware","SDDC_FX3.img");
  // Queue depth, default 16; 32 sometimes overflows
  int const queuedepth = config_getint(Dictionary,section,"queuedepth",16);
  if(queuedepth < 1 || queuedepth > 64) {
    fprintf(stdout,"Invalid queue depth %d\n",queuedepth);
    return -1;
  }
  // Packets per transfer request, default 32
  int const reqsize = config_getint(Dictionary,section,"reqsize",32);
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
  sdr->dither = config_getboolean(Dictionary,section,"dither",false);
  // Enable/output output randomization
  sdr->randomizer = config_getboolean(Dictionary,section,"rand",false);
  rx888_set_dither_and_randomizer(sdr,sdr->dither,sdr->randomizer);

  // Attenuation, default 0
  {
    float att = fabsf(config_getfloat(Dictionary,section,"att",0));
    if(att > 31.5)
      att = 31.5;
    rx888_set_att(sdr,att);

    // Gain Mode low/high, default high
    char const *gainmode = config_getstring(Dictionary,section,"gainmode","high");
    if(strcmp(gainmode, "high") == 0)
      sdr->highgain = true;
    else if(strcmp(gainmode, "low") == 0)
      sdr->highgain = false;
    else {
      fprintf(stdout,"Invalid gain mode %s, default high\n",gainmode);
      sdr->highgain = true;
    }
    // Gain value, default +1.5 dB
    float gain = config_getfloat(Dictionary,section,"gain",1.5);
    if(gain > 34.0)
      gain = 34.0;
    
    rx888_set_gain(sdr,gain);

    // Sample Rate, default 129.6 MHz
    unsigned int samprate = config_getint(Dictionary,section,"samprate",129600000);
    if(samprate < 1000000){
      int const minsamprate = 1000000; // 1 MHz?
      fprintf(stdout,"Invalid sample rate %'d, forcing %'d\n",samprate,minsamprate);
      samprate = minsamprate;
    }
    rx888_set_samprate(sdr,samprate);
    frontend->sdr.samprate = samprate;
    frontend->sdr.min_IF = 0;
    frontend->sdr.max_IF = 0.47 * frontend->sdr.samprate; // Just an estimate - get the real number somewhere
    frontend->sdr.isreal = true; // Make sure the right kind of filter gets created!
    frontend->sdr.calibrate = config_getdouble(Dictionary,section,"calibrate",0);
  }
  frontend->sdr.gain = dB2voltage(frontend->sdr.rf_gain - frontend->sdr.rf_atten);
  {
    char const *p = config_getstring(Dictionary,section,"description","rx888");
    if(p != NULL){
      strlcpy(frontend->sdr.description,p,sizeof(frontend->sdr.description));
      fprintf(stdout,"%s: ",frontend->sdr.description);
    }
  }
  fprintf(stdout,"Samprate %'d Hz, Gain %.1f dB, Atten %.1f dB, Dither %d, Randomizer %d, USB Queue depth %d, USB Request size %'d * pktsize %'d = %'d bytes (%g sec)\n",
	  frontend->sdr.samprate,frontend->sdr.rf_gain,frontend->sdr.rf_atten,sdr->dither,sdr->randomizer,sdr->queuedepth,sdr->reqsize,sdr->pktsize,sdr->reqsize * sdr->pktsize,
	  (float)(sdr->reqsize * sdr->pktsize) / (sizeof(int16_t) * frontend->sdr.samprate));

  return 0;
}

// Come back here after common stuff has been set up (filters, etc)
int rx888_startup(struct frontend *frontend){
  struct sdrstate *sdr = (struct sdrstate *)frontend->sdr.context;

  // Start processing A/D data
  pthread_create(&sdr->proc_thread,NULL,proc_rx888,sdr);
  fprintf(stdout,"rx888 running\n");
  return 0;
}

static void *proc_rx888(void *arg){
  struct sdrstate *sdr = (struct sdrstate *)arg;
  assert(sdr != NULL);
  pthread_setname("proc_rx888");

  realtime();
  {
    int ret __attribute__ ((unused));
    ret = rx888_start_rx(sdr,rx_callback);
    assert(ret == 0);
  }
  do {
    libusb_handle_events(NULL);
  } while (!Stop_transfers);

  fprintf(stderr, "RX888 streaming complete. Stopping transfers\n");
  rx888_stop_rx(sdr);
  rx888_close(sdr);
  fprintf(stdout,"rx888 is done streaming, proc_rx888 thread exiting\n");
  return NULL;
}

// Callback called with incoming receiver data from A/D
static void rx_callback(struct libusb_transfer *transfer){
  assert(transfer != NULL);
  struct sdrstate * const sdr = (struct sdrstate *)transfer->user_data;
  struct frontend *frontend = sdr->frontend;

  sdr->xfers_in_progress--;

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
  uint64_t in_energy = 0; // A/D energy accumulator for integer formats only	
  int16_t const * const samples = (int16_t *)transfer->buffer;
  float * const wptr = frontend->in->input_write_pointer.r;
  int const sampcount = size / sizeof(int16_t);
  if(sdr->randomizer){
    for(int i=0; i < sampcount; i++){
      int s = samples[i] ^ (-2 * (samples[i] & 1)); // if LSB is set, flip all other bits
      in_energy += s * s;
      wptr[i] = s * SCALE16;
    }
  } else {
    for(int i=0; i < sampcount; i++){
      int s = samples[i];
      in_energy += s * s;
      wptr[i] = s * SCALE16;
    }
  }
  write_rfilter(frontend->in,NULL,sampcount); // Update write pointer, invoke FFT
  frontend->sdr.output_level = 2 * in_energy * SCALE16 * SCALE16 / sampcount;
  frontend->input.samples += sampcount;
  if(!Stop_transfers) {
    if(libusb_submit_transfer(transfer) == 0)
      sdr->xfers_in_progress++;
  }
}

static int rx888_usb_init(struct sdrstate *sdr,const char *firmware,unsigned int queuedepth,unsigned int reqsize){
  {
    int ret = libusb_init(NULL);
    if(ret != 0){
      fprintf(stdout,"Error initializing libusb: %s\n",
	      libusb_error_name(ret));
      return -1;
    }
  }
  {
    // Temporary ID when device doesn't already have firmware
    uint16_t const vendor_id = 0x04b4;
    uint16_t const product_id = 0x00f3;
    // does it already have firmware?
    sdr->dev_handle =
      libusb_open_device_with_vid_pid(NULL,vendor_id,product_id);
    if(sdr->dev_handle){
      // No, doesn't have firmware
      if(firmware == NULL){
	fprintf(stdout,"Firmware not loaded and not available\n");
	return -1;
      }
      char full_firmware_file[PATH_MAX];
      full_firmware_file[0] = '\0';
      dist_path(full_firmware_file,sizeof(full_firmware_file),firmware);
      fprintf(stdout,"Loading rx888 firmware file %s\n",full_firmware_file);
      struct libusb_device *dev = libusb_get_device(sdr->dev_handle);
      
      if(ezusb_load_ram(sdr->dev_handle,full_firmware_file,FX_TYPE_FX3,IMG_TYPE_IMG,1) == 0){
	fprintf(stdout,"Firmware updated\n");
      } else {
	fprintf(stdout,"Firmware upload of %s failed for device %d.%d (logical).\n",
		full_firmware_file,
		libusb_get_bus_number(dev),libusb_get_device_address(dev));
	return -1;
      }
      sleep(1); // how long should this be?
    }
  }
  // Device changes product_id when it has firmware
  uint16_t const vendor_id = 0x04b4;
  uint16_t const product_id = 0x00f1;
  sdr->dev_handle = libusb_open_device_with_vid_pid(NULL,vendor_id,product_id);
  if(!sdr->dev_handle){
    fprintf(stdout,"Error or device could not be found, try loading firmware\n");
    goto close;
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
	goto close;
      }
    }
  }
  struct libusb_device *dev = libusb_get_device(sdr->dev_handle);
  assert(dev != NULL);
  enum libusb_speed usb_speed = libusb_get_device_speed(dev);
  // fv
  fprintf(stdout,"USB speed: %d\n",usb_speed);
  if(usb_speed < LIBUSB_SPEED_SUPER){
    fprintf(stdout,"USB device speed (%d) is not at least SuperSpeed\n",usb_speed);
    return -1;
  }
  libusb_get_config_descriptor(dev, 0, &sdr->config);
  {
    int const ret = libusb_claim_interface(sdr->dev_handle, sdr->interface_number);
    if(ret != 0){
      fprintf(stderr, "Error claiming interface\n");
      goto end;
    }
  }
  fprintf(stdout,"Successfully claimed interface\n");
  {
    // All this just to get sdr->pktsize?
    struct libusb_interface_descriptor const *interfaceDesc = &(sdr->config->interface[0].altsetting[0]);
    assert(interfaceDesc != NULL);
    struct libusb_endpoint_descriptor const *endpointDesc = &interfaceDesc->endpoint[0];
    assert(endpointDesc != NULL);
    struct libusb_device_descriptor desc;
    memset(&desc,0,sizeof(desc));
    libusb_get_device_descriptor(dev,&desc);
    struct libusb_ss_endpoint_companion_descriptor *ep_comp = NULL;
    int const rc = libusb_get_ss_endpoint_companion_descriptor(NULL,endpointDesc,&ep_comp);
    if(rc != 0){
      fprintf(stdout,"libusb_get_ss_endpoint_companion_descriptor returned: %s (%d)\n",libusb_error_name(rc),rc);
      return -1;
    }
    assert(ep_comp != NULL);
    sdr->pktsize = endpointDesc->wMaxPacketSize * (ep_comp->bMaxBurst + 1);
    libusb_free_ss_endpoint_companion_descriptor(ep_comp);
  }
  bool allocfail = false;
  sdr->databuffers = (u_char **)calloc(queuedepth,sizeof(u_char *));
  sdr->transfers = (struct libusb_transfer **)calloc(queuedepth,sizeof(struct libusb_transfer *));

  if((sdr->databuffers != NULL) && (sdr->transfers != NULL)){
    for(unsigned int i = 0; i < queuedepth; i++){
      sdr->databuffers[i] = (u_char *)malloc(reqsize * sdr->pktsize);
      sdr->transfers[i] = libusb_alloc_transfer(0);
      if((sdr->databuffers[i] == NULL) || (sdr->transfers[i] == NULL)) {
        allocfail = true;
        break;
      }
    }
  } else {
    allocfail = true;
  }

  if(allocfail) {
    fprintf(stdout,"Failed to allocate buffers and transfers\n");
    // Is it OK if one or both of these is already null?
    free_transfer_buffers(sdr->databuffers,sdr->transfers,sdr->queuedepth);
    sdr->databuffers = NULL;
    sdr->transfers = NULL;
  }
  sdr->queuedepth = queuedepth;
  sdr->reqsize = reqsize;
  return 0;

end:
  if(sdr->dev_handle)
    libusb_release_interface(sdr->dev_handle,sdr->interface_number);

  if(sdr->config)
    libusb_free_config_descriptor(sdr->config);
  sdr->config = NULL;

close:
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

  frontend->sdr.rf_atten = att;
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
  frontend->sdr.rf_gain = val2gain(arg); // Store actual nearest value
}

static void rx888_set_samprate(struct sdrstate *sdr,unsigned int samprate){
  assert(sdr != NULL);
  struct frontend *frontend = sdr->frontend;
  usleep(5000);
  command_send(sdr->dev_handle,STARTADC,samprate);
  frontend->sdr.samprate = samprate;
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
    libusb_handle_events(NULL);
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
    libusb_release_interface(sdr->dev_handle,sdr->interface_number);

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
  int g = round(dB2voltage(gain) / (Vernier * (1 + (Pregain - 1)* highgain)));

  if(g > 127)
    g = 127;
  if(g < 0)
    g = 0;
  g |= (highgain << 7);
  return g;
}
