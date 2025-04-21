// Core of 'radiod' program - create/delete channels, control LOs, set frequency/mode, etc
// Copyright 2018-2024, Phil Karn, KA9Q
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

pthread_mutex_t Channel_list_mutex = PTHREAD_MUTEX_INITIALIZER;
struct channel Channel_list[Nchannels];
int Active_channel_count; // Active channels
float Power_smooth = 0.05; // Arbitrary exponential smoothing factor


// Find chan by ssrc
struct channel *lookup_chan(uint32_t ssrc){
  struct channel *chan = NULL;
  pthread_mutex_lock(&Channel_list_mutex);
  for(int i=0; i < Nchannels; i++){
    if(Channel_list[i].inuse && Channel_list[i].output.rtp.ssrc == ssrc){
      chan = &Channel_list[i];
      break;
    }
  }
  pthread_mutex_unlock(&Channel_list_mutex);
  return chan;
}


// Atomically create chan only if the ssrc doesn't already exist
struct channel *create_chan(uint32_t ssrc){
  if(ssrc == 0xffffffff)
    return NULL; // reserved
  pthread_mutex_lock(&Channel_list_mutex);
  for(int i=0; i < Nchannels; i++){
    if(Channel_list[i].inuse && Channel_list[i].output.rtp.ssrc == ssrc){
      pthread_mutex_unlock(&Channel_list_mutex);
      return NULL; // sorry, already taken
    }
  }
  struct channel *chan = NULL;
  for(int i=0; i < Nchannels; i++){
    if(!Channel_list[i].inuse){
      chan = &Channel_list[i];
      break;
    }
  }
  if(chan == NULL){
    fprintf(stdout,"Warning: out of chan table space (%'d)\n",Active_channel_count);
    // Abort here? Or keep going?
  } else {
    // Because the memcpy clobbers the ssrc, we must keep the lock held on Channel_list_mutex
    memcpy(chan,&Template,sizeof *chan);
    chan->inuse = true;
    chan->output.rtp.ssrc = ssrc; // Stash it
    Active_channel_count++;
    chan->lifetime = 20 * 1000 / Blocktime; // If freq == 0, goes away 20 sec after last command
  }
  pthread_mutex_unlock(&Channel_list_mutex);
  return chan;
}

static const float N0_smooth = .001; // exponential smoothing rate for (noisy) bin noise

// experimental
// estimate n0 by finding the FFT bin with the least energy
// in the chan's pre-filter nyquist bandwidth
// Works better than global estimation when noise floor is not flat, e.g., on HF
static float estimate_noise(struct channel *chan,int shift){
  assert(chan != NULL);
  if(chan == NULL)
    return NAN;
  struct filter_out *slave = &chan->filter.out;
  assert(slave != NULL);
  if(slave == NULL)
    return NAN;
  if(slave->bins <= 0)
    return 0;
  // Should do some range checking on 'shift'
  if(chan->filter.energies == NULL)
    chan->filter.energies = calloc(sizeof *chan->filter.energies,slave->bins);

  float * const energies = chan->filter.energies;
  struct filter_in const * const master = slave->master;
  // slave->next_jobnum already incremented by execute_filter_output
  float complex const * const fdomain = master->fdomain[(slave->next_jobnum - 1) % ND];

  float min_bin_energy = INFINITY;
  if(master->in_type == REAL){
    // Only half as many bins as with complex input, all positive or all negative
    // if shift < 0, the spectrum is inverted and we'll look at it in reverse order but that's OK
    int mbin = abs(shift) - slave->bins/2;
    for(int i=0; i < slave->bins && mbin < master->bins; i++,mbin++){
      if(mbin >= 0){
	if(energies[i] == 0){
	  // Bias first estimate high to keep it from being considered until
	  // it's had a chance to settle down to a better average
	  energies[i] = 10 * cnrmf(fdomain[mbin]);
	} else {
	  energies[i] += (cnrmf(fdomain[mbin]) - energies[i]) * N0_smooth;
	  if(min_bin_energy > energies[i])
	    min_bin_energy = energies[i];
	}
      }
    }
  } else {
    int mbin = shift - slave->bins/2; // Start at lower channel edge (upper for inverted real)
    // Complex input that often straddles DC
    if(mbin < 0)
      mbin += master->bins; // starting in negative frequencies
    assert(mbin >= 0 && mbin < master->bins); // probably should just return without doing anything

    for(int i=0; i < slave->bins; i++){
      if(energies[i] == 0) {
	// Bias first estimate high to keep it from being considered until
	// it's had a chance to settle down to a better average
	energies[i] = 10 * cnrmf(fdomain[mbin]); // Quick startup
      } else {
	energies[i] += (cnrmf(fdomain[mbin]) - energies[i]) * N0_smooth;
	if(min_bin_energy > energies[i])
	  min_bin_energy = energies[i];
      }
      if(++mbin == master->bins)
	mbin = 0; // wrap around from neg freq to pos freq
      if(mbin == master->bins/2)
	break; // fallen off the right edge
    }
  }
  if(!isfinite(min_bin_energy)) // Never got set!
    return 0;

  // correct for FFT scaling and normalize to 1 Hz
  // With an unnormalized FFT, the noise energy in each bin scales proportionately with the number of points in the FFT
  return min_bin_energy / ((float)master->bins * Frontend.samprate);
}


void *demod_thread(void *p){
  assert(p != NULL);
  struct channel *chan = (struct channel *)p;
  if(chan == NULL)
    return NULL;

  pthread_detach(pthread_self());

  // Repeatedly invoke appropriate demodulator
  // When a demod exits, the appropriate one is restarted,
  // which can be the same one if demod_type hasn't changed
  // A demod can terminate completely by setting an invalid demod_type and exiting
  int status = 0;
  while(status == 0){ // A demod returns non-zero to signal a fatal error, don't restart
    switch(chan->demod_type){
    case LINEAR_DEMOD:
      status = demod_linear(p);
      break;
    case FM_DEMOD:
      status = demod_fm(p);
      break;
    case WFM_DEMOD:
      status = demod_wfm(p);
      break;
    case SPECT_DEMOD:
      status = demod_spectrum(p);
      break;
    default:
      status = -1; // Unknown demod, quit
      break;
    }
  }
  close_chan(chan);
  return NULL;
}

// start demod thread on already-initialized chan structure
int start_demod(struct channel * chan){
  assert(chan != NULL);
  if(chan == NULL)
    return -1;

  if(Verbose){
    fprintf(stdout,"start_demod: ssrc %'u, output %s, demod %d, freq %'.3lf, preset %s, filter (%'+.0f,%'+.0f)\n",
	    chan->output.rtp.ssrc, chan->output.dest_string, chan->demod_type, chan->tune.freq, chan->preset, chan->filter.min_IF, chan->filter.max_IF);
  }
  pthread_create(&chan->demod_thread,NULL,demod_thread,chan);
  return 0;
}

// Called by a demodulator to clean up its own resources
int close_chan(struct channel *chan){
  if(chan == NULL)
    return -1;

  pthread_t nullthread = {0};

  if(chan->rtcp.thread != nullthread){
    pthread_cancel(chan->rtcp.thread);
    pthread_join(chan->rtcp.thread,NULL);
  }
  if(chan->sap.thread != nullthread){
    pthread_cancel(chan->sap.thread);
    pthread_join(chan->sap.thread,NULL);
  }

  pthread_mutex_lock(&chan->status.lock);
  FREE(chan->status.command);
  FREE(chan->filter.energies);
  FREE(chan->spectrum.bin_data);
  delete_filter_output(&chan->filter.out);
  if(chan->output.opus != NULL){
    opus_encoder_destroy(chan->output.opus);
    chan->output.opus = NULL;
  }
  pthread_mutex_unlock(&chan->status.lock);
  pthread_mutex_lock(&Channel_list_mutex);
  if(chan->inuse){
    // Should be set, but check just in case to avoid messing up Active_channel_count
    chan->inuse = false;
    Active_channel_count--;
  }
  pthread_mutex_unlock(&Channel_list_mutex);
  return 0;
}

pthread_mutex_t Freq_mutex = PTHREAD_MUTEX_INITIALIZER;

// Set receiver frequency
// The new IF is computed here only to determine if the front end needs retuning
// The second LO frequency is actually set when the new front end frequency is
// received back from the front end metadata
double set_freq(struct channel * const chan,double const f){
  assert(chan != NULL);
  if(chan == NULL)
    return NAN;

  assert(!isnan(f));
  chan->tune.freq = f;

  // Tuning to 0 Hz is a special case, don't move front end
  // Essentially disables a chan
  if(f == 0)
    return f;

  pthread_mutex_lock(&Freq_mutex); // Protect front end tuner
  // Determine new IF
  double new_if = f - Frontend.frequency;

  // Flip sign to convert LO2 frequency to IF carrier frequency
  // Tune an extra kHz to account for front end roundoff
  // Ideally the front end would just round in a preferred direction
  // but it doesn't know where our IF will be so it can't make the right choice
  // Retuning the front end will cause all the other channels to recalculate their own IFs
  // What if the IF is wider than the receiver can supply?
  double const fudge = 1000;
  if(new_if > Frontend.max_IF - chan->filter.max_IF){
    // Retune LO1 as little as possible
    new_if = Frontend.max_IF - chan->filter.max_IF - fudge;
    set_first_LO(chan,f - new_if);
  } else if(new_if < Frontend.min_IF - chan->filter.min_IF){
    // Also retune LO1 as little as possible
    new_if = Frontend.min_IF - chan->filter.min_IF + fudge;
    set_first_LO(chan,f - new_if);
  }
  pthread_mutex_unlock(&Freq_mutex);
  return f;
}

// Set first (front end tuner) oscillator
// Note: single precision floating point is not accurate enough at VHF and above
// chan->first_LO is NOT updated here!
// It is set by incoming status frames so this will take time
double set_first_LO(struct channel const * const chan,double const first_LO){
  assert(chan != NULL);
  if(chan == NULL)
    return NAN;

  double const current_lo1 = Frontend.frequency;

  // Just return actual frequency without changing anything
  if(first_LO == current_lo1 || first_LO <= 0)
    return first_LO;

  // Direct tuning through local module if available
  if(Frontend.tune != NULL)
    return (*Frontend.tune)(&Frontend,first_LO);

  return first_LO;
}

// Compute FFT bin shift and time-domain fine tuning offset for specified LO frequency
// N = input fft length
// M = input buffer overlap
// samprate = input sample rate
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
  (void)M;
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
    return -1; // Chan thread will wait for the front end status to change
  return 0;
}

/* Session announcement protocol - highly experimental, off by default
   The whole point was to make it easy to use VLC and similar tools, but they either don't actually implement SAP (e.g. in iOS)
   or implement some vague subset that you have to guess how to use
   Will probably work better with Opus streams from the opus transcoder, since they're always 48000 Hz stereo; no switching midstream
*/
void *sap_send(void *p){
  struct channel *chan = (struct channel *)p;
  assert(chan != NULL);
  if(chan == NULL)
    return NULL;

  int64_t start_time = utc_time_sec() + NTP_EPOCH; // NTP uses UTC, not GPS

  // These should change when a change is made elsewhere
  uint16_t const id = random(); // Should be a hash, but it changes every time anyway
  int const sess_version = 1;

  for(;;){
    char message[PKTSIZE],*wp;
    int space = sizeof(message);
    wp = message;

    *wp++ = 0x20; // SAP version 1, ipv4 address, announce, not encrypted, not compressed
    *wp++ = 0; // No authentication
    *wp++ = id >> 8;
    *wp++ = id & 0xff;
    space -= 4;

    // our sending ipv4 address
    struct sockaddr_in const *sin = (struct sockaddr_in *)&chan->output.source_socket;
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
      char hostname[sysconf(_SC_HOST_NAME_MAX)];
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
      char *mcast = strdup(formatsock(&chan->output.dest_socket,false));
      assert(mcast != NULL);
      // Remove :port field, confuses the vlc listener
      char *cp = strchr(mcast,':');
      if(cp)
	*cp = '\0';
      len = snprintf(wp,space,"c=IN IP4 %s/%d\r\n",mcast,Mcast_ttl);
      wp += len;
      space -= len;
      FREE(mcast);
    }


#if 0 // not currently used
    int64_t current_time = utc_time_sec() + NTP_EPOCH;
#endif

    // t= (time description)
    len = snprintf(wp,space,"t=%lld %lld\r\n",(long long)start_time,0LL); // unbounded
    wp += len;
    space -= len;

    // m = media description
    // set from current state. This will require changing the session version and IDs, and
    // it's not clear that clients like VLC will do the right thing anyway
    len = snprintf(wp,space,"m=audio 5004/1 RTP/AVP %d\r\n",chan->output.rtp.type);
    wp += len;
    space -= len;

    len = snprintf(wp,space,"a=rtpmap:%d %s/%d/%d\r\n",
		   chan->output.rtp.type,
		   encoding_string(chan->output.encoding),
		   chan->output.samprate,
		   chan->output.channels);
    wp += len;
    space -= len;

    if(sendto(Output_fd,message,wp - message,0,&chan->sap.dest_socket,sizeof(chan->sap.dest_socket)) < 0)
      chan->output.errors++;
    sleep(5);
  }
}

// Run top-of-loop stuff common to all demod types
// 1. If dynamic and sufficiently idle, terminate
// 2. Process any commands from the common command/status channel
// 3. Send any requested delayed status to the common status channel
// 4. Send any status to the output channel
//    if the processed command requires a restart, return +1
// 5. Block until front end is in range
// 6. compute FFT bin shift & fine tuning remainder
// 7. Set fine tuning oscillator frequency & phase
// 8. Run output half (IFFT) of filter
// 9. Update noise estimate
// 10. Run fine tuning, compute average power

// Baseband samples placed in chan->filter.out->output.c
int downconvert(struct channel *chan){
  assert(chan != NULL);
  if(chan == NULL)
    return -1;

  int shift = 0;
  double remainder = 0;

  while(true){
    // Should we die?
    // Will be slower if 0 Hz is outside front end coverage because of slow timed wait below
    // But at least it will eventually go away
    if(chan->tune.freq == 0 && chan->lifetime > 0){
      if(--chan->lifetime <= 0){
	chan->demod_type = -1;  // No demodulator
	if(Verbose > 1)
	  fprintf(stdout,"chan %d terminate needed\n",chan->output.rtp.ssrc);
	return -1; // terminate needed
      }
    }
    // Process any commands and return status
    bool restart_needed = false;
    pthread_mutex_lock(&chan->status.lock);

    if(chan->status.output_interval != 0 && chan->status.output_timer == 0 && !chan->output.silent)
      chan->status.output_timer = 1; // channel has become active, send update on this pass

    // Look on the single-entry command queue and grab it atomically
    if(chan->status.command != NULL){
      restart_needed = decode_radio_commands(chan,chan->status.command,chan->status.length);
      send_radio_status(&Metadata_dest_socket,&Frontend,chan); // Send status in response
      chan->status.global_timer = 0; // Just sent one
      // Also send to output stream
      if(chan->demod_type != SPECT_DEMOD){
	// Only send spectrum on status channel, and only in response to poll
	// Spectrum channel output socket isn't set anyway
	send_radio_status(&chan->status.dest_socket,&Frontend,chan);
      }
      chan->status.output_timer = chan->status.output_interval; // Reload
      FREE(chan->status.command);
      reset_radio_status(chan); // After both are sent
    } else if(chan->status.global_timer != 0 && --chan->status.global_timer <= 0){
      // Delayed status request, used mainly by all-channel polls to avoid big bursts
      send_radio_status(&Metadata_dest_socket,&Frontend,chan); // Send status in response
      chan->status.global_timer = 0; // to make sure
      reset_radio_status(chan);
    } else if(chan->status.output_interval != 0 && chan->status.output_timer > 0){
      // Timer is running for status on output stream
      if(--chan->status.output_timer == 0){
	// Timer has expired; send status on output channel
	send_radio_status(&chan->status.dest_socket,&Frontend,chan);
	reset_radio_status(chan);
	if(!chan->output.silent)
	  chan->status.output_timer = chan->status.output_interval; // Restart timer only if channel is active
      }
    }

    pthread_mutex_unlock(&chan->status.lock);
    if(restart_needed){
      if(Verbose > 1)
	fprintf(stdout,"chan %d restart needed\n",chan->output.rtp.ssrc);
      return +1; // Restart needed
    }
    // To save CPU time when the front end is completely tuned away from us, block (with timeout) until the front
    // end status changes rather than process zeroes. We must still poll the terminate flag.
    pthread_mutex_lock(&Frontend.status_mutex);

    /* Note sign conventions:
       When the radio frequency is above the front end frequency, as in direct sampling (Frontend.frequency == 0)
       or in low side injection, tune.second_LO is negative and so is 'shift'

       For the real A/D streams from TV tuners with high-side injection, e.g., Airspy R2,
       tune.second_LO and shift are both positive and the spectrum is inverted

       For complex SDRs, tune.second_LO can be either positive or negative, so shift will be negative or positive

       Note minus on 'shift' parameter to execute_filter_output() and estimate_noise()
    */
    chan->tune.second_LO = Frontend.frequency - chan->tune.freq;
    double const freq = chan->tune.doppler + chan->tune.second_LO; // Total logical oscillator frequency
    if(compute_tuning(Frontend.in.ilen + Frontend.in.impulse_length - 1,
		      Frontend.in.impulse_length,
		      Frontend.samprate,
		      &shift,&remainder,freq) != 0){
      // No front end coverage of our carrier; wait one block time for it to retune
      chan->sig.bb_power = 0;
      chan->sig.snr = 0;
      chan->output.power = 0;
      struct timespec timeout; // Needed to avoid deadlock if no front end is available
      clock_gettime(CLOCK_REALTIME,&timeout);
      timeout.tv_nsec += Blocktime * MILLION; // milliseconds to nanoseconds
      if(timeout.tv_nsec > BILLION){
	timeout.tv_sec += 1; // 1 sec in the future
	timeout.tv_nsec -= BILLION;
      }
      pthread_cond_timedwait(&Frontend.status_cond,&Frontend.status_mutex,&timeout);
      pthread_mutex_unlock(&Frontend.status_mutex);
      continue;
    }
    pthread_mutex_unlock(&Frontend.status_mutex);

    // Note minus on 'shift' parameter; see discussion inside compute_tuning() on sign conventions
    execute_filter_output(&chan->filter.out,-shift); // block until new data frame
    chan->status.blocks_since_poll++;

    if(chan->filter.out.out_type == SPECTRUM){ // No output time-domain buffer in spectrum mode
      chan->filter.bin_shift = shift; // Also used by spectrum.c:demod_spectrum() to know where to read direct from master
      return 0;
    }
    // set fine tuning frequency & phase
    // avoid them both being 0 at startup; init chan->filter.remainder as NAN
    if(shift != chan->filter.bin_shift || remainder != chan->filter.remainder){ // Detect startup
      assert(isfinite(chan->tune.doppler_rate));
      set_osc(&chan->fine,remainder/chan->output.samprate,chan->tune.doppler_rate/(chan->output.samprate * chan->output.samprate));
      chan->filter.remainder = remainder;
    }
    /* Block phase adjustment (folded into the fine tuning osc) in two parts:
       (a) phase_adjust is applied on each block when FFT bin shifts aren't divisible by V; otherwise it's unity
       (b) second term keeps the phase continuous when shift changes; found empirically, dunno yet why it works!
       Be sure to Initialize chan->filter.bin_shift at startup to something bizarre to force this inequality on first call */
    if(shift != chan->filter.bin_shift){
      const int V = 1 + (Frontend.in.ilen / (Frontend.in.impulse_length - 1)); // Overlap factor
      chan->filter.phase_adjust = cispi(-2.0*(shift % V)/(double)V); // Amount to rotate on each block for shifts not divisible by V
      chan->fine.phasor *= cispi((shift - chan->filter.bin_shift) / (2.0 * (V-1))); // One time adjust for shift change
      chan->filter.bin_shift = shift;
    }
    chan->fine.phasor *= chan->filter.phase_adjust;

    // Make fine tuning correction before secondary filtering
    for(int n=0; n < chan->filter.out.olen; n++)
      chan->filter.out.output.c[n] *= step_osc(&chan->fine);

    if(chan->filter2.blocking == 0){
      // No secondary filtering, done
      chan->baseband = chan->filter.out.output.c;
      chan->sampcount = chan->filter.out.olen;
      break;
    }
    int r = write_cfilter(&chan->filter2.in,chan->filter.out.output.c,chan->filter.out.olen); // Will trigger execution of input side if buffer is full, returning 1
    if(r > 0){
      execute_filter_output(&chan->filter2.out,0); // No frequency shifting
      chan->baseband = chan->filter2.out.output.c;
      chan->sampcount = chan->filter2.out.olen;
      break;
    }
  }
  float energy = 0;
  for(int n=0; n < chan->sampcount; n++)
    energy += cnrmf(chan->baseband[n]);
  chan->sig.bb_power = energy / chan->sampcount;

  /* The N0 noise estimator has a long smoothing time constant, so clamp it when the front end is saturated, e.g. by a local transmitter
     This works well for channels tuned well away from the transmitter, but not when a channel is tuned near or to the transmit frequency
     because the transmitted noise is enough to severely increase the estimate even before it begins to transmit
     enough power to saturate the A/D. I still need a better, more general way of adjusting N0 smoothing rate,
     e.g. for when the channel is retuned by a lot */
  float maxpower = (1 << (Frontend.bitspersample - 1));
  maxpower *= maxpower * 0.5; // 0 dBFS
  if(Frontend.if_power < maxpower)
    chan->sig.n0 = estimate_noise(chan,-shift); // Negative, just like compute_tuning. Note: must follow execute_filter_output()
  return 0;
}

int set_channel_filter(struct channel *chan){
  // Limit to Nyquist rate
  float lower = max(chan->filter.min_IF, -(float)chan->output.samprate/2);
  float upper = min(chan->filter.max_IF, (float)chan->output.samprate/2);

  if(Verbose > 1)
    fprintf(stdout,"new filter for chan %'u: IF=[%'.0f,%'.0f], samprate %'d, kaiser beta %.1f\n",
	    chan->output.rtp.ssrc, lower, upper,
	    chan->output.samprate, chan->filter.kaiser_beta);

  delete_filter_output(&chan->filter2.out);
  delete_filter_input(&chan->filter2.in);
  if(chan->filter2.blocking > 0){
    extern int Overlap;
    unsigned int const blocksize = chan->filter2.blocking * chan->output.samprate * Blocktime / 1000;
    float const binsize = (1000.0f / Blocktime) * ((float)(Overlap - 1) / Overlap);
    float const margin = 4 * binsize; // 4 bins should be enough even for large Kaiser betas

    unsigned int n = round2(2 * blocksize); // Overlap >= 50%
    unsigned int order = n - blocksize;
    fprintf(stdout,"filter2 create: L = %d, M = %d, N = %d\n",blocksize,order+1,n);
    // Secondary filter running at 1:1 sample rate with order = filter2.blocking * inblock
    create_filter_input(&chan->filter2.in,blocksize,order+1,COMPLEX);
    chan->filter2.in.perform_inline = true;
    create_filter_output(&chan->filter2.out,&chan->filter2.in,NULL,blocksize,chan->filter2.isb ? CROSS_CONJ : COMPLEX);
    chan->filter2.low = lower;
    chan->filter2.high = upper;
    if(chan->filter2.kaiser_beta < 0 || !isfinite(chan->filter2.kaiser_beta))
      chan->filter2.kaiser_beta = chan->filter.kaiser_beta;
    set_filter(&chan->filter2.out,
	       lower/chan->output.samprate,
	       upper/chan->output.samprate,
	       chan->filter2.kaiser_beta);
    // Widen the main filter a little to keep its broad skirts from cutting into filter2's response
    // I.e., the main filter becomes a roofing filter
    // Again limit to Nyquist rate
    lower -= margin;
    lower = max(lower, -(float)chan->output.samprate/2);
    upper += margin;
    upper = min(upper, (float)chan->output.samprate/2);
  }
  // Set main filter
  set_filter(&chan->filter.out,
	     lower/chan->output.samprate,
	     upper/chan->output.samprate,
	     chan->filter.kaiser_beta);

  chan->filter.remainder = NAN; // Force re-init of fine oscillator
  return 0;
}

// scale A/D output power to full scale for monitoring overloads
float scale_ADpower2FS(struct frontend const *frontend){
  assert(frontend != NULL);
  if(frontend == NULL)
    return NAN;

  assert(frontend->bitspersample > 0);
  float scale = 1.0f / (1 << (frontend->bitspersample - 1)); // Important to force the numerator to float, otherwise the divide produces zero!
  scale *= scale;
  // Scale real signals up 3 dB so a rail-to-rail sine will be 0 dBFS, not -3 dBFS
  // Complex signals carry twice as much power, divided between I and Q
  if(frontend->isreal)
    scale *= 2;
  return scale;
}
// Returns multiplicative factor for converting raw samples to floats with analog gain correction
float scale_AD(struct frontend const *frontend){
  assert(frontend != NULL);
  if(frontend == NULL)
    return NAN;

  assert(frontend->bitspersample > 0);
  float scale = (1 << (frontend->bitspersample - 1));

  // net analog gain, dBm to dBFS, that we correct for to maintain unity gain, i.e., 0 dBm -> 0 dBFS
  float analog_gain = frontend->rf_gain - frontend->rf_atten + frontend->rf_level_cal;
  // Will first get called before the filter input is created
  return (M_SQRT2 * dB2voltage(-analog_gain)) / scale; // Front end gain as amplitude ratio
}
