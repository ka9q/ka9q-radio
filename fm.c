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
int demod_fm(void *arg){
  struct channel * const chan = arg;
  assert(chan != NULL);
  if(chan == NULL)
    return -1;

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
  int status = create_filter_output(&chan->filter.out,&Frontend.in,NULL,blocksize,COMPLEX);
  pthread_mutex_unlock(&chan->status.lock);
  if(status != 0)
    return -1; // Fatal

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


  struct goertzel tone_detect; // PL tone detector state
  float lpf_energy = 0;
  struct iir lpf = {0};
  setIIRlp(&lpf,300. / chan->output.samprate);
  if(chan->fm.tone_freq != 0){
    // Set up PL tone squelch
    init_goertzel(&tone_detect,chan->fm.tone_freq/chan->output.samprate);
  }

  float deemph_state = 0;
  int squelch_state = 0; // Number of blocks for which squelch remains open
  int const N = chan->filter.out.olen;
  float const one_over_olen = 1.0f / N; // save some divides
  int const pl_integrate_samples = chan->output.samprate * 0.24; // 240 milliseconds (spec is < 250 ms). 12 blocks @ 24 kHz
  int pl_sample_count = 0;
  float old_pl_phase = 0;
  bool tone_mute = true; // When tone squelch enabled, mute until the tone is detected
  chan->output.gain = (2 * chan->output.headroom *  chan->output.samprate) / fabsf(chan->filter.min_IF - chan->filter.max_IF);

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
	reset_goertzel(&tone_detect);
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
      reset_goertzel(&tone_detect);
      send_output(chan,NULL,N,true); // Keep track of timestamps and mute state
      continue;
    }
    float baseband[N];    // Demodulated FM baseband
    // Actual FM demodulation
    for(int n=0; n < N; n++){
      float np = M_1_PIf * cargf(buffer[n]); // Scale to -1 to +1 (half rotations/sample)
      float x = np - phase_memory;
      phase_memory = np;
      baseband[n] = x > 1 ? x - 2 : x < -1 ? x + 2 : x; // reduce to -1 to +1
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

      // remove DC before tone squelch; energy measurement responds to DC
      if(chan->fm.rate != 0){
	// Remove DC
	for(int n=0; n < N; n++)
	  baseband[n] -= 2 * chan->sig.foffset / chan->output.samprate;
      }
      if(chan->fm.tone_freq != 0){
	// PL/CTCSS tone squelch
	// use samples after DC removal but before de-emphasis and gain scaling
	for(int n=0; n < N; n++){
	  update_goertzel(&tone_detect,baseband[n]); // input is -1 to +1
	  float y = applyIIR(&lpf,baseband[n]); // should be unity gain in passband
	  lpf_energy += y*y;
	  if(chan->options & (1LL<0)){
	    // Test option: let's hear the LPF output
	    baseband[n] = y;
	  }
	  pl_sample_count++;
	  if(pl_sample_count >= pl_integrate_samples){
	    // Peak deviation of PL tone in Hz
	    complex float const c = output_goertzel(&tone_detect); // gain of N/2 scales half cycles per sample to full cycles per interval
	    float const g = cabsf(c) / pl_sample_count; // peak PL tone deviation in Hz per sample
	    chan->fm.tone_deviation = chan->output.samprate * g; // peak PL tone deviation in Hz
	    // Compute phase jump between integration periods as a fine frequency error indication
	    float const p = cargf(c) / (2*M_PI); // +/- 0.5 rev
	    float iptr = 0;
	    // Update previous phase by the number of intervening PL tone cycles
	    old_pl_phase += chan->fm.tone_freq * pl_sample_count / chan->output.samprate;
	    float np = 2 * modff(p - old_pl_phase,&iptr); // see how much it's jumped, scale to +/-1 *half* rev
	    old_pl_phase = p;
	    np = np < -1 ? np + 2 : np > 1 ? np - 2 : np; // and bring to principal range, -1 to +1 half cycle per interval: 0.5 Hz / .24 sec = 2 Hz
	    assert(np >= -1.0 && np <= 1.0);
	    chan->tp2 = np; // monitor phase error in PL tone, 0 means exactly on frequency, + means high, - means low

	    lpf_energy /= pl_sample_count; // filter output average energy per sample, range 0 to +1 half-rev^2 per sample
	    chan->tp1 = power2dB(2*g*g / lpf_energy); // scale g from full rev peak to half-rev^2 average per sample
	    if(chan->options & (1LL<1)){
	      // Experimental, needs a new 300 Hz audio LPF before it is ready. Otherwise lots of low frequency voice can falsely gate it off
	      // Scale g*g to half revs per sample^2, same as lpf_energy
	      tone_mute = (2*g*g / lpf_energy) < 0.25; // boolean result: if tone -6 dB to LPF total, mute.
	    } else {
	      // Use old tone mute threshold
	      tone_mute = chan->fm.tone_deviation < 250	|| fabsf(np) > .10; // note boolean result. ~0.2 Hz offset
	    }
	    reset_goertzel(&tone_detect);
	    lpf_energy = 0;
	    pl_sample_count = 0;
	  }
	}
	if(tone_mute){
	  send_output(chan,NULL,N,true); // Keep track of timestamps and mute state
	  continue;
	}
      }
    }
    if(chan->fm.rate != 0){
      // Apply de-emphasis if configured
      for(int n=0; n < N; n++){
	baseband[n] = deemph_state += chan->fm.rate * (chan->fm.gain * baseband[n] - deemph_state);
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
  return 0; // Normal exit
}
