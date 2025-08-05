/* Written by KC2DAC Dec 2024, adapted from existing KA9Q SDR handler programs
   Modified by Phil Karn KA9Q Feb 2024: approximate gain scaling, got direct sample mode working

In direct sample mode the two A/Ds are fed directly by the two HF inputs through fixed 20 dB gain
amplifiers, so there is no manual or automatic gain control.

The FFT is currently complex in both tuner and direct sample mode. In tuner mode it accepts a complex sample
stream centered on the tuner LO, and in direct sample mode it treats the two inputs as I and Q.
So you can connect a HF antenna to just one input and it will work but half the FFT is wasted since
the input is then purely real.

*/
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

#define INPUT_PRIORITY 95
static double Power_alpha = 0.05; // Calculate this properly someday

static char const *Fobos_keys[] = {
  "clk_source",
  "description",
  "device",
  "direct_sampling",
  "frequency",
  "hf_input",
  "library",
  "lna_gain",
  "samprate",
  "serial",
  "vga_gain",
  NULL
};

// Global variables set by config file options
extern int Verbose;
extern char const *Description;

struct fobos_dev_t *dev = NULL;

struct sdrstate {
  struct frontend *frontend;
  struct fobos_dev_t *dev;
  bool direct_sampling;
  int hf_input; // 0 = both HF1 and HF2 used as I/Q; 1 = only HF1; 2 = only HF2
  int lna_gain;
  int vga_gain;
  int buff_count;
  int max_buff_count;
  int device;
  unsigned int next_sample_num;
  float scale; // Scale samples for #bits and front end gain
  pthread_t monitor_thread;
};

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
  config_validate_section(stderr, dictionary, section, Fobos_keys, NULL);
  struct sdrstate *const sdr = calloc(1, sizeof(struct sdrstate));
  // Cross-link generic and hardware-specific control structures
  assert(sdr != NULL);
  sdr->frontend = frontend;
  frontend->context = sdr;
  frontend->isreal = false; // Make sure the right kind of filter gets created!
  // The Fobos apparently provides scaled float samples
  frontend->bitspersample = 1; // only used for gain scaling
  frontend->rf_agc = false; // On by default unless gain or atten is specified
  sdr->scale = scale_AD(frontend);

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
    char const *cp = config_getstring(dictionary, section, "description", Description ? Description : "fobos");
    if(cp != NULL){
      strlcpy(frontend->description,cp,sizeof(frontend->description));
      Description = cp;
    }
  }
  double requestsample =
      config_getdouble(dictionary, section, "samprate", 8000000.0);
  const char *serialnumcfg =
      config_getstring(dictionary, section, "serial", NULL);
  int clk_sourcecfg = config_getint(dictionary, section, "clk_source", 0);

  // Get Fobos Library and Driver Version
  int result = 0;
  char lib_version[32];
  char drv_version[32];

  result = fobos_rx_get_api_info(lib_version, drv_version);
  if (result != 0) {
    fprintf(
        stderr,
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
      fprintf(stderr, "%s %s serial %s, hardware %s, lib %s, driver %s firmware %s\n",
	      manufacturer,product,serial,hw_revision, lib_version,drv_version,fw_version);
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
      fprintf(stderr, "Supported Sample Rates for SDR #%d: ", sdr->device);
      for (unsigned int i = 0; i < samplecount; i++) {
        fprintf(stderr, " %.0f", sampvalues[i]);
      }
      fprintf(stderr, "\n");
    } else {
      fprintf(stderr, "Error fetching sample rates (error code: %d)\n", result);
      fobos_rx_close(dev);
      return -1;
    }
    FREE(sampvalues);
    // End of fetching sample rates here

    // Set the Actual Sample Rate
    double samprate_actual = 0.0;
    result = fobos_rx_set_samplerate(dev, requestsample, &samprate_actual);
    if (result == FOBOS_ERR_OK) {
      frontend->samprate = samprate_actual;
    } else {
      fprintf(stderr, "Error setting sample rate %f\n", requestsample);
      fobos_rx_close(dev);
      return -1;
    }
    // Set Direct Sampling
    sdr->direct_sampling = config_getboolean(dictionary, section, "direct_sampling", 0);
    result = fobos_rx_set_direct_sampling(dev, sdr->direct_sampling);
    if (result != FOBOS_ERR_OK) {
      fprintf(stderr,
              "fobos_rx_set_direct_sampling failed with error code: %d\n",
              result);
      return -1;
    }
    if(sdr->direct_sampling){
      frontend->frequency = 0;
      frontend->rf_gain = 0;
      frontend->rf_atten = 0;
      frontend->rf_level_cal = 20; // Gain of LTC6401; needs to be calibrated
      sdr->hf_input = config_getint(dictionary, section, "hf_input",0);
      if(sdr->hf_input == 0) {
	frontend->isreal = false;
	frontend->min_IF = -0.47 * frontend->samprate;
      } else {
	frontend->isreal = true;
	frontend->min_IF = 0;
      }
      frontend->max_IF = 0.47 * frontend->samprate;
    } else {
      const char *frequencycfg =
	config_getstring(dictionary, section, "frequency", "100m0");
      // Set Frequency
      double init_frequency = parse_frequency(frequencycfg, false);
      double frequency_actual = 0.0;
      // Wow, a library API that returns the *actual* tuner frequency. Bravo!
      int result = fobos_rx_set_frequency(dev, init_frequency, &frequency_actual);
      if (result != 0) {
	fprintf(stderr, "fobos_rx_set_frequency failed with error code: %d\n",
		result);
	fobos_rx_close(dev);
	return -1;
      }
      frontend->frequency = frequency_actual;
      frontend->min_IF = -0.47 * frontend->samprate;
      frontend->max_IF = 0.47 * frontend->samprate;

      sdr->lna_gain = config_getint(dictionary, section, "lna_gain", 0);
      sdr->vga_gain = config_getint(dictionary, section, "vga_gain", 0);

      // These gains are not used in direct sample mode; the MAX2830 is bypassed
      // Set LNA Gain 0..3
      // MAX2830 datasheet, p21: 11 => max gain, 10 => -16 dB, 0X => -33 dB
      result = fobos_rx_set_lna_gain(dev, sdr->lna_gain);
      if (result != FOBOS_ERR_OK) {
	fprintf(stderr, "fobos_rx_set_lna_gain failed with error code: %d\n",
		result);
	return -1;
      }
      // Get VGA Gain 0..31
      // MAX2830 datasheet, p21: 2 dB steps, 0-62 dB
      result = fobos_rx_set_vga_gain(dev, sdr->vga_gain);
      if (result != FOBOS_ERR_OK) {
	fprintf(stderr, "fobos_rx_set_vga_gain failed with error code: %d\n",
		result);
	return -1;
      }
      frontend->rf_gain = 2 * sdr->vga_gain + (sdr->lna_gain == 2 ? 16.0 : sdr->lna_gain == 3 ? 33.0 : 0);
      frontend->rf_atten = 0;
      frontend->rf_level_cal = 41; // very rough approximation, needs to be measured
    }
    // Set Clock Source
    result = fobos_rx_set_clk_source(dev, clk_sourcecfg);
    if (result != FOBOS_ERR_OK) {
      fprintf(stderr, "fobos_rx_set_clk_source failed with error code: %d\n",
              result);
      return -1;
    }
    if(sdr->direct_sampling){
      fprintf(stderr,"samprate %'d Hz, direct sampling, hf_input %d (%s)\n",
	      frontend->samprate,
	      sdr->hf_input,
	      sdr->hf_input == 0 ? "both/IQ" : sdr->hf_input == 1 ? "HF1" : "HF2");
    } else {
      fprintf(stderr,"samprate %'d Hz, tuner %'.3lf Hz, lna_gain %d (%d dB) vga_gain %d (%d dB)\n",
	      frontend->samprate,
	      frontend->frequency,
	      sdr->lna_gain,
	      sdr->lna_gain == 2 ? 33 : sdr->lna_gain == 1 ? 16 : 0,
	      sdr->vga_gain,
	      sdr->vga_gain * 2);
    }
    // SDR is open here
  }
  return 0;
} // End of Setup

/* command to set analog gain. Turn off AGC if it was on
  MAX2830 datasheet: vga gain 0-63 dB in 2 dB steps (0x00 - 0x1F)
  lna gain: 00 -> -33 dB; 10 -> -16 dB; 11 -> 0 dB
  NF improves with higher lna gain setting, so use it as soon as possible
  Absolute receiver gain not yet measured, but the Fobos block diagram has
  1. QPL9547 LNA (19.5 dB, 0-2.4 GHz) or 9504 (21.6 dB, 2.4-6.0 GHz)
  2. RFFC5072 mixer (-2 dB)
  3. SKY6540S "VGA" (actually fixed at 14-15 dB depending on -11 or -20 version)
  4. MAX 2830 (what we're apparently programming) ~ 1-99 dB
  5. Linear LTC2143 A/D: 1V p-p or 2V p-p
*/
float fobos_gain(struct frontend * const frontend, float gain){
  if(frontend->rf_agc)
    fprintf(stderr,"manual gain setting, turning off AGC\n");

  // Just the MAX2830 gain here
  float vgain = gain;
  int lna = 0;
  if(vgain >= 33){
    lna = 3;
    vgain -= 33;
  } else if(vgain >= 16){
    lna = 2;
    vgain -= 16;
  }
  if(vgain > 63)
    vgain = 63;
  vgain /= 2; // into 2 dB steps

  frontend->rf_agc = false;
  frontend->rf_gain = gain;

  int result = fobos_rx_set_lna_gain(dev, lna);
  if (result != FOBOS_ERR_OK) {
    fprintf(stderr, "fobos_rx_set_lna_gain failed with error code: %d\n",
	    result);
  }

  // Set VGA Gain 0..31
  result = fobos_rx_set_vga_gain(dev, (int)vgain);
  if (result != FOBOS_ERR_OK) {
    fprintf(stderr, "fobos_rx_set_vga_gain failed with error code: %d\n",
	    result);
  }
  frontend->rf_gain = 2 * vgain + (lna == 2 ? 16.0 : lna == 3 ? 33.0 : 0);
  return frontend->rf_gain;
}

static void *fobos_monitor(void *p) {
  struct sdrstate *const sdr = (struct sdrstate *)p;
  assert(sdr != NULL);
  pthread_setname("fobos-mon");

  fprintf(stderr, "Starting asynchronous read\n");
  realtime(INPUT_PRIORITY);
  stick_core();
  int result = fobos_rx_read_async(dev, rx_callback, sdr, 16, 65536);
  if (result != 0) {
    fprintf(stderr, "fobos_rx_read_async failed with error code: %d\n", result);
    exit(EXIT_FAILURE); // Exit the thread due to an error
  }
  return NULL; // Return NULL when the thread exits cleanly
}

static bool Name_set = false;
static void rx_callback(float *buf, uint32_t len, void *ctx) {
  struct sdrstate * const sdr = (struct sdrstate *)ctx;
  assert(sdr != NULL);
  struct frontend * const frontend = sdr->frontend;
  assert(frontend != NULL);

  if (!Name_set) {
    pthread_setname("fobos-cb");
    Name_set = true;
  }

  float in_energy = 0;
  int const sampcount = len;
  assert(len % 2 == 0);    // Ensure len is a valid even number (interleaved I/Q samples)
  if(!sdr->direct_sampling || sdr->hf_input == 0){
    float complex *const wptr = frontend->in.input_write_pointer.c;
    assert(wptr != NULL);

    for (int i = 0; i < sampcount; i++) {
      float complex const samp = CMPLXF(buf[2*i],buf[2*i+1]);
      in_energy += cnrmf(samp);       // Calculate energy of the sample
      wptr[i] = samp * sdr->scale;    // Store sample in write pointer buffer
    }
    write_cfilter(&frontend->in, NULL,
		  sampcount); // Update write pointer, invoke FFT
  } else {
    // Use only one input in real mode
    // There **has** to be a cleaner method than dropping half the input samples
    // Also, DC removal is unnecessary in direct sampling mode, and using
    // just one input makes the I/Q gain balancing stuff in the library unnecessary.
    // And it's a pretty big CPU sink
    float *const wptr = frontend->in.input_write_pointer.r;
    assert(wptr != NULL);

    // read even samples for HF1, odd samples for HF2
    int const offs = sdr->hf_input == 2 ? 1 : 0;
    for (int i=0; i < sampcount; i++){
      float const samp = buf[2 * i + offs];
      in_energy += samp * samp;       // Calculate energy of the sample
      wptr[i] = samp * sdr->scale;    // Store sample in write pointer buffer
    }
    write_rfilter(&frontend->in, NULL,
		  sampcount); // Update write pointer, invoke FFT
  }
  frontend->samples += sampcount;

  if (isfinite(in_energy))
    frontend->if_power += Power_alpha * (in_energy / sampcount - frontend->if_power);
}

int fobos_startup(struct frontend *const frontend) {
  struct sdrstate *const sdr = (struct sdrstate *)frontend->context;
  sdr->scale = scale_AD(frontend);
  pthread_create(&sdr->monitor_thread, NULL, fobos_monitor, sdr);
  fprintf(stderr, "fobos read thread running\n");
  return 0;
}

double fobos_tune(struct frontend *const frontend, double const freq) {
  struct sdrstate *const sdr = (struct sdrstate *)frontend->context;
  if(sdr->direct_sampling)
    return 0.0; // No tuning in direct sample mode


  if(Verbose)
    fprintf(stderr, "Trying to tune to: %f\n", freq);
  double frequency_actual = 0.0;
  int result = fobos_rx_set_frequency(dev, freq, &frequency_actual);
  if (result != 0) {
    fprintf(stderr, "fobos_rx_set_frequency failed with error code: %d\n",
            result);
    fobos_rx_close(dev);
    return frequency_actual;
  }
  frontend->frequency = frequency_actual;
  return frontend->frequency;
}
