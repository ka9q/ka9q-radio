/**
 * @file
 * @brief SoapySDR front-end shim for ka9q-radio radiod
 *
 * This module deliberately uses the SoapySDR C API.  It opens and configures
 * the device in soapy_setup(), but does not tune or start it unless an explicit
 * frequency appears in the configuration.  Streaming begins in
 * soapy_startup() and stops in soapy_shutdown().
 * Written by ChatGPT - untested!
 */
#include <SoapySDR/Device.h>
#include <SoapySDR/Errors.h>
#include <SoapySDR/Formats.h>
#include <SoapySDR/Types.h>

#include <assert.h>
#include <complex.h>
#include <errno.h>
#include <iniparser/iniparser.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sysexits.h>
#include <unistd.h>

#include "config.h"
#include "misc.h"
#include "radio.h"
#include "sched.h"

extern char const *Description;

enum state {
  STOPPED,
  STARTING,
  STOPPING,
  RUNNING
};

enum sample_format {
  FORMAT_CS16,
  FORMAT_CF32,
  FORMAT_CS8
};

struct sdrstate {
  struct frontend *frontend;
  SoapySDRDevice *device;
  SoapySDRStream *stream;
  size_t channel;
  size_t mtu;
  void *input;
  char format[16];
  enum sample_format sample_format;
  double scale;
  double nominal_gain;
  double attenuation;
  pthread_t read_thread;
  pthread_mutex_t control_mutex;
  _Atomic enum state state;
};

static char const *Soapy_keys[] = {
  "agc",
  "antenna",
  "args",
  "atten",
  "calibrate",
  "channel",
  "description",
  "device",
  "driver",
  "format",
  "frequency",
  "gain",
  "gaincal",
  "hardware",
  "if-fraction",
  "library",
  "samprate",
  "serial",
  NULL
};

static void destroy_state(struct sdrstate *sdr);
static int create_stream(struct sdrstate *sdr);
static void close_stream(struct sdrstate *sdr);
static void print_stream_formats(SoapySDRDevice const *device,size_t channel);
static bool format_available(SoapySDRDevice const *device,size_t channel,
                             char const *wanted);
static double select_sample_rate(SoapySDRDevice const *device,size_t channel,
                                 double wanted);
static int apply_gain(struct sdrstate *sdr);
static void *soapy_rx_thread(void *arg);
static void process_cs16(struct sdrstate *sdr,size_t count);
static void process_cf32(struct sdrstate *sdr,size_t count);
static void process_cs8(struct sdrstate *sdr,size_t count);

double soapy_tune(struct frontend *frontend,double frequency);
double soapy_gain(struct frontend *frontend,double gain);
double soapy_atten(struct frontend *frontend,double atten);

static void destroy_state(struct sdrstate * const sdr){
  if(sdr == NULL)
    return;
  close_stream(sdr);
  if(sdr->device != NULL)
    SoapySDRDevice_unmake(sdr->device);
  pthread_mutex_destroy(&sdr->control_mutex);
  free(sdr);
}
static void close_stream(struct sdrstate * const sdr){
  if(sdr->stream != NULL && sdr->device != NULL)
    SoapySDRDevice_closeStream(sdr->device,sdr->stream);
  sdr->stream = NULL;
  free(sdr->input);
  sdr->input = NULL;
  sdr->mtu = 0;
}
static int create_stream(struct sdrstate * const sdr){
  assert(sdr != NULL);
  assert(sdr->device != NULL);
  if(sdr->stream != NULL)
    return 0;

  size_t stream_channel = sdr->channel;
  sdr->stream = SoapySDRDevice_setupStream(sdr->device,SOAPY_SDR_RX,sdr->format,&stream_channel,1,NULL);
  if(sdr->stream == NULL){
    fprintf(stderr,"SoapySDR setupStream(%s): %s\n",sdr->format,
            SoapySDRDevice_lastError());
    return -1;
  }
  sdr->mtu = SoapySDRDevice_getStreamMTU(sdr->device,sdr->stream);
  if(sdr->mtu == 0)
    sdr->mtu = 16384;
  size_t bytes_per_element = 0;
  switch(sdr->sample_format){
  case FORMAT_CS16:
    bytes_per_element = 2 * sizeof(int16_t);
    break;
  case FORMAT_CF32:
    bytes_per_element = 2 * sizeof(float);
    break;
  case FORMAT_CS8:
    bytes_per_element = 2 * sizeof(int8_t);
    break;
  }
  sdr->input = malloc(sdr->mtu * bytes_per_element);
  if(sdr->input == NULL){
    fprintf(stderr,"SoapySDR input buffer allocation: %s\n",strerror(errno));
    close_stream(sdr);
    return -1;
  }
  return 0;
}
static bool format_available(SoapySDRDevice const *device,size_t channel,char const *wanted){
  size_t count = 0;
  char **formats = SoapySDRDevice_getStreamFormats(device,SOAPY_SDR_RX,
                                                    channel,&count);
  bool found = false;
  for(size_t i=0; i < count; i++){
    if(strcmp(formats[i],wanted) == 0)
      found = true;
  }
  SoapySDRStrings_clear(&formats,count);
  return found;
}
static void print_stream_formats(SoapySDRDevice const * const device,size_t channel){
  size_t count = 0;
  char **formats = SoapySDRDevice_getStreamFormats(device,SOAPY_SDR_RX,
                                                    channel,&count);
  fprintf(stderr,"SoapySDR stream formats:");
  for(size_t i=0; i < count; i++)
    fprintf(stderr," %s",formats[i]);
  fputc('\n',stderr);
  SoapySDRStrings_clear(&formats,count);
}
static double candidate_in_range(SoapySDRRange const * const range,double wanted){
  double candidate = fmax(range->minimum,fmin(wanted,range->maximum));
  if(range->step > 0){
    candidate = range->minimum
      + nearbyint((candidate - range->minimum) / range->step) * range->step;
    candidate = fmax(range->minimum,fmin(candidate,range->maximum));
  }
  return candidate;
}
static double select_sample_rate(SoapySDRDevice const * const device,size_t channel,double wanted){
  size_t count = 0;
  SoapySDRRange *ranges = SoapySDRDevice_getSampleRateRange(
    device,SOAPY_SDR_RX,channel,&count);

  if(count == 0 || ranges == NULL){
    SoapySDR_free(ranges);
    return wanted;
  }
  double selected = candidate_in_range(&ranges[0],wanted);
  double error = fabs(selected - wanted);
  fprintf(stderr,"SoapySDR sample-rate ranges:");
  for(size_t i=0; i < count; i++){
    fprintf(stderr," [%'.0f,%'.0f",ranges[i].minimum,ranges[i].maximum);
    if(ranges[i].step > 0)
      fprintf(stderr,", step %'.0f",ranges[i].step);
    fputc(']',stderr);
    double const candidate = candidate_in_range(&ranges[i],wanted);
    double const candidate_error = fabs(candidate - wanted);
    if(candidate_error < error){
      selected = candidate;
      error = candidate_error;
    }
  }
  fputc('\n',stderr);
  SoapySDR_free(ranges);
  return selected;
}
static int apply_gain(struct sdrstate * const sdr){
  struct frontend * const frontend = sdr->frontend;
  double const requested = sdr->nominal_gain - sdr->attenuation;
  int const result = SoapySDRDevice_setGain(sdr->device,SOAPY_SDR_RX,sdr->channel,requested);
  if(result != 0){
    fprintf(stderr,"SoapySDR setGain(%.1f dB): %s\n",
            requested,SoapySDRDevice_lastError());
    return -1;
  }
  double const actual = SoapySDRDevice_getGain(sdr->device,SOAPY_SDR_RX,sdr->channel);
  /* Preserve radiod's gain-minus-attenuation representation even though
     generic SoapySDR exposes only one overall gain control. */
  frontend->rf_atten = sdr->attenuation;
  frontend->rf_gain = actual + sdr->attenuation;
  return 0;
}

int soapy_setup(struct frontend * const frontend,
                dictionary const * const dictionary,
                char const * const section){
  assert(frontend != NULL);
  assert(dictionary != NULL);
  assert(section != NULL);

  char const * const device_name = config_getstring(dictionary,section,"device",section);
  if(strcasecmp(device_name,"soapy") != 0)
    return -1;
  config_validate_section(stderr,dictionary,section,Soapy_keys,NULL);

  struct sdrstate * const sdr = calloc(1,sizeof(*sdr));
  if(sdr == NULL){
    fprintf(stderr,"SoapySDR calloc: %s\n",strerror(errno));
    return -1;
  }
  if(pthread_mutex_init(&sdr->control_mutex,NULL) != 0){
    free(sdr);
    return -1;
  }
  sdr->frontend = frontend;
  frontend->context = sdr;
  atomic_init(&sdr->state,STOPPED);
  int const channel = config_getint(dictionary,section,"channel",0);
  if(channel < 0){
    fprintf(stderr,"SoapySDR channel must be nonnegative\n");
    goto fail;
  }
  sdr->channel = (size_t)channel;
  char const *args_text = config_getstring(dictionary,section,"args","");
  SoapySDRKwargs args = SoapySDRKwargs_fromString(args_text);
  char const *driver = config_getstring(dictionary,section,"driver",NULL);
  char const *serial = config_getstring(dictionary,section,"serial",NULL);
  if(driver != NULL && SoapySDRKwargs_set(&args,"driver",driver) != 0){
    SoapySDRKwargs_clear(&args);
    goto fail;
  }
  if(serial != NULL && SoapySDRKwargs_set(&args,"serial",serial) != 0){
    SoapySDRKwargs_clear(&args);
    goto fail;
  }
  if(args.size == 0){
    fprintf(stderr,"SoapySDR requires driver= or args= in [%s]\n",section);
    SoapySDRKwargs_clear(&args);
    goto fail;
  }
  sdr->device = SoapySDRDevice_make(&args);
  SoapySDRKwargs_clear(&args);
  if(sdr->device == NULL){
    fprintf(stderr,"SoapySDRDevice_make: %s\n",SoapySDRDevice_lastError());
    goto fail;
  }
  size_t const channels = SoapySDRDevice_getNumChannels(sdr->device,SOAPY_SDR_RX);
  if(sdr->channel >= channels){
    fprintf(stderr,"SoapySDR RX channel %zu requested; device has %zu\n",
            sdr->channel,channels);
    goto fail;
  }
  char *driver_key = SoapySDRDevice_getDriverKey(sdr->device);
  char *hardware_key = SoapySDRDevice_getHardwareKey(sdr->device);
  fprintf(stderr,"SoapySDR driver %s, hardware %s, RX channel %zu\n",
          driver_key != NULL ? driver_key : "<unknown>",
          hardware_key != NULL ? hardware_key : "<unknown>",sdr->channel);
  char const *description = config_getstring(dictionary,section,"description",
					     hardware_key != NULL && *hardware_key != '\0' ? hardware_key : "SoapySDR");
  strlcpy(frontend->description,description,sizeof(frontend->description));
  Description = frontend->description;
  SoapySDR_free(driver_key);
  SoapySDR_free(hardware_key);
  char const * const rate_text = config_getstring(dictionary,section,"samprate",NULL);
  double wanted_rate = 2000000;
  if(rate_text != NULL)
    wanted_rate = parse_frequency(rate_text,false);
  if(!isfinite(wanted_rate) || wanted_rate <= 0){
    fprintf(stderr,"Invalid SoapySDR sample rate %s\n",
            rate_text != NULL ? rate_text : "");
    goto fail;
  }
  double const selected_rate = select_sample_rate(sdr->device, sdr->channel, wanted_rate);
  if(SoapySDRDevice_setSampleRate(sdr->device, SOAPY_SDR_RX, sdr->channel, selected_rate) != 0){
    fprintf(stderr,"SoapySDR setSampleRate(%'.0f): %s\n",selected_rate, SoapySDRDevice_lastError());
    goto fail;
  }
  frontend->samprate = SoapySDRDevice_getSampleRate(
    sdr->device,SOAPY_SDR_RX,sdr->channel);
  if(!isfinite(frontend->samprate) || frontend->samprate <= 0){
    fprintf(stderr,"SoapySDR returned invalid sample rate %.9g\n",
            frontend->samprate);
    goto fail;
  }
  fprintf(stderr,"SoapySDR requested sample rate %'.0f Hz, actual %'.0f Hz\n",
          wanted_rate,frontend->samprate);
  print_stream_formats(sdr->device,sdr->channel);
  char const * const requested_format = config_getstring(dictionary,section,"format",NULL);
  if(requested_format != NULL){
    if(!format_available(sdr->device,sdr->channel,requested_format)){
      fprintf(stderr,"SoapySDR stream format %s is unavailable\n",requested_format);
      goto fail;
    }
    strlcpy(sdr->format,requested_format,sizeof(sdr->format));
  } else if(format_available(sdr->device,sdr->channel,SOAPY_SDR_CS16)){
    strlcpy(sdr->format,SOAPY_SDR_CS16,sizeof(sdr->format));
  } else if(format_available(sdr->device,sdr->channel,SOAPY_SDR_CF32)){
    strlcpy(sdr->format,SOAPY_SDR_CF32,sizeof(sdr->format));
  } else if(format_available(sdr->device,sdr->channel,SOAPY_SDR_CS8)){
    strlcpy(sdr->format,SOAPY_SDR_CS8,sizeof(sdr->format));
  } else {
    fprintf(stderr,"SoapySDR device supports none of CS16, CF32 or CS8\n");
    goto fail;
  }
  fprintf(stderr,"SoapySDR using stream format %s\n",sdr->format);
  if(strcmp(sdr->format,SOAPY_SDR_CS16) == 0){
    sdr->sample_format = FORMAT_CS16;
    frontend->bitspersample = 16;
  } else if(strcmp(sdr->format,SOAPY_SDR_CF32) == 0){
    sdr->sample_format = FORMAT_CF32;
    frontend->bitspersample = 1;
  } else if(strcmp(sdr->format,SOAPY_SDR_CS8) == 0){
    sdr->sample_format = FORMAT_CS8;
    frontend->bitspersample = 8;
  } else {
    fprintf(stderr,"Unsupported configured SoapySDR format %s\n",sdr->format);
    goto fail;
  }
  frontend->isreal = false;
  frontend->frequency = 0;
  frontend->calibrate = config_getdouble(dictionary,section,"calibrate",0);
  frontend->rf_level_cal = config_getdouble(dictionary,section,"gaincal",NAN);
  double if_fraction = config_getdouble(dictionary,section,"if-fraction",0.47);
  if(!isfinite(if_fraction) || if_fraction <= 0 || if_fraction > 0.5){
    fprintf(stderr,"Invalid if-fraction %.6g; using 0.47\n",if_fraction);
    if_fraction = 0.47;
  }
  frontend->min_IF = -if_fraction * frontend->samprate;
  frontend->max_IF = +if_fraction * frontend->samprate;
  char const * const antenna = config_getstring(dictionary,section,"antenna",NULL);
  if(antenna != NULL &&
     SoapySDRDevice_setAntenna(sdr->device,SOAPY_SDR_RX,sdr->channel,
                               antenna) != 0){
    fprintf(stderr,"SoapySDR setAntenna(%s): %s\n",antenna,
            SoapySDRDevice_lastError());
    goto fail;
  }
  bool const has_agc = SoapySDRDevice_hasGainMode(sdr->device, SOAPY_SDR_RX, sdr->channel);
  frontend->rf_agc = has_agc && config_getboolean(dictionary,section,"agc",false);
  if(has_agc && SoapySDRDevice_setGainMode(sdr->device, SOAPY_SDR_RX, sdr->channel, frontend->rf_agc) != 0){
    fprintf(stderr,"SoapySDR setGainMode: %s\n",SoapySDRDevice_lastError());
    goto fail;
  }
  sdr->nominal_gain = SoapySDRDevice_getGain(sdr->device, SOAPY_SDR_RX, sdr->channel);
  sdr->attenuation = 0;
  char const * const gain_text = config_getstring(dictionary,section,"gain",NULL);
  char const * const atten_text = config_getstring(dictionary,section,"atten",NULL);
  if(gain_text != NULL)
    sdr->nominal_gain = strtod(gain_text,NULL);
  if(atten_text != NULL)
    sdr->attenuation = fabs(strtod(atten_text,NULL));
  frontend->rf_gain = sdr->nominal_gain;
  frontend->rf_atten = sdr->attenuation;
  if(gain_text != NULL || atten_text != NULL){
    if(frontend->rf_agc){
      if(SoapySDRDevice_setGainMode(sdr->device,SOAPY_SDR_RX,sdr->channel, false) != 0){
        fprintf(stderr,"SoapySDR disabling AGC: %s\n", SoapySDRDevice_lastError());
        goto fail;
      }
      frontend->rf_agc = false;
    }
    if(apply_gain(sdr) != 0)
      goto fail;
  }
  if(create_stream(sdr) != 0)
    goto fail;
  /* A configured frequency is an explicit request and locks the tuner, as in
     the native radiod drivers.  With no frequency key, leave it at zero and
     wait for radiod to call soapy_tune(). */
  char const * const frequency_text = config_getstring(dictionary,section,"frequency",NULL);
  if(frequency_text != NULL){
    double const frequency = parse_frequency(frequency_text,false);
    if(soapy_tune(frontend,frequency) == 0)
      goto fail;
    frontend->lock = true;
    fprintf(stderr,"Locked SoapySDR tuner frequency %'.3f Hz\n",
            frontend->frequency);
  }
  fprintf(stderr,"SoapySDR ready, MTU %zu complex samples, AGC %s, gain %.1f dB, attenuation %.1f dB\n",
	  sdr->mtu,
          frontend->rf_agc ? "on" : "off",frontend->rf_gain,
          frontend->rf_atten);
  return 0;
fail:
  frontend->context = NULL;
  destroy_state(sdr);
  return -1;
}
int soapy_startup(struct frontend * const frontend){
  assert(frontend != NULL);
  struct sdrstate * const sdr = frontend->context;
  assert(sdr != NULL);
  if(create_stream(sdr) != 0)
    return -1;
  for(;;){
    enum state expected = STOPPED;
    if(atomic_compare_exchange_strong(&sdr->state,&expected,STARTING))
      break;
    if(expected == RUNNING)
      return 0;
    usleep(10000);
  }
  sdr->scale = scale_AD(frontend);
  int const result = SoapySDRDevice_activateStream(sdr->device, sdr->stream, 0, 0, 0);
  if(result != 0){
    fprintf(stderr,"SoapySDR activateStream: %s\n",SoapySDR_errToStr(result));
    atomic_store(&sdr->state,STOPPED);
    return -1;
  }
  int const error = pthread_create(&sdr->read_thread,NULL,soapy_rx_thread,sdr);
  if(error != 0){
    fprintf(stderr,"SoapySDR pthread_create: %s\n",strerror(error));
    SoapySDRDevice_deactivateStream(sdr->device,sdr->stream,0,0);
    atomic_store(&sdr->state,STOPPED);
    return -1;
  }
  atomic_store(&sdr->state,RUNNING);
  fprintf(stderr,"SoapySDR running\n");
  return 0;
}

int soapy_shutdown(struct frontend * const frontend){
  assert(frontend != NULL);
  struct sdrstate * const sdr = frontend->context;
  assert(sdr != NULL);
  for(;;){
    enum state expected = RUNNING;
    if(atomic_compare_exchange_strong(&sdr->state,&expected,STOPPING))
      break;
    if(expected == STOPPED)
      return 0;
    usleep(10000);
  }
  pthread_join(sdr->read_thread,NULL);
  int const result = SoapySDRDevice_deactivateStream(sdr->device,sdr->stream, 0, 0);
  close_stream(sdr);
  atomic_store(&sdr->state,STOPPED);
  if(result != 0){
    fprintf(stderr,"SoapySDR deactivateStream: %s\n",
            SoapySDR_errToStr(result));
    return -1;
  }
  fprintf(stderr,"SoapySDR stopped\n");
  return 0;
}
double soapy_tune(struct frontend * const frontend,double const frequency){
  assert(frontend != NULL);
  struct sdrstate * const sdr = frontend->context;
  assert(sdr != NULL);
  if(frontend->lock)
    return frontend->frequency;
  if(!isfinite(frequency) || frequency <= 0)
    return 0;
  pthread_mutex_lock(&sdr->control_mutex);
  double const device_frequency = frequency / (1 + frontend->calibrate);
  int const result = SoapySDRDevice_setFrequency(
    sdr->device,SOAPY_SDR_RX,sdr->channel,device_frequency,NULL);
  if(result == 0){
    double const actual = SoapySDRDevice_getFrequency(sdr->device, SOAPY_SDR_RX, sdr->channel);
    frontend->frequency = actual * (1 + frontend->calibrate);
  }
  pthread_mutex_unlock(&sdr->control_mutex);
  if(result != 0){
    fprintf(stderr,"SoapySDR setFrequency(%'.3f): %s\n", frequency, SoapySDRDevice_lastError());
    return 0;
  }
  return frontend->frequency;
}
double soapy_gain(struct frontend * const frontend,double const gain){
  assert(frontend != NULL);
  struct sdrstate * const sdr = frontend->context;
  assert(sdr != NULL);
  if(!isfinite(gain))
    return frontend->rf_gain;

  pthread_mutex_lock(&sdr->control_mutex);
  if(frontend->rf_agc){
    if(SoapySDRDevice_setGainMode(sdr->device, SOAPY_SDR_RX, sdr->channel, false) != 0){
      fprintf(stderr,"SoapySDR disabling AGC: %s\n",SoapySDRDevice_lastError());
      pthread_mutex_unlock(&sdr->control_mutex);
      return frontend->rf_gain;
    }
    frontend->rf_agc = false;
  }
  double const old_gain = sdr->nominal_gain;
  sdr->nominal_gain = gain;
  if(apply_gain(sdr) != 0)
    sdr->nominal_gain = old_gain;
  pthread_mutex_unlock(&sdr->control_mutex);
  return frontend->rf_gain;
}
double soapy_atten(struct frontend * const frontend,double atten){
  assert(frontend != NULL);
  struct sdrstate * const sdr = frontend->context;
  assert(sdr != NULL);
  if(!isfinite(atten))
    return frontend->rf_atten;
  atten = fabs(atten);
  pthread_mutex_lock(&sdr->control_mutex);
  if(frontend->rf_agc){
    if(SoapySDRDevice_setGainMode(sdr->device,SOAPY_SDR_RX,sdr->channel, false) != 0){
      fprintf(stderr,"SoapySDR disabling AGC: %s\n", SoapySDRDevice_lastError());
      pthread_mutex_unlock(&sdr->control_mutex);
      return frontend->rf_atten;
    }
    frontend->rf_agc = false;
  }
  double const old_attenuation = sdr->attenuation;
  sdr->attenuation = atten;
  if(apply_gain(sdr) != 0)
    sdr->attenuation = old_attenuation;
  pthread_mutex_unlock(&sdr->control_mutex);
  return frontend->rf_atten;
}
static void process_cs16(struct sdrstate *sdr,size_t const count){
  struct frontend * const frontend = sdr->frontend;
  int16_t const *input = sdr->input;
  float complex * const output = frontend->in.input_write_pointer.c;
  double energy = 0;
  uint64_t overranges = 0;
  for(size_t i=0; i < count; i++){
    int16_t const ii = input[2*i];
    int16_t const qq = input[2*i+1];
    bool const over = ii == INT16_MIN || ii == INT16_MAX || qq == INT16_MIN || qq == INT16_MAX;
    overranges += over;
    frontend->samp_since_over = over ? 0 : frontend->samp_since_over + 1;
    energy += (double)ii * ii + (double)qq * qq;
    output[i] = (float complex)(sdr->scale * (ii + I * (double)qq));
  }
  frontend->overranges += overranges;
  frontend->samples += count;
  write_cfilter(&frontend->in,NULL,count);
  if(count != 0 && isfinite(energy)){
    double const alpha = -expm1(-(double)count / (0.1 * frontend->samprate));
    frontend->if_power += alpha * (energy / count - frontend->if_power);
  }
}
static void process_cf32(struct sdrstate * const sdr,size_t const count){
  struct frontend * const frontend = sdr->frontend;
  float const * const input = sdr->input;
  float complex * const output = frontend->in.input_write_pointer.c;
  double energy = 0;
  uint64_t overranges = 0;
  for(size_t i=0; i < count; i++){
    float const ii = input[2*i];
    float const qq = input[2*i+1];
    bool const over = fabsf(ii) >= 1 || fabsf(qq) >= 1;
    overranges += over;
    frontend->samp_since_over = over ? 0 : frontend->samp_since_over + 1;
    energy += (double)ii * ii + (double)qq * qq;
    output[i] = sdr->scale * (ii + I * qq);
  }
  frontend->overranges += overranges;
  frontend->samples += count;
  write_cfilter(&frontend->in,NULL,count);
  if(count != 0 && isfinite(energy)){
    double const alpha = -expm1(-(double)count / (0.1 * frontend->samprate));
    frontend->if_power += alpha * (energy / count - frontend->if_power);
  }
}
static void process_cs8(struct sdrstate * const sdr,size_t const count){
  struct frontend * const frontend = sdr->frontend;
  int8_t const * const input = sdr->input;
  float complex * const output = frontend->in.input_write_pointer.c;
  double energy = 0;
  uint64_t overranges = 0;
  for(size_t i=0; i < count; i++){
    int8_t const ii = input[2*i];
    int8_t const qq = input[2*i+1];
    bool const over = ii == INT8_MIN || ii == INT8_MAX ||
                      qq == INT8_MIN || qq == INT8_MAX;
    overranges += over;
    frontend->samp_since_over = over ? 0 : frontend->samp_since_over + 1;
    energy += (double)ii * ii + (double)qq * qq;
    output[i] = (float complex)(sdr->scale * (ii + I * (double)qq));
  }
  frontend->overranges += overranges;
  frontend->samples += count;
  write_cfilter(&frontend->in,NULL,count);
  if(count != 0 && isfinite(energy)){
    double const alpha = -expm1(-(double)count / (0.1 * frontend->samprate));
    frontend->if_power += alpha * (energy / count - frontend->if_power);
  }
}
static void *soapy_rx_thread(void *arg){
  struct sdrstate * const sdr = arg;
  assert(sdr != NULL);
  //  struct frontend *frontend = sdr->frontend;
  //  assert(frontend != NULL);
  pthread_setname("soapy-rx");
  stick_core();

  enum state state;
  while((state = atomic_load(&sdr->state)) == STARTING || state == RUNNING){
    void *buffers[] = {sdr->input};
    int flags = 0;
    long long time_ns = 0;
    int const result = SoapySDRDevice_readStream(sdr->device,sdr->stream,buffers,sdr->mtu,&flags,&time_ns,100000);
    if(result > 0){
      size_t const count = (size_t)result;
      switch(sdr->sample_format){
      case FORMAT_CS16:
	process_cs16(sdr,count);
	break;
      case FORMAT_CF32:
	process_cf32(sdr,count);
	break;
      case FORMAT_CS8:
	process_cs8(sdr,count);
	break;
      }
      continue;
    }
    if(result == SOAPY_SDR_TIMEOUT)
      continue;
    if(result == SOAPY_SDR_OVERFLOW){
      fprintf(stderr,"SoapySDR input overflow\n");
      continue;
    }
    if(atomic_load(&sdr->state) == STOPPING)
      break;
    fprintf(stderr,"SoapySDR readStream: %s\n",SoapySDR_errToStr(result));
    exit(EX_NOINPUT);
  }
  return NULL;
}
