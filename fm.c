// $Id: fm.c,v 1.137 2022/06/23 22:13:29 karn Exp karn $
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
static int const power_squelch = 1; // Enable experimental pre-squelch to save CPU on idle channels

// FM demodulator thread
void *demod_fm(void *arg){
  assert(arg != NULL);
  struct demod * const demod = arg;  
  
  {
    char name[100];
    snprintf(name,sizeof(name),"fm %u",demod->output.rtp.ssrc);
    pthread_setname(name);
  }

  complex float sample_memory = 0;
  demod->output.channels = 1; // Only mono for now
  if(isnan(demod->squelch_open) || demod->squelch_open == 0)
    demod->squelch_open = 6.3;  // open above ~ +8 dB
  if(isnan(demod->squelch_close) || demod->squelch_close == 0)
    demod->squelch_close = 4; // close below ~ +6 dB

  int const blocksize = demod->output.samprate * Blocktime / 1000.0F;
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
  
  if(demod->fm.tone_freq != 0){
    // Set up PL tone squelch
    init_goertzel(&demod->fm.tonedetect,demod->fm.tone_freq/demod->output.samprate);
  }

  float deemph_state = 0;
  int squelch_state = 0; // Number of blocks for which squelch remains open
  int const N = demod->filter.out->olen;
  float const one_over_olen = 1.0F / N; // save some divides
  int const pl_integrate_samples = demod->output.samprate * 0.24; // 240 milliseconds (spec is < 250 ms)
  int pl_sample_count = 0;
  bool tone_mute = true; // When tone squelch enabled, mute until the tone is detected

  while(!demod->terminate){
    if(downconvert(demod) == -1) // received terminate
      break;

    if(power_squelch && squelch_state == 0){
      // quick check SNR from raw signal power to save time on variance-based squelch
      // Variance squelch is still needed to suppress various spurs and QRM
      float const snr = (demod->sig.bb_power / (demod->sig.n0 * fabsf(demod->filter.max_IF - demod->filter.min_IF))) - 1.0f;
      if(snr < demod->squelch_close){
	// squelch closed, reset everything and mute output
	sample_memory = 0;
	squelch_state = 0;
	pl_sample_count = 0;
	reset_goertzel(&demod->fm.tonedetect);
	send_mono_output(demod,NULL,N,true); // Keep track of timestamps and mute state
	continue;
      }
    }
    complex float const * const buffer = demod->filter.out->output.c; // for convenience
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
      demod->sig.snr = max(0.0f,snr); // Smoothed values can be a little inconsistent
    }
    // Hysteresis squelch
    int const squelch_state_max = demod->squelchtail + 1;
    if(demod->sig.snr >= demod->squelch_open
       || (squelch_state > 0 && demod->sig.snr >= demod->squelch_close)){
      // Squelch is fully open
      // tail timing is in blocks (usually 10 or 20 ms each)
      squelch_state = squelch_state_max;
    } else if(--squelch_state > 0) {
      // In tail, squelch still open
    } else {
      // squelch closed, reset everything and mute output
      sample_memory = 0;
      squelch_state = 0;
      pl_sample_count = 0;
      reset_goertzel(&demod->fm.tonedetect);
      send_mono_output(demod,NULL,N,true); // Keep track of timestamps and mute state
      continue;
    }
    float baseband[N];    // Demodulated FM baseband
    // Actual FM demodulation
    baseband[0] = cargf(buffer[0] * conjf(sample_memory));
    for(int n=1; n < N; n++)
      baseband[n] = cargf(buffer[n] * conjf(buffer[n-1]));
    sample_memory = buffer[N-1];
    
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
      frequency_offset *= one_over_olen;  // Average FM output is freq offset
      // Update frequency offset and peak deviation, with smoothing to attenuate PL tones
      // alpha = blocktime in millisec is an approximation to a 1 sec time constant assuming blocktime << 1 sec
      // exact value would be 1 - exp(-blocktime/tc)
      float const alpha = .001f * Blocktime;
      demod->sig.foffset += alpha * (demod->output.samprate  * frequency_offset * M_1_2PI - demod->sig.foffset);
      
      // Remove frequency offset from deviation peaks and scale
      peak_positive_deviation *= demod->output.samprate * M_1_2PI;
      peak_negative_deviation *= demod->output.samprate * M_1_2PI;      
      peak_positive_deviation -= demod->sig.foffset;
      peak_negative_deviation -= demod->sig.foffset;
      demod->fm.pdeviation = max(peak_positive_deviation,-peak_negative_deviation);
    }
    if(demod->sig.snr < 20) { // take 13 dB as "full quieting"
      // Experimental threshold reduction (pop/click suppression)
      float const noise_thresh = (0.4F * avg_amp);
      float const noise_reduct_scale = 1 / noise_thresh;

      for(int n=0; n < N; n++){
	if(amplitudes[n] < noise_thresh)
	  baseband[n] *= amplitudes[n] * noise_reduct_scale; // Reduce amplitude of weak RF samples
      }
    }
    if(demod->fm.tone_freq != 0){
      // PL/CTCSS tone squelch
      // use samples before de-emphasis and gain scaling
      if(squelch_state == squelch_state_max){
	for(int n=0; n < N; n++)
	  update_goertzel(&demod->fm.tonedetect,baseband[n]);
	
	pl_sample_count += N;
	if(pl_sample_count >= pl_integrate_samples){
	  // Peak deviation of PL tone in Hz
	  // Not sure the calibration is correct
	  demod->fm.tone_deviation = 2 * M_1_PI * demod->output.samprate * cabsf(output_goertzel(&demod->fm.tonedetect)) / pl_sample_count;
	  pl_sample_count = 0;
	  reset_goertzel(&demod->fm.tonedetect);
	  tone_mute = demod->fm.tone_deviation < 250 ? true : false;
	}
      } else
	tone_mute = true; // No squelch tail when tone decoding is active
      if(tone_mute){
	send_mono_output(demod,NULL,N,true); // Keep track of timestamps and mute state
	continue;
      }
    }
    if(demod->deemph.rate != 0){
      // Apply de-emphasis if configured
      float const r = 1 - demod->deemph.rate;
      for(int n=0; n < N; n++){
	deemph_state += r * (baseband[n] - deemph_state);
	baseband[n] = deemph_state * demod->deemph.gain;
      }
    }
    // Compute audio output level
    // Constant gain used by FM only; automatically adjusted by AGC in linear modes
    // We do this in the loop because BW can change
    // Force reasonable parameters if they get messed up or aren't initialized
    demod->output.gain = (demod->output.headroom *  M_1_PI * demod->output.samprate) / fabsf(demod->filter.min_IF - demod->filter.max_IF);
    
    float output_level = 0;
    for(int n=0; n < N; n++){
      baseband[n] *= demod->output.gain;
      output_level += baseband[n] * baseband[n];
    }
    output_level *= one_over_olen;
    demod->output.level = output_level;
    if(send_mono_output(demod,baseband,N,false) < 0)
      break; // no valid output stream; terminate!
  } // while(!demod->terminate)
 quit:;
  if(demod->filter.energies)
    free(demod->filter.energies);
  demod->filter.energies = NULL;
  delete_filter_output(&demod->filter.out);
  return NULL;
}
