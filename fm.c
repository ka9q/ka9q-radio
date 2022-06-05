// $Id: fm.c,v 1.130 2022/06/05 02:28:22 karn Exp karn $
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
  int const N = demod->filter.out->olen;
  float const one_over_olen = 1. / N; // save some divides

  while(!demod->terminate){
    if(downconvert(demod) == -1) // received terminate
      break;

    // Constant gain used by FM only; automatically adjusted by AGC in linear modes
    // We do this in the loop because BW can change
    // Force reasonable parameters if they get messed up or aren't initialized
    demod->output.gain = (demod->output.headroom *  M_1_PI * demod->output.samprate) / fabsf(demod->filter.min_IF - demod->filter.max_IF);

    float bb_power = 0;
    float avg_amp = 0;
    float amplitudes[N];
    complex float * const buffer = demod->filter.out->output.c; // for convenience

    for(int n = 0; n < N; n++){
      // Apply frequency shifts
      complex float s = buffer[n] * step_osc(&demod->fine);
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

    {
      // Compute signal-to-noise, see if we should open the squelch
      float const snr = fm_snr(avg_amp*avg_amp * (N-1) / fm_variance);
      demod->sig.snr = max(0.0f,snr); // Smoothed values can be a little inconsistent
    }

    // Hysteresis squelch
    int const squelch_state_max = squelchzeroes + demod->squelchtail + 1;
    if(demod->sig.snr >= demod->squelch_open
       || (squelch_state > squelchzeroes && demod->sig.snr >= demod->squelch_close))
      // Squelch is fully open
      // tail timing is in blocks (usually 10 or 20 ms each)
      squelch_state = squelch_state_max;
    else if(squelch_state > 0)
      squelch_state--;
    else
      squelch_state = 0;

    float baseband[N];    // Demodulated FM baseband
    if(squelch_state > squelchzeroes){ // Squelch is (still) open
      // Actual FM demodulation
      float peak_positive_deviation = 0;
      float peak_negative_deviation = 0;   // peak neg deviation
      float frequency_offset = 0;      // Average frequency
      float output_level = 0;

      for(int n=0; n < N; n++){
	// Although deviation can be zero, argf() is defined as returning 0, not NAN
	float const deviation = cargf(buffer[n] * conjf(state));
	state = buffer[n];
	// If state ever goes NaN, it will permanently pollute the output stream!
	if(isnan(__real__ state) || isnan(__imag__ state)){
	  fprintf(stdout,"NaN state!\n");
	  state = 0;
	}
	if(squelch_state == squelch_state_max){
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
      demod->output.level = output_level;
      if(squelch_state == squelch_state_max){
	frequency_offset *= one_over_olen;  // Average FM output is freq offset
	// Update frequency offset and peak deviation
	demod->sig.foffset = demod->output.samprate  * frequency_offset * M_1_2PI;
	
	// Remove frequency offset from deviation peaks and scale
	peak_positive_deviation -= frequency_offset;
	peak_negative_deviation -= frequency_offset;
	demod->fm.pdeviation = demod->output.samprate * max(peak_positive_deviation,-peak_negative_deviation) * M_1_2PI;
      }
    } else if(squelch_state > 0){ // Squelch closed, but emitting padding
      state = 0; // Soft-open squelch next time
      memset(baseband,0,sizeof(baseband));
    }

    // mute output unless time is left on the squelch_state timer
    if(send_mono_output(demod,baseband,N,squelch_state <= 0) < 0)
      break; // no valid output stream; terminate!

  } // while(!demod->terminate)
 quit:;
  if(demod->filter.energies)
    free(demod->filter.energies);
  demod->filter.energies = NULL;
  delete_filter_output(&demod->filter.out);
  return NULL;
}
