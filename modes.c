// $Id: modes.c,v 1.52 2022/04/14 10:50:43 karn Exp karn $
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

#include "modes.h"
#include "misc.h"
#include "radio.h"
#include "config.h"


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

extern char const *Libdir;
static dictionary *Mdict;

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




// Load mode table entry presets
int preset_mode(struct demod * const demod,char const * const mode){
  assert(demod != NULL);
  if(demod == NULL)
    return -1;

  if(Mdict == NULL){
    Mdict = iniparser_load(Modefile); // Kept open for duration of program
    if(Mdict == NULL){
      fprintf(stdout,"Can't load mode file %s\n",Modefile);
      return -1;
    }
  }

  char const * const name = config_getstring(Mdict,mode,"demod",NULL);
  if(name == NULL){
    fprintf(stdout,"Demodulator name missing from section %s\n",mode);
    return -1;
  }
  {
    int i;
    for(i = 0; i < Ndemod; i++){
      char const *tname = demod_name_from_type(i);
      if(tname != NULL && strncasecmp(tname,name,sizeof(Demodtab[i])) == 0){
	demod->demod_type = Demodtab[i].type;
	break;
      }
    }
    if(i >= Ndemod){
      fprintf(stderr,"Demodulator '%s' unknown in section %s\n",name,mode);
      return -1;
    }
  }

  demod->filter.kaiser_beta = config_getfloat(Mdict,mode,"kaiser-beta",DEFAULT_KAISER_BETA);

  // Defaults really shouldn't be used
  float const low = config_getfloat(Mdict,mode,"low",DEFAULT_LOW);
  float const high = config_getfloat(Mdict,mode,"high",DEFAULT_HIGH);

  if(high < low){ // Ensure high > low
    demod->filter.min_IF = high;
    demod->filter.max_IF = low;
  } else {
    demod->filter.min_IF = low;
    demod->filter.max_IF = high;
  }
  float squelch_open = config_getfloat(Mdict,mode,"squelch-open",DEFAULT_SQUELCH_OPEN);
  assert(!isnan(squelch_open));
  float squelch_close = config_getfloat(Mdict,mode,"squelch-close",DEFAULT_SQUELCH_CLOSE);
  assert(!isnan(squelch_close));
  if(squelch_open < squelch_close)
    fprintf(stdout,"[%s]: warning: squelch_open (%.1f) < squelch_close (%.1f)\n",mode,squelch_open,squelch_close);

  demod->squelch_open = dB2power(squelch_open);
  demod->squelch_close = dB2power(squelch_close);
  demod->squelchtail = abs(config_getint(Mdict,mode,"squelchtail",DEFAULT_SQUELCHTAIL));

  demod->tune.doppler = 0;
  demod->tune.doppler_rate = 0;
  float headroom = config_getfloat(Mdict,mode,"headroom",DEFAULT_HEADROOM);
  assert(!isnan(headroom));
  demod->output.headroom = dB2voltage(-fabsf(headroom)); // always treat as <= 0 dB

  // De-emphasis defaults to off, enabled only in FM modes
  demod->deemph.rate = 0;
  demod->deemph.gain = 1.0;
  
  switch(demod->demod_type){
  case LINEAR_DEMOD:
    demod->output.channels = config_getint(Mdict,mode,"channels",1); // Default mono, i.e., not IQ
    if(config_getboolean(Mdict,mode,"stereo",0))
      demod->output.channels = 2;
    if(config_getboolean(Mdict,mode,"mono",0))
      demod->output.channels = 1;
    if(demod->output.channels != 2 && demod->output.channels != 1)
      demod->output.channels = 1;
    // Parameters not used in FM
    demod->tune.shift = config_getfloat(Mdict,mode,"shift",0.0);
    {
      // dB/sec -> voltage ratio/block
      float x = config_getfloat(Mdict,mode,"recovery-rate",DEFAULT_RECOVERY_RATE);
      assert(!isnan(x));
      demod->linear.recovery_rate = dB2voltage(fabsf(x) * .001 * Blocktime);
    }
    {
      // time in seconds -> time in blocks
      float x = config_getfloat(Mdict,mode,"hang-time",DEFAULT_HANGTIME);
      assert(!isnan(x));
      demod->linear.hangtime = fabsf(x) / (.001 * Blocktime); // Always >= 0
    }
    {
      float x = config_getfloat(Mdict,mode,"threshold",DEFAULT_THRESHOLD);
      assert(!isnan(x));
      demod->linear.threshold = dB2voltage(-fabsf(x)); // Always <= unity
    }
    {
      float x = config_getfloat(Mdict,mode,"gain",DEFAULT_GAIN);
      assert(!isnan(x));
      demod->output.gain = dB2voltage(x); // Can be more or less than unity
    }
    demod->linear.env = config_getboolean(Mdict,mode,"envelope",0);   // default off 
    demod->linear.pll = config_getboolean(Mdict,mode,"pll",0);        // default off. On also enables squelch!
    demod->linear.square = config_getboolean(Mdict,mode,"square",0);  // default off. On implies PLL on
    if(demod->linear.square)
      demod->linear.pll = 1;
    demod->filter.isb = config_getboolean(Mdict,mode,"conj",0);       // default off (unimplemented anyway)
    {
      demod->linear.loop_bw = DEFAULT_PLL_BW;
      float x = config_getfloat(Mdict,mode,"pll-bw",demod->linear.loop_bw);
      assert(!isnan(x));
      if(x > 0)
	demod->linear.loop_bw = x; // never 0 or negative
    }
    demod->linear.agc = config_getboolean(Mdict,mode,"agc",1);        // default ON
    break;
  case FM_DEMOD:
    demod->output.channels = 1;                 // always mono
    demod->output.gain = 1; // Gets overridden anyway?
    {
      // Default to 530.5 microseconds (1/300 Hz). Set deemph = 0 for flat FM
      float tc = config_getfloat(Mdict,mode,"deemph-tc",530.5);
      if(tc != 0.0){
	demod->deemph.rate = expf(-1.0 / (tc * 1e-6 * demod->output.samprate));
	demod->deemph.gain = config_getfloat(Mdict,mode,"deemph-gain",4.0); // empirical value, needs work
      }
    }
    break;
  case WFM_DEMOD:
    demod->output.channels = 2;      // always stereo
    demod->output.gain = 1;
    demod->output.samprate = 384000; // downconverter samprate forced for FM stereo decoding. Output also forced to 48 kHz
    {
      // Default 75 microseconds for north american FM broadcasting
      float tc = config_getfloat(Mdict,mode,"deemph-tc",75.0);
      if(tc != 0){
	demod->deemph.rate = expf(-1.0 / (tc * 1e-6 * 48000)); // hardwired output sample rate -- needs cleanup
	demod->deemph.gain = config_getfloat(Mdict,mode,"deemph-gain",4.0);  // empirical value, needs work
      }
    }
    break;
  }
  return 0;
}      
