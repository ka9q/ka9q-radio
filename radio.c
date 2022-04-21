// $Id: radio.c,v 1.214 2022/04/21 08:11:30 karn Exp $
// Core of 'radio' program - control LOs, set frequency/mode, etc
// Copyright 2018, Phil Karn, KA9Q
#define _GNU_SOURCE 1
#include <assert.h>
#include <stdlib.h>
#include <limits.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#if defined(linux)
#include <bsd/string.h>
#endif
#include <math.h>
#include <complex.h>
#include <fftw3.h>
#undef I
#include <netinet/in.h>

// For SAP/SDP
#include <sys/time.h>
#include <sys/types.h>
#include <pwd.h>
#include <uuid/uuid.h>

#include "misc.h"
#include "osc.h"
#include "radio.h"
#include "filter.h"
#include "status.h"

float Blocktime;
struct frontend Frontend;

pthread_mutex_t Demod_mutex;
int const Demod_alloc_quantum = 1000;
struct demod *Demod_list; // Contiguous array
int Demod_list_length; // Length of array
int Active_demod_count; // Active demods


static float const SCALE12 = 1/2048.;
static float const SCALE16 = 1./SHRT_MAX; // Scale signed 16-bit int to float in range -1, +1
static float const SCALE8 = 1./INT8_MAX;  // Scale signed 8-bit int to float in range -1, +1

struct demod *alloc_demod(void){
  pthread_mutex_lock(&Demod_mutex);
  if(Demod_list == NULL){
    Demod_list = (struct demod *)calloc(Demod_alloc_quantum,sizeof(struct demod));
    Demod_list_length = Demod_alloc_quantum;
    Active_demod_count = 0;
  }
  struct demod *demod = NULL;
  for(int i=0; i < Demod_list_length; i++){
    if(!Demod_list[i].inuse){
      demod = &Demod_list[i];
      break;
    }
  }
  if(demod == NULL){
    fprintf(stdout,"Warning: out of demod table space (%d)\n",Active_demod_count);
  } else {
    memset(demod,0,sizeof(struct demod));
    demod->inuse = 1;
    Active_demod_count++;
  }
  pthread_mutex_unlock(&Demod_mutex);
  return demod;
}

// takes pointer to pointer to demod so we can zero it out to avoid use of freed pointer
void free_demod(struct demod **demod){
  if(demod != NULL && *demod != NULL){
    pthread_mutex_lock(&Demod_mutex);
    if((*demod)->inuse){
      (*demod)->inuse = 0;
      Active_demod_count--;
    }
    pthread_mutex_unlock(&Demod_mutex);  
    *demod = NULL;
  }
}

#if 0 // Turned off in favor of per-demod estimation
void *estimate_n0(void *arg){
  pthread_setname("estn0");

  int init = 0;
  struct filter_in * const master = Frontend.in;
  unsigned int blocknum = 0;

  // bins is points/2 +1 in case of real input, so min/max IF must both be neg or positive
  // bins is points in complex input
  float avg_pwrs[master->bins];
  memset(avg_pwrs,0,sizeof(avg_pwrs));

  assert(Frontend.sdr.samprate != 0); // main should have waited until it isn't
  int first_bin = master->bins * Frontend.sdr.min_IF / Frontend.sdr.samprate;
  int last_bin = master->bins * Frontend.sdr.max_IF / Frontend.sdr.samprate;
  int bincnt = last_bin - first_bin;
  if(bincnt < 0)
    bincnt += master->bins;

  if(first_bin < 0)
    first_bin += master->bins;

  assert(first_bin >= 0);
  assert(bincnt >=0 && bincnt <= master->bins);

  while(1){
    // Wait for new block of frequency domain data from front end
    pthread_mutex_lock(&master->filter_mutex);
    while(blocknum == master->blocknum)
      pthread_cond_wait(&master->filter_cond,&master->filter_mutex);
    pthread_mutex_unlock(&master->filter_mutex);

#if 0
    if(master->bins > 10000 && (blocknum & 7) != 0)
      continue; // HACK!! Do only 8th block to cut CPU consumption to roughly 1 FM demod on airspy
#endif
    
    // Update average bin powers
    float min_bin_power = INFINITY;
    complex float * const fdomain = master->fdomain[blocknum % ND];
    if(init){
      int bin = first_bin;
      for(int i=0; i < bincnt; i++){
	avg_pwrs[i] += (cnrmf(fdomain[bin]) - avg_pwrs[i]) * .02; // tune or auto adjust this?
	min_bin_power = min(min_bin_power,avg_pwrs[i]);
	if(++bin == master->bins)
	  bin = 0;
      }
    } else {
      // Doesn't really help since the main problem is after a retuning
      // and there can be a time skew between the frequency change and changing data
      int bin = first_bin;
      for(int i=0; i < bincnt; i++){
	avg_pwrs[i] = cnrmf(fdomain[bin]);
	min_bin_power = min(min_bin_power,avg_pwrs[i]);
	if(++bin == master->bins)
	  bin = 0;
      }
      init = 1;
    }
    // Not sure of the math here. Doubling N0 when the front end is real seems to give the right result;
    // it was 3dB low without it, probably because there are only half as many bins as in complex
    // Also adjust for overlap
    Frontend.n0 = (Frontend.sdr.isreal ? 2 : 1) * ((float)Frontend.in->ilen / (Frontend.in->ilen + Frontend.in->impulse_length - 1))
      * 2 * min_bin_power / ((float)Frontend.in->bins * Frontend.sdr.samprate);
#if 0
    if((blocknum & 0xff) == 0)
      fprintf(stdout,"min_IF %.1f max_IF %.1f bins %d first_bin %d last_bin %d Frontend.n0 = %g (%.2f dB)\n",
	      Frontend.sdr.min_IF,Frontend.sdr.max_IF,
	      master->bins,first_bin,last_bin,
	      Frontend.n0,power2dB(Frontend.n0));
#endif
    blocknum++;
  }
}
#endif

// experimental
// estimate n0 by finding the FFT bin with the least energy
// in the demod's pre-filter nyquist bandwidth
// Works better than global estimation when noise floor is not flat
float estimate_noise(struct demod *demod,int rotate){
  struct filter_out const * const slave = demod->filter.out;
  if(demod->filter.energies == NULL)
    demod->filter.energies = calloc(sizeof(float),slave->bins);

  float * const energies = demod->filter.energies;
  struct filter_in const * const master = slave->master;
  // slave->blocknum already incremented by execute_filter_output
  complex float const * const fdomain = master->fdomain[(slave->blocknum - 1) % ND];
  int mbin = rotate - slave->bins/2;
  float min_bin_energy = INFINITY;
  if(master->in_type == REAL){
    // Only half as many bins as with complex input
    for(int i=0; i < slave->bins; i++,mbin++){
      int n = abs(mbin); // Doesn't really handle the mirror well
      if(n < master->bins){
	energies[i] += (cnrmf(fdomain[n]) - energies[i]) * 0.02; // blocknum was already incremented
	if(min_bin_energy > energies[i])
	  min_bin_energy = energies[i];
      } else
	break;  // off the end
      mbin++;
    }
  } else {
    // Complex input that often straddles DC
    if(mbin < 0)
      mbin += master->bins; // starting in negative frequencies
    for(int i=0; i < slave->bins; i++,mbin++){	
      if(mbin >= 0 && mbin < master->bins){
	energies[i] += (cnrmf(fdomain[mbin]) - energies[i]) * 0.02; // blocknum was already incremented
	if(min_bin_energy > energies[i])
	  min_bin_energy = energies[i];
      }
      mbin++;
      if(mbin == master->bins)
	mbin = 0; // wrap around from neg freq to pos freq
      if(mbin == master->bins/2)
	break; // fallen off the right edge
    }
  }
  // Don't double-count the energy in the overlap
  return ((float)master->ilen / (master->ilen + master->impulse_length - 1))
      * 2 * min_bin_energy / ((float)master->bins * Frontend.sdr.samprate);
}


// thread for first half of demodulator
// Preprocessing of samples performed for all demodulators
// Pass to input of pre-demodulation filter
// Update power measurement
void *proc_samples(void *arg){
  pthread_setname("procsamp");

  while(1){
    // Packet consists of Ethernet, IP and UDP header (already stripped)
    // then standard Real Time Protocol (RTP), a status header and the PCM
    // I/Q data. RTP is an IETF standard, so it uses big endian numbers
    // The status header and I/Q data (now obsolete) are *not* standard, so we save time
    // by using machine byte order (almost certainly little endian).
    // Note this is a portability problem if this system and the one generating
    // the data have opposite byte orders. But who's big endian anymore?
    // Receive I/Q data from front end
    // Incoming RTP packets

    struct packet pkt;

    socklen_t socksize = sizeof(Frontend.input.data_source_address);
    int size = recvfrom(Frontend.input.data_fd,pkt.content,sizeof(pkt.content),0,(struct sockaddr *)&Frontend.input.data_source_address,&socksize);
    if(size <= 0){    // ??
      perror("recvfrom");
      usleep(50000);
      continue;
    }
    if(size < RTP_MIN_SIZE)
      continue; // Too small for RTP, ignore

    uint8_t const * restrict dp = ntoh_rtp(&pkt.rtp,pkt.content);
    size -= (dp - pkt.content);
    
    if(pkt.rtp.pad){
      // Remove padding
      size -= dp[size-1];
      pkt.rtp.pad = 0;
    }
    if(size <= 0)
      continue; // Bogus RTP header?

    int sc = 0;
    switch(pkt.rtp.type){
    case IQ_FLOAT:
      sc = size / (sizeof(complex float));
      break;
    case AIRSPY_PACKED:
      sc = 2 * size / (3 * sizeof(int8_t));
      break;
    case PCM_MONO_PT: // 16-bit real
      sc = size / sizeof(int16_t);
      break;
    case REAL_PT12: // 12-bit real
      sc = 2 * size / (3 * sizeof(int8_t));
      break;
    case REAL_PT8:  // 8-bit real
      sc = size / sizeof(int8_t);
      break;
    case IQ_PT8: // 8-bit ints no metadata
      sc = size / (2 * sizeof(int8_t));
      break;
    case PCM_STEREO_PT: // Big-endian 16 bits, no metadata header
      sc = size / (2 * sizeof(int16_t));
      break;
    case IQ_PT12:       // Big endian packed 12 bits, no metadata
      sc = size / (3 * sizeof(int8_t));
      break;
    }
    int const sampcount = sc; // gets used a lot, flag it const
    if(pkt.rtp.ssrc != Frontend.input.rtp.ssrc){
      // SSRC changed; reset sample count.
      // rtp_process will reset packet count
      Frontend.input.samples = 0;
    }
    int const time_step = rtp_process(&Frontend.input.rtp,&pkt.rtp,sampcount);
    if(time_step < 0 || time_step > 192000){ // NOTE HARDWIRED SAMPRATE
      // Old samples, or too big a jump; drop. Shouldn't happen if sequence number isn't old
      continue;
    } else if(time_step > 0){
      // Samples were lost. Inject enough zeroes to keep the sample count and LO phase correct
      // Arbitrary 1 sec limit just to keep things from blowing up
      // Good enough for the occasional lost packet or two
      // Note: we don't use marker bits since we don't suppress silence
      Frontend.input.samples += time_step;
      if(Frontend.in->input.r != NULL){
	for(int i=0;i < time_step; i++)
	  write_rfilter(Frontend.in,0);
      } else if(Frontend.in->input.c != NULL){
	for(int i=0;i < time_step; i++)
	  write_cfilter(Frontend.in,0);
      }
    }
    // Convert and scale samples to internal float-32 format
    Frontend.input.samples += sampcount;

    switch(pkt.rtp.type){
    case IQ_FLOAT: // E.g., AirspyHF+
      if(Frontend.in->input.c != NULL){
	float const inv_gain = 1.0 / Frontend.sdr.gain;
	float f_energy = 0; // energy accumulator
	complex float const *up = (complex float *)dp;
	for(int i=0; i < sampcount; i++){
	  complex float s = *up++;
	  f_energy += cnrmf(s);
	  write_cfilter(Frontend.in,s*inv_gain); // undo front end analog gain
	}
	Frontend.sdr.output_level = f_energy / sampcount; // average A/D level, not including analog gain
      }
      break;
    case AIRSPY_PACKED:
      if(Frontend.in->input.r != NULL){    // Ensure the data is the right type for the filter to avoid segfaults
	// idiosyncratic packed format from Airspy-R2
	// Some tricky optimizations here.
	// Input samples are 12 bits encoded in excess-2048, which makes them
	// unsigned. 'up' is also unsigned to avoid unwanted sign extension on right shift
	// Probably assumes little-endian byte order
	float const inv_gain = SCALE12 / Frontend.sdr.gain;
	uint64_t in_energy = 0; // Accumulate as integer for efficiency
	uint32_t const *up = (uint32_t *)dp;
	for(int i=0; i<sampcount; i+= 8){ // assumes multiple of 8
	  int s[8];
	  s[0] =  *up >> 20;
	  s[1] =  *up >> 8;
	  s[2] =  *up++ << 4;
	  s[2] |= *up >> 28;
	  s[3] =  *up >> 16;
	  s[4] =  *up >> 4;
	  s[5] =  *up++ << 8;
	  s[5] |= *up >> 24;
	  s[6] =  *up >> 12;
	  s[7] =  *up++;
	  for(int j=0; j < 8; j++){
	    int const x = (s[j] & 0xfff) - 2048; // not actually necessary for s[0]
	    in_energy += x * x;
	    write_rfilter(Frontend.in,x*inv_gain);
	  }
	}
	Frontend.sdr.output_level = 2 * in_energy * SCALE12 * SCALE12 / sampcount;
      }
      break;
    case REAL_PT12: // 12-bit packed integer real
      if(Frontend.in->input.r != NULL){    // Ensure the data is the right type for the filter to avoid segfaults
	uint64_t in_energy = 0; // A/D energy accumulator for integer formats only
	float const inv_gain = SCALE12 / Frontend.sdr.gain;
	for(int i=0; i<sampcount; i+=2){
	  int16_t const s0 = ((dp[0] << 8) | dp[1]) & 0xfff0;
	  int16_t const s1 = ((dp[1] << 8) | dp[2]) << 4;
	  in_energy += s0 * s0;
	  in_energy += s1 * s1;
	  write_rfilter(Frontend.in,s0*inv_gain);
	  write_rfilter(Frontend.in,s1*inv_gain);
	  dp += 3;
	}
	Frontend.sdr.output_level = 2 * in_energy * SCALE12 * SCALE12 / sampcount;
      }
      break;
    case PCM_MONO_PT: // 16 bits big-endian integer real
      if(Frontend.in->input.r != NULL){
	uint64_t in_energy = 0; // A/D energy accumulator for integer formats only	
	float const inv_gain = SCALE16 / Frontend.sdr.gain;
	uint16_t const *sp = (uint16_t *)dp;
	for(int i=0; i<sampcount; i++){
	  // ntohs() returns UNSIGNED so the cast is necessary!
	  int const s = (int16_t)ntohs(*sp++);
	  in_energy += s * s;
	  write_rfilter(Frontend.in,s * inv_gain);
	}
	Frontend.sdr.output_level = 2 * in_energy * SCALE16 * SCALE16 / sampcount;
      }
      break;
    case REAL_PT8: // 8 bit integer real
      if(Frontend.in->input.r != NULL){
	float const inv_gain = SCALE8 / Frontend.sdr.gain;
	uint64_t in_energy = 0; // A/D energy accumulator for integer formats only		
	for(int i=0; i<sampcount; i++){
	  int16_t const s = (int8_t)*dp++;
	  in_energy += s * s;
	  write_rfilter(Frontend.in,s * inv_gain);
	}
	Frontend.sdr.output_level = 2 * in_energy * SCALE8 * SCALE8 / sampcount;
      }
      break;
    default: // shuts up lint
    case IQ_PT12:      // two 12-bit signed integers (one complex sample) packed big-endian into 3 bytes
      if(Frontend.in->input.c != NULL){
	uint64_t in_energy = 0; // A/D energy accumulator for integer formats only	
	float const inv_gain = SCALE12 / Frontend.sdr.gain;
	for(int i=0; i<sampcount; i++){
	  int16_t const rs = ((dp[0] << 8) | dp[1]) & 0xfff0;
	  int16_t const is = ((dp[1] << 8) | dp[2]) << 4;
	  in_energy += rs * rs + is * is;
	  complex float samp;
	  __real__ samp = rs;
	  __imag__ samp = is;
	  write_cfilter(Frontend.in,samp * inv_gain);
	  dp += 3;
	}
	Frontend.sdr.output_level = in_energy * SCALE12 * SCALE12 / sampcount;
      }
      break;
    case PCM_STEREO_PT:      // Two 16-bit signed integers, **BIG ENDIAN** (network order)
      if(Frontend.in->input.c != NULL){
	uint64_t in_energy = 0; // A/D energy accumulator for integer formats only		
	float const inv_gain = SCALE16 / Frontend.sdr.gain;
	int16_t const *sp = (int16_t *)dp;
	for(int i=0; i<sampcount; i++){
	  // ntohs() returns UNSIGNED
	  int const rs = (int16_t)ntohs(*sp++);
	  int const is = (int16_t)ntohs(*sp++);
	  in_energy += rs * rs + is * is;
	  complex float samp;
	  __real__ samp = rs;
	  __imag__ samp = is;
	  write_cfilter(Frontend.in,samp * inv_gain);
	}
	Frontend.sdr.output_level = in_energy * SCALE16 * SCALE16 / sampcount;
      }
      break;
    case IQ_PT8:      // Two signed 8-bit integers
      if(Frontend.in->input.c != NULL){
	uint64_t in_energy = 0; // A/D energy accumulator for integer formats only	
	float const inv_gain = SCALE8 / Frontend.sdr.gain;
	for(int i=0; i<sampcount; i++){
	  int16_t const rs = (int8_t)*dp++;
	  int16_t const is = (int8_t)*dp++;
	  in_energy += rs * rs + is * is;
	  complex float samp;
	  __real__ samp = rs;
	  __imag__ samp = is;
	  write_cfilter(Frontend.in,samp * inv_gain);
	}
	Frontend.sdr.output_level = in_energy * SCALE8 * SCALE8 / sampcount;
      }
      break;
    }

    
  } // end of main loop
}

// start demodulator thread on already-initialized demod structure
int start_demod(struct demod * demod){
  if(demod == NULL)
    return -1;

  // Stop previous demodulator, if any
  if(demod->demod_thread != (pthread_t)0){
#if 1
    demod->terminate = 1;
    pthread_join(demod->demod_thread,NULL);
    demod->terminate = 0;
#else
    pthread_cancel(demod->demod_thread);
    pthread_join(demod->demod_thread,NULL);
#endif    
  }

  // Start demodulators; only one actually runs at a time
  switch(demod->demod_type){
  case WFM_DEMOD:
    pthread_create(&demod->demod_thread,NULL,demod_wfm,demod);
    break;
  case FM_DEMOD:
    pthread_create(&demod->demod_thread,NULL,demod_fm,demod);
    break;
  case LINEAR_DEMOD:
    pthread_create(&demod->demod_thread,NULL,demod_linear,demod);
    break;
  }
  return 0;
}

int kill_demod(struct demod **p){
  if(p == NULL)
    return -1;
  struct demod *demod = *p;
  if(demod == NULL)
    return -1;

#if 1
  demod->terminate = 1;
#else
  if(demod->demod_thread != (pthread_t)0)
    pthread_cancel(demod->demod_thread);
#endif
  pthread_join(demod->demod_thread,NULL);
  if(demod->filter.out)
    delete_filter_output(&demod->filter.out);
  if(demod->rtcp_thread != (pthread_t)0){
    pthread_cancel(demod->rtcp_thread);
    pthread_join(demod->rtcp_thread,NULL);
  }
  if(demod->sap_thread != (pthread_t)0){
    pthread_cancel(demod->sap_thread);
    pthread_join(demod->sap_thread,NULL);
  }
    
#if 0
  // Don't close these as they're often shared across demods
  // Really should keep a reference count so they can be closed when
  // the last demod using them closes
  if(demod->output.rtcp_fd > 2)
    close(demod->output.rtcp_fd);
  if(demod->output.data_fd > 2)
    close(demod->output.data_fd);
  if(demod->output.sap_fd > 2)
    close(demod->output.sap_fd);
#endif
  if(demod->filter.energies)
    free(demod->filter.energies);
  free_demod(p);
  return 0;
}


// Set receiver frequency
double set_freq(struct demod * const demod,double const f){
  assert(demod != NULL);
  if(demod == NULL)
    return NAN;

  assert(!isnan(f));
  demod->tune.freq = f;

  // Tuning to 0 Hz is a special case, don't move front end
  // Essentially disables a demod
  if(f == 0)
    return f;

  // Determine new IF
  double new_if = f - Frontend.sdr.frequency;

  // Flip sign to convert LO2 frequency to IF carrier frequency
  // Tune an extra kHz to account for front end roundoff
  // Ideally the front end would just round in a preferred direction
  // but it doesn't know where our IF will be so it can't make the right choice
  double const fudge = 1000;
  if(new_if > Frontend.sdr.max_IF - demod->filter.max_IF){
    // Retune LO1 as little as possible
    new_if = Frontend.sdr.max_IF - demod->filter.max_IF - fudge;
  } else if(new_if < Frontend.sdr.min_IF - demod->filter.min_IF){
    // Also retune LO1 as little as possible
    new_if = Frontend.sdr.min_IF - demod->filter.min_IF + fudge;
  } else
    return f; // OK where it is

  double const new_lo1 = f - new_if;
  // the front end will send its actual new frequency in its status stream,
  // the front end status decoder will pick it up, and the demods will recalculate their new LOs
  set_first_LO(demod,new_lo1);
  return f;
}

// Set first (front end tuner) oscillator
// Note: single precision floating point is not accurate enough at VHF and above
// demod->first_LO is NOT updated here!
// It is set by incoming status frames so this will take time
double set_first_LO(struct demod const * const demod,double const first_LO){
  assert(demod != NULL);
  if(demod == NULL)
    return NAN;

  double const current_lo1 = Frontend.sdr.frequency;

  // Just return actual frequency without changing anything
  if(first_LO == current_lo1 || first_LO <= 0)
    return first_LO;

  uint8_t packet[8192],*bp;
  memset(packet,0,sizeof(packet));
  bp = packet;
  *bp++ = 1; // Command
  Frontend.sdr.command_tag = random();
  encode_int32(&bp,COMMAND_TAG,Frontend.sdr.command_tag);
  encode_double(&bp,RADIO_FREQUENCY,first_LO);
  encode_eol(&bp);
  int len = bp - packet;
  send(Frontend.input.ctl_fd,packet,len,0);
  return first_LO;
}  

// Compute FFT bin shift and time-domain fine tuning offset for specified LO frequency
// N = input fft length
// M = input buffer overlap
// samprate = input sample rate
// flip = invert (or not) every baseband sample
// remainder = fine LO frequency (double)
// freq = frequency to mix by (double)
int compute_tuning(int N, int M, int samprate,int *flip,int *rotate,double *remainder, double freq){
  double const hzperbin = (double)samprate / N;
  int const quantum = N / (M - 1);       // rotate by multiples of this number of bins due to overlap-save
                                         // check for non-zero remainder and warn?
  int const r = quantum * round(freq/(hzperbin * quantum));
  if(rotate)
    *rotate = r;

  if(remainder)
    *remainder = freq - (r * hzperbin);

  if(flip)
    *flip = (r % (2*quantum)) ? -1 : +1; // Flip fine osc phase on every other shift (not sure why, but it works)

  // Check if there's no overlap in the range we want
  // Intentionally allow real input to go both ways, for front ends with high and low side injection
  // Even though only one works, this lets us manually check for images
  // No point in tuning to aliases, though
  if(abs(r) > N/2)
    return -1; // Demod thread will wait for the front end status to change
  return 0;
}
/* Session announcement protocol - highly experimental, off by default
   The whole point was to make it easy to use VLC and similar tools, but they either don't actually implement SAP (e.g. in iOS)
   or implement some vague subset that you have to guess how to use
   Will probably work better with Opus streams from the opus transcoder, since they're always 48000 Hz stereo; no switching midstream
*/
void *sap_send(void *p){
  struct demod *demod = (struct demod *)p;
  assert(demod != NULL);

  long long start_time;
  {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME,&ts);
    start_time = ts.tv_sec + NTP_EPOCH;
  }
  // These should change when a change is made elsewhere
  uint16_t const id = random(); // Should be a hash, but it changes every time anyway
  int const sess_version = 1;

  for(;;){
    char message[1500],*wp;
    int space = sizeof(message);
    wp = message;
    
    *wp++ = 0x20; // SAP version 1, ipv4 address, announce, not encrypted, not compressed
    *wp++ = 0; // No authentication
    *wp++ = id >> 8;
    *wp++ = id & 0xff;
    space -= 4;
    
    // our sending ipv4 address
    struct sockaddr_in const *sin = (struct sockaddr_in *)&demod->output.data_source_address;
    uint32_t *src = (uint32_t *)wp;
    *src = sin->sin_addr.s_addr; // network byte order
    wp += 4;
    space -= 4;
    
    int len = snprintf(wp,space,"application/sdp");
    wp += len + 1; // allow space for the trailing null
    space -= (len + 1);
    
    // End of SAP header, beginning of SDP
    
    // Version v=0 (always)
    len = snprintf(wp,space,"v=0\r\n");
    wp += len;
    space -= len;
    
    {
      // Originator o=
      char hostname[128];
      gethostname(hostname,sizeof(hostname));
      
      struct passwd pwd,*result = NULL;
      char buf[1024];
      
      getpwuid_r(getuid(),&pwd,buf,sizeof(buf),&result);
      len = snprintf(wp,space,"o=%s %lld %d IN IP4 %s\r\n",
		     result ? result->pw_name : "-",
		     start_time,sess_version,hostname);
      
      wp += len;
      space -= len;
    }
    
    // s= (session name)
    len = snprintf(wp,space,"s=radio %s\r\n",Frontend.sdr.description);
    wp += len;
    space -= len;
    
    // i= (human-readable session information)
    len = snprintf(wp,space,"i=PCM output stream from ka9q-radio on %s\r\n",Frontend.sdr.description);
    wp += len;
    space -= len;
    
    {
      char mcast[128];
      strlcpy(mcast,formatsock(&demod->output.data_dest_address),sizeof(mcast));
      // Remove :port field, confuses the vlc listener
      char *cp = strchr(mcast,':');
      if(cp)
	*cp = '\0';
      len = snprintf(wp,space,"c=IN IP4 %s/%d\r\n",mcast,Mcast_ttl);
      wp += len;
      space -= len;
    }  
    

#if 0 // not currently used
    long long current_time;
    {
      struct timespec ts;
      clock_gettime(CLOCK_REALTIME,&ts);
      current_time = ts.tv_sec + NTP_EPOCH;
    }
#endif

    // t= (time description)
    len = snprintf(wp,space,"t=%lld %lld\r\n",start_time,0LL); // unbounded
    wp += len;
    space -= len;
    
    // m = media description
#if 1  
    {
      // Demod type can change, but not the sample rate
      int mono_type = pt_from_info(demod->output.samprate,1);
      int stereo_type = pt_from_info(demod->output.samprate,2);
      int fm_type = pt_from_info(demod->output.samprate,1);
      
      len = snprintf(wp,space,"m=audio 5004/1 RTP/AVP %d %d %d\r\n",mono_type,stereo_type,fm_type);
      wp += len;
      space -= len;
      
      len = snprintf(wp,space,"a=rtpmap:%d L16/%d/%d\r\n",mono_type,demod->output.samprate,1);
      wp += len;
      space -= len;
      
      len = snprintf(wp,space,"a=rtpmap:%d L16/%d/%d\r\n",stereo_type,demod->output.samprate,2);
      wp += len;
      space -= len;
      
      len = snprintf(wp,space,"a=rtpmap:%d L16/%d/%d\r\n",fm_type,demod->output.samprate,1);
      wp += len;
      space -= len;
    }
#else
    {
      // set from current state. This will require changing the session version and IDs, and
      // it's not clear that clients like VLC will do the right thing anyway
      int type = pt_from_info(demod->output.samprate,demod->output.channels,demod->demod_type);

      len = snprintf(wp,space,"m=audio 5004/1 RTP/AVP %d\r\n",type);
      wp += len;
      space -= len;
      
      len = snprintf(wp,space,"a=rtpmap:%d L16/%d/%d\r\n",type,demod->output.samprate,demod->output.channels);
      wp += len;
      space -= len;
    }
#endif    
    send(demod->output.sap_fd,message,wp - message,0);
    sleep(5);
  }
}

void *demod_reaper(void *arg){
  while(1){
    for(int i=0;i<Demod_list_length;i++){
      struct demod *demod = &Demod_list[i];
      if(demod->inuse && demod->tune.freq == 0 && demod->lifetime > 0){
	demod->lifetime--;
	if(demod->lifetime == 0){
	  kill_demod(&demod);
	}
      }

    }
    sleep(1);
  }
  return NULL;
}
