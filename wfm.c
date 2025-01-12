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
int demod_wfm(void *arg){
  assert(arg != NULL);
  struct channel * chan = arg;
  if(chan == NULL)
    return -1;

  {
    char name[100];
    snprintf(name,sizeof(name),"wfm %u",chan->output.rtp.ssrc);
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
  int status = create_filter_output(&chan->filter.out,&Frontend.in,NULL,blocksize,COMPLEX);
  pthread_mutex_unlock(&chan->status.lock);
  if(status != 0)
    return -1; // fatal, don't restart

  // Set null here in case we quit early and try to free them
  struct filter_in composite;
  struct filter_out mono;
  struct filter_out lminusr;
  struct filter_out pilot;

  float phase_memory = 0;  // Demodulator input phase memory

  // NB: this is the sample rate from the FM demodulator, which is much faster than the actual audio output sample rate
  // forced to be fast enough for 200 kHz broadcast channel
  chan->output.samprate = Composite_samprate;

  if(chan->output.channels == 0)
    chan->output.channels = 2; // Default to stereo

  set_filter(&chan->filter.out,
	     chan->filter.min_IF/chan->output.samprate,
	     chan->filter.max_IF/chan->output.samprate,
	     chan->filter.kaiser_beta);

  int squelch_state = 0; // Number of blocks for which squelch remains open

  // Make these blocksizes depend on front end sample rate and blocksize
  int const composite_L = roundf(chan->output.samprate * Blocktime * .001); // Intermediate sample rate
  int const composite_M = composite_L + 1; // 2:1 overlap (50%)
  int const composite_N = composite_L + composite_M - 1;

  // output forced to 48 kHz for now
  const int audio_L = roundf(Audio_samprate * Blocktime * .001);

  // Composite signal 50 Hz - 15 kHz contains mono (L+R) signal
  create_filter_input(&composite,composite_L,composite_M,REAL);

  assert(composite.ilen == chan->filter.out.olen);

  if(composite_L < audio_L)
    goto quit; // Front end sample rate is too low - should probably fix filter to allow interpolation

  // Composite filters, decimate from 384 Khz to 48 KHz
  create_filter_output(&mono,&composite,NULL,audio_L, REAL);

  set_filter(&mono,50.0/Audio_samprate, 15000.0/Audio_samprate, chan->filter.kaiser_beta);

  // Narrow filter at 19 kHz for stereo pilot
  create_filter_output(&pilot,&composite,NULL,audio_L, COMPLEX);

  // FCC says +/- 2 Hz, with +/- 20 Hz protected (73.322)
  set_filter(&pilot,-20./Audio_samprate, 20./Audio_samprate, chan->filter.kaiser_beta);

  // Stereo difference (L-R) information on DSBSC carrier at 38 kHz
  // Extends +/- 15 kHz around 38 kHz
  create_filter_output(&lminusr,&composite,NULL,audio_L, COMPLEX);

  set_filter(&lminusr,-15000./Audio_samprate, 15000./Audio_samprate, chan->filter.kaiser_beta);

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

  complex float stereo_deemph = 0;
  float mono_deemph = 0;

  realtime();

  while(downconvert(chan) == 0){
    if(power_squelch && squelch_state == 0){
      // quick check SNR from raw signal power to save time on variance-based squelch
      // Variance squelch is still needed to suppress various spurs and QRM
      float const snr = (chan->sig.bb_power / (chan->sig.n0 * fabsf(chan->filter.max_IF - chan->filter.min_IF))) - 1;
      if(snr < chan->fm.squelch_close){
	// squelch closed, reset everything and mute output
	phase_memory = 0;
	squelch_state = 0;
	continue;
      }
    }

    // Find average amplitude and variance for SNR estimation
    // Use two passes to avoid possible numerical problems
    float amplitudes[composite_L];
    complex float * const buffer = chan->filter.out.output.c;
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
    chan->sig.snr = max(0.0f,snr); // Smoothed values can be a little inconsistent

    // Hysteresis squelch
    int const squelch_state_max = chan->fm.squelch_tail + 1;
    if(chan->sig.snr >= chan->fm.squelch_open
       || (squelch_state > 0 && snr >= chan->fm.squelch_close))
      // Squelch is fully open
      // tail timing is in blocks (usually 10 or 20 ms each)
      squelch_state = squelch_state_max;
    else if(--squelch_state > 0){
      // In tail, squelch still open
    } else {
      squelch_state = 0; // Squelch closed
      phase_memory = 0;
      send_output(chan,NULL,audio_L,true); // Keep track of timestamps and mute state
      continue;
    }
    // Actual FM chanulation
    for(int n=0; n < composite_L; n++){
      // Although deviation can be zero, argf() is defined as returning 0, not NAN
      float np = M_1_PIf * cargf(buffer[n]); // -1 to +1
      float x = np - phase_memory;
      phase_memory = np;
      composite.input_write_pointer.r[n] = x > 1 ? x - 2 : x < -1 ? x + 2 : x; // reduce difference to -1 to +1
    } // for(int n=0; n < composite_L; n++){
    if(squelch_state == squelch_state_max){
      // Squelch fully open; look at deviation peaks
      float peak_positive_deviation = 0;
      float peak_negative_deviation = 0;
      float frequency_offset = 0;
      
      for(int n=0; n < composite_L; n++){
	frequency_offset += composite.input_write_pointer.r[n];
	if(composite.input_write_pointer.r[n] > peak_positive_deviation)
	  peak_positive_deviation = composite.input_write_pointer.r[n];
	else if(composite.input_write_pointer.r[n] < peak_negative_deviation)
	  peak_negative_deviation = composite.input_write_pointer.r[n];
      }
      frequency_offset *= chan->output.samprate * 0.5f / composite_L;  // scale to Hz
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
    // Filter & decimate to audio output sample rate
    execute_filter_input(&composite);  // Composite at 384 kHz
    execute_filter_output(&mono,0);    // L+R composite at 48 kHz
    // Compute audio output level
    // Constant gain used by FM only; automatically adjusted by AGC in linear modes
    // We do this in the loop because headroom and BW can change
    // Force reasonable parameters if they get messed up or aren't initialized
    chan->output.gain = (2 * chan->output.headroom * chan->output.samprate) / fabsf(chan->filter.min_IF - chan->filter.max_IF);

    bool pilot_present = false;
    if(chan->output.channels == 2){
      // See if a subcarrier is present
      // shift signs for pilot and subcarrier don't matter because the filters are real input with symmetric spectra
      execute_filter_output(&pilot,pilot_shift); // pilot spun to 0 Hz, 48 kHz rate
      // I really need a better pilot detector here so we'll switch back to mono without it
      // Probably lock a PLL to it and look at the inphase/quadrature power ratio
      float subc_amp = 0;
      for(int n=0; n < audio_L; n++)
	subc_amp += cnrmf(pilot.output.c[n]);

      subc_amp /= audio_L;
      if(subc_amp > 1e-6) // empirical constant, test this some more
	pilot_present = true;
    }
    if(pilot_present){
      // Stereo multiplex processing
      execute_filter_output(&lminusr,subc_shift); // L-R composite spun down to 0 Hz, 48 kHz rate

      float complex stereo_buffer[audio_L];
      float output_level = 0;
      for(int n = 0; n < audio_L; n++){
	complex float subc_phasor = pilot.output.c[n]; // 19 kHz pilot
	subc_phasor = (subc_phasor * subc_phasor) / cnrmf(subc_phasor); // square to 38 kHz and normalize
	float subc_info = __imag__ (conjf(subc_phasor) * lminusr.output.c[n]); // Carrier is in quadrature
	assert(!isnan(subc_info));
	assert(!isnan(mono.output.r[n]));
	// demultiplex: 2L = (L+R) + (L-R); 2R = (L+R) - (L-R)
	// L+R = mono.output.r[n]; L-R = subc_info
	// real(s) = L, imag(s) = R
	float complex s = mono.output.r[n] + subc_info + I * (mono.output.r[n] - subc_info);
	if(chan->fm.rate != 0)
	  s = stereo_deemph += chan->fm.rate * (chan->fm.gain * s - stereo_deemph);

	stereo_buffer[n] = s * chan->output.gain;
	output_level += cnrmf(stereo_buffer[n]);
      }
      output_level /= (2 * audio_L); // Halve power to get level per channel
      chan->output.energy += output_level;
      assert(chan->output.channels == 2); // Has to be, to get here
      if(send_output(chan,(const float *)stereo_buffer,audio_L,false) < 0)
	break; // No output stream! Terminate
    } else { // pilot_present == false
      // Mono processing
      float output_level = 0;
      if(chan->fm.rate != 0){
	// Apply deemphasis
	for(int n=0; n < audio_L; n++){
	  float s = mono_deemph += chan->fm.rate * (chan->fm.gain * mono.output.r[n] - mono_deemph);
	  mono.output.r[n] = s;
	  output_level += s * s;
	}
      } else {
	for(int n=0; n < audio_L; n++){
	  float s = mono.output.r[n] *= chan->output.gain;
	  output_level += s * s;
	}
      }
      output_level /= audio_L;
      chan->output.energy += output_level;

      // stash channel count in case user is requesting stereo when it's not available
      int channels_save = chan->output.channels;
      chan->output.channels = 1;
      if(send_output(chan,mono.output.r,audio_L,false) < 0)
	break; // No output stream! Terminate
      chan->output.channels = channels_save;
    }
  }
 quit:;
  delete_filter_output(&mono);
  delete_filter_output(&lminusr);
  delete_filter_output(&pilot);
  delete_filter_input(&composite);

  return 0;
}
