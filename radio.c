// $Id: radio.c,v 1.227 2022/12/29 05:39:27 karn Exp $
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
#include <errno.h>
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
#include <sys/resource.h>
#include <sys/types.h>
#include <pwd.h>
#include <uuid/uuid.h>

#include "misc.h"
#include "osc.h"
#include "radio.h"
#include "filter.h"
#include "status.h"

extern float Blocktime;
struct frontend Frontend;

pthread_mutex_t Demod_list_mutex = PTHREAD_MUTEX_INITIALIZER;
int const Demod_alloc_quantum = 1000;
struct demod *Demod_list; // Contiguous array
int Demod_list_length; // Length of array
int Active_demod_count; // Active demods

static float estimate_noise(struct demod *demod,int shift);

struct demod *alloc_demod(void){
  pthread_mutex_lock(&Demod_list_mutex);
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
    demod->inuse = true;
    Active_demod_count++;
  }
  pthread_mutex_unlock(&Demod_list_mutex);
  return demod;
}

// takes pointer to pointer to demod so we can zero it out to avoid use of freed pointer
void free_demod(struct demod **demod){
  if(demod != NULL && *demod != NULL){
    pthread_mutex_lock(&Demod_list_mutex);
    if((*demod)->inuse){
      (*demod)->inuse = false;
      Active_demod_count--;
    }
    pthread_mutex_unlock(&Demod_list_mutex);  
    *demod = NULL;
  }
}


// experimental
// estimate n0 by finding the FFT bin with the least energy
// in the demod's pre-filter nyquist bandwidth
// Works better than global estimation when noise floor is not flat
static float estimate_noise(struct demod *demod,int shift){
  struct filter_out const * const slave = demod->filter.out;
  if(demod->filter.energies == NULL)
    demod->filter.energies = calloc(sizeof(float),slave->bins);

  float * const energies = demod->filter.energies;
  struct filter_in const * const master = slave->master;
  // slave->next_jobnum already incremented by execute_filter_output
  complex float const * const fdomain = master->fdomain[(slave->next_jobnum - 1) % ND];
  int mbin = shift - slave->bins/2;
  float min_bin_energy = INFINITY;
  if(master->in_type == REAL){
    // Only half as many bins as with complex input
    for(int i=0; i < slave->bins; i++,mbin++){
      int n = abs(mbin); // Doesn't really handle the mirror well
      if(n < master->bins){
	energies[i] += (cnrmf(fdomain[n]) - energies[i]) * 0.02; // blocknum was already incremented
	if(min_bin_energy > energies[i]){
	  min_bin_energy = energies[i];
	}
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
	if(min_bin_energy > energies[i]){
	  min_bin_energy = energies[i];
	}
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
      * 2 * min_bin_energy / ((float)master->bins * Frontend.samprate);
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
  case SPECT_DEMOD:
    if(demod->tune.freq != 0)
      pthread_create(&demod->demod_thread,NULL,demod_spectrum,demod); // spectrum demod can't change freq, so just don't start it at 0
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
  FREE(demod->filter.energies);
  FREE(demod->spectrum.bin_data);

  free_demod(p);
  return 0;
}


// Set receiver frequency
// The new IF is computed here only to determine if the front end needs retuning
// The second LO frequency is actually set when the new front end frequency is
// received back from the front end metadata
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
  double new_if = f - Frontend.frequency;

  // Flip sign to convert LO2 frequency to IF carrier frequency
  // Tune an extra kHz to account for front end roundoff
  // Ideally the front end would just round in a preferred direction
  // but it doesn't know where our IF will be so it can't make the right choice
  double const fudge = 1000;
  if(new_if > Frontend.max_IF - demod->filter.max_IF){
    // Retune LO1 as little as possible
    new_if = Frontend.max_IF - demod->filter.max_IF - fudge;
  } else if(new_if < Frontend.min_IF - demod->filter.min_IF){
    // Also retune LO1 as little as possible
    new_if = Frontend.min_IF - demod->filter.min_IF + fudge;
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

  double const current_lo1 = Frontend.frequency;

  // Just return actual frequency without changing anything
  if(first_LO == current_lo1 || first_LO <= 0)
    return first_LO;

  // Direct tuning through local module if available
  if(Frontend.tune != NULL){
    return (*Frontend.tune)(&Frontend,first_LO);
  }
  return first_LO;
}  

// Compute FFT bin shift and time-domain fine tuning offset for specified LO frequency
// N = input fft length
// M = input buffer overlap
// samprate = input sample rate
// adjust = complex value to multiply by each sample to correct phasing
// remainder = fine LO frequency (double)
// freq = frequency to mix by (double)
// This version tunes to arbitrary FFT bin rotations and computes the necessary
// block phase correction factor described in equation (12) of
// "Analysis and Design of Efficient and Flexible Fast-Convolution Based Multirate Filter Banks"
// by Renfors, Yli-Kaakinen & Harris, IEEE Trans on Signal Processing, Aug 2014
// We seem to be using opposite sign conventions for 'shift'
int compute_tuning(int N, int M, int samprate,int *shift,double *remainder, double freq){
  double const hzperbin = (double)samprate / N;
#if 0
  // Round to multiples of V (not needed anymore)
  int const V = N / (M-1);
  int const r = V * round((freq/hzperbin) / V);
#else
  int const r = round(freq/hzperbin);
#endif
  if(shift)
    *shift = r;

  if(remainder)
    *remainder = freq - (r * hzperbin);

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

  int64_t start_time = utc_time_sec() + NTP_EPOCH; // NTP uses UTC, not GPS

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
		     (long long)start_time,sess_version,hostname);
      
      wp += len;
      space -= len;
    }
    
    // s= (session name)
    len = snprintf(wp,space,"s=radio %s\r\n",Frontend.description);
    wp += len;
    space -= len;
    
    // i= (human-readable session information)
    len = snprintf(wp,space,"i=PCM output stream from ka9q-radio on %s\r\n",Frontend.description);
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
    int64_t current_time = utc_time_sec() + NTP_EPOCH;
#endif

    // t= (time description)
    len = snprintf(wp,space,"t=%lld %lld\r\n",(long long)start_time,0LL); // unbounded
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

// Walk through demodulator list culling dynamic demodulators that
// have become inactive
void *demod_reaper(void *arg){
  pthread_setname("dreaper");
  while(1){
    int actives = 0;
    for(int i=0;i<Demod_list_length && actives < Active_demod_count;i++){
      struct demod *demod = &Demod_list[i];
      if(demod->inuse){
	actives++;
	if(demod->tune.freq == 0 && demod->lifetime > 0){
	  demod->lifetime--;
	  if(demod->lifetime == 0){
	    kill_demod(&demod); // clears demod->inuse
	    actives--;
	  }
	}
      }
    }
    sleep(1);
  }
  return NULL;
}
// Run digital downconverter, common to all demods
// 1. Block until front end is in range
// 2. compute FFT bin shift & fine tuning remainder
// 3. Set fine tuning oscillator frequency & phase
// 4. Run output half (IFFT) of filter
// 5. Update noise estimate
// 6. Run fine tuning, compute average power

// Baseband samples placed in demod->filter.out->output.c
int downconvert(struct demod *demod){
  // To save CPU time when the front end is completely tuned away from us, block until the front
    // end status changes rather than process zeroes. We must still poll the terminate flag.
    pthread_mutex_lock(&Frontend.status_mutex);
    int shift;
    double remainder;

    while(1){
      if(demod->terminate){
	pthread_mutex_unlock(&Frontend.status_mutex);
	return -1;
      }
      demod->tune.second_LO = Frontend.frequency - demod->tune.freq;
      double const freq = demod->tune.doppler + demod->tune.second_LO; // Total logical oscillator frequency
      if(compute_tuning(Frontend.in->ilen + Frontend.in->impulse_length - 1,
			Frontend.in->impulse_length,
			Frontend.samprate,
			&shift,&remainder,freq) == 0)
	break; // We can get at least part of the spectrum we want

      // No front end coverage of our passband; wait for it to retune
      demod->sig.bb_power = 0;
      demod->sig.bb_energy = 0;
      demod->output.energy = 0;
      struct timespec timeout; // Needed to avoid deadlock if no front end is available
      clock_gettime(CLOCK_REALTIME,&timeout);
      timeout.tv_sec += 1; // 1 sec in the future
      pthread_cond_timedwait(&Frontend.status_cond,&Frontend.status_mutex,&timeout);
    }
    pthread_mutex_unlock(&Frontend.status_mutex);

    // Reasonable parameters?
    assert(isfinite(demod->tune.doppler_rate));
    assert(isfinite(demod->tune.shift));

#if 0
    demod->tp1 = shift;
    demod->tp2 = remainder;
#endif

    complex float * const buffer = demod->filter.out->output.c; // Working output time-domain buffer (if any)
    // set fine tuning frequency & phase. Do before execute_filter blocks (can't remember why)
    if(buffer != NULL){ // No output time-domain buffer in spectrum mode
      // avoid them both being 0 at startup; init demod->filter.remainder as NAN
      if(remainder != demod->filter.remainder){
	set_osc(&demod->fine,remainder/demod->output.samprate,demod->tune.doppler_rate/(demod->output.samprate * demod->output.samprate));
	demod->filter.remainder = remainder;
      }
      // Block phase adjustment (folded into the fine tuning osc) in two parts:
      // (a) phase_adjust is applied on each block when FFT bin shifts aren't divisible by V; otherwise it's unity
      // (b) second term keeps the phase continuous when shift changes; found empirically, dunno yet why it works!
      // Be sure to Initialize demod->filter.bin_shift at startup to something bizarre to force this inequality on first call
      if(shift != demod->filter.bin_shift){
	const int V = 1 + (Frontend.in->ilen / (Frontend.in->impulse_length - 1)); // Overlap factor
	demod->filter.phase_adjust = cispi(-2.0f*(shift % V)/(double)V); // Amount to rotate on each block for shifts not divisible by V
	demod->fine.phasor *= cispi((shift - demod->filter.bin_shift) / (2.0f * (V-1))); // One time adjust for shift change
      }
      demod->fine.phasor *= demod->filter.phase_adjust;
    }
    execute_filter_output(demod->filter.out,-shift); // block until new data frame

    demod->blocks_since_poll++;
    if(buffer != NULL){ // No output time-domain buffer in spectral analysis mode
      const int N = demod->filter.out->olen; // Number of raw samples in filter output buffer
      float energy = 0;
      for(int n=0; n < N; n++){
	buffer[n] *= step_osc(&demod->fine);
	energy += cnrmf(buffer[n]);
      }
      energy /= N;
      demod->sig.bb_power = energy;
      demod->sig.bb_energy += energy;
    }
    demod->filter.bin_shift = shift; // We need this in any case (not really?)
    demod->sig.n0 = estimate_noise(demod,-shift); // Negative, just like compute_tuning. Note: must follow execute_filter_output()

    return 0;
}
