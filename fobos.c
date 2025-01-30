// Written by KC2DAC Dec 2024, adapted from existing KA9Q SDR handler programs

#define _GNU_SOURCE 1
#include <assert.h>
#include <errno.h>
#include <fobos.h>
#include <iniparser/iniparser.h>
#include <pthread.h>
#include <unistd.h>
#if defined(linux)
#include <bsd/string.h>
#endif

#include "conf.h"
#include "config.h"
#include "misc.h"
#include "radio.h"
#include <strings.h>
#include <sysexits.h>

static char const *fobos_keys[] = {
    "library", "device", "description", "serial",   "samprate",   "frequency",
    "direct_sampling", "lna_gain",    "vga_gain", "clk_source", NULL};

// Global variables set by config file options
extern int Verbose;

struct fobos_dev_t *dev = NULL;

struct sdrstate {
  struct frontend *frontend;
  struct fobos_dev_t *dev;
  int buff_count;
  int max_buff_count;
  int device;
  unsigned int next_sample_num;
  float scale; // Scale samples for #bits and front end gain
  pthread_t monitor_thread;
};

static float Power_smooth = 0.05; // Calculate this properly someday
static void rx_callback(float *buf, uint32_t buf_length, void *ctx);
static void *fobos_monitor(void *p);

int find_serial_position(const char *serials, const char *serialnumcfg) {
  if (serialnumcfg == NULL) {
    return -1; // No serial number to search for
  }

  char serials_copy[256];
  strncpy(serials_copy, serials, sizeof(serials_copy) - 1);
  serials_copy[sizeof(serials_copy) - 1] = '\0'; // Ensure null termination

  char *token = strtok(serials_copy, " "); // Tokenize the space-delimited list
  int position = 0;

  while (token != NULL) {
    if (strcmp(token, serialnumcfg) == 0) {
      return position; // Found the serial number
    }
    token = strtok(NULL, " "); // Get the next token
    position++;
  }

  return -1; // Serial number not found
}

///////////////////////////////////////////////////////////
int fobos_setup(struct frontend *const frontend, dictionary *const dictionary,
                char const *const section) {
  assert(dictionary != NULL);
  config_validate_section(stdout, dictionary, section, fobos_keys, NULL);
  struct sdrstate *const sdr = calloc(1, sizeof(struct sdrstate));
  // Cross-link generic and hardware-specific control structures
  assert(sdr != NULL);
  sdr->frontend = frontend;
  frontend->context = sdr;
  frontend->isreal = false; // Make sure the right kind of filter gets created!
  frontend->bitspersample = 14; // For gain scaling
  frontend->rf_agc = false; // On by default unless gain or atten is specified

  sdr->buff_count = 0;
  sdr->max_buff_count = 2048;

  // Read Config Files
  {
    char const *device = config_getstring(dictionary, section, "device", section);
    if (strcasecmp(device, "fobos") != 0)
      return -1; // Leave if not Fobos in the config
  }
  sdr->device = -1;
  {
    char const *cp = config_getstring(dictionary, section, "description", "fobos");
    if(cp != NULL)
      strlcpy(frontend->description,cp,sizeof(frontend->description));
  }
  double requestsample =
      config_getdouble(dictionary, section, "samprate", 8000000.0);
  const char *serialnumcfg =
      config_getstring(dictionary, section, "serial", NULL);
  const char *frequencycfg =
      config_getstring(dictionary, section, "frequency", "100m0");
  int dirsamplecfg = config_getint(dictionary, section, "direct_sampling", 0);
  int lna_gaincfg = config_getint(dictionary, section, "lna_gain", 0);
  int vga_gaincfg = config_getint(dictionary, section, "vga_gain", 0);
  int clk_sourcecfg = config_getint(dictionary, section, "clk_source", 0);

  // Get Fobos Library and Driver Version
  int result = 0;
  char lib_version[32];
  char drv_version[32];

  result = fobos_rx_get_api_info(lib_version, drv_version);
  if (result != 0) {
    fprintf(
        stdout,
        "Unable to find Fobos Drivers. Please check libfobos is installed.\n");
    return -1;
  }

  // Look for connected Fobos Devices and fetch serial numbers
  char serialnumlist[256] = {0};
  int fobos_count = fobos_rx_list_devices(serialnumlist);
  if (fobos_count < 1) {
    fprintf(stderr, "No Fobos SDR devices found\n");
    return -1;
  }
  fprintf(stderr, "Found %d Fobos SDR device(s)\n", fobos_count);

  // If the config specifies a serial number look for it in the list --
  // otherwise assume device 0
  if (serialnumcfg == NULL) {
    // Use the first device by default
    sdr->device = 0;
  } else {
    int position = find_serial_position(serialnumlist, serialnumcfg);
    if (position >= 0) {
      sdr->device = position;
    } else {
      fprintf(stderr,
              "Serial number '%s' not found in the list of connected Fobos "
              "devices\n",
              serialnumcfg);
      return -1;
    }
  }

  // Open the SDR
  result = fobos_rx_open(&dev, sdr->device);
  if (result != FOBOS_ERR_OK) {
    fprintf(stderr, "Could not open device: %d\n", sdr->device);
    return -1;
  } else {
    char hw_revision[32];
    char fw_version[32];
    char manufacturer[32];
    char product[32];
    char serial[32];

    result = fobos_rx_get_board_info(dev, hw_revision, fw_version, manufacturer,
                                     product, serial);
    if (result == FOBOS_ERR_OK) {
      fprintf(stdout, "--------------------------------------------\n");
      fprintf(stdout, "Library Version:    %s\n", lib_version);
      fprintf(stdout, "Driver Version:     %s\n", drv_version);
      fprintf(stdout, "Hardware Revision:  %s\n", hw_revision);
      fprintf(stdout, "Firmware Version:   %s\n", fw_version);
      fprintf(stdout, "Manufacturer:       %s\n", manufacturer);
      fprintf(stdout, "Product:            %s\n", product);
      fprintf(stdout, "--------------------------------------------\n");
    } else {
      fprintf(stderr, "Error fetching device info from fobos device: %d\n",
              sdr->device);
      return -1;
    }

    // Get Sample Rates offered by the Fobos
    double *sampvalues = NULL;    // Pointer to hold sample rates
    unsigned int samplecount = 0; // Initialize sample count

    // First call to get the count of sample rates
    result = fobos_rx_get_samplerates(dev, NULL, &samplecount);
    if (result != FOBOS_ERR_OK) {
      fprintf(stderr, "Error fetching sample rate count (error code: %d)\n",
              result);
      fobos_rx_close(dev); // Close the device before returning
      return -1;
    }

    // Allocate memory for the sample rates array
    sampvalues = (double *)malloc(samplecount * sizeof(double));
    if (sampvalues == NULL) {
      fprintf(stderr, "Error: Memory allocation failed for sample rates.\n");
      fobos_rx_close(dev); // Close the device before returning
      return -1;
    }

    // Second call to fetch the actual sample rates
    result = fobos_rx_get_samplerates(dev, sampvalues, &samplecount);
    if (result == FOBOS_ERR_OK) {
      fprintf(stdout, "--------------------------------------------\n");
      fprintf(stdout, "Supported Sample Rates for SDR #%d:\n", sdr->device);
      for (unsigned int i = 0; i < samplecount; i++) {
        fprintf(stdout, "  %.0f \n", sampvalues[i]);
      }
      fprintf(stdout, "--------------------------------------------\n");
    } else {
      fprintf(stderr, "Error fetching sample rates (error code: %d)\n", result);
      fobos_rx_close(dev);
      return -1;
    }
    // End of fetching sample rates here

    // Set the Actual Sample Rate
    double samprate_actual = 0.0;
    result = fobos_rx_set_samplerate(dev, requestsample, &samprate_actual);
    if (result == FOBOS_ERR_OK) {
      frontend->samprate = samprate_actual;
      frontend->min_IF = -0.47 * frontend->samprate;
      frontend->max_IF = 0.47 * frontend->samprate;
      fprintf(stdout, "Sample rate set to %f:\n", samprate_actual);
    } else {
      fprintf(stderr, "Error setting sample rate %f\n", requestsample);
      fobos_rx_close(dev);
      return -1;
    }

    // Set Frequency
    double init_frequency = parse_frequency(frequencycfg, false);
    double frequency_actual = 0.0;
    int result = fobos_rx_set_frequency(dev, init_frequency, &frequency_actual);
    if (result != 0) {
      fprintf(stderr, "fobos_rx_set_frequency failed with error code: %d\n",
              result);
      fobos_rx_close(dev);
      return -1;
    }
    frontend->frequency = frequency_actual;

    // Set Direct Sampling vs. Non
    result = fobos_rx_set_direct_sampling(dev, dirsamplecfg);
    if (result != FOBOS_ERR_OK) {
      fprintf(stderr,
              "fobos_rx_set_direct_sampling failed with error code: %d\n",
              result);
      return -1;
    }

    // Set LNA Gain
    result = fobos_rx_set_lna_gain(dev, lna_gaincfg);
    if (result != FOBOS_ERR_OK) {
      fprintf(stderr, "fobos_rx_set_lna_gain failed with error code: %d\n",
              result);
      return -1;
    }

    // Get VGA Gain
    result = fobos_rx_set_vga_gain(dev, vga_gaincfg);
    if (result != FOBOS_ERR_OK) {
      fprintf(stderr, "fobos_rx_set_vga_gain failed with error code: %d\n",
              result);
      return -1;
    }

    // Set Clock Source
    result = fobos_rx_set_clk_source(dev, clk_sourcecfg);
    if (result != FOBOS_ERR_OK) {
      fprintf(stderr, "fobos_rx_set_clk_source failed with error code: %d\n",
              result);
      return -1;
    }

    // SDR is open here
  }
  return 0;
} // End of Setup

static void *fobos_monitor(void *p) {
  struct sdrstate *const sdr = (struct sdrstate *)p;
  assert(sdr != NULL);
  pthread_setname("fobos-mon");

  fprintf(stdout, "Starting asynchronous read\n");
  realtime();
  int result = fobos_rx_read_async(dev, rx_callback, sdr, 16, 65536);
  if (result != 0) {
    fprintf(stderr, "fobos_rx_read_async failed with error code: %d\n", result);
    exit(EXIT_FAILURE); // Exit the thread due to an error
  }

  return NULL; // Return NULL when the thread exits cleanly
}

static bool Name_set = false;
static void rx_callback(float *buf, uint32_t len, void *ctx) {
  struct sdrstate *sdr = (struct sdrstate *)ctx;
  assert(sdr != NULL);
  struct frontend *const frontend = sdr->frontend;
  assert(frontend != NULL);

  if (!Name_set) {
    pthread_setname("fobos-cb");
    Name_set = true;
  }

  // Ensure len is a valid even number (interleaved I/Q samples)
  assert(len % 2 == 0);
  int const sampcount = len;

  float complex *const wptr = frontend->in.input_write_pointer.c;
  assert(wptr != NULL);

  float in_energy = 0;
  for (int i = 0; i < sampcount; i++) {
    float complex const samp = CMPLXF(buf[2*i],buf[2*i+1]);
    in_energy += cnrmf(samp);       // Calculate energy of the sample
    wptr[i] = samp;                 // Store sample in write pointer buffer
  }

  frontend->samples += sampcount;
  frontend->timestamp = gps_time_ns();
  write_cfilter(&frontend->in, NULL,
                sampcount); // Update write pointer, invoke FFT
  // fprintf(stderr, "write_cfilter invoked with sampcount: %d\n", sampcount);

  if (isfinite(in_energy)) {
    frontend->if_power_instant = in_energy / sampcount;
    frontend->if_power +=
        Power_smooth * (frontend->if_power_instant - frontend->if_power);
  }
}

int fobos_startup(struct frontend *const frontend) {
  struct sdrstate *const sdr = (struct sdrstate *)frontend->context;
  ASSERT_ZEROED(&sdr->monitor_thread,sizeof sdr->monitor_thread);
  pthread_create(&sdr->monitor_thread, NULL, fobos_monitor, sdr);
  fprintf(stdout, "fobos read thread running\n");
  return 0;
}

double fobos_tune(struct frontend *const frontend, double const freq) {
  fprintf(stdout, "Trying to tune to: %f\n", freq);
  double frequency_actual = 0.0;
  int result = fobos_rx_set_frequency(dev, freq, &frequency_actual);
  if (result != 0) {
    fprintf(stderr, "fobos_rx_set_frequency failed with error code: %d\n",
            result);
    fobos_rx_close(dev);
    return -1;
  }
  frontend->frequency = frequency_actual;
  return frontend->frequency;
}
