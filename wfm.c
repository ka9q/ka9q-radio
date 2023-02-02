// $Id: wfm.c,v 1.34 2022/06/14 07:38:23 karn Exp $
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

static const int squelchzeroes = 2; // Frames of PCM zeroes after squelch closes, to flush downstream filters (eg, packet)

// Forced sample rates; config file values are ignored for now
// The audio output sample rate can probably eventually be made configurable,
// but the composite sample rate needs to handle the bandwidth
int const Composite_samprate = 384000;
float const Audio_samprate = 48000;

// FM demodulator thread
void *demod_wfm(void *arg){
  assert(arg != NULL);
  struct demod * demod = arg;

  {
    char name[100];
    snprintf(name,sizeof(name),"wfm %u",demod->output.rtp.ssrc);
    pthread_setname(name);
  }

  // Set null here in case we quit early and try to free them
  struct filter_in *composite = NULL;
  struct filter_out *mono = NULL;
  struct filter_out *stereo = NULL;
  struct filter_out *pilot = NULL;

  complex float state = 0;  // Demodulator input phase memory

  // NB: this is the sample rate from the FM demodulator, which is much faster than the actual audio output sample rate
  // forced to be fast enough for 200 kHz broadcast channel
  demod->output.samprate = Composite_samprate;

  if(demod->output.channels == 0)
    demod->output.channels = 2; // Default to stereo

  int const blocksize = demod->output.samprate * Blocktime / 1000;
  delete_filter_output(&demod->filter.out);
  demod->filter.out = create_filter_output(Frontend.in,NULL,blocksize,COMPLEX);
  if(demod->filter.out == NULL){
    fprintf(stdout,"unable to create filter for ssrc %lu\n",(unsigned long)demod->output.rtp.ssrc);
    free_demod(&demod);
    return NULL;
  }
  set_filter(demod->filter.out,
	     demod->filter.min_IF/demod->output.samprate,
	     demod->filter.max_IF/demod->output.samprate,
	     demod->filter.kaiser_beta);

  float lastaudio = 0; // state for impulse noise removal
  int squelch_state = 0; // Number of blocks for which squelch remains open

  // Make these blocksizes depend on front end sample rate and blocksize
  int const composite_L = roundf(demod->output.samprate * Blocktime * .001); // Intermediate sample rate
  int const composite_M = composite_L + 1; // 2:1 overlap (50%)
  int const composite_N = composite_L + composite_M - 1;

  // output forced to 48 kHz for now
  const int audio_L = roundf(Audio_samprate * Blocktime * .001);

  // Composite signal 50 Hz - 15 kHz contains mono (L+R) signal
  if((composite = create_filter_input(composite_L,composite_M,REAL)) == NULL)
    goto quit;

  assert(composite->ilen == demod->filter.out->olen);

  if(composite_L < audio_L)
    goto quit; // Front end sample rate is too low - should probably fix filter to allow interpolation

  // Composite filters, decimate from 384 Khz to 48 KHz
  if((mono = create_filter_output(composite,NULL,audio_L, REAL)) == NULL)
    goto quit;

  set_filter(mono,50.0/Audio_samprate, 15000.0/Audio_samprate, demod->filter.kaiser_beta);

  // Narrow filter at 19 kHz for stereo pilot
  if((pilot = create_filter_output(composite,NULL,audio_L, COMPLEX)) == NULL)
    goto quit;

  // FCC says +/- 2 Hz, with +/- 20 Hz protected (73.322)
  set_filter(pilot,-20./Audio_samprate, 20./Audio_samprate, demod->filter.kaiser_beta);

  // Stereo difference (L-R) information on DSBSC carrier at 38 kHz
  // Extends +/- 15 kHz around 38 kHz
  if((stereo = create_filter_output(composite,NULL,audio_L, COMPLEX)) == NULL)
    goto quit;

  set_filter(stereo,-15000./Audio_samprate, 15000./Audio_samprate, demod->filter.kaiser_beta);

  // The asserts should be valid for clean sample rates multiples of 50/100 Hz (20/10 ms)
  // If not, then a mop-up oscillator has to be provided
  int pilot_shift;
  double pilot_remainder;
  compute_tuning(composite_N,composite_M,Composite_samprate,&pilot_shift,&pilot_remainder,19000.);
  assert((pilot_shift % 4) == 0 && pilot_remainder == 0);

  int subc_shift;
  double subc_remainder;
  compute_tuning(composite_N,composite_M,Composite_samprate,&subc_shift,&subc_remainder,38000.);
  assert((subc_shift % 4) == 0 && subc_remainder == 0);

  realtime();

  while(!demod->terminate){
    if(downconvert(demod) == -1)
      break;

    // Constant gain used by FM only; automatically adjusted by AGC in linear modes
    // We do this in the loop because headroom and BW can change
    // Force reasonable parameters if they get messed up or aren't initialized
    demod->output.gain = (demod->output.headroom *  M_1_PI * demod->output.samprate) / fabsf(demod->filter.min_IF - demod->filter.max_IF);

    // Find average amplitude and variance for SNR estimation
    // Use two passes to avoid possible numerical problems
    float amplitudes[composite_L];
    complex float * const buffer = demod->filter.out->output.c;
    float avg_amp = 0;
    for(int n=0; n < composite_L; n++){
      //      avg_amp += amplitudes[n] = approx_magf(buffer[n]);
      avg_amp += amplitudes[n] = cabsf(buffer[n]); // May give more accurate SNRs
    }
    avg_amp /= composite_L;

    // Second pass over amplitudes to compute variance
    float fm_variance = 0;
    for(int n=0; n < composite_L; n++)
      fm_variance += (amplitudes[n] - avg_amp) * (amplitudes[n] - avg_amp);

    fm_variance /= (composite_L - 1);

    const float snr = fm_snr(avg_amp*avg_amp/fm_variance);
    demod->sig.snr = max(0.0f,snr); // Smoothed values can be a little inconsistent

    // Hysteresis squelch
    int const squelch_state_max = squelchzeroes + demod->squelchtail + 1;
    if(demod->sig.snr >= demod->squelch_open
       || (squelch_state > squelchzeroes && snr >= demod->squelch_close))
      // Squelch is fully open
      // tail timing is in blocks (usually 10 or 20 ms each)
      squelch_state = squelch_state_max;
    else if(squelch_state > 0)
      squelch_state--; // Squelch closing
    else
      squelch_state = 0; // Squelch closed

    if(squelch_state >= squelchzeroes){ // Squelch is not completely closed
      // Actual FM demodulation
      float peak_positive_deviation = 0;
      float peak_negative_deviation = 0;
      float frequency_offset = 0;
      float output_level = 0;

      for(int n=0; n < composite_L; n++){
	// Although deviation can be zero, argf() is defined as returning 0, not NAN
	float const deviation = cargf(buffer[n] * conjf(state));
	state = buffer[n]; // Remember for next sample
	// If state ever goes NaN, it will permanently pollute the output stream!
	if(isnan(__real__ state) || isnan(__imag__ state)){
	  fprintf(stdout,"NaN state!\n");
	  state = 0;
	}
	if(squelch_state == squelch_state_max){
	  // Remember deviation peaks only when squelch is fully open, not during tail
	  frequency_offset += deviation; // Direct FM for frequency measurement
	  if(deviation > peak_positive_deviation)
	    peak_positive_deviation = deviation;
	  else if(deviation < peak_negative_deviation)
	    peak_negative_deviation = deviation;
	}
	assert(!isnan(composite->input.r[n])); // Shouldn't happen, but that's what asserts are for
	composite->input.r[n] = deviation * demod->output.gain; // Straight FM, no threshold extension
	output_level += composite->input.r[n] * composite->input.r[n];
      } // for(int n=0; n < composite_L; n++){
      lastaudio = composite->input.r[composite_L - 1]; // Starting point for soft decay if squelch closes
      demod->output.level = output_level / composite_L;

      if(squelch_state == squelch_state_max){
	frequency_offset /= composite_L;  // Average FM output is freq offset
	// Update frequency offset and peak deviation
	float const offset = demod->output.samprate  * frequency_offset * M_1_2PI;
	if(!isfinite(demod->sig.foffset))
	  demod->sig.foffset = offset;
	else
	  demod->sig.foffset += (offset - demod->sig.foffset) * .01; // Smooth it down
      
	// Remove frequency offset from deviation peaks and scale
	peak_positive_deviation -= frequency_offset;
	peak_negative_deviation -= frequency_offset;
	// Fast attack, slow decay
	float const peak = demod->output.samprate * max(peak_positive_deviation,-peak_negative_deviation) * M_1_2PI;
	if(!isfinite(demod->fm.pdeviation) || peak >= demod->fm.pdeviation)
	  demod->fm.pdeviation = peak;
	else
	  demod->fm.pdeviation -= demod->fm.pdeviation * .01;
      }
    } else if(squelch_state >= 0){ // Squelch closed, but emitting padding
      // Exponentially decay padding to avoid squelch-closing thump
      // Does this really work?
      state = 0; // Soft-open squelch next time
      for(int n=0; n < composite_L; n++){
	lastaudio *= 0.9999; // empirical - compute this more rigorously
	composite->input.r[n] = lastaudio;
      }
      demod->output.level = 0;
    }
    // Filter & decimate to audio output sample rate
    execute_filter_input(composite);  // Composite at 384 kHz
    execute_filter_output(mono,0);    // L+R composite at 48 kHz
    if(demod->output.channels == 2){
      // Stereo multiplex processing
      // shift signs for pilot and subcarrier don't matter because the filters are real input with symmetric spectra
      execute_filter_output(pilot,pilot_shift); // pilot spun to 0 Hz, 48 kHz rate
      execute_filter_output(stereo,subc_shift); // L-R composite spun down to 0 Hz, 48 kHz rate

      float complex stereo_buffer[audio_L];
      for(int n= 0; n < audio_L; n++){
	float complex s;
	__imag__ s = __real__ s = mono->output.r[n];
	assert(!isnan(__real__ s));
	assert(!isnan(__imag__ s));	

	// I really need a better pilot detector here so we'll switch back to mono without it
	// Probably lock a PLL to it and look at the inphase/quadrature power ratio
	complex float subc_phasor = pilot->output.c[n]; // 19 kHz pilot
	//	float subc_mag = approx_magf(subc_phasor);
	float subc_mag = cabsf(subc_phasor);
	if(subc_mag > .001){ // Hack!!
	  subc_phasor *= subc_phasor;       // double to 38 kHz
	  subc_phasor /= subc_mag * subc_mag;  // and normalize
	  float subc_info = __imag__ (conjf(subc_phasor) * stereo->output.c[n]); // Carrier is in quadrature

	  __real__ s += subc_info; // Left channel
	  __imag__ s -= subc_info; // Right channel
	}
	assert(!isnan(__real__ s));
	assert(!isnan(__imag__ s));	
	if(demod->deemph.rate != 0){
	  assert(!isnan(__real__ demod->deemph.state));
	  assert(!isnan(__imag__ demod->deemph.state));
	  demod->deemph.state *= demod->deemph.rate;
	  demod->deemph.state += demod->deemph.gain * (1 - demod->deemph.rate) * s;
	  stereo_buffer[n] = demod->deemph.state;
	} else {
	  stereo_buffer[n] = s;
	}
      }
      if(send_stereo_output(demod,(const float *)stereo_buffer,audio_L,squelch_state < 0) < 0)
	break; // No output stream! Terminate
    } else {
      // Mono
      if(demod->deemph.rate != 0){
	// Apply deemphasis
	assert(!isnan(__real__ demod->deemph.state));
	for(int n=0; n < audio_L; n++){
	  float s = mono->output.r[n];
	  __real__ demod->deemph.state *= demod->deemph.rate;
	  __real__ demod->deemph.state += demod->deemph.gain * (1 - demod->deemph.rate) * s; 
	  mono->output.r[n] = __real__ demod->deemph.state;
	}
      }
      // mute output unless time is left on the squelch_state timer
      if(send_mono_output(demod,mono->output.r,audio_L,squelch_state < 0) < 0)
	break; // No output stream! Terminate
    }
  } // while(!demod->terminate)
 quit:;
  delete_filter_output(&mono);
  delete_filter_output(&stereo);
  delete_filter_output(&pilot);
  delete_filter_input(&composite);
  if(demod->filter.energies)
    free(demod->filter.energies);
  demod->filter.energies = NULL;
  delete_filter_output(&demod->filter.out);
  return NULL;
}
