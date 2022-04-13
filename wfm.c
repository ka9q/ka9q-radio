// $Id: wfm.c,v 1.22 2021/11/19 06:42:22 karn Exp $
// Wideband FM demodulation and squelch
// Adapted from narrowband demod
// Copyright 2020, Phil Karn, KA9Q
#define _GNU_SOURCE 1
#include <assert.h>
#include <pthread.h>
#include <string.h>
#include <math.h>
#include <complex.h>
#include <fftw3.h>

#include "misc.h"
#include "filter.h"
#include "iir.h"
#include "radio.h"
#include "status.h"

static const int squelchtail = 1; // Frames to hold open after loss of SNR
static const int squelchzeroes = 2; // Frames of PCM zeroes after squelch closes, to flush downstream filters (eg, packet)

// FM demodulator thread
void *demod_wfm(void *arg){
  assert(arg != NULL);
  struct demod * const demod = arg;

  {
    char name[100];
    snprintf(name,sizeof(name),"wfm %u",demod->output.rtp.ssrc);
    pthread_setname(name);
  }

  complex float state = 0;
  if(demod->output.samprate == 0)
    demod->output.samprate = 384000; // fast enough for 200 kHz broadcast channel

  if(demod->output.channels == 0)
    demod->output.channels = 1; // Default to mono

  float lastaudio = 0; // state for impulse noise removal
  int squelch_state = 0; // Number of blocks for which squelch remains open

  demod->output.gain = (demod->output.headroom *  M_1_PI * demod->output.samprate) / fabsf(demod->filter.min_IF - demod->filter.max_IF);
  
  // Make these blocksizes depend on front end sample rate and blocksize
  int const L = demod->output.samprate * Blocktime * .001; // Intermediate sample rate
  int const M = L + 1;

  const float audio_samprate = 48000;
  const int audio_N = roundf(audio_samprate * Blocktime * .001);

  // Baseband signal 50 Hz - 15 kHz contains mono (L+R) signal
  struct filter_in * const baseband = create_filter_input(L,M,REAL);
  if(baseband == NULL)
    return NULL;
  int const baseband_N = baseband->ilen; // Sample count in intermediate (384 kHz sample rate)

  if(baseband_N < audio_N)
    return NULL; // Front end sample rate is too low - should probably fix filter to allow interpolation

  // Baseband filters, decimate from 300 Khz to 48 KHz
  struct filter_out * const mono = create_filter_output(baseband,NULL,audio_N, REAL);
  if(mono == NULL)
    return NULL;
  set_filter(mono,50.0/audio_samprate, 15000.0/audio_samprate, demod->filter.kaiser_beta);

  // Narrow filter at 19 kHz for stereo pilot
  struct filter_out * const pilot = create_filter_output(baseband,NULL,audio_N, COMPLEX);
  if(pilot == NULL)
    return NULL;
  set_filter(pilot,-100./audio_samprate, 100./audio_samprate, demod->filter.kaiser_beta);

  // Stereo difference (L-R) information on DSBSC carrier at 38 kHz
  // Extends +/- 15 kHz around 38 kHz
  struct filter_out * const stereo = create_filter_output(baseband,NULL,audio_N, COMPLEX);
  if(stereo == NULL)
    return NULL;
  set_filter(stereo,-15000./audio_samprate, 15000./audio_samprate, demod->filter.kaiser_beta);

  // Assume the remainder is zero, as it is for clean sample rates multiples of 50/100 Hz (20/10 ms)
  // If not, then a mop-up oscillator has to be provided
  int pilot_flip, pilot_rotate;
  double pilot_remainder;
  compute_tuning(demod,&pilot_flip,&pilot_rotate,&pilot_remainder,19000.);

  int subc_flip, subc_rotate;
  double subc_remainder;
  compute_tuning(demod,&subc_flip,&subc_rotate,&subc_remainder,38000.);

  struct osc fine;
  memset(&fine,0,sizeof(fine));

  while(!demod->terminate){
    if(demod->tune.freq == 0){
      // Special case: idle mode
      execute_filter_output_idle(demod->filter.out);
      continue;
    }
    assert(baseband->ilen == demod->filter.out->olen);

    // Note: tune.shift ignored in FM mode
    demod->tune.second_LO = Frontend.sdr.frequency - demod->tune.freq;
    double freq = demod->tune.doppler + demod->tune.second_LO; // Total logical oscillator frequency
    double remainder;
    int rotate,flip;
    compute_tuning(demod,&flip,&rotate,&remainder,freq);

    // set before execute_filter blocks
    set_osc(&fine,remainder, demod->tune.doppler_rate);

    // Wait for next block of frequency domain data
    execute_filter_output(demod->filter.out,-rotate);

    // Apply frequency shifts
    for(int n=0; n<baseband_N; n++)
      demod->filter.out->output.c[n] *= flip * step_osc(&fine);

    // Constant gain used by FM only; automatically adjusted by AGC in linear modes
    // We do this in the loop because headroom and BW can change
    // Force reasonable parameters if they get messed up or aren't initialized
    if(!isfinite(demod->output.gain) || demod->output.gain <= 0)
      demod->output.gain = (demod->output.headroom *  M_1_PI * demod->output.samprate) / fabsf(demod->filter.min_IF - demod->filter.max_IF);

    // Find average amplitude and variance for SNR estimation
    // Use two passes to avoid possible numerical problems
    float amplitudes[baseband_N];
    demod->sig.bb_power = 0;
    float avg_amp = 0;
    for(int n=0; n < baseband_N; n++){
      float t = cnrmf(demod->filter.out->output.c[n]);
      demod->sig.bb_power += t;
      avg_amp += amplitudes[n] = sqrtf(t);
    }
    demod->sig.bb_power /= baseband_N;
    avg_amp /= baseband_N;

    float fm_variance = 0;
    for(int n=0; n < baseband_N; n++)
      fm_variance += (amplitudes[n] - avg_amp) * (amplitudes[n] - avg_amp);

    fm_variance /= (baseband_N - 1);

    const float snr = fm_snr(avg_amp*avg_amp/fm_variance);
    demod->sig.snr = max(0.0f,snr); // Smoothed values can be a little inconsistent

    float const thresh = 4; // +6dB?
    if(demod->sig.snr > thresh) // Temp force open squelch
      // tail timing is in blocks (usually 20 ms each)
      squelch_state = squelchzeroes + squelchtail;
    else if(squelch_state >= 0)
      squelch_state--;

    if(squelch_state >= squelchzeroes){ // Squelch is (still) open
      // Actual FM demodulation
      float pdev_pos = 0;
      float pdev_neg = 0;
      float avg_f = 0;

      for(int n=0; n < baseband_N; n++){
	complex float p = demod->filter.out->output.c[n] * conjf(state);
	// Although p can be zero, argf() is defined as returning 0, not NAN
	float const ang = cargf(p);
	state = demod->filter.out->output.c[n];

	if(demod->sig.snr > thresh){
	  // Perform only when squelch is fully open, not during tail
	  avg_f += ang; // Direct FM for frequency measurement
	  if(ang > pdev_pos)
	    pdev_pos = ang;
	  else if(ang < pdev_neg)
	    pdev_neg = ang;
	}
	baseband->input.r[n] = ang * demod->output.gain; // Straight FM, no threshold extension
      } // for(int n=0; n < baseband_N; n++){
      lastaudio = baseband->input.r[baseband_N - 1]; // Starting point for soft decay if squelch closes

      if(demod->sig.snr > thresh){
	avg_f /= baseband_N;  // Average FM output is freq offset
	// Update frequency offset and peak deviation
	float const offset = demod->output.samprate  * avg_f * M_1_2PI;
	if(!isfinite(demod->sig.foffset))
	  demod->sig.foffset = offset;
	else
	  demod->sig.foffset += (offset - demod->sig.foffset) * .002; // Smooth it down
      
	// Remove frequency offset from deviation peaks and scale
	pdev_pos -= avg_f;
	pdev_neg -= avg_f;
	// Fast attack, slow decay
	float const peak = demod->output.samprate * max(pdev_pos,-pdev_neg) * M_1_2PI;
	if(!isfinite(demod->fm.pdeviation) || peak > demod->fm.pdeviation)
	  demod->fm.pdeviation = peak;
	else
	  demod->fm.pdeviation += (offset - demod->fm.pdeviation) * .002;
      }
      float output_level = 0;
      for(int n=0; n < baseband_N; n++)
	output_level += baseband->input.r[n] * baseband->input.r[n];
      demod->output.level = output_level / baseband_N;
    } else if(squelch_state >= 0){ // Squelch closed, but emitting padding
      // Exponentially decay padding to avoid squelch-closing thump
      state = 0; // Soft-open squelch next time
      for(int n=0; n < baseband_N; n++){
	lastaudio *= 0.9999; // empirical - compute this more rigorously
	baseband->input.r[n] = lastaudio;
      }
      demod->output.level = 0;
    }
    // Decimate to audio sample rate, do stereo processing
    execute_filter_input(baseband);    // Composite at 384 kHz
    execute_filter_output(mono,0);    // L+R baseband at 48 kHz
    if(demod->output.channels == 2){
      execute_filter_output(pilot,pilot_rotate); // pilot spun to 0 Hz, 48 kHz rate
      execute_filter_output(stereo,subc_rotate); // L-R baseband spun down to 0 Hz, 48 kHz rate

      float complex stereo_buffer[audio_N];
      for(int n= 0; n < audio_N; n++){
	complex float subc_phasor = pilot->output.c[n]; // 19 kHz pilot
	subc_phasor *= subc_phasor;       // double to 38 kHz
	subc_phasor /= approx_magf(subc_phasor);  // and normalize
	float subc_info = __imag__ (conjf(subc_phasor) * stereo->output.c[n]); // Carrier is in quadrature
	__real__ stereo_buffer[n] = mono->output.r[n] + subc_info; // Left channel
	__imag__ stereo_buffer[n] = mono->output.r[n] - subc_info; // Right channel
      }
      if(send_stereo_output(demod,(const float *)stereo_buffer,audio_N,squelch_state < 0) < 0)
	break; // No output stream! Terminate
    } else {
      // Mono
      // mute output unless time is left on the squelch_state timer
      if(send_mono_output(demod,mono->output.r,audio_N,squelch_state < 0) < 0)
	break; // No output stream! Terminate
    }
  } // while(1)
  return NULL;
}
