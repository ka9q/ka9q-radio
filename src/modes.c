// Load and search ka9q-radio preset definition table in presets.conf
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
      {SPECT_DEMOD, "Spectrum", }, // Spectrum analysis
};

static int const DEFAULT_TTL = 0;                // Don't blast cheap switches and access points unless the user says so
static enum demod_type DEFAULT_DEMOD = LINEAR_DEMOD;
static int   const DEFAULT_LINEAR_SAMPRATE = 12000;
static double const DEFAULT_KAISER_BETA = 11.0;   // reasonable tradeoff between skirt sharpness and sidelobe height
static double const DEFAULT_LOW = -5000.0;        // Ballpark numbers, should be properly set for each mode
static double const DEFAULT_HIGH = 5000.0;
static double const DEFAULT_HEADROOM = -15.0;     // keep gaussian signals from clipping
static double const DEFAULT_SQUELCH_OPEN = 8.0;   // open when SNR > 8 dB
static double const DEFAULT_SQUELCH_CLOSE = 7.0;  // close when SNR < 7 dB
static bool const  DEFAULT_SNR_SQUELCH = false;  // enables squelch when true, so don't enable except in modes that use squelch
static double const DEFAULT_RECOVERY_RATE = 20.0; // 20 dB/s gain increase
static double const DEFAULT_THRESHOLD = -15.0;    // Don't let noise rise above -15 relative to headroom
static double const DEFAULT_GAIN = 50.0;         // Unused in FM, usually adjusted automatically in linear
static double const DEFAULT_HANGTIME = 1.1;       // keep low gain 1.1 sec before increasing
static double const DEFAULT_PLL_BW = 10.0;       // Reasonable for AM
static int   const DEFAULT_SQUELCH_TAIL = 1;     // close on frame *after* going below threshold, may let partial frame noise through
static int   const DEFAULT_UPDATE = 25;         // 2 Hz for a 20 ms frame time
#if 0
static int   const DEFAULT_FM_SAMPRATE = 24000;
static double const DEFAULT_NBFM_TC = 530.5;      // Time constant for NBFM emphasis (300 Hz corner)
static double const DEFAULT_WFM_TC = 75.0;        // Time constant for FM broadcast (North America/Korea standard)
static double const DEFAULT_FM_DEEMPH_GAIN = 12.0; // +12 dB to give subjectively equal loudness with deemphsis
static double const DEFAULT_WFM_DEEMPH_GAIN = 0.0;
#endif
static int   const DEFAULT_BITRATE = 0;       // Default Opus compressed bit rate. 0 means OPUS_AUTO, the encoder decides
static int   const DEFAULT_DC_TC = 0;         // Time constant for AM carrier removal, default off
static double const DEFAULT_CROSSOVER = 200;   // About where the two spectral analysis algorithms use equal CPU
static double const DEFAULT_SPECTRUM_KAISER_BETA = 7.0; // Default for spectral analysis window
static enum window_type const DEFAULT_WINDOW_TYPE = KAISER_WINDOW;
extern int Overlap;

// Valid keys in presets file, [global] section, and any channel section
char const *Channel_keys[] = {
  "dns",
  "disable",
  "data",
  "dc-cut",
  "demod",
  "beam",
  "A-amp",
  "A-phase",
  "B-amp",
  "B-phase",
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
  "tone0",
  "tone1",
  "tone2",
  "tone3",
  "tone4",
  "tone5",
  "tone6",
  "tone7",
  "tone8",
  "tone9",
  "pl", // do these too (sigh)
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
  "raster",
  "raster0",
  "raster1",
  "raster2",
  "raster3",
  "raster4",
  "raster5",
  "raster6",
  "raster7",
  "raster8",
  "raster9",
  "except",
  "except0",
  "except1",
  "except2",
  "except3",
  "except4",
  "except5",
  "except6",
  "except7",
  "except8",
  "except9",
  "ttl",
  "snr-squelch",
  "filter2",
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
// Note frontend entry must be set in radio.c since Frontend global is static
int set_defaults(struct channel *chan){
  if(chan == NULL)
    return -1;

  chan->inuse = true;
  chan->demod_type = DEFAULT_DEMOD;
  chan->prio = default_prio();

  chan->output.samprate = round_samprate(DEFAULT_LINEAR_SAMPRATE); // Don't trust even a compile constant
  chan->output.encoding = S16BE;
  chan->output.opus_bitrate = DEFAULT_BITRATE;
  chan->output.headroom = dB2voltage(DEFAULT_HEADROOM);
  chan->output.channels = 1;
  if(chan->output.gain <= 0 || isnan(chan->output.gain))
     chan->output.gain = dB2voltage(DEFAULT_GAIN); // Set only if out of bounds
  chan->output.pacing = false;
  chan->output.silent = true; // Prevent burst of FM status messages on output channel at startup
  chan->output.minpacket = 0;  // No output buffering
  chan->output.ttl = DEFAULT_TTL;

  chan->tune.doppler = 0;
  chan->tune.doppler_rate = 0;
  chan->tune.shift = 0.0;

  chan->filter.kaiser_beta = DEFAULT_KAISER_BETA;
  chan->filter.min_IF = DEFAULT_LOW;
  chan->filter.max_IF = DEFAULT_HIGH;
  chan->filter.remainder = NAN;      // Important to force downconvert() to call set_osc() on first call
  chan->filter.bin_shift = -1000999; // Force initialization here too

  chan->filter2.blocking = 0;        // Off by default
  chan->filter2.low = DEFAULT_LOW;
  chan->filter2.high = DEFAULT_HIGH;
  chan->filter2.kaiser_beta = DEFAULT_KAISER_BETA;
  chan->filter2.isb = false;

  chan->squelch_open = dB2power(DEFAULT_SQUELCH_OPEN);
  chan->squelch_close = dB2power(DEFAULT_SQUELCH_CLOSE);
  chan->squelch_tail = DEFAULT_SQUELCH_TAIL;
  chan->snr_squelch_enable = DEFAULT_SNR_SQUELCH;
  // De-emphasis defaults to off, enabled only in FM modes
  chan->fm.rate = 0;
  chan->fm.gain = 1.0;

  chan->linear.recovery_rate = dB2voltage(DEFAULT_RECOVERY_RATE);
  chan->linear.hangtime = DEFAULT_HANGTIME;
  chan->linear.threshold = dB2voltage(DEFAULT_THRESHOLD);
  chan->linear.env = false;
  chan->linear.agc = true;
  chan->pll.enable = false;
  chan->pll.square = false;
  chan->pll.loop_bw = DEFAULT_PLL_BW;
  chan->linear.dc_tau = DEFAULT_DC_TC;

  double r = remainder(Blocktime * chan->output.samprate,1.0);
  if(r != 0){
    fprintf(stderr,"Warning: non-integral samples in %.3f ms block at sample rate %d Hz: remainder %g\n",
	    Blocktime,chan->output.samprate,r);
    assert(false);
  }
  chan->status.output_interval = DEFAULT_UPDATE;
  chan->spectrum.fft_avg = 0; // will get set by client or by default in spectrum.c
  chan->spectrum.window_type = DEFAULT_WINDOW_TYPE;
  chan->spectrum.crossover = DEFAULT_CROSSOVER;
  chan->spectrum.shape = DEFAULT_SPECTRUM_KAISER_BETA;
  chan->spectrum.window = NULL;
  chan->spectrum.plan = NULL;
  chan->spectrum.bin_data = NULL;
  chan->tp1 = chan->tp2 = NAN;
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
      int s = (int)round(parse_frequency(p,false));
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
  chan->filter.kaiser_beta = config_getdouble(table,sname,"kaiser-beta",chan->filter.kaiser_beta);

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
    double t = chan->filter.min_IF;
    chan->filter.min_IF = chan->filter.max_IF;
    chan->filter.max_IF = t;
  }
  {
    char const *cp = config_getstring(table,sname,"squelch-open",NULL);
    if(cp)
      chan->squelch_open = dB2power(strtod(cp,NULL));
  }
  {
    char const *cp = config_getstring(table,sname,"squelch-close",NULL);
    if(cp)
      chan->squelch_close = dB2power(strtod(cp,NULL));
  }
  chan->squelch_tail = config_getint(table,sname,"squelchtail",chan->squelch_tail); // historical
  chan->squelch_tail = config_getint(table,sname,"squelch-tail",chan->squelch_tail);
  {
    char const *cp = config_getstring(table,sname,"headroom",NULL);
    if(cp)
      chan->output.headroom = dB2voltage(-fabs(strtod(cp,NULL))); // always treat as <= 0 dB
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
      double x = strtod(cp,NULL);
      chan->linear.recovery_rate = dB2voltage(fabs(x));
    }
  }
  {
    // time in seconds -> time in blocks
    char const *cp = config_getstring(table,sname,"hang-time",NULL);
    if(cp){
      double x = strtod(cp,NULL);
      chan->linear.hangtime = fabs(x);
    }
  }
  {
    char const *cp = config_getstring(table,sname,"threshold",NULL);
    if(cp){
      double x = strtod(cp,NULL);
      chan->linear.threshold = dB2voltage(-fabs(x)); // Always <= unity
    }
  }
  {
    char const *cp = config_getstring(table,sname,"gain",NULL);
    if(cp){
      double x = strtod(cp,NULL);
      chan->output.gain = dB2voltage(x); // Can be more or less than unity
    }
  }
  chan->linear.env = config_getboolean(table,sname,"envelope",chan->linear.env);
  chan->pll.enable = config_getboolean(table,sname,"pll",chan->pll.enable);
  chan->pll.square = config_getboolean(table,sname,"square",chan->pll.square);  // On implies PLL on
  if(chan->pll.square)
    chan->pll.enable = true; // Square implies PLL

  chan->filter2.isb = config_getboolean(table,sname,"conj",chan->filter2.isb);
  chan->pll.loop_bw = config_getdouble(table,sname,"pll-bw",chan->pll.loop_bw);
  chan->linear.agc = config_getboolean(table,sname,"agc",chan->linear.agc);
  chan->fm.threshold = config_getboolean(table,sname,"extend",chan->fm.threshold); // FM threshold extension
  chan->fm.threshold = config_getboolean(table,sname,"threshold-extend",chan->fm.threshold); // FM threshold extension
  chan->snr_squelch_enable = config_getboolean(table,sname,"snr-squelch",chan->snr_squelch_enable);
  double cutoff = config_getdouble(table,sname,"dc-cut",-987);
  if(cutoff != -987)
    chan->linear.dc_tau = -expm1(-2.0 * M_PI * cutoff/(chan->output.samprate));

  {
    char const *cp = config_getstring(table,sname,"deemph-tc",NULL);
    if(cp){
      double const tc = strtod(cp,NULL) * 1e-6;
      unsigned int samprate = (chan->demod_type == WFM_DEMOD) ? 48000 : chan->output.samprate;
      chan->fm.rate = -expm1(-1.0 / (tc * samprate));
    }
  }
  {
    char const *cp = config_getstring(table,sname,"deemph-gain",NULL);
    if(cp){
      double const g = strtod(cp,NULL);
      chan->fm.gain = dB2voltage(g);
    }
  }
  // "tone", "pl" and "ctcss" are synonyms
  chan->fm.tone_freq = config_getdouble(table,sname,"tone",chan->fm.tone_freq);
  chan->fm.tone_freq = config_getdouble(table,sname,"pl",chan->fm.tone_freq);
  chan->fm.tone_freq = config_getdouble(table,sname,"ctcss",chan->fm.tone_freq);
  chan->fm.tone_freq = fabs(chan->fm.tone_freq);
  if(chan->fm.tone_freq > 3000){
    fprintf(stderr,"Tone %.1f out of range\n",chan->fm.tone_freq);
    chan->fm.tone_freq = 0;
  }
  chan->output.pacing = config_getboolean(table,sname,"pacing",chan->output.pacing);
  {
    char const *cp = config_getstring(table,sname,"encoding",NULL);
    if(cp)
      chan->output.encoding = parse_encoding(cp);
  }
  chan->output.opus_bitrate = config_getint(table,sname,"bitrate",chan->output.opus_bitrate);
  if(chan->output.opus_bitrate > 510000){
    fprintf(stderr,"opus bitrate %u out of range, using 0 (auto)\n",chan->output.opus_bitrate);
    chan->output.opus_bitrate = 0;
  }
  chan->status.output_interval = config_getint(table,sname,"update",chan->status.output_interval);
  if(chan->status.output_interval < 0)
    chan->status.output_interval = 0;
  chan->output.minpacket = config_getint(table,sname,"buffer",chan->output.minpacket);
  if(chan->output.minpacket > 4){
    fprintf(stderr,"buffer %u out of range, using 0\n",chan->output.minpacket);
    chan->output.minpacket = 0;
  }
  chan->filter2.blocking = config_getint(table,sname,"filter2",chan->filter2.blocking);
  if(chan->filter2.blocking > 10){
    fprintf(stderr,"filter2 blocking %u out of range, using 10\n",chan->filter2.blocking);
    chan->filter2.blocking = 10;
  }
  chan->prio = config_getint(table,sname,"prio",chan->prio);
  chan->output.ttl = config_getint(table,sname,"ttl",chan->output.ttl);

  chan->filter.beam = config_getboolean(table,sname,"beam",false);
  if(chan->filter.beam){
    double a_amp = config_getdouble(table,sname,"a-amp",1.0);
    double a_phase = config_getdouble(table,sname,"a-phase",0.0);
    double b_amp = config_getdouble(table,sname,"b-amp",0.0);
    double b_phase = config_getdouble(table,sname,"b-phase",0.0);
    chan->filter.a_weight = a_amp * csincospi(a_phase / 180.);
    chan->filter.b_weight = b_amp * csincospi(b_phase / 180.);
  }
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
  // For reasons yet not understood, only even multiples of 200 Hz seem to work
  double const baserate = (1. / Blocktime) * (Overlap - 1);

  if(x < baserate)
    return (int)round(baserate); // Output one (two) iFFT bin minimum, i.e., blockrate (*2)

  return (int)(baserate * round(x / baserate)); // Nearest multiple of (2*) block rate * (Overlap - 1)
}
