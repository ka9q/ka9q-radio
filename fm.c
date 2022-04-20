// $Id: fm.c,v 1.126 2022/04/19 07:26:01 karn Exp $
// FM demodulation and squelch
// Copyright 2018, Phil Karn, KA9Q
#define _GNU_SOURCE 1
#include <assert.h>
#include <pthread.h>
#include <string.h>
#include <math.h>
#include <complex.h>
#include <fftw3.h>

#include "misc.h"
#include "iir.h"
#include "filter.h"
#include "radio.h"

// These could be made settable if needed
static int const squelchzeroes = 2; // Frames of PCM zeroes after squelch closes, to flush downstream filters (eg, packet)


// FM demodulator thread
void *demod_fm(void *arg){
  assert(arg != NULL);
  struct demod * demod = arg;  
  
  {
    char name[100];
    snprintf(name,sizeof(name),"fm %u",demod->output.rtp.ssrc);
    pthread_setname(name);
  }

  complex float state = 0;
  demod->output.channels = 1; // Only mono for now
  if(isnan(demod->squelch_open) || demod->squelch_open == 0)
    demod->squelch_open = 6.3;  // open above ~ +8 dB
  if(isnan(demod->squelch_close) || demod->squelch_close == 0)
    demod->squelch_close = 4; // close below ~ +6 dB

  int const blocksize = demod->output.samprate * Blocktime / 1000;
  if(demod->filter.out)
    delete_filter_output(&demod->filter.out);
  demod->filter.out = create_filter_output(Frontend.in,NULL,blocksize,COMPLEX);
  if(demod->filter.out == NULL){
    fprintf(stdout,"unable to create filter for ssrc %lu\n",(unsigned long)demod->output.rtp.ssrc);
    goto quit;
  }
  set_filter(demod->filter.out,
	     demod->filter.min_IF/demod->output.samprate,
	     demod->filter.max_IF/demod->output.samprate,
	     demod->filter.kaiser_beta);
  
  int squelch_state = 0; // Number of blocks for which squelch remains open
  // Reasonable starting gain
  demod->output.gain = (demod->output.headroom *  M_1_PI * demod->output.samprate) / fabsf(demod->filter.min_IF - demod->filter.max_IF);

  int const N = demod->filter.out->olen;
  float const one_over_olen = 1. / N; // save some divides

  while(!demod->terminate){
    // Constant gain used by FM only; automatically adjusted by AGC in linear modes
    // We do this in the loop because BW can change
    // Force reasonable parameters if they get messed up or aren't initialized
    demod->output.gain = (demod->output.headroom *  M_1_PI * demod->output.samprate) / fabsf(demod->filter.min_IF - demod->filter.max_IF);

    float bb_power = 0;
    float avg_amp = 0;
    float amplitudes[N];
    complex float * const buffer = demod->filter.out->output.c;

    double remainder;
    int flip;
    int rotate;

    // To save CPU time when the front end is completely tuned away from us, block until the front
    // end status changes rather than process zeroes
    pthread_mutex_lock(&Frontend.sdr.status_mutex);
    while(1){
      if(demod->terminate){
	// Note: relies on periodic front end status messages for polling
	pthread_mutex_unlock(&Frontend.sdr.status_mutex);
	goto quit;
      }
      // Note: tune.shift ignored in FM mode
      demod->tune.second_LO = Frontend.sdr.frequency - demod->tune.freq;
      double const freq = demod->tune.doppler + demod->tune.second_LO; // Total logical oscillator frequency
      if(compute_tuning(Frontend.in->ilen + Frontend.in->impulse_length - 1,
			Frontend.in->impulse_length,
			Frontend.sdr.samprate,
			&flip,&rotate,&remainder,freq) == 0)
	break;      // We can get at least part of the spectrum we want

      // No front end coverage of our passband; wait for it to retune
      demod->sig.bb_power = 0;
      demod->output.level = 0;
      struct timespec timeout;
      clock_gettime(CLOCK_REALTIME,&timeout);
      timeout.tv_sec += 1; // 1 sec in the future
      pthread_cond_timedwait(&Frontend.sdr.status_cond,&Frontend.sdr.status_mutex,&timeout);
    }
    pthread_mutex_unlock(&Frontend.sdr.status_mutex);
    // first pass: measure average power and compute sample amplitudes for variance calculation

#undef FULL
    /* Save time by not applying the fine frequency shift.  The error
       will be small if the blocktime is small (100 Hz for 10 ms).
       This would normally be executed even on round frequency
       channels because of front end fractional-N and
       calibration offsets */

#if FULL
    set_osc(&demod->fine,remainder, demod->tune.doppler_rate);
#endif
    execute_filter_output(demod->filter.out,-rotate);
    for(int n = 0; n < N; n++){
      // Apply frequency shifts
#if FULL
      complex float s = buffer[n] * flip * step_osc(&demod->fine);
#else
      complex float s = buffer[n] * flip;
#endif
      buffer[n] = s;
      bb_power += cnrmf(s);
      avg_amp += amplitudes[n] = approx_magf(s); // Saves a few % CPU on lots of demods vs sqrtf(t)
    }
    demod->sig.bb_power = bb_power * one_over_olen;
    avg_amp *= one_over_olen;
    float const noise_reduct_scale = 1 / (0.4 * avg_amp);

    // Compute variance in second pass.
    // Two passes are supposed to be more numerically stable, but is it really necessary?
    float fm_variance = 0;
    for(int n=0; n < N; n++)
      fm_variance += (amplitudes[n] - avg_amp) * (amplitudes[n] - avg_amp);

    // Compute signal-to-noise, see if we should open the squelch
    float const snr = fm_snr(avg_amp*avg_amp * (N-1) / fm_variance);
    demod->sig.snr = snr;

    // Hysteresis squelch
    if(snr >= demod->squelch_open
       || (squelch_state > squelchzeroes && snr >= demod->squelch_close))
      // tail timing is in blocks (usually 10 or 20 ms each)
      squelch_state = squelchzeroes + demod->squelchtail + 1;
    else if(squelch_state > 0)
      squelch_state--;
    else
      squelch_state = 0;

    float baseband[N];    // Demodulated FM baseband
    float peak_positive_deviation = 0;
    float peak_negative_deviation = 0;   // peak neg deviation
    float frequency_offset = 0;      // Average frequency
    float output_level = 0;
    if(squelch_state > squelchzeroes){ // Squelch is (still) open
      // Actual FM demodulation
      for(int n=0; n < N; n++){
	// actual FM demodulation 
	float const deviation = cargf(buffer[n] * conjf(state));
	state = buffer[n];
	if(squelch_state > squelchzeroes + demod->squelchtail){
	  // Perform only when squelch is fully open, not during tail
	  frequency_offset += deviation; // Direct FM for frequency measurement
	  if(deviation > peak_positive_deviation)
	    peak_positive_deviation = deviation;
	  else if(deviation < peak_negative_deviation)
	    peak_negative_deviation = deviation;
	}
	baseband[n] = deviation * demod->output.gain;
	// Experimental click reduction
	if(amplitudes[n] < 0.4 * avg_amp)
	  baseband[n] *= amplitudes[n] * noise_reduct_scale;

	// Apply de-emphasis if configured
	if(demod->deemph.rate != 0){
	  __real__ demod->deemph.state *= demod->deemph.rate;
	  __real__ demod->deemph.state += demod->deemph.gain * (1 - demod->deemph.rate) * baseband[n];
	  baseband[n] = __real__ demod->deemph.state;
	}
	output_level += baseband[n] * baseband[n];
      } // for(int n=0; n < N; n++)
      output_level *= one_over_olen;
    } else if(squelch_state > 0){ // Squelch closed, but emitting padding
      state = 0; // Soft-open squelch next time
      memset(baseband,0,sizeof(baseband));
    }
    demod->output.level = output_level;
    // mute output unless time is left on the squelch_state timer
    if(send_mono_output(demod,baseband,N,squelch_state <= 0) < 0)
      break; // no valid output stream; terminate!

    if(squelch_state > squelchzeroes + demod->squelchtail){
      frequency_offset *= one_over_olen;  // Average FM output is freq offset
      // Update frequency offset and peak deviation
      demod->sig.foffset = demod->output.samprate  * frequency_offset * M_1_2PI;
      
      // Remove frequency offset from deviation peaks and scale
      peak_positive_deviation -= frequency_offset;
      peak_negative_deviation -= frequency_offset;
      demod->fm.pdeviation = demod->output.samprate * max(peak_positive_deviation,-peak_negative_deviation) * M_1_2PI;
    }
  } // while(!demod->terminate)
 quit:;
  delete_filter_output(&demod->filter.out);
  return NULL;
}
