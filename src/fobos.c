/* Written by KC2DAC Dec 2024, adapted from existing KA9Q SDR handler programs
   Modified by Phil Karn KA9Q Feb 2024: approximate gain scaling, got direct sample mode working
   Modified by Phil Karn May 2026: handle repeated stop/start
In direct sample mode the two A/Ds are fed directly by the two HF inputs through fixed 20 dB gain
amplifiers, so there is no manual or automatic gain control.

The FFT is currently complex in both tuner and direct sample mode. In tuner mode it accepts a complex sample
stream centered on the tuner LO.

In direct sample mode you can also treat the inputs as complex, or select or combine them
on a per-channel basis by selecting output filter type BEAM and calling set_filter_weights() in filter.c
*/

// enable new raw-mode upcall added to library
// Requires my modified libfobos library with raw mode support
//#define RAW 1
#define CACHED_STORE 1

#include <assert.h>
#include <errno.h>
#include <fobos.h>
#include <iniparser/iniparser.h>
#include <pthread.h>
#include <unistd.h>
#include <string.h>
#if defined(linux)
#include <bsd/string.h>
#endif
#include <strings.h>
#include <sysexits.h>
#include <stdatomic.h>

#include "config.h"
#include "misc.h"
#include "radio.h"
#include "sched.h"
#include "defaults.h"


static double Power_alpha; // compute during first callback
static bool Name_set = false;

// hf_input has been removed, use i-weight and q-weight in individual channels
static char const *Fobos_keys[] = {
  "clk_source",
  "description",
  "device",
  "direct_sampling",
  "ext_clock", // Synonymous with clk_source, more descriptive
  "frequency",
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

enum state {
  STOPPED,
  STARTING,
  STOPPING,
  RUNNING
};
struct sdrstate {
  struct frontend *frontend;
  struct fobos_dev_t *dev;
  bool direct_sampling;
  int lna_gain;
  int vga_gain;
  int buff_count;
  int max_buff_count;
  int device;
  float dc_i,dc_q;
  unsigned int next_sample_num;
  double scale; // Scale samples for #bits and front end gain
  pthread_t monitor_thread;
  _Atomic enum state state;
};

#ifdef RAW
static void fobos_raw_callback(const uint16_t * restrict samples, uint32_t complex_count, void *ctx);
#else
static void rx_callback(float * restrict buf, unsigned buf_length, void * restrict ctx);
#endif
static void *fobos_monitor(void *p);

static int find_serial_position(const char *serials, const char *serialnumcfg) {
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
int fobos_setup(struct frontend *const frontend, dictionary const * const dictionary,
                char const *const section) {
  assert(dictionary != NULL);
  config_validate_section(stderr, dictionary, section, Fobos_keys, NULL);
  frontend->isreal = false; // Make sure the right kind of filter gets created!
#ifdef RAW
  frontend->bitspersample = 14; // gain scaling = 2^-14
#else
  // The Fobos apparently provides scaled float samples
  frontend->bitspersample = 1;  // gain scaling = 1
#endif
  frontend->rf_agc = false; // On by default unless gain or atten is specified

  // Read Config Files
  {
    char const *device = config_getstring(dictionary, section, "device", section);
    if (strcasecmp(device, "fobos") != 0)
      return -1; // Leave if not Fobos in the config
  }
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

  const char *serialnumcfg =
      config_getstring(dictionary, section, "serial", NULL);
  // If the config specifies a serial number look for it in the list --
  // otherwise assume device 0
  int position = 0;
  if (serialnumcfg != NULL) {
    position = find_serial_position(serialnumlist, serialnumcfg);
    if (position >= 0) {
    } else {
      fprintf(stderr,
              "Serial number '%s' not found in the list of connected Fobos "
              "devices\n",
              serialnumcfg);
      return -1;
    }
  }
  // Open the SDR
  struct sdrstate *const sdr = calloc(1, sizeof(struct sdrstate));
  // Cross-link generic and hardware-specific control structures
  assert(sdr != NULL);
  sdr->frontend = frontend;
  frontend->context = sdr;
  sdr->scale = scale_AD(frontend);
  sdr->buff_count = 0;
  sdr->max_buff_count = 2048;
  sdr->device = position;
  result = fobos_rx_open(&sdr->dev, sdr->device);
  if (result != FOBOS_ERR_OK) {
    fprintf(stderr, "Could not open device: %d\n", sdr->device);
    goto quit; // frees sdr, cleans up
  }
  {
    char const *cp = config_getstring(dictionary, section, "description", Description ? Description : "fobos");
    if(cp != NULL){
      strlcpy(frontend->description,cp,sizeof(frontend->description));
      Description = cp;
    }
  }
  char hw_revision[32];
  char fw_version[32];
  char manufacturer[32];
  char product[32];
  char serial[32];

  result = fobos_rx_get_board_info(sdr->dev, hw_revision, fw_version, manufacturer,
				   product, serial);
  if (result == FOBOS_ERR_OK) {
    fprintf(stderr, "%s %s serial %s, hardware %s, lib %s, driver %s firmware %s\n",
	    manufacturer,product,serial,hw_revision, lib_version,drv_version,fw_version);
  } else {
    fprintf(stderr, "Error fetching device info from fobos device: %d\n",
	    sdr->device);
    goto quit;
  }
  // Get Sample Rates offered by the Fobos
  double *sampvalues = NULL;    // Pointer to hold sample rates
  unsigned int samplecount = 0; // Initialize sample count

  // First call to get the count of sample rates
  result = fobos_rx_get_samplerates(sdr->dev, NULL, &samplecount);
  if (result != FOBOS_ERR_OK) {
    fprintf(stderr, "Error fetching sample rate count (error code: %d)\n",
	    result);
    fobos_rx_close(sdr->dev); // Close the device before returning
    goto quit;
  }
  // Allocate memory for the sample rates array
  sampvalues = (double *)malloc(samplecount * sizeof(double));
  if (sampvalues == NULL) {
    fprintf(stderr, "Error: Memory allocation failed for sample rates.\n");
    fobos_rx_close(sdr->dev); // Close the device before returning
    goto quit;
  }

  // Second call to fetch the actual sample rates
  result = fobos_rx_get_samplerates(sdr->dev, sampvalues, &samplecount);
  if (result == FOBOS_ERR_OK) {
    fprintf(stderr, "Supported Sample Rates for SDR #%d: ", sdr->device);
    for (unsigned int i = 0; i < samplecount; i++) {
      fprintf(stderr, " %'.0f", sampvalues[i]);
    }
    fprintf(stderr, "\n");
  } else {
    fprintf(stderr, "Error fetching sample rates (error code: %d)\n", result);
    fobos_rx_close(sdr->dev);
    goto quit;
  }
  FREE(sampvalues);
  // End of fetching sample rates here
  double requestsample =
      config_getdouble(dictionary, section, "samprate", 8000000.0);
  bool clk_sourcecfg = config_getboolean(dictionary, section, "clk_source", 0);
  clk_sourcecfg = config_getboolean(dictionary, section, "ext_clock", clk_sourcecfg);

  // Set the Actual Sample Rate
  double samprate_actual = 0.0;
  result = fobos_rx_set_samplerate(sdr->dev, requestsample, &samprate_actual);
  if (result == FOBOS_ERR_OK) {
    frontend->samprate = samprate_actual;
  } else {
    fprintf(stderr, "Error setting sample rate %f\n", requestsample);
    fobos_rx_close(sdr->dev);
    goto quit;
  }
  // Set Direct Sampling
  sdr->direct_sampling = config_getboolean(dictionary, section, "direct_sampling", 0);
  result = fobos_rx_set_direct_sampling(sdr->dev, sdr->direct_sampling);
  if (result != FOBOS_ERR_OK) {
    fprintf(stderr,
	    "fobos_rx_set_direct_sampling failed with error code: %d\n",
	    result);
    goto quit;
  }
  frontend->min_IF = -0.47 * frontend->samprate;
  frontend->max_IF = 0.47 * frontend->samprate;

  if(sdr->direct_sampling){
    // With -40 dBm @ 15 MHz on B input and nothing on A input,
    // A/D reads -42.2 dBm
    // So level_cal = -0.8 dB gives (average) input of -43.0 dBm
    // Note I flipped the sign convention on rf_level_cal to have units of dBm/FS
    // ie, how many dBm on the input gives 0 dBFS
    frontend->frequency = 0;
    frontend->rf_gain = 0;
    frontend->rf_atten = 0;
    frontend->rf_level_cal = -0.8;
  } else {
    const char *frequencycfg =
      config_getstring(dictionary, section, "frequency", "100m0");
    // Set Frequency
    double init_frequency = parse_frequency(frequencycfg, false);
    double frequency_actual = 0.0;
    // Wow, a library API that returns the *actual* tuner frequency. Bravo!
    int result = fobos_rx_set_frequency(sdr->dev, init_frequency, &frequency_actual);
    if (result != 0) {
      fprintf(stderr, "fobos_rx_set_frequency failed with error code: %d\n",
	      result);
      fobos_rx_close(sdr->dev);
      goto quit;
    }
    frontend->frequency = frequency_actual;

    sdr->lna_gain = config_getint(dictionary, section, "lna_gain", 0);
    sdr->vga_gain = config_getint(dictionary, section, "vga_gain", 0);

    // These gains are not used in direct sample mode; the MAX2830 is bypassed
    // Set LNA Gain 0..3
    // MAX2830 datasheet, p21: 11 => max gain, 10 => -16 dB, 0X => -33 dB
    result = fobos_rx_set_lna_gain(sdr->dev, sdr->lna_gain);
    if (result != FOBOS_ERR_OK) {
      fprintf(stderr, "fobos_rx_set_lna_gain failed with error code: %d\n",
	      result);
      goto quit;
    }
    // Get VGA Gain 0..31
    // MAX2830 datasheet, p21: 2 dB steps, 0-62 dB
    result = fobos_rx_set_vga_gain(sdr->dev, sdr->vga_gain);
    if (result != FOBOS_ERR_OK) {
      fprintf(stderr, "fobos_rx_set_vga_gain failed with error code: %d\n",
	      result);
      goto quit;
    }
    frontend->rf_gain = 2 * sdr->vga_gain + (sdr->lna_gain == 2 ? 16.0 : sdr->lna_gain == 3 ? 33.0 : 0);
    frontend->rf_atten = 0;
    frontend->rf_level_cal = -41; // very rough approximation, needs to be measured
  }
  // Set Clock Source
  result = fobos_rx_set_clk_source(sdr->dev, clk_sourcecfg);
  if (result != FOBOS_ERR_OK) {
    fprintf(stderr, "fobos_rx_set_clk_source failed with error code: %d\n",
	    result);
    goto quit;
  }
  fprintf(stderr,"samprate %'lf Hz, tuner %'.3lf Hz, lna_gain %d (%d dB) vga_gain %d (%d dB)\n",
	  frontend->samprate,
	  frontend->frequency,
	  sdr->lna_gain,
	  sdr->lna_gain == 2 ? 33 : sdr->lna_gain == 1 ? 16 : 0,
	  sdr->vga_gain,
	  sdr->vga_gain * 2);
  // SDR is open here
  return 0;
 quit:
  free(sdr);
  frontend->context = NULL;
  return -1;

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
double fobos_gain(struct frontend * const frontend, double gain){
  if(frontend->rf_agc)
    fprintf(stderr,"manual gain setting, turning off AGC\n");

  // Just the MAX2830 gain here
  double vgain = gain;
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

  struct sdrstate * const sdr = (struct sdrstate *)frontend->context;

  int result = fobos_rx_set_lna_gain(sdr->dev, lna);
  if (result != FOBOS_ERR_OK) {
    fprintf(stderr, "fobos_rx_set_lna_gain failed with error code: %d\n",
	    result);
  }

  // Set VGA Gain 0..31
  result = fobos_rx_set_vga_gain(sdr->dev, (int)vgain);
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
  int const buffer_count = 16;
  int const buffer_length = 65536;

  stick_core();
#if RAW
  int result = fobos_rx_read_async_raw(sdr->dev, fobos_raw_callback, sdr, buffer_count, buffer_length);
  if(result != 0) {
    fprintf(stderr, "fobos_rx_read_async_raw failed with error code: %d\n", result);
    exit(EXIT_FAILURE); // Exit the thread due to an error
  }
#else
  int result = fobos_rx_read_async(sdr->dev, rx_callback, sdr, buffer_count, buffer_length);
  if (result != 0) {
    fprintf(stderr, "fobos_rx_read_async failed with error code: %d\n", result);
    exit(EXIT_FAILURE); // Exit the thread due to an error
  }
#endif
  return NULL; // Return NULL when the thread exits cleanly
}


#ifndef RAW
// Callback for original Fobos floating point mode
// Library does int16->float conversion, DC removal, gain balancing but not phase balancing
static void rx_callback(float * restrict buf, unsigned sampcount, void *ctx) {
  struct sdrstate * const sdr = (struct sdrstate *)ctx;
  assert(sdr != NULL);
  struct frontend * restrict frontend = sdr->frontend;
  assert(frontend != NULL);
  assert(len != 0);
  if (!Name_set) {
    pthread_setname("fobos-cb");
    Name_set = true;
  }
  if(Power_alpha == 0){
    // Intialize smoothing parameter for power estimation to give 20 ms time constant
    Power_alpha = -expm1(-(double)sampcount/ (Blocktime * frontend->samprate));
    assert(Power_alpha >= 0 && Power_alpha <= 1);
  }
  float in_energy = 0;
  // Cast to real float to help vectorization; complex values are always IQIQ...
  float * const restrict wptr = (float *)frontend->in.input_write_pointer.c;
  assert(wptr != NULL);

  for (unsigned int n = 0; n < sampcount*2; n++) { // twice as many real samples as complex
    float const samp = buf[n];
    in_energy += samp * samp;       // Calculate energy of the sample
    wptr[n] = samp * (float)sdr->scale;    // Store sample in write pointer buffer
  }
  write_cfilter(&frontend->in, NULL,sampcount); // Update write pointer, invoke FFT
  frontend->samples += sampcount;

  if (sampcount != 0 && isfinite(in_energy))
    frontend->if_power += Power_alpha * (in_energy / sampcount - frontend->if_power);
}
#else // RAW
#if defined(__x86_64__)
#include <immintrin.h>
// Vectorized raw conversion

__attribute__((target("avx2")))
static inline int64_t hsum_i64x4(__m256i x){
  __m128i lo = _mm256_castsi256_si128(x);
  __m128i hi = _mm256_extracti128_si256(x, 1);
  __m128i sum = _mm_add_epi64(lo, hi);
  return (int64_t)_mm_cvtsi128_si64(sum) + (int64_t)_mm_extract_epi64(sum, 1);
}

__attribute__((target("avx2")))
static inline uint64_t hsum_u64x4(__m256i x){
  __m128i lo = _mm256_castsi256_si128(x);
  __m128i hi = _mm256_extracti128_si256(x, 1);
  __m128i sum = _mm_add_epi64(lo, hi);
  return (uint64_t)_mm_cvtsi128_si64(sum) + (uint64_t)_mm_extract_epi64(sum, 1);
}

__attribute__((target("avx2")))
static void fobos_convert_avx2(float *restrict dst,
			       uint16_t const *restrict src,
			       uint32_t sampcount,
			       float scale,
			       uint64_t *restrict energy,
			       int64_t *restrict dc_q,
			       int64_t *restrict dc_i){
  __m256i const mask14 = _mm256_set1_epi16(0x3fff);
  __m256i const midpoint = _mm256_set1_epi16(8192);

  // Each 32-bit lane contains Q in its low 16 bits and I in its
  // high 16 bits. These coefficients select one member of each pair.
  __m256i const select_q = _mm256_set1_epi32(0x00000001);
  __m256i const select_i = _mm256_set1_epi32(0x00010000);
  __m256 const vscale = _mm256_set1_ps(scale);

  __m256i energy_lo = _mm256_setzero_si256();
  __m256i energy_hi = _mm256_setzero_si256();

  __m256i qsum_lo = _mm256_setzero_si256();
  __m256i qsum_hi = _mm256_setzero_si256();
  __m256i isum_lo = _mm256_setzero_si256();
  __m256i isum_hi = _mm256_setzero_si256();

  uint32_t n = 0;
  for (; n + 8 <= sampcount; n += 8) {
    // Load:
    // Q0 I0 Q1 I1 ... Q7 I7
    __m256i x16 = _mm256_loadu_si256((__m256i const *)(src + 2 * n));
    x16 = _mm256_and_si256(x16, mask14);
    x16 = _mm256_sub_epi16(x16, midpoint);

    // Produce eight 32-bit Q and I values. madd performs:
    // word[0] * coefficient[0] + word[1] * coefficient[1]
    __m256i q32 = _mm256_madd_epi16(x16, select_q);
    __m256i i32 = _mm256_madd_epi16(x16, select_i);

    __m128i q32lo = _mm256_castsi256_si128(q32);
    __m128i q32hi = _mm256_extracti128_si256(q32, 1);
    __m128i i32lo = _mm256_castsi256_si128(i32);
    __m128i i32hi = _mm256_extracti128_si256(i32, 1);

    qsum_lo = _mm256_add_epi64(qsum_lo, _mm256_cvtepi32_epi64(q32lo));
    qsum_hi = _mm256_add_epi64(qsum_hi, _mm256_cvtepi32_epi64(q32hi));
    isum_lo = _mm256_add_epi64(isum_lo, _mm256_cvtepi32_epi64(i32lo));
    isum_hi = _mm256_add_epi64(isum_hi, _mm256_cvtepi32_epi64(i32hi));
    // Eight q*q + i*i values. Each result fits in int32_t:
    // 2 * 8192^2 = 134217728
    __m256i pair_energy = _mm256_madd_epi16(x16, x16);
    __m128i elo = _mm256_castsi256_si128(pair_energy);
    __m128i ehi = _mm256_extracti128_si256(pair_energy, 1);

    energy_lo = _mm256_add_epi64(energy_lo, _mm256_cvtepu32_epi64(elo));
    energy_hi = _mm256_add_epi64(energy_hi, _mm256_cvtepu32_epi64(ehi));
    // Widen the 16-bit Q,I words to 32 bits, convert to float,
    // scale, and preserve Q,I,Q,I ordering.
    __m128i words_lo = _mm256_castsi256_si128(x16);
    __m128i words_hi = _mm256_extracti128_si256(x16, 1);

    __m256 out_lo = _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_cvtepi16_epi32(words_lo)),vscale);
    __m256 out_hi = _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_cvtepi16_epi32(words_hi)),vscale);

#ifdef CACHED_STORE // cached version
    _mm256_storeu_ps(dst + 2 * n, out_lo);
    _mm256_storeu_ps(dst + 2 * n + 8, out_hi);
#else
    // non-temporal store (bypass the cache) because it's not going to stick around anyway until the FFT reads it
    // up to 20 ms from now
    _mm256_stream_ps(dst + 2 * n, out_lo);
    _mm256_stream_ps(dst + 2 * n + 8, out_hi);
#endif
  }
  *energy += hsum_u64x4(energy_lo) + hsum_u64x4(energy_hi);
  *dc_q += hsum_i64x4(qsum_lo) + hsum_i64x4(qsum_hi);
  *dc_i += hsum_i64x4(isum_lo) + hsum_i64x4(isum_hi);

  // Support buffer sizes that are not multiples of eight
  for (; n < sampcount; n++) {
    int q = (src[2 * n] & 0x3fff) - 8192;
    int i = (src[2 * n + 1] & 0x3fff) - 8192;

    *dc_q += q;
    *dc_i += i;
    *energy += (uint64_t)((int64_t)q * q) + (uint64_t)((int64_t)i * i);

    dst[2 * n] = (float)q * scale;
    dst[2 * n + 1] = (float)i * scale;
  }
  // must precede publication of the new write pointer
#ifndef CACHED_STORE
  _mm_sfence();
#endif
}
#endif // x86_64
// c version of raw conversion
static void fobos_convert(float *restrict dst,
			  uint16_t const *restrict src,
			  uint32_t sampcount,
			  float scale,
			  uint64_t *restrict energy,
			  int64_t *restrict dc_q,
			  int64_t *restrict dc_i){

  for(uint32_t n = 0; n < sampcount; n++){
    int q = (src[2 * n] & 0x3fff) - 8192;
    int i = (src[2 * n + 1] & 0x3fff) - 8192;

    *dc_q += q;
    *dc_i += i;
    *energy += (uint64_t)((int64_t)q * q) + (uint64_t)((int64_t)i * i);
    dst[2 * n] = (float)q * scale;
    dst[2 * n + 1] = (float)i * scale;
  }
}

// Raw-mode callback from modified libfobos
static void fobos_raw_callback(uint16_t const * restrict samples, uint32_t sampcount, void *ctx){
  struct sdrstate * const sdr = (struct sdrstate *)ctx;
  assert(sdr != NULL);
  struct frontend * restrict frontend = sdr->frontend;
  assert(frontend != NULL);
  assert(sampcount != 0);
  if (!Name_set) {
    pthread_setname("fobos-raw-cb");
    Name_set = true;
  }
  if(Power_alpha == 0){
    // Intialize smoothing parameter for power estimation to give 20 ms time constant
    Power_alpha = -expm1(-(double)sampcount/ (Blocktime * frontend->samprate));
    assert(Power_alpha >= 0 && Power_alpha <= 1);
  }
  uint64_t in_energy = 0;
  int64_t dc_i = 0;
  int64_t dc_q = 0;
  // Cast to real float to help vectorization; complex values are always IQIQ...
  float * const restrict wptr = (float *)frontend->in.input_write_pointer.c;
  assert(wptr != NULL);

  // samples contains 2 * complex_count words:
  // samples[] = Q0 I0 Q1 I1 ...
  // Each word contains an unsigned 14-bit offset-binary value in bits 0–13.
#if defined(__x86_64__)
  if(__builtin_cpu_supports("avx2") &&  __builtin_cpu_supports("popcnt"))
    fobos_convert_avx2(wptr,samples,sampcount,sdr->scale,&in_energy,&dc_q,&dc_i);
  else
#endif
    fobos_convert(wptr,samples,sampcount,sdr->scale,&in_energy,&dc_q,&dc_i);


  // I/Q gain and phase balancing still needs to be done
  write_cfilter(&frontend->in, NULL,sampcount); // Update write pointer, invoke FFT
  frontend->samples += sampcount;
  if (sampcount != 0){
    sdr->dc_q += Power_alpha * ((float)dc_q / sampcount - sdr->dc_q);
    sdr->dc_i += Power_alpha * ((float)dc_i / sampcount - sdr->dc_i);
    float dc_power = sdr->dc_i * sdr->dc_i + sdr->dc_q * sdr->dc_q;
    frontend->if_power += Power_alpha * (in_energy / sampcount - dc_power - frontend->if_power);
  }
}
#endif // RAW
int fobos_startup(struct frontend *const frontend) {
  struct sdrstate *const sdr = (struct sdrstate *)frontend->context;
  while(true){
    enum state s = STOPPED;
    if(atomic_compare_exchange_strong(&sdr->state,&s,STARTING))
      break;
    if(s == RUNNING)
      return 0; // Already running
    usleep(10000); // 10 ms
  }
  sdr->scale = scale_AD(frontend);
  pthread_create(&sdr->monitor_thread, NULL, fobos_monitor, sdr);
  atomic_store(&sdr->state,RUNNING);
  fprintf(stderr, "fobos read thread running\n");
  return 0;
}

int fobos_shutdown(struct frontend *const frontend) {
  struct sdrstate *const sdr = (struct sdrstate *)frontend->context;
  while(true){
    enum state s = RUNNING;
    if(atomic_compare_exchange_strong(&sdr->state,&s,STOPPING))
      break;
    if(s == STOPPED)
      return 0;
    usleep(10000);
  }
  fobos_rx_cancel_async(sdr->dev);
  pthread_join(sdr->monitor_thread,NULL);
  atomic_store(&sdr->state,STOPPED);
  fprintf(stderr, "fobos read thread stopped\n");
  return 0;
}


double fobos_tune(struct frontend *const frontend, double const freq) {
  struct sdrstate *const sdr = (struct sdrstate *)frontend->context;
  if(sdr->direct_sampling)
    return 0.0; // No tuning in direct sample mode


  if(Verbose)
    fprintf(stderr, "Trying to tune to: %f\n", freq);
  double frequency_actual = 0.0;
  int result = fobos_rx_set_frequency(sdr->dev, freq, &frequency_actual);
  if (result != 0) {
    fprintf(stderr, "fobos_rx_set_frequency failed with error code: %d\n",
            result);
    fobos_rx_close(sdr->dev);
    return frequency_actual;
  }
  frontend->frequency = frequency_actual;
  return frontend->frequency;
}
