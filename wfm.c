// Wideband FM demodulation and squelch for ka9q-radio's radiod.
// Adapted from narrowband demod
// Copyright 2020-2023, Phil Karn, KA9Q
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

// Forced sample rates; config file values are ignored for now
// The audio output sample rate can probably eventually be made configurable,
// but the composite sample rate needs to handle the bandwidth
int const Composite_samprate = 384000;
float const Audio_samprate = 48000;

// These could be made settable if needed
static int const power_squelch = 1; // Enable experimental pre-squelch to save CPU on idle channels

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
  struct filter_out *lminusr = NULL;
  struct filter_out *pilot = NULL;

  float phase_memory = 0;  // Demodulator input phase memory

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
  if((lminusr = create_filter_output(composite,NULL,audio_L, COMPLEX)) == NULL)
    goto quit;

  set_filter(lminusr,-15000./Audio_samprate, 15000./Audio_samprate, demod->filter.kaiser_beta);

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

    if(power_squelch && squelch_state == 0){
      // quick check SNR from raw signal power to save time on variance-based squelch
      // Variance squelch is still needed to suppress various spurs and QRM
      float const snr = (demod->sig.bb_power / (demod->sig.n0 * fabsf(demod->filter.max_IF - demod->filter.min_IF))) - 1;
      if(snr < demod->squelch_close){
	// squelch closed, reset everything and mute output
	phase_memory = 0;
	squelch_state = 0;
	continue;
      }
    }

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
    int const squelch_state_max = demod->squelchtail + 1;
    if(demod->sig.snr >= demod->squelch_open
       || (squelch_state > 0 && snr >= demod->squelch_close))
      // Squelch is fully open
      // tail timing is in blocks (usually 10 or 20 ms each)
      squelch_state = squelch_state_max;
    else if(--squelch_state > 0){
      // In tail, squelch still open
    } else {
      squelch_state = 0; // Squelch closed
      phase_memory = 0;
      send_mono_output(demod,NULL,audio_L,true); // Keep track of timestamps and mute state
      continue;
    }
    // Actual FM demodulation
    for(int n=0; n < composite_L; n++){
      // Although deviation can be zero, argf() is defined as returning 0, not NAN
      float np = M_1_PIf * cargf(buffer[n]); // -1 to +1
      float x = np - phase_memory;
      phase_memory = np;
      composite->input_write_pointer.r[n] = x > 1 ? x - 2 : x < -1 ? x + 2 : x; // reduce difference to -1 to +1
    } // for(int n=0; n < composite_L; n++){
    if(squelch_state == squelch_state_max){
      // Squelch fully open; look at deviation peaks
      float peak_positive_deviation = 0;
      float peak_negative_deviation = 0;
      float frequency_offset = 0;
      
      for(int n=0; n < composite_L; n++){
	frequency_offset += composite->input_write_pointer.r[n];
	if(composite->input_write_pointer.r[n] > peak_positive_deviation)
	  peak_positive_deviation = composite->input_write_pointer.r[n];
	else if(composite->input_write_pointer.r[n] < peak_negative_deviation)
	  peak_negative_deviation = composite->input_write_pointer.r[n];
      }
      frequency_offset *= demod->output.samprate * 0.5f / composite_L;  // scale to Hz
      // Update frequency offset and peak deviation, with smoothing to attenuate PL tones
      // alpha = blocktime in millisec is an approximation to a 1 sec time constant assuming blocktime << 1 sec
      // exact value would be 1 - exp(-blocktime/tc)
      float const alpha = .001f * Blocktime;
      demod->sig.foffset += alpha * (frequency_offset - demod->sig.foffset);
      
      // Remove frequency offset from deviation peaks and scale to full cycles
      peak_positive_deviation *= demod->output.samprate * 0.5f;
      peak_negative_deviation *= demod->output.samprate * 0.5f;
      peak_positive_deviation -= demod->sig.foffset;
      peak_negative_deviation -= demod->sig.foffset;
      demod->fm.pdeviation = max(peak_positive_deviation,-peak_negative_deviation);
    }
    // Filter & decimate to audio output sample rate
    execute_filter_input(composite);  // Composite at 384 kHz
    execute_filter_output(mono,0);    // L+R composite at 48 kHz
    // Compute audio output level
    // Constant gain used by FM only; automatically adjusted by AGC in linear modes
    // We do this in the loop because headroom and BW can change
    // Force reasonable parameters if they get messed up or aren't initialized
    demod->output.gain = (2 * demod->output.headroom * demod->output.samprate) / fabsf(demod->filter.min_IF - demod->filter.max_IF);

    bool stereo_on = false;
    if(demod->output.channels == 2){
      // See if a subcarrier is present
      // shift signs for pilot and subcarrier don't matter because the filters are real input with symmetric spectra
      execute_filter_output(pilot,pilot_shift); // pilot spun to 0 Hz, 48 kHz rate
      // I really need a better pilot detector here so we'll switch back to mono without it
      // Probably lock a PLL to it and look at the inphase/quadrature power ratio
      float subc_amp = 0;
      for(int n=0; n < audio_L; n++)
	subc_amp += cnrmf(pilot->output.c[n]);

      subc_amp /= audio_L;
      if(subc_amp > 1e-6) // empirical constant, test this some more
	stereo_on = true;
    }
    if(stereo_on){
      // Stereo multiplex processing
      execute_filter_output(lminusr,subc_shift); // L-R composite spun down to 0 Hz, 48 kHz rate

      float complex stereo_buffer[audio_L];
      float output_level = 0;
      for(int n = 0; n < audio_L; n++){
	complex float subc_phasor = pilot->output.c[n]; // 19 kHz pilot
	subc_phasor = (subc_phasor * subc_phasor) / cnrmf(subc_phasor); // square and normalize
	float subc_info = __imag__ (conjf(subc_phasor) * lminusr->output.c[n]); // Carrier is in quadrature
	assert(!isnan(subc_info));
	assert(!isnan(mono->output.r[n]));
	float complex s = mono->output.r[n] + subc_info + I * (mono->output.r[n] - subc_info);
	if(demod->deemph.rate != 0){
	  assert(!isnan(__real__ demod->deemph.state));
	  assert(!isnan(__imag__ demod->deemph.state));
	  demod->deemph.state *= demod->deemph.rate;
	  s = demod->deemph.state += demod->deemph.gain * (1 - demod->deemph.rate) * s;
	}
	stereo_buffer[n] = s * demod->output.gain;
	output_level += cnrmf(stereo_buffer[n]);
      }
      output_level /= (2 * audio_L); // Halve power to get level per channel
      demod->output.energy += output_level;
      if(send_stereo_output(demod,(const float *)stereo_buffer,audio_L,false) < 0)
	break; // No output stream! Terminate
    } else { // stereo_on == false
      // Mono processing
      float output_level = 0;
      if(demod->deemph.rate != 0){
	// Apply deemphasis
	assert(!isnan(__real__ demod->deemph.state));
	for(int n=0; n < audio_L; n++){
	  float s = mono->output.r[n];
	  __real__ demod->deemph.state *= demod->deemph.rate;
	  s = __real__ demod->deemph.state += demod->deemph.gain * (1 - demod->deemph.rate) * s; 
	  s *= demod->output.gain;
	  mono->output.r[n] = s;
	  output_level += s * s;
	}
      } else {
	for(int n=0; n < audio_L; n++){
	  float s = mono->output.r[n] *= demod->output.gain;
	  output_level += s * s;
	}
      }
      output_level /= audio_L;
      demod->output.energy += output_level;
      // mute output unless time is left on the squelch_state timer
      if(send_mono_output(demod,mono->output.r,audio_L,false) < 0)
	break; // No output stream! Terminate
    }
  } // while(!demod->terminate)
 quit:;
  delete_filter_output(&mono);
  delete_filter_output(&lminusr);
  delete_filter_output(&pilot);
  delete_filter_input(&composite);
  FREE(demod->filter.energies);
  delete_filter_output(&demod->filter.out);
  return NULL;
}
