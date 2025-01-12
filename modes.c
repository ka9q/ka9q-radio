// Load and search ka9q-radio preset definition table in modes.conf
// Copyright 2018-2023, Phil Karn, KA9Q

#define _GNU_SOURCE 1  // Avoid warnings when including dsp.h
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
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

struct demodtab Demodtab[] = {
      {LINEAR_DEMOD, "Linear"}, // Coherent demodulation of AM, DSB, BPSK; calibration on WWV/WWVH/CHU carrier
      {FM_DEMOD,     "FM",   }, // NBFM and noncoherent PM
      {WFM_DEMOD,    "WFM",  }, // NBFM and noncoherent PM
      {SPECT_DEMOD,  "Spectrum", }, // Spectrum analysis
};

static enum demod_type DEFAULT_DEMOD = LINEAR_DEMOD;
static int   const DEFAULT_LINEAR_SAMPRATE = 12000;
static float const DEFAULT_KAISER_BETA = 11.0;   // reasonable tradeoff between skirt sharpness and sidelobe height
static float const DEFAULT_LOW = -5000.0;        // Ballpark numbers, should be properly set for each mode
static float const DEFAULT_HIGH = 5000.0;
static float const DEFAULT_HEADROOM = -15.0;     // keep gaussian signals from clipping
static float const DEFAULT_SQUELCH_OPEN = 8.0;   // open when SNR > 8 dB
static float const DEFAULT_SQUELCH_CLOSE = 7.0;  // close when SNR < 7 dB
static float const DEFAULT_RECOVERY_RATE = 20.0; // 20 dB/s gain increase
static float const DEFAULT_THRESHOLD = -15.0;    // Don't let noise rise above -15 relative to headroom
static float const DEFAULT_GAIN = 50.0;         // Unused in FM, usually adjusted automatically in linear
static float const DEFAULT_HANGTIME = 1.1;       // keep low gain 1.1 sec before increasing
static float const DEFAULT_PLL_BW = 10.0;       // Reasonable for AM
static int   const DEFAULT_SQUELCH_TAIL = 1;     // close on frame *after* going below threshold, may let partial frame noise through
static int   const DEFAULT_UPDATE = 25;         // 2 Hz for a 20 ms frame time
#if 0
static int   const DEFAULT_FM_SAMPRATE = 24000;
static float const DEFAULT_NBFM_TC = 530.5;      // Time constant for NBFM emphasis (300 Hz corner)
static float const DEFAULT_WFM_TC = 75.0;        // Time constant for FM broadcast (North America/Korea standard)
static float const DEFAULT_FM_DEEMPH_GAIN = 12.0; // +12 dB to give subjectively equal loudness with deemphsis
static float const DEFAULT_WFM_DEEMPH_GAIN = 0.0;
#endif
static int   const DEFAULT_BITRATE = 0;       // Default Opus compressed bit rate. 0 means OPUS_AUTO, the encoder decides
extern int Overlap;

// Valid keys in presets file, [global] section, and any channel section
char const *Channel_keys[] = {
  "disable",
  "data",
  "demod",
  "mode",
  "preset",

  "samprate",
  "mono",
  "stereo",
  "low",
  "high",
  "squelch-open",
  "squelch-close",
  "squelchtail",
  "squelch-tail",
  "headroom",
  "shift",
  "recovery-rate",
  "hang-time",
  "threshold",
  "gain",
  "envelope",
  "pll",
  "square",
  "conj",
  "pll-bw",
  "agc",
  "extend",
  "threshold-extend",
  "deemph-tc",
  "deemph-gain",
  "tone",
  "pl",
  "ctcss",
  "pacing",
  "encoding",
  "bitrate",
  "update",
  "buffer",
  "freq",
  "freq0",
  "freq1",
  "freq2",
  "freq3",
  "freq4",
  "freq5",
  "freq6",
  "freq7",
  "freq8",
  "freq9",
  NULL
};

int demod_type_from_name(char const *name){
  for(enum demod_type n = 0; n < N_DEMOD; n++){
    if(strncasecmp(name,Demodtab[n].name,sizeof(Demodtab[n].name)) == 0)
      return Demodtab[n].type;
  }
  return -1;
}


char const *demod_name_from_type(enum demod_type type){
  if(type >= 0 && type < N_DEMOD)
    return Demodtab[type].name;
  return NULL;
}

// Set reasonable defaults before reading preset or config tables
int set_defaults(struct channel *chan){
  if(chan == NULL)
    return -1;

  chan->tp1 = chan->tp2 = NAN;
  chan->tune.doppler = 0;
  chan->tune.doppler_rate = 0;
  // De-emphasis defaults to off, enabled only in FM modes
  chan->fm.rate = 0;
  chan->fm.gain = 1.0;

  chan->demod_type = DEFAULT_DEMOD;
  chan->filter.kaiser_beta = DEFAULT_KAISER_BETA;
  chan->filter.min_IF = DEFAULT_LOW;
  chan->filter.max_IF = DEFAULT_HIGH;
  // ************ temp for testing
  chan->filter2.low = DEFAULT_LOW;
  chan->filter2.high = DEFAULT_HIGH;
  chan->filter2.kaiser_beta = DEFAULT_KAISER_BETA;

  chan->filter.remainder = NAN;      // Important to force downconvert() to call set_osc() on first call
  chan->filter.bin_shift = -1000999; // Force initialization here too
  chan->fm.squelch_open = dB2power(DEFAULT_SQUELCH_OPEN);
  chan->fm.squelch_close = dB2power(DEFAULT_SQUELCH_CLOSE);
  chan->fm.squelch_tail = DEFAULT_SQUELCH_TAIL;
  chan->output.headroom = dB2voltage(DEFAULT_HEADROOM);
  chan->output.channels = 1;
  chan->tune.shift = 0.0;
  chan->linear.recovery_rate = dB2voltage(DEFAULT_RECOVERY_RATE * .001f * Blocktime);
  chan->linear.hangtime = DEFAULT_HANGTIME / (.001f * Blocktime);
  chan->linear.threshold = dB2voltage(DEFAULT_THRESHOLD);
  if(chan->output.gain <= 0 || isnan(chan->output.gain))
     chan->output.gain = dB2voltage(DEFAULT_GAIN); // Set only if out of bounds
  chan->linear.env = false;
  chan->pll.enable = false;
  chan->pll.square = false;
  chan->filter2.isb = false;
  chan->pll.loop_bw = DEFAULT_PLL_BW;
  chan->linear.agc = true;
  chan->output.samprate = round_samprate(DEFAULT_LINEAR_SAMPRATE); // Don't trust even a compile constant
  chan->output.encoding = S16BE;
  chan->output.opus_bitrate = DEFAULT_BITRATE;
  double r = remainder(Blocktime * chan->output.samprate * .001,1.0);
  if(r != 0){
    fprintf(stdout,"Warning: non-integral samples in %.3f ms block at sample rate %d Hz: remainder %g\n",
	    Blocktime,chan->output.samprate,r);
  }
  chan->output.pacing = false;
  chan->status.output_interval = DEFAULT_UPDATE;
  chan->output.silent = true; // Prevent burst of FM status messages on output channel at startup
  chan->output.minpacket = 0;  // No output buffering
  return 0;
}

// Set selected section of specified config file into current chan structure
// Caller must (re) initialize pre-demod filter and (re)start demodulator thread
int loadpreset(struct channel *chan,dictionary const *table,char const *sname){
  if(chan == NULL || table == NULL || sname == NULL || strlen(sname) == 0)
    return -1;

  char const * demod_name = config_getstring(table,sname,"demod",NULL);
  if(demod_name){
    int const x = demod_type_from_name(demod_name);
    if(chan->demod_type >= 0)
      chan->demod_type = x;
  }
  {
    char const *p = config_getstring(table,sname,"samprate",NULL);
    if(p != NULL){
      int s = parse_frequency(p,false);
      chan->output.samprate = round_samprate(s);
    }
  }
  // This test can't fail since round_samprate() forces it to a minimium of the blockrate; not sure what is ideal here
  if(chan->output.samprate == 0)
    chan->output.samprate = round_samprate(DEFAULT_LINEAR_SAMPRATE); // Make sure it gets set to *something*, even if wrong (e.g. for FM)
  chan->output.channels = config_getint(table,sname,"channels",chan->output.channels);
  if(config_getboolean(table,sname,"mono",false))
    chan->output.channels = 1;
  if(config_getboolean(table,sname,"stereo",false))
    chan->output.channels = 2;
  chan->filter.kaiser_beta = config_getfloat(table,sname,"kaiser-beta",chan->filter.kaiser_beta);

  // Pre-detection filter limits
  {
    char const *low = config_getstring(table,sname,"low",NULL);
    if(low != NULL)
      chan->filter.min_IF = parse_frequency(low,false);

    char const *high = config_getstring(table,sname,"high",NULL);
    if(high != NULL)
      chan->filter.max_IF = parse_frequency(high,false);
  }
  if(chan->filter.min_IF > chan->filter.max_IF){
    // Ensure max >= min
    float t = chan->filter.min_IF;
    chan->filter.min_IF = chan->filter.max_IF;
    chan->filter.max_IF = t;
  }
  {
    char const *cp = config_getstring(table,sname,"squelch-open",NULL);
    if(cp)
      chan->fm.squelch_open = dB2power(strtof(cp,NULL));
  }
  {
    char const *cp = config_getstring(table,sname,"squelch-close",NULL);
    if(cp)
      chan->fm.squelch_close = dB2power(strtof(cp,NULL));
  }
  chan->fm.squelch_tail = config_getint(table,sname,"squelchtail",chan->fm.squelch_tail); // historical
  chan->fm.squelch_tail = config_getint(table,sname,"squelch-tail",chan->fm.squelch_tail);
  {
    char const *cp = config_getstring(table,sname,"headroom",NULL);
    if(cp)
      chan->output.headroom = dB2voltage(-fabsf(strtof(cp,NULL))); // always treat as <= 0 dB
  }
  {
    char const *p = config_getstring(table,sname,"shift",NULL);
    if(p != NULL)
      chan->tune.shift = parse_frequency(p,false);
  }
  {
    char const *cp = config_getstring(table,sname,"recovery-rate",NULL);
    if(cp){
      // dB/sec -> voltage ratio/block
      float x = strtof(cp,NULL);
      chan->linear.recovery_rate = dB2voltage(fabsf(x) * .001f * Blocktime);
    }
  }
  {
    // time in seconds -> time in blocks
    char const *cp = config_getstring(table,sname,"hang-time",NULL);
    if(cp){
      float x = strtof(cp,NULL);
      chan->linear.hangtime = fabsf(x) / (.001 * Blocktime); // Always >= 0
    }
  }
  {
    char const *cp = config_getstring(table,sname,"threshold",NULL);
    if(cp){
      float x = strtof(cp,NULL);
      chan->linear.threshold = dB2voltage(-fabsf(x)); // Always <= unity
    }
  }
  {
    char const *cp = config_getstring(table,sname,"gain",NULL);
    if(cp){
      float x = strtof(cp,NULL);
      chan->output.gain = dB2voltage(x); // Can be more or less than unity
    }
  }
  chan->linear.env = config_getboolean(table,sname,"envelope",chan->linear.env);
  chan->pll.enable = config_getboolean(table,sname,"pll",chan->pll.enable);
  chan->pll.square = config_getboolean(table,sname,"square",chan->pll.square);  // On implies PLL on
  if(chan->pll.square)
    chan->pll.enable = true; // Square implies PLL

  chan->filter2.isb = config_getboolean(table,sname,"conj",chan->filter2.isb);
  chan->pll.loop_bw = config_getfloat(table,sname,"pll-bw",chan->pll.loop_bw);
  chan->linear.agc = config_getboolean(table,sname,"agc",chan->linear.agc);
  chan->fm.threshold = config_getboolean(table,sname,"extend",chan->fm.threshold); // FM threshold extension
  chan->fm.threshold = config_getboolean(table,sname,"threshold-extend",chan->fm.threshold); // FM threshold extension
  {
    char const *cp = config_getstring(table,sname,"deemph-tc",NULL);
    if(cp){
      float const tc = strtof(cp,NULL) * 1e-6;
      chan->fm.rate = -expm1f(-1.0f / (tc * chan->output.samprate));
    }
  }
  {
    char const *cp = config_getstring(table,sname,"deemph-gain",NULL);
    if(cp){
      float const g = strtof(cp,NULL);
      chan->fm.gain = dB2voltage(g);
    }
  }
  // "tone", "pl" and "ctcss" are synonyms
  chan->fm.tone_freq = config_getfloat(table,sname,"tone",chan->fm.tone_freq);
  chan->fm.tone_freq = config_getfloat(table,sname,"pl",chan->fm.tone_freq);
  chan->fm.tone_freq = config_getfloat(table,sname,"ctcss",chan->fm.tone_freq);
  chan->fm.tone_freq = fabsf(chan->fm.tone_freq);
  if(chan->fm.tone_freq > 3000){
    fprintf(stdout,"Tone %.1f out of range\n",chan->fm.tone_freq);
    chan->fm.tone_freq = 0;
  }
  chan->output.pacing = config_getboolean(table,sname,"pacing",chan->output.pacing);
  {
    char const *cp = config_getstring(table,sname,"encoding",NULL);
    if(cp)
      chan->output.encoding = parse_encoding(cp);
  }
  chan->output.opus_bitrate = config_getint(table,sname,"bitrate",chan->output.opus_bitrate);
  chan->status.output_interval = config_getint(table,sname,"update",chan->status.output_interval);
  chan->output.minpacket = config_getint(table,sname,"buffer",chan->output.minpacket);

  return 0;
}

// force an output sample rate to a multiple of the FFT block rate times the number of
// new blocks in each FFT interval.
// For the default block time of 20 ms and overlap of 1/5, this is (1/20 ms)*(5-1) = 50 Hz*4 = 200 Hz
// Should we limit the sample rate? In principle it could be greater than the input sample rate,
// and the filter should just interpolate. But there should be practical limits

// Should sample rates be integers when the block rate could in principle not be?
// Usually Blocktime = 20.0000 ms (50.00000 Hz), which avoids the problem
unsigned int round_samprate(unsigned int x){
  float const baserate = (1000. / Blocktime) * (Overlap - 1);

  if(x < baserate)
    return roundf(baserate); // Output one iFFT bin minimum, i.e., blockrate

  return baserate * roundf(x / baserate); // Nearest multiple of block rate * (Overlap - 1)
}
