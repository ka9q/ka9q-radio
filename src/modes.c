// Load and search ka9q-radio preset definition table in presets.conf
// Copyright 2018-2023, Phil Karn, KA9Q

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
#include "window.h"
#include "sched.h"

struct demodtab Demodtab[] = {
      {LINEAR_DEMOD,   "linear"}, // Coherent demodulation of AM, DSB, BPSK; calibration on WWV/WWVH/CHU carrier
      {FM_DEMOD,       "fm",   }, // NBFM and noncoherent PM
      {WFM_DEMOD,      "wfm",  }, // NBFM and noncoherent PM
      {SPECT_DEMOD,    "spectrum", }, // Spectrum analysis
      {SPECT2_DEMOD,   "spectrum2", },
      {IDLE_DEMOD,     "idle", },
};

static int    const DEFAULT_TTL = 0;                // Don't blast cheap switches and access points unless the user says so
static enum demod_type const DEFAULT_DEMOD = IDLE_DEMOD;
static int    const DEFAULT_LINEAR_SAMPRATE = 12000;
static double const DEFAULT_LIFETIME = 0; // Infinite

// Channel filter
static double const DEFAULT_KAISER_BETA = 11.0;   // reasonable tradeoff between skirt sharpness and sidelobe height
static double const DEFAULT_LOW = -5000.0;        // Ballpark numbers, should be properly set for each mode
static double const DEFAULT_HIGH = 5000.0;

// Squelch
static double const DEFAULT_SQUELCH_OPEN = 8.0;   // open when SNR > 8 dB
static double const DEFAULT_SQUELCH_CLOSE = 7.0;  // close when SNR < 7 dB
static bool   const DEFAULT_SNR_SQUELCH = false;  // enables squelch when true, so don't enable except in modes that use squelch

// per-channel AGC
static double const DEFAULT_HEADROOM = -15.0;     // keep gaussian signals from clipping
static double const DEFAULT_RECOVERY_RATE = 20.0; // 20 dB/s gain increase
static double const DEFAULT_THRESHOLD = -15.0;    // Don't let noise rise above -15 relative to headroom
static double const DEFAULT_GAIN = 50.0;         // Unused in FM, usually adjusted automatically in linear
static double const DEFAULT_HANGTIME = 1.1;       // keep low gain 1.1 sec before increasing

// PLL
static double const DEFAULT_PLL_BW = 10.0;       // Reasonable for AM
static int    const DEFAULT_SQUELCH_TAIL = 1;     // close on frame *after* going below threshold, may let partial frame noise through
static int    const DEFAULT_UPDATE = 25;         // 2 Hz for a 20 ms frame time

// FM de-emphasis
static int    const DEFAULT_NBFM_SAMPRATE = 24000;
static double const DEFAULT_NBFM_TC = 530.5e-6;      // Time constant for NBFM emphasis (300 Hz corner)
static double const DEFAULT_NBFM_DEEMPH_GAIN = 12.0; // +12 dB to give subjectively equal loudness with deemphsis

// For FM broadcasting North and South America and South Korea use 75 μs; the rest of the world uses 50 μs
static int    const DEFAULT_WFM_SAMPRATE = 48000;
static double const DEFAULT_WFM_TC = 75.0e-6;        // Time constant for FM broadcast (America/Korea standard)
static double const DEFAULT_WFM_DEEMPH_GAIN = 0.0;

static double const DEFAULT_DC_CUT = 0;         // high pass cutoff for AM carrier removal, default 0 (off)
static double const DEFAULT_CROSSOVER = 200;   // About where the two spectral analysis algorithms use equal CPU
static double const DEFAULT_SPECTRUM_KAISER_BETA = 7.0; // Default for spectral analysis window
static enum window_type const DEFAULT_WINDOW_TYPE = KAISER_WINDOW;
static int    const DEFAULT_FFT_AVG = 10;      // number of FFTs averaged per spectrum display
static double const DEFAULT_FFT_OVERLAP = 0;

// Opus encoder defaults
static int  const DEFAULT_OPUS_APPLICATION = OPUS_APPLICATION_AUDIO;
static int  const DEFAULT_OPUS_BITRATE = 0; // automatic
static int  const DEFAULT_OPUS_BANDWIDTH = OPUS_BANDWIDTH_FULLBAND;
static int  const DEFAULT_OPUS_SIGNAL = OPUS_AUTO;
static bool const DEFAULT_OPUS_DTX = false;
static int  const DEFAULT_OPUS_FEC = 0; // disabled

extern int Overlap;

// Valid keys in presets file, [global] section, and any channel section
char const *Channel_keys[] = {
  "a-amp",
  "a-phase",
  "advertise",
  "agc",
  "b-amp",
  "b-phase",
  "beam",
  "bitrate",
  "buffer",
  "channels",
  "conj",
  "ctcss",
  "data",
  "dc-cut",
  "deemph-gain",
  "deemph-tc",
  "demod",
  "disable",
  "dns",
  "encoding",
  "envelope",
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
  "extend",
  "filter2",
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
  "gain",
  "hang-time",
  "headroom",
  "high",
  "kaiser-beta",
  "low",
  "mode",
  "mono",
  "opus-application",
  "opus-bitrate",
  "opus-dtx",
  "opus-fec",
  "opus-signal",
  "pacing",
  "pl", // do these too (sigh)
  "pll-bw",
  "pll",
  "preset",
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
  "recovery-rate",
  "samprate",
  "shift",
  "snr-squelch",
  "square",
  "squelch-close",
  "squelch-open",
  "squelch-tail",
  "squelchtail",
  "stereo",
  "threshold-extend",
  "threshold",
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
  "ttl",
  "update",
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
int set_defaults(chan_t *chan){
  assert(chan != NULL && Blocktime > 0);
  if(chan == NULL || Blocktime == 0)
    return -1;

  chan->frontend = &Frontend;
  chan->advertise = true;
  strlcpy(chan->name, "new chan", sizeof chan->name);
  chan->lifestart = chan->lifetime = DEFAULT_LIFETIME / Blocktime;
  chan->demod_type = DEFAULT_DEMOD;
  chan->prio = default_prio();

  chan->status.output_interval = DEFAULT_UPDATE;

  chan->output.gain = dB2voltage(DEFAULT_GAIN);
  chan->output.headroom = dB2voltage(DEFAULT_HEADROOM);
  chan->output.ttl = DEFAULT_TTL;
  chan->output.pacing = false;
  chan->output.maxdelay = 0;  // No output buffering
  chan->output.queue = NULL;
  chan->output.queue_length = 0;
  chan->output.silent = true; // Prevent burst of FM status messages on output channel at startup
  chan->output.samprate = round_samprate(DEFAULT_LINEAR_SAMPRATE); // Don't trust even a compile constant
  chan->output.encoding = S16BE;
  chan->output.channels = 1;
  {
    double r = remainder(Blocktime * chan->output.samprate,1.0);
    if(r != 0)
      fprintf(stderr,"Warning: non-integral samples in %.3f ms block at sample rate %d Hz: remainder %g\n",
	      Blocktime,chan->output.samprate,r);
  }
  chan->output.rtp.type = pt_from_info(chan->output.samprate,chan->output.channels,chan->output.encoding);

  chan->linear.env = false;
  chan->linear.agc = true;
  chan->linear.recovery_rate = dB2voltage(DEFAULT_RECOVERY_RATE);
  chan->linear.hangtime = DEFAULT_HANGTIME;
  chan->linear.threshold = dB2voltage(DEFAULT_THRESHOLD);
  chan->linear.dc_alpha = DEFAULT_DC_CUT == 0 ? 0.0 : -expm1(-2.0 * M_PI * DEFAULT_DC_CUT/chan->output.samprate);
  assert(isfinite(chan->linear.dc_alpha) && chan->linear.dc_alpha >= 0 && chan->linear.dc_alpha <= 1);

  chan->opus.signal = DEFAULT_OPUS_SIGNAL;
  chan->opus.application = DEFAULT_OPUS_APPLICATION;
  chan->opus.bandwidth = DEFAULT_OPUS_BANDWIDTH;
  chan->opus.bitrate = DEFAULT_OPUS_BITRATE;
  chan->opus.dtx = DEFAULT_OPUS_DTX;
  chan->opus.fec = DEFAULT_OPUS_FEC;

  chan->tune.doppler = 0;
  chan->tune.doppler_rate = 0;
  chan->tune.shift = 0.0;

  // Primary channel filter
  chan->filter.kaiser_beta = DEFAULT_KAISER_BETA;
  chan->filter.min_IF = DEFAULT_LOW;
  chan->filter.max_IF = DEFAULT_HIGH;
  chan->filter.remainder = NAN;      // Important to force downconvert() to call set_osc() on first call
  chan->filter.bin_shift = -1000999; // Force initialization here too

  // Post-detection audio filter
  chan->filter2.blocking = 0;        // Off by default
  chan->filter2.low = DEFAULT_LOW;
  chan->filter2.high = DEFAULT_HIGH;
  chan->filter2.kaiser_beta = DEFAULT_KAISER_BETA;
  chan->filter2.out.isb = false;

  chan->squelch.open = dB2power(DEFAULT_SQUELCH_OPEN);
  chan->squelch.close = dB2power(DEFAULT_SQUELCH_CLOSE);
  chan->squelch.tail = DEFAULT_SQUELCH_TAIL;
  chan->squelch.snr_enable = DEFAULT_SNR_SQUELCH;

  // elements depend on FM type
  switch(chan->demod_type){
  case FM_DEMOD:
    chan->fm.rate = -expm1(-1.0 / (DEFAULT_NBFM_TC * DEFAULT_NBFM_SAMPRATE));
    assert(isfinite(chan->fm.rate) && chan->fm.rate > 0 && chan->fm.rate < 1);
    chan->fm.gain = DEFAULT_NBFM_DEEMPH_GAIN;
    break;
  case WFM_DEMOD:
    chan->fm.rate = -expm1(-1.0 / (DEFAULT_WFM_TC * DEFAULT_WFM_SAMPRATE));
    assert(isfinite(chan->fm.rate) && chan->fm.rate > 0 && chan->fm.rate < 1);
    chan->fm.gain = DEFAULT_WFM_DEEMPH_GAIN;
    break;
  default:
    break;
  }
  chan->pll.enable = false;
  chan->pll.square = false;
  chan->pll.loop_bw = DEFAULT_PLL_BW;

  chan->spectrum.overlap = DEFAULT_FFT_OVERLAP;
  chan->spectrum.fft_avg = DEFAULT_FFT_AVG;
  chan->spectrum.window_type = DEFAULT_WINDOW_TYPE;
  chan->spectrum.crossover = DEFAULT_CROSSOVER;
  chan->spectrum.shape = DEFAULT_SPECTRUM_KAISER_BETA;
  chan->spectrum.window = NULL;
  chan->spectrum.plan = NULL;
  chan->spectrum.bin_data = NULL;
  chan->spectrum.base = -150; // dB == value 0
  chan->spectrum.step = 0.5;  // dB/step

  chan->tp1 = chan->tp2 = NAN;
  return 0;
}
// Set selected section of specified config file into current chan structure
// Caller must (re) initialize pre-demod filter and (re)start demodulator thread
int loadpreset(chan_t *chan,dictionary const *table,char const *sname){
  assert(chan != NULL && table != NULL && sname != NULL && strlen(sname) > 0);
  if(chan == NULL || table == NULL || sname == NULL || strlen(sname) == 0)
    return -1;

  char const * demod_name = config_getstring(table,sname,"demod",NULL);
  if(demod_name){
    int const x = demod_type_from_name(demod_name);
    if(chan->demod_type >= 0)
      chan->demod_type = x;
    snprintf(chan->name, sizeof chan->name, "%s %u", demod_name, chan->output.rtp.ssrc);
  }
  chan->use_dns = config_getboolean(table, sname, "dns", chan->use_dns);
  chan->advertise = config_getboolean(table,sname,"advertise",chan->advertise);
  double lifetime = config_getdouble(table,sname,"lifetime",chan->lifetime * Blocktime);
  chan->lifestart = chan->lifetime = lifetime / Blocktime;
  {
    char const *p = config_getstring(table,sname,"samprate",NULL);
    if(p != NULL){
      int s = labs(lrint(parse_frequency(p,false)));
      if(s > 0)
	chan->output.samprate = round_samprate(s);
    }
  }
  assert(chan->output.samprate > 0); // should have been set at least by default
  chan->output.channels = config_getint(table,sname,"channels",chan->output.channels);
  if(config_getboolean(table,sname,"mono",false))
    chan->output.channels = 1;
  if(config_getboolean(table,sname,"stereo",false))
    chan->output.channels = 2;
  {
    char const *cp = config_getstring(table,sname,"encoding",NULL);
    if(cp){
      int e = parse_encoding(cp);
      if(e != NO_ENCODING)
	chan->output.encoding = e;
      else
	fprintf(stderr,"%s: invalid encoding %s\n",chan->name,cp);
    }
  }
  {
    // We have what we need to assign a RTP payload type
    int pt = pt_from_info(chan->output.samprate,chan->output.channels,chan->output.encoding);
    if(pt == -1)
      fprintf(stderr,"loadpreset(%s): can't allocate payload type for samprate %'d, channels %d, encoding %d\n",
	      sname,chan->output.samprate,chan->output.channels,chan->output.encoding);
    chan->output.rtp.type = pt;
  }
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
      chan->squelch.open = dB2power(strtod(cp,NULL));
  }
  {
    char const *cp = config_getstring(table,sname,"squelch-close",NULL);
    if(cp)
      chan->squelch.close = dB2power(strtod(cp,NULL));
  }
  chan->squelch.tail = config_getint(table,sname,"squelchtail",chan->squelch.tail); // historical
  chan->squelch.tail = config_getint(table,sname,"squelch-tail",chan->squelch.tail);
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

  chan->filter2.out.isb = config_getboolean(table,sname,"conj",chan->filter2.out.isb);
  chan->pll.loop_bw = config_getdouble(table,sname,"pll-bw",chan->pll.loop_bw);
  chan->linear.agc = config_getboolean(table,sname,"agc",chan->linear.agc);
  chan->fm.threshold = config_getboolean(table,sname,"extend",chan->fm.threshold); // FM threshold extension
  chan->fm.threshold = config_getboolean(table,sname,"threshold-extend",chan->fm.threshold); // FM threshold extension
  chan->squelch.snr_enable = config_getboolean(table,sname,"snr-squelch",chan->squelch.snr_enable);
  double cutoff = config_getdouble(table,sname,"dc-cut",-987);
  if(cutoff != -987)
    chan->linear.dc_alpha = -expm1(-2.0 * M_PI * cutoff/chan->output.samprate);
  assert(isfinite(chan->linear.dc_alpha) && chan->linear.dc_alpha >= 0 && chan->linear.dc_alpha <= 1);
  {
    char const *cp = config_getstring(table,sname,"deemph-tc",NULL);
    if(cp){
      double const tc = fabs(strtod(cp,NULL) * 1e-6);
      if(tc == 0)
	chan->fm.rate = 0;
      else {
	double const samprate = (chan->demod_type == WFM_DEMOD) ? FULL_SAMPRATE : chan->output.samprate;
	chan->fm.rate = -expm1(-1.0 / (tc * samprate));
	assert(isfinite(chan->fm.rate) && chan->fm.rate >= 0 && chan->fm.rate <= 1);
      }
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
  {
    double tone = config_getdouble(table,sname,"tone",chan->fm.tone_freq);
    tone = config_getdouble(table,sname,"pl",tone);
    tone = fabs(config_getdouble(table,sname,"ctcss",tone));
    if(tone > 3000)
      fprintf(stderr,"%s: Tone %.1lf out of range\n",chan->name,tone);
    else
      chan->fm.tone_freq = tone;
  }
  chan->output.pacing = config_getboolean(table,sname,"pacing",chan->output.pacing);
  {
    int bitrate = abs(config_getint(table,sname,"bitrate",chan->opus.bitrate));
    bitrate = abs(config_getint(table,sname,"opus-bitrate",bitrate));
    if(bitrate > 510000)
      fprintf(stderr,"%s: opus bitrate %d out of range\n",chan->name,bitrate);
    else
      chan->opus.bitrate = bitrate;
  }
  chan->opus.dtx = config_getboolean(table,sname,"opus-dtx",chan->opus.dtx);
  {
    int const fec = abs(config_getint(table,sname,"opus-fec",chan->opus.fec));
    if(fec > 100)
      fprintf(stderr,"%s: opus FEC %dxx out of range\n",chan->name,fec);
    else
      chan->opus.fec = fec;
  }
  {
    char const *cp = config_getstring(table,sname,"opus-application",NULL);
    if(cp && strlen(cp) > 0){
      for(int i=0; ; i++){
	if(Opus_application[i].str == NULL){
	  fprintf(stderr,"%s: opus application '%s' unknown\n",chan->name,cp);
	  break;
	}
	if(strncmp(cp,Opus_application[i].str,strlen(cp)) == 0){
	  chan->opus.application = Opus_application[i].value;
	  break;
	}
      }
    }
  }
  {
    char const *cp = config_getstring(table,sname,"opus-signal",NULL);
    if(cp && strlen(cp) > 0){
      for(int i=0; ; i++){
	if(Opus_signal[i].str == NULL){
	  fprintf(stderr,"%s: opus signal type '%s' unknown\n",chan->name,cp);
	  break;
	}
	if(strncmp(cp,Opus_signal[i].str,strlen(cp)) == 0){
	  chan->opus.signal = Opus_signal[i].value;
	  break;
	}
      }
    }
  }
  chan->status.output_interval = abs(config_getint(table,sname,"update",chan->status.output_interval));
  {
    int maxdelay = abs(config_getint(table,sname,"buffer",chan->output.maxdelay));
    if(maxdelay > 4)
      fprintf(stderr,"%s: buffer %u out of range, using 0\n",chan->name,maxdelay);
    else
      chan->output.maxdelay = maxdelay;
  }
  {
    int blocking = abs(config_getint(table,sname,"filter2",chan->filter2.blocking));
    if(blocking > 10)
      fprintf(stderr,"%s: filter2 blocking %u out of range\n",chan->name,blocking);
    else
      chan->filter2.blocking = blocking;
  }
  chan->prio = abs(config_getint(table,sname,"prio",chan->prio));
  if(chan->prio >  default_prio()){
    fprintf(stderr,"%s: prio %d too high; max %d\n",chan->name,chan->prio,default_prio());
    chan->prio = default_prio();
  }
  chan->output.ttl = abs(config_getint(table,sname,"ttl",chan->output.ttl));

  chan->filter.beam = config_getboolean(table,sname,"beam",false);
  if(chan->filter.beam){
    double a_amp = config_getdouble(table,sname,"a-amp",1.0);
    double a_phase = config_getdouble(table,sname,"a-phase",0.0);
    double b_amp = config_getdouble(table,sname,"b-amp",0.0);
    double b_phase = config_getdouble(table,sname,"b-phase",0.0);
    chan->filter.a_weight = a_amp * csincospi(a_phase / 180.);
    chan->filter.b_weight = b_amp * csincospi(b_phase / 180.);
  }
  {
    char const *data = config_getstring(table,sname,"data",chan->output.dest_string);
    strlcpy(chan->output.dest_string,data,sizeof chan->output.dest_string);
  }
  if(!chan->use_dns || resolve_mcast(chan->output.dest_string, &chan->output.dest_socket,DEFAULT_RTP_PORT,NULL,0,2) != 0){
    // Not using DNS, or DNS resolution failed: create a IPv4 multicast address from a hash of the name
    struct sockaddr_in *sin = (struct sockaddr_in *)&chan->output.dest_socket;
    uint32_t addr = make_maddr(chan->output.dest_string);
    sin->sin_family = AF_INET;
    sin->sin_addr.s_addr = htonl(addr);
    sin->sin_port = htons(DEFAULT_RTP_PORT);
  }
  // --> Should ensure the channel data stream is distinct from the radiod status port !!
  // Status sent to same data stream group, different port
  memcpy(&chan->status.dest_socket, &chan->output.dest_socket, sizeof chan->status.dest_socket);
  struct sockaddr const *sa = (struct sockaddr *)&chan->status.dest_socket;
  switch(sa->sa_family){
  case AF_INET:
    {
      struct sockaddr_in *sin = (struct sockaddr_in *)&chan->status.dest_socket;
      sin->sin_port = htons(DEFAULT_STAT_PORT);
    }
    break;
  case AF_INET6:
    {
      struct sockaddr_in6 *sin = (struct sockaddr_in6 *)&chan->status.dest_socket;
      sin->sin6_port = htons(DEFAULT_STAT_PORT);
      // should support avahi advertising of IPv6
    }
  default:
    break;
  }
  return 0;
}
// The output sample rate must at least be a multiple of the FFT block rate (usually 50 Hz)
// because there must be an integral number of samples in each frame.

// Moreover since only 4/5 of the samples are output (for an overlap of 1/5), the output sample rate must be
// a multiple of 4 samples per frame, or 200 Hz for a 50 Hz (20 ms) frame rate. Equivalently it must also
// be a multiple of the FFT bin spacing (40 Hz).

// But odd multiples of 200 Hz give odd IFFT sizes, which are hairy - no Nyquist bin, unequal numbers
// of positive and negative frequencies, etc. So we restrict the output sample rate to even multiples of 400 Hz.

// Should we limit the sample rate? In principle it could be greater than the input sample rate,
// and the filter should just interpolate. But there should be practical limits

// Should sample rates be integers when the block rate could in principle not be?
// Usually Blocktime = 20.0000 ms (50.00000 Hz), which avoids the problem
unsigned int round_samprate(unsigned int x){
  double const baserate = (2.0 / Blocktime) * (Overlap - 1);
  return lrint(baserate * round((double)x / baserate)); // Nearest multiple of (2*) block rate * (Overlap - 1)
}
