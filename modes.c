// $Id: modes.c,v 1.59 2022/04/24 09:07:42 karn Exp $
// Load and search mode definition table in /usr/local/share/ka9q-radio/modes.conf

// Copyright 2018, Phil Karn, KA9Q
#define _GNU_SOURCE 1  // Avoid warnings when including dsp.h
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#if defined(linux)
#include <bsd/string.h>
#endif
#include <string.h>
#include <ctype.h>
#include <iniparser/iniparser.h>
#include <pthread.h>

#include "misc.h"
#include "radio.h"
#include "config.h"

static int const DEFAULT_LINEAR_SAMPRATE = 24000;
static int const DEFAULT_FM_SAMPRATE = 24000;
static float const DEFAULT_KAISER_BETA = 11.0;   // reasonable tradeoff between skirt sharpness and sidelobe height
static float const DEFAULT_LOW = -5000.0;        // Ballpark numbers, should be properly set for each mode
static float const DEFAULT_HIGH = 5000.0;
static float const DEFAULT_HEADROOM = -15.0;     // keep gaussian signals from clipping
static float const DEFAULT_SQUELCH_OPEN = 8.0;   // open when SNR > 8 dB
static float const DEFAULT_SQUELCH_CLOSE = 7.0;  // close when SNR < 7 dB
static float const DEFAULT_RECOVERY_RATE = 20.0; // 20 dB/s gain increase
static float const DEFAULT_THRESHOLD = -15.0;    // Don't let noise rise above -15 relative to headroom
static float const DEFAULT_GAIN = 80.0;          // Unused in FM, usually adjusted automatically in linear
static float const DEFAULT_HANGTIME = 1.1;       // keep low gain 1.1 sec before increasing
static float const DEFAULT_PLL_BW = 100.0;       // Reasonable for AM
static int const DEFAULT_SQUELCHTAIL = 1;        // close on frame *after* going below threshold, may let partial frame noise through
static float const DEFAULT_NBFM_TC = 530.5;      // Time constant for NBFM emphasis (300 Hz corner)
static float const DEFAULT_WFM_TC = 75.0;        // Time constant for FM broadcast (North America/Korea standard)
static float const DEFAULT_FM_DEEMPH_GAIN = 12.0; // +12 dB to give subjectively equal loudness with deemphsis
static float const DEFAULT_WFM_DEEMPH_GAIN = 0.0;

struct demodtab Demodtab[] = {
      {LINEAR_DEMOD, "Linear"}, // Coherent demodulation of AM, DSB, BPSK; calibration on WWV/WWVH/CHU carrier
      {FM_DEMOD,     "FM",   }, // NBFM and noncoherent PM
      {WFM_DEMOD,    "WFM",  }, // NBFM and noncoherent PM
};
int Ndemod = sizeof(Demodtab)/sizeof(struct demodtab);

int demod_type_from_name(char const *name){
  for(int n = 0; n < Ndemod; n++){
    if(strncasecmp(name,Demodtab[n].name,sizeof(Demodtab[n].name)) == 0)
      return Demodtab[n].type;
  }
  return -1;
}


char const *demod_name_from_type(enum demod_type type){
  if(type >= 0 && Ndemod)
    return Demodtab[type].name;
  return NULL;
}

// Set reasonable defaults before reading mode or config tables
static int set_defaults(struct demod *demod){
  if(demod == NULL)
    return -1;

  demod->tp1 = demod->tp2 = NAN;
  demod->tune.doppler = 0;
  demod->tune.doppler_rate = 0;
  // De-emphasis defaults to off, enabled only in FM modes
  demod->deemph.rate = 0;
  demod->deemph.gain = 1.0;

  demod->filter.kaiser_beta = DEFAULT_KAISER_BETA;
  demod->filter.min_IF = DEFAULT_LOW;
  demod->filter.max_IF = DEFAULT_HIGH;
  demod->squelch_open = dB2power(DEFAULT_SQUELCH_OPEN);
  demod->squelch_close = dB2power(DEFAULT_SQUELCH_CLOSE);
  demod->squelchtail = DEFAULT_SQUELCHTAIL;
  demod->output.headroom = dB2voltage(DEFAULT_HEADROOM);
  demod->output.channels = 1;
  demod->tune.shift = 0.0;
  demod->linear.recovery_rate = dB2voltage(DEFAULT_RECOVERY_RATE * .001f * Blocktime);
  demod->linear.hangtime = DEFAULT_HANGTIME / (.001 * Blocktime);
  demod->linear.threshold = dB2voltage(DEFAULT_THRESHOLD);
  demod->output.gain = dB2voltage(DEFAULT_GAIN);
  demod->linear.env = 0;
  demod->linear.pll = 0;
  demod->linear.square = 0;
  demod->filter.isb = 0;
  demod->linear.loop_bw = DEFAULT_PLL_BW;
  demod->linear.agc = 1;
  switch(demod->demod_type){
  case LINEAR_DEMOD:
    demod->output.samprate = DEFAULT_LINEAR_SAMPRATE;
    break;
  case FM_DEMOD:
    demod->output.samprate = DEFAULT_FM_SAMPRATE;
    {
      float tc = DEFAULT_NBFM_TC;
      demod->deemph.rate = expf(-1.0 / (tc * 1e-6 * demod->output.samprate));
      demod->deemph.gain = dB2voltage(DEFAULT_FM_DEEMPH_GAIN);
    }
    break;
  case WFM_DEMOD:
    demod->output.channels = 2;      // always stereo
    demod->output.samprate = 384000; // downconverter samprate forced for FM stereo decoding. Output also forced to 48 kHz
    {
      // Default 75 microseconds for north american FM broadcasting
      float tc = DEFAULT_WFM_TC;
      demod->deemph.rate = expf(-1.0 / (tc * 1e-6 * 48000)); // hardwired output sample rate -- needs cleanup
      demod->deemph.gain = dB2voltage(DEFAULT_WFM_DEEMPH_GAIN);
    }
  }
  return 0;
}

// Set selected section of specified config file into current demod structure
// Caller must (re) initialize pre-demod filter and (re)start demodulator thread
int loadmode(struct demod *demod,dictionary const *table,char const *mode,int use_defaults){
  if(demod == NULL || table == NULL || mode == NULL || strlen(mode) == 0)
    return -1;

  // No default
  char const * demod_name = config_getstring(table,mode,"demod",NULL);
  if(demod_name == NULL)
    return -1;

  {
    int x = demod_type_from_name(demod_name);
    if(demod->demod_type >= 0){
      demod->demod_type = x;
    }
  }
  if(use_defaults)
    set_defaults(demod); // must be called after demod_type is set
  demod->output.samprate = config_getint(table,mode,"samprate",demod->output.samprate);
  demod->output.channels = config_getint(table,mode,"channels",demod->output.channels);
  if(config_getboolean(table,mode,"mono",0))
    demod->output.channels = 1;
  if(config_getboolean(table,mode,"stereo",0))
    demod->output.channels = 2;
  demod->filter.kaiser_beta = config_getfloat(table,mode,"kaiser-beta",demod->filter.kaiser_beta);

  // Pre-detection filter limits
  demod->filter.min_IF = config_getfloat(table,mode,"low",demod->filter.min_IF);
  demod->filter.max_IF = config_getfloat(table,mode,"high",demod->filter.max_IF);
  if(demod->filter.min_IF > demod->filter.max_IF){
    // Ensure max >= min
    float t = demod->filter.min_IF;
    demod->filter.min_IF = demod->filter.max_IF;
    demod->filter.max_IF = t;
  }
  {
    char const *cp = config_getstring(table,mode,"squelch-open",NULL);
    if(cp)
      demod->squelch_open = dB2power(strtof(cp,NULL));
  }
  {
    char const *cp = config_getstring(table,mode,"squelch-close",NULL);
    if(cp)
      demod->squelch_close = dB2power(strtof(cp,NULL));
  }
  demod->squelchtail = config_getint(table,mode,"squelchtail",demod->squelchtail);
  {
    char const *cp = config_getstring(table,mode,"headroom",NULL);
    if(cp)
      demod->output.headroom = dB2voltage(-fabsf(strtof(cp,NULL))); // always treat as <= 0 dB
  }
  demod->tune.shift = config_getfloat(table,mode,"shift",demod->tune.shift);
  {
    char const *cp = config_getstring(table,mode,"recovery-rate",NULL);
    if(cp){
      // dB/sec -> voltage ratio/block
      float x = strtof(cp,NULL);
      demod->linear.recovery_rate = dB2voltage(fabsf(x) * .001f * Blocktime);
    }
  }
  {
    // time in seconds -> time in blocks
    char const *cp = config_getstring(table,mode,"hang-time",NULL);
    if(cp){
      float x = strtof(cp,NULL);
      demod->linear.hangtime = fabsf(x) / (.001 * Blocktime); // Always >= 0
    }
  }
  {
    char const *cp = config_getstring(table,mode,"threshold",NULL);
    if(cp){
      float x = strtof(cp,NULL);
      demod->linear.threshold = dB2voltage(-fabsf(x)); // Always <= unity
    }
  }
  {
    char const *cp = config_getstring(table,mode,"gain",NULL);
    if(cp){
      float x = strtof(cp,NULL);
      demod->output.gain = dB2voltage(x); // Can be more or less than unity
    }
  }
  demod->linear.env = config_getboolean(table,mode,"envelope",demod->linear.env);
  demod->linear.pll = config_getboolean(table,mode,"pll",demod->linear.pll);
  demod->linear.square = config_getboolean(table,mode,"square",demod->linear.square);  // On implies PLL on
  if(demod->linear.square)
    demod->linear.pll = 1; // Square implies PLL
  
  demod->filter.isb = config_getboolean(table,mode,"conj",demod->filter.isb);       // (unimplemented anyway)
  demod->linear.loop_bw = config_getfloat(table,mode,"pll-bw",demod->linear.loop_bw);
  demod->linear.agc = config_getboolean(table,mode,"agc",demod->linear.agc);
  {
    char const *cp = config_getstring(table,mode,"deemph-tc",NULL);
    if(cp){
      float tc = strtof(cp,NULL);
      demod->deemph.rate = expf(-1.0 / (tc * 1e-6 * demod->output.samprate));
    }
  }
  {
    char const *cp = config_getstring(table,mode,"deemph-gain",NULL);
    if(cp){
      float g = strtof(cp,NULL);
      demod->deemph.gain = dB2voltage(g);
    }
  }
  return 0;
}

