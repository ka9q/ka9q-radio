// FM demodulation and squelch for ka9q-radio
// Copyright 2018-2025, Phil Karn, KA9Q
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

#define M_1_PI2 (0.5/M_PI) // 1/(2pi)

// FM demodulator thread
int demod_fm(void *arg){
  struct channel * const chan = arg;
  assert(chan != NULL);
  if(chan == NULL)
    return -1;

  snprintf(chan->name,sizeof(chan->name),"fm %u",chan->output.rtp.ssrc);
  pthread_setname(chan->name);
  if(Verbose > 1)
    fprintf(stderr,"%s freq %'.3lf Hz starting\n",chan->name,chan->tune.freq);

  assert(Blocktime != 0);
  assert(chan->frontend != NULL);

  pthread_mutex_init(&chan->status.lock,NULL);
  pthread_mutex_lock(&chan->status.lock);
  int const samprate = chan->output.samprate; // Doesn't change, keep local copy
  int const blocksize = (int)round(samprate * Blocktime);

  int const status = create_filter_output(&chan->filter.out,&chan->frontend->in,NULL,blocksize,
				    chan->filter.beam ? BEAM : COMPLEX);
  if(status != 0){
    pthread_mutex_unlock(&chan->status.lock);
    return -1;
  }
  if(chan->filter.beam)
    set_filter_weights(&chan->filter.out,chan->filter.a_weight,chan->filter.b_weight);

  set_filter(&chan->filter.out,
	     chan->filter.min_IF/samprate,
	     chan->filter.max_IF/samprate,
	     chan->filter.kaiser_beta);

  chan->filter.remainder = NAN;   // Force init of fine downconversion oscillator
  set_freq(chan,chan->tune.freq); // Retune if necessary to accommodate edge of passband

  double complex phase_memory = 0;
  chan->output.channels = 1; // Only mono for now
  if(isnan(chan->squelch_open) || chan->squelch_open == 0)
    chan->squelch_open = 6.3;  // open above ~ +8 dB
  if(isnan(chan->squelch_close) || chan->squelch_close == 0)
    chan->squelch_close = 4; // close below ~ +6 dB

  chan->fm.devmax = 5000.; // nominal peak deviation Hz
  chan->fm.modbw = 3000.;   // maximum modulating frequency Hz


  struct goertzel tone_detect; // PL tone detector state
  double lpf_energy = 0;
  struct iir lpf = {0};
  setIIRlp(&lpf,300. / samprate);
  if(chan->fm.tone_freq != 0){
    // Set up PL tone squelch
    init_goertzel(&tone_detect,chan->fm.tone_freq/samprate);
  }
  double deemph_state = 0;
  int squelch_state = 0; // Number of blocks for which squelch remains open
  int const pl_integrate_samples = (int)(samprate * 0.24); // 240 milliseconds (spec is < 250 ms). 12 blocks @ 24 kHz
  int pl_sample_count = 0;
  double old_pl_phase = 0;
  bool tone_mute = true; // When tone squelch enabled, mute until the tone is detected
  chan->output.gain = (2 * chan->output.headroom *  samprate) / fabs(chan->filter.min_IF - chan->filter.max_IF);
  bool response_needed = true;
  bool restart_needed = false;
  pthread_mutex_unlock(&chan->status.lock);
  realtime(chan->prio);

  do {
    response(chan,response_needed);
    response_needed = false;

    pthread_mutex_lock(&chan->status.lock);
    // Look on the single-entry command queue and grab it atomically
    if(chan->status.command != NULL){
      restart_needed = decode_radio_commands(chan,chan->status.command,chan->status.length);
      FREE(chan->status.command);
      response_needed = true;
    }
    pthread_mutex_unlock(&chan->status.lock);
    if(restart_needed || downconvert(chan) != 0)
      break; // restart or terminate

    float complex const * restrict const buffer = chan->baseband; // For convenience
    int const N = chan->sampcount;


    /* there are two SNR estimators:
       1. fast and general using the existing signal power and noise density estimates
       2. a FM-specific estimator that computes the variance of amplitude to its mean
          since FM is constant envelope, any variance is noise

       The simple estimator is used first. If it's below threshold and the squelch is closed, don't bother with the variance estimator.
       Above threshold, or if the squelch is still open, run the variance estimator and use it
    */

    // Simple SNR estimate: Power/(N0 * Bandwidth) - 1
    double avg_amp = 0;
    double amplitudes[N];

    double const snr = (chan->sig.bb_power / (chan->sig.n0 * fabs(chan->filter.max_IF - chan->filter.min_IF))) - 1.0;
    if(chan->snr_squelch_enable || (squelch_state <= 0 && snr < chan->squelch_close)){ // Save the trouble if the signal just isn't there
      chan->fm.snr = snr;
    } else {
      // variance estimation. first get average amplitude (lots of square roots)
      for(int n = 0; n < N; n++)
	avg_amp += amplitudes[n] = cabsf(buffer[n]);    // Use cabsf() rather than approx_magf(); may give more accurate SNRs?
      avg_amp /= N;
      {
	// Compute variance in second pass.
	// Two passes are supposed to be more numerically stable, but is it really necessary?
	double fm_variance = 0;
	for(int n=0; n < N; n++)
	  fm_variance += (amplitudes[n] - avg_amp) * (amplitudes[n] - avg_amp);

	// The result is biased up at low SNR:
	// complex gaussian RVs have Rayleigh/Ricean amplitudes and exponentially distributed powers
	// Run through a correction function
	// The bias is small at high SNR, but we most need accuracy near threshold
	double const snr = fm_snr(avg_amp*avg_amp * (N-1) / fm_variance); // power ratio
	chan->fm.snr = max(0.0,snr); // Just make sure it isn't negative. Log() doesn't like that.
      }
    }
    /* Hysteresis squelch using selected SNR (basic signal SNR or FM ampitude variance/average
       'squelch_state' is a block countdown timer that sequences squelch closing
       A. 'squelch_state' is restarted at squelch_state_max (>= 4 frames) whenever the SNR is above the open threshold
       B. Else if the squelch is open but the SNR falls below the close threshold, decrement the squelch_state
       C. Else leave squelch_state alone. (ie, SNR is in the hysteresis region)
       if squelch_state == 1, 2 or 3 send a silent (zeroes) frame and wait for next frame.
       I.e, send 3 frames of silence after the squelch closes
       When squelch_state reaches 0, stop decrementing and stop sending, but maintain rtp timestamps.
       squelch_state == 0 : fully closed, no RTP stream
                     == 1, 2, 3 : send a silent RTP frame
                            >=4 : squelch is fully open

       The user parameter chan->squelch_tail thus controls how many blocks the squelch will remain open
       after the SNR falls below the close threshold. This is always followed by 3 blocks of silence, then the stream stops.
       If at any time the SNR >= open threshold, the sequence is aborted, the timer is restarted and normal operation continues
    */
    int const squelch_state_max = chan->squelch_tail + 4;
    if(chan->fm.snr >= chan->squelch_open){
      // Squelch is fully open
      // tail timing is in blocks (usually 20 ms each)
      squelch_state = squelch_state_max; // hold timer at start
    } else if(squelch_state > 0 && chan->fm.snr < chan->squelch_close)
      squelch_state--; // Begin to close it. If squelch_tail == 0, this will result in zeroes being emitted right away (no tail)

    // mini state machine for multi-frame squelch closing sequence
    // squelch_state decrements 3..2..1..0
    switch(squelch_state){
    case 3:
      response_needed = true; // force update to indicate squelch is closing
      reset_goertzel(&tone_detect);
      phase_memory = 0;
      pl_sample_count = 0;
      chan->output.power = 0;  // don't keep resending previous value
      [[fallthrough]];
    case 2:
      [[fallthrough]];
    case 1:
      send_output(chan,NULL,N,false); // buffer of zeroes no longer needed
      continue;
    case 0: // squelch completely closed
      chan->output.power = 0;  // don't keep resending previous value
      send_output(chan,NULL,N,true); // Keep track of timestamps and mute state
      continue;
    default: // 4 and above - squelch is open
      break;
    }
    float baseband[N];    // Demodulated FM baseband
    if(chan->pll.enable){
      if(!chan->pll.was_on){
	chan->pll.was_on = true;
	init_pll(&chan->pll.pll,chan->output.samprate);
	set_pll_params(&chan->pll.pll,500.0,M_SQRT1_2);
	double dev = chan->fm.devmax / samprate;
	set_pll_limits(&chan->pll.pll, -dev, +dev); // clip to +/-deviation
      }
      for(int n=0; n < N; n++){
	double phase = M_1_PI2 * carg(buffer[n] * conj(pll_phasor(&chan->pll.pll))); // mix vco with input, -0.5 to +0.5 cycle/sample
	// Clamp to peak deviation
	if(fabs(phase) > chan->fm.devmax/samprate)
	  phase = copysign(chan->fm.devmax/samprate, phase);

	// Weight by IF amplitude
	double a = cnrmf(buffer[n]) / chan->sig.bb_power;
	if(a < 1)
	  phase *= a;
	baseband[n] = 2 * run_pll(&chan->pll.pll,phase)/chan->output.samprate; // scale output to -1 to +1
      }
    } else {
      chan->pll.was_on = false;
      for(int n=0; n < N; n++){
	double phase = M_1_PI * carg(buffer[n] * conj(phase_memory)); // Scale to -1 to +1 peak
	// Clamp to peak deviation
	if(fabs(phase) > chan->fm.devmax/samprate)
	  phase = copysign(chan->fm.devmax/samprate, phase);

	// Weight by IF amplitude
	double a = cnrmf(buffer[n]) / chan->sig.bb_power;
	if(a < 1)
	  phase *= a;

	baseband[n] = phase;
	phase_memory = buffer[n];
      }
      if(chan->fm.snr < 20 && chan->fm.threshold) { // take 10*log10(20) = 13 dB as "full quieting"
	// Experimental threshold reduction (popcorn/click suppression)
	// Compute avg amp if not computed earlier
	// could do all this with sample energies to avoid the sqrt() in cabsf()
	if(avg_amp == 0){
	  for(int n = 0; n < N; n++)
	    avg_amp += amplitudes[n] = cabsf(buffer[n]);    // Use cabsf() rather than approx_magf(); may give more accurate SNRs?
	  avg_amp /= N;
	}
#if 0
	double const noise_thresh = 0.4 * avg_amp;
	double const noise_reduct_scale = 1. / noise_thresh;

	// baseband[i] actually depends on buffer[i] and buffer[i-1], so we should probably look at both
	for(int n=0; n < N; n++){
	  if(amplitudes[n] < noise_thresh)
	    baseband[n] *= amplitudes[n] * noise_reduct_scale; // Reduce amplitude of weak RF samples
      }
#elif 1
	// New experimental algorithm 2/2023
	// Find segments of low amplitude, look for clicks within them, and replace with interpolated values
	// doesn't yet handle bad samples at beginning and end of buffer, but this gets most of them
	double const nthresh = 0.4 * avg_amp;

	// start scan at 1 so we can use the 0th sample as the start if necessary
	for(int i=1; i < N; i++){
	  // find i = first weak sample
	  if(amplitudes[i] < nthresh){ // each baseband sample i depends on IF samples i-1 and i
	    float const start = baseband[i-1]; // Last good value before bad segment
	    // Find next good sample
	    int j;
	    double finish = 0; // default if we can't find a good sample
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
	    double phase_change = 0;
	    for(int k=0; k < steps-1; k++)
	      phase_change += fabsf(baseband[i+k]);

	    if(fabs(phase_change) >= 1.0){
	      // Linear interpolation
	      double const increment = (finish - start) / steps;
	      for(int k=0; k < steps-1; k++)
		baseband[i+k] = (float)(baseband[i+k-1] + increment); // also why i starts at 1
	    }
	    i = j; // advance so increment will test the next sample after the last we know is good
	  }
	}
#else
	// Simple blanker of big spikes
	for(int n=0; n < N; n++){
	  if(fabsf(baseband[n]) > 0.5f)
	    baseband[n] = 0;
	}
#endif
      }
    }
    if(squelch_state == squelch_state_max){
      // Squelch fully open; look at deviation peaks
      double peak_positive_deviation = 0;
      double peak_negative_deviation = 0;   // peak neg deviation
      double frequency_offset = 0;      // Average frequency

      for(int n=0; n < N; n++){
	frequency_offset += baseband[n];
	if(baseband[n] > peak_positive_deviation)
	  peak_positive_deviation = baseband[n];
	else if(baseband[n] < peak_negative_deviation)
	  peak_negative_deviation = baseband[n];
      }
      frequency_offset *= samprate * 0.5 / N;  // scale to Hz
      // Update frequency offset and peak deviation, with smoothing to attenuate PL tones
      // alpha = blocktime in millisec is an approximation to a 1 sec time constant assuming blocktime << 1 sec
      // exact value would be 1 - exp(-blocktime/tc) = -expm1(-blockime/tc)
      double const alpha = 1 * Blocktime;
      chan->sig.foffset += alpha * (frequency_offset - chan->sig.foffset);

      // Remove frequency offset from deviation peaks and scale to full cycles
      peak_positive_deviation *= samprate * 0.5;
      peak_negative_deviation *= samprate * 0.5;
      peak_positive_deviation -= chan->sig.foffset;
      peak_negative_deviation -= chan->sig.foffset;
      chan->fm.pdeviation = max(peak_positive_deviation,-peak_negative_deviation);

      // remove DC before tone squelch; energy measurement responds to DC
      if(chan->fm.rate != 0){
	// Remove DC
	float const dc = (float)(2 * chan->sig.foffset / samprate);
	for(int n=0; n < N; n++)
	  baseband[n] -= dc;
      }
      if(chan->fm.tone_freq != 0){
	// PL/CTCSS tone squelch
	// use samples after DC removal but before de-emphasis and gain scaling
	for(int n=0; n < N; n++){
	  update_goertzel(&tone_detect,baseband[n]); // input is -1 to +1
	  double const y = applyIIR(&lpf,baseband[n]); // should be unity gain in passband
	  lpf_energy += y*y;
	  if(chan->options & (1LL<0)){
	    // Test option: let's hear the LPF output
	    baseband[n] = (float)y;
	  }
	  pl_sample_count++;
	  if(pl_sample_count >= pl_integrate_samples){
	    // Peak deviation of PL tone in Hz
	    double complex const c = output_goertzel(&tone_detect); // gain of N/2 scales half cycles per sample to full cycles per interval
	    double const g = cabs(c) / pl_sample_count; // peak PL tone deviation in Hz per sample
	    chan->fm.tone_deviation = samprate * g; // peak PL tone deviation in Hz
	    // Compute phase jump between integration periods as a fine frequency error indication
	    double const p = carg(c) / (2*M_PI); // +/- 0.5 rev
	    double iptr = 0;
	    // Update previous phase by the number of intervening PL tone cycles
	    old_pl_phase += chan->fm.tone_freq * pl_sample_count / samprate;
	    double np = 2 * modf(p - old_pl_phase,&iptr); // see how much it's jumped, scale to +/-1 *half* rev
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
	      tone_mute = chan->fm.tone_deviation < 250	|| fabs(np) > .10; // note boolean result. ~0.2 Hz offset
	    }
	    reset_goertzel(&tone_detect);
	    lpf_energy = 0;
	    pl_sample_count = 0;
	  }
	}
	if(tone_mute){
	  chan->output.power = 0;
	  send_output(chan,NULL,N,true); // Keep track of timestamps and mute state
	  continue;
	}
      }
    }
    if(chan->fm.rate != 0){
      // Apply de-emphasis if configured
      double const rate = chan->fm.rate;
      double const gain = chan->fm.gain;
      for(int n=0; n < N; n++){
	deemph_state += rate * (gain * baseband[n] - deemph_state);
	baseband[n] = (float)deemph_state;
      }
    }
    // Compute audio output level
    // Constant gain used by FM only; automatically adjusted by AGC in linear modes
    // We do this in the loop because BW can change
    // Force reasonable parameters if they get messed up or aren't initialized
    double const gain = (2 * chan->output.headroom *  samprate) / fabs(chan->filter.min_IF - chan->filter.max_IF);
    chan->output.gain = gain;

    double output_energy = 0;
    for(int n=0; n < N; n++){
      double const s = gain * baseband[n];
      output_energy += s * s;
      baseband[n] = (float)s;
    }
    chan->output.power = output_energy / N;
    if(send_output(chan,baseband,N,false) < 0)
      break; // no valid output stream; terminate!

  } while(true);
  if(Verbose > 1)
    fprintf(stderr,"%s exiting\n",chan->name);

  // clean up
  flush_output(chan,false,true); // if still set, marker won't get sent since it wasn't sent last time
  mirror_free((void *)&chan->output.queue,chan->output.queue_size * sizeof(float)); // Nails pointer
  FREE(chan->status.command);
  if(chan->opus.encoder != NULL){
    opus_encoder_destroy(chan->opus.encoder);
    chan->opus.encoder = NULL;
  }
  delete_filter_output(&chan->filter.out);
  chan->baseband = NULL;
  return 0; // Normal exit
}
