// FM demodulation and squelch for ka9q-radio
// Copyright 2018-2023, Phil Karn, KA9Q
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
static int const power_squelch = 1; // Enable experimental pre-squelch to save CPU on idle channels

// FM demodulator thread
void *demod_fm(void *arg){
  assert(arg != NULL);
  struct channel * const chan = arg;  
  
  {
    char name[100];
    snprintf(name,sizeof(name),"fm %u",chan->output.rtp.ssrc);
    pthread_setname(name);
  }
  pthread_mutex_init(&chan->status.lock,NULL);
  pthread_mutex_lock(&chan->status.lock);
  FREE(chan->status.command);
  FREE(chan->filter.energies);
  FREE(chan->spectrum.bin_data);
  if(chan->output.opus != NULL){
    opus_encoder_destroy(chan->output.opus);
    chan->output.opus = NULL;
  }

  int const blocksize = chan->output.samprate * Blocktime / 1000;
  delete_filter_output(&chan->filter.out);
  create_filter_output(&chan->filter.out,&Frontend.in,NULL,blocksize,COMPLEX);
  pthread_mutex_unlock(&chan->status.lock);

  set_filter(&chan->filter.out,
	     chan->filter.min_IF/chan->output.samprate,
	     chan->filter.max_IF/chan->output.samprate,
	     chan->filter.kaiser_beta);
  
  float phase_memory = 0;
  chan->output.channels = 1; // Only mono for now
  if(isnan(chan->fm.squelch_open) || chan->fm.squelch_open == 0)
    chan->fm.squelch_open = 6.3;  // open above ~ +8 dB
  if(isnan(chan->fm.squelch_close) || chan->fm.squelch_close == 0)
    chan->fm.squelch_close = 4; // close below ~ +6 dB

  if(chan->fm.tone_freq != 0){
    // Set up PL tone squelch
    init_goertzel(&chan->fm.tone_detect,chan->fm.tone_freq/chan->output.samprate);
  }

  float deemph_state = 0;
  int squelch_state = 0; // Number of blocks for which squelch remains open
  int const N = chan->filter.out.olen;
  float const one_over_olen = 1.0f / N; // save some divides
  int const pl_integrate_samples = chan->output.samprate * 0.24; // 240 milliseconds (spec is < 250 ms)
  int pl_sample_count = 0;
  bool tone_mute = true; // When tone squelch enabled, mute until the tone is detected
  int badsegments = 0;
  int badsamples = 0;
  chan->output.gain = (2 * chan->output.headroom *  chan->output.samprate) / fabsf(chan->filter.min_IF - chan->filter.max_IF);

  float dc_rate = -expm1f(-1.0f / (0.1 * chan->output.samprate)); // experimental DC removal, 100 ms time constant (~10 Hz)
  float dc = 0; // DC removal filter state


  realtime();

  while(downconvert(chan) == 0){
    if(power_squelch && squelch_state == 0){
      // quick check SNR from raw signal power to save time on variance-based squelch
      // Variance squelch is still needed to suppress various spurs and QRM
      float const snr = (chan->sig.bb_power / (chan->sig.n0 * fabsf(chan->filter.max_IF - chan->filter.min_IF))) - 1.0f;
      if(snr < chan->fm.squelch_close){
	// squelch closed, reset everything and mute output
	chan->sig.snr = snr; // Copy to FM SNR so monitor, etc, will see it
	phase_memory = 0;
	squelch_state = 0;
	pl_sample_count = 0;
	reset_goertzel(&chan->fm.tone_detect);
	send_output(chan,NULL,N,true); // Keep track of timestamps and mute state
	continue;
      }
    }
    complex float const * const buffer = chan->filter.out.output.c; // for convenience
    float amplitudes[N];
    float avg_amp = 0;
    for(int n = 0; n < N; n++)
      avg_amp += amplitudes[n] = cabsf(buffer[n]);    // Use cabsf() rather than approx_magf(); may give more accurate SNRs?
    avg_amp *= one_over_olen;
    {
      // Compute variance in second pass.
      // Two passes are supposed to be more numerically stable, but is it really necessary?
      float fm_variance = 0;
      for(int n=0; n < N; n++)
	fm_variance += (amplitudes[n] - avg_amp) * (amplitudes[n] - avg_amp);
      
      // Compute signal-to-noise, see if we should open the squelch
      float const snr = fm_snr(avg_amp*avg_amp * (N-1) / fm_variance);
      chan->sig.snr = max(0.0f,snr); // Smoothed values can be a little inconsistent
    }
    // Hysteresis squelch
    int const squelch_state_max = chan->fm.squelch_tail + 1;
    if(chan->sig.snr >= chan->fm.squelch_open
       || (squelch_state > 0 && chan->sig.snr >= chan->fm.squelch_close)){
      // Squelch is fully open
      // tail timing is in blocks (usually 10 or 20 ms each)
      squelch_state = squelch_state_max;
    } else if(--squelch_state > 0) {
      // In tail, squelch still open
    } else {
      // squelch closed, reset everything and mute output
      phase_memory = 0;
      squelch_state = 0;
      pl_sample_count = 0;
      reset_goertzel(&chan->fm.tone_detect);
      send_output(chan,NULL,N,true); // Keep track of timestamps and mute state
      continue;
    }
    float baseband[N];    // Demodulated FM baseband
    // Actual FM demodulation
    for(int n=0; n < N; n++){
      float np = M_1_PIf * cargf(buffer[n]); // Scale to -1 to +1 (half rotations)
      float x = np - phase_memory;
      phase_memory = np;
      x = x > 1 ? x - 2 : x < -1 ? x + 2 : x; // reduce to -1 to +1
      baseband[n] = x;
    }
    if(chan->sig.snr < 20 && chan->fm.threshold) { // take 13 dB as "full quieting"
      // Experimental threshold reduction (popcorn/click suppression)
#if 0
      float const noise_thresh = (0.4f * avg_amp);
      float const noise_reduct_scale = 1 / noise_thresh;

      for(int n=0; n < N; n++){
	if(amplitudes[n] < noise_thresh)
	  baseband[n] *= amplitudes[n] * noise_reduct_scale; // Reduce amplitude of weak RF samples
      }
#elif 1
      // New experimental algorithm 2/2023
      // Find segments of low amplitude, look for clicks within them, and replace with interpolated values
      // doesn't yet handle bad samples at beginning and end of buffer, but this gets most of them
      float const nthresh = 0.4 * avg_amp;
      
      // start scan at 1 so we can use the 0th sample as the start if necessary
      for(int i=1; i < N; i++){
	// find i = first weak sample
	if(amplitudes[i] < nthresh){ // each baseband sample i depends on IF samples i-1 and i
	  badsegments++;
	  float const start = baseband[i-1]; // Last good value before bad segment
	  // Find next good sample
	  int j;
	  float finish = 0; // default if we can't find a good sample
	  int steps = N - i + 1;
	  for(j=i+2 ; j < N; j++){	 // If amplitude[i] is weak, then both baseband[i] and baseband[i+1] will be bad
	    // find j = good sample after bad segment
	    if(amplitudes[j-1] >= nthresh && amplitudes[j] >= nthresh){ // each baseband sample j depends on IF samples j-1 and j
	      finish = baseband[j];
	      steps = j - i + 1;
	      break;
	    }
	  }
	  // Is a click present in the weak segment?
	  float phase_change = 0;
	  for(int k=0; k < steps-1; k++)
	    phase_change += fabsf(baseband[i+k]);
	  
	  if(fabsf(phase_change) >= 1.0){
	    // Linear interpolation
	    float const increment = (finish - start) / steps;
	    for(int k=0; k < steps-1; k++)
	      baseband[i+k] = baseband[i+k-1] + increment; // also why i starts at 1
	    
	    badsamples += steps-1;
	  }
	  i = j; // advance so increment will test the next sample after the last we know is good
	}
      }
#else
      // Simple blanker
      for(int n=0; n < N; n++){
	if(fabsf(baseband[n]) > 0.5f)
	  baseband[n] = 0;
      }
#endif      
    }
    chan->tp1 = badsegments;
    chan->tp2 = badsamples;

    if(squelch_state == squelch_state_max){
      // Squelch fully open; look at deviation peaks
      float peak_positive_deviation = 0;
      float peak_negative_deviation = 0;   // peak neg deviation
      float frequency_offset = 0;      // Average frequency
      
      for(int n=0; n < N; n++){
	frequency_offset += baseband[n];
	if(baseband[n] > peak_positive_deviation)
	  peak_positive_deviation = baseband[n];
	else if(baseband[n] < peak_negative_deviation)
	  peak_negative_deviation = baseband[n];
      }
      frequency_offset *= chan->output.samprate * 0.5f * one_over_olen;  // scale to Hz
      // Update frequency offset and peak deviation, with smoothing to attenuate PL tones
      // alpha = blocktime in millisec is an approximation to a 1 sec time constant assuming blocktime << 1 sec
      // exact value would be 1 - exp(-blocktime/tc)
      float const alpha = .001f * Blocktime;
      chan->sig.foffset += alpha * (frequency_offset - chan->sig.foffset);
      
      // Remove frequency offset from deviation peaks and scale to full cycles
      peak_positive_deviation *= chan->output.samprate * 0.5f;
      peak_negative_deviation *= chan->output.samprate * 0.5f;
      peak_positive_deviation -= chan->sig.foffset;
      peak_negative_deviation -= chan->sig.foffset;
      chan->fm.pdeviation = max(peak_positive_deviation,-peak_negative_deviation);
    }
    if(chan->fm.tone_freq != 0){
      // PL/CTCSS tone squelch
      // use samples before de-emphasis and gain scaling
      if(squelch_state == squelch_state_max){
	for(int n=0; n < N; n++)
	  update_goertzel(&chan->fm.tone_detect,baseband[n]);
	
	pl_sample_count += N;
	if(pl_sample_count >= pl_integrate_samples){
	  // Peak deviation of PL tone in Hz
	  // Not sure the calibration is correct
	  chan->fm.tone_deviation = 2 * chan->output.samprate * cabsf(output_goertzel(&chan->fm.tone_detect)) / pl_sample_count;
	  pl_sample_count = 0;
	  reset_goertzel(&chan->fm.tone_detect);
	  tone_mute = chan->fm.tone_deviation < 250 ? true : false;
	}
      } else
	tone_mute = true; // No squelch tail when tone decoding is active
      if(tone_mute){
	send_output(chan,NULL,N,true); // Keep track of timestamps and mute state
	continue;
      }
    }
    if(chan->fm.rate != 0){
      // Apply de-emphasis if configured
      for(int n=0; n < N; n++){
	float s = deemph_state += chan->fm.rate * (chan->fm.gain * baseband[n] - deemph_state);
	// Experimental DC removal for carrier frequency offsets
	baseband[n] = s - (dc += dc_rate * (s - dc)); // note assigment to dc
      }
    }
    // Compute audio output level
    // Constant gain used by FM only; automatically adjusted by AGC in linear modes
    // We do this in the loop because BW can change
    // Force reasonable parameters if they get messed up or aren't initialized
    chan->output.gain = (2 * chan->output.headroom *  chan->output.samprate) / fabsf(chan->filter.min_IF - chan->filter.max_IF);
    
    float output_level = 0;
    for(int n=0; n < N; n++){
      baseband[n] *= chan->output.gain;
      output_level += baseband[n] * baseband[n];
    }
    output_level *= one_over_olen;
    chan->output.energy += output_level;
    if(send_output(chan,baseband,N,false) < 0)
      break; // no valid output stream; terminate!
  }
  return NULL;
}
