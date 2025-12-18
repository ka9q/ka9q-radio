// General purpose linear demodulator for ka9q-radio
// Handles USB/IQ/CW/etc, all modes but FM
// Copyright May 2022-2023 Phil Karn, KA9Q

#define DEFAULT_SHIFT (0.0)          // Post detection frequency shift, Hz
#define DEFAULT_HEADROOM (-10.0)     // Target average output level, dBFS
#define DEFAULT_HANGTIME (1.1)       // AGC gain hang time, sec
#define DEFAULT_RECOVERY_RATE (20.0)  // AGC recovery rate after hang expiration, dB/s
#define DEFAULT_GAIN (0.)           // Linear gain, dB
#define DEFAULT_THRESHOLD (-15.0)     // AGC threshold, dB (noise will be at HEADROOM + THRESHOLD)
#define DEFAULT_PLL_DAMPING (M_SQRT1_2); // PLL loop damping factor; 1/sqrt(2) is "critical" damping
#define DEFAULT_PLL_LOCKTIME (.5);  // time, sec PLL stays above/below threshold SNR to lock/unlock

#define _GNU_SOURCE 1
#include <assert.h>
#include <complex.h>
#include <math.h>
#include <fftw3.h>
#include <pthread.h>
#include <string.h>
#include <stdbool.h>

#include "misc.h"
#include "filter.h"
#include "radio.h"

int demod_linear(void *arg){
  struct channel * const chan = arg;
  assert(chan != NULL);
  if(chan == NULL)
    return -1; // in case asserts are off

  {
    char name[100];
    snprintf(name,sizeof(name),"lin %u",chan->output.rtp.ssrc);
    pthread_setname(name);
  }
  struct frontend const * const frontend = chan->frontend;
  assert(frontend != NULL);
  assert(Blocktime != 0);

  pthread_mutex_init(&chan->status.lock,NULL);
  pthread_mutex_lock(&chan->status.lock);
  int const blocksize = (int)round(chan->output.samprate * Blocktime);
  int const status = create_filter_output(&chan->filter.out,&chan->frontend->in,NULL,blocksize,
				    chan->filter.beam ? BEAM : COMPLEX);
  if(status != 0){
    pthread_mutex_unlock(&chan->status.lock);
    return -1;
  }
  if(chan->filter.beam)
    set_filter_weights(&chan->filter.out,chan->filter.a_weight,chan->filter.b_weight);

  set_channel_filter(chan);
  chan->filter.remainder = NAN;   // Force re-init of fine downconversion osc
  set_freq(chan,chan->tune.freq); // Retune if necessary to accommodate edge of passband
  // Coherent mode parameters
  double const damping = DEFAULT_PLL_DAMPING;
  double const lock_time = DEFAULT_PLL_LOCKTIME;

  int const lock_limit = (int)round(lock_time * chan->output.samprate);
  init_pll(&chan->pll.pll,(double)chan->output.samprate);
  double am_dc = 0; // Carrier removal filter, removes squelch opening thump in aviation AM

  bool first_run = false;
  bool response_needed = true;
  bool restart_needed = false;
  bool squelch_open = true; // memory for squelch hysteresis, starts open
  if(chan->pll.enable || chan->snr_squelch_enable)
    squelch_open = false; // Start closed when squelch is enabled
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
    if(restart_needed)
      break;

    if(downconvert(chan) != 0)
      break; // Dynamic channel termination

    int const N = chan->sampcount; // Number of raw samples in filter output buffer
    float complex * restrict const buffer = chan->baseband; // Working buffer

    if (!first_run && frontend->L != 0){
      double const block_rate = frontend->samprate / frontend->L;
      uint32_t const first_block = chan->filter.out.next_jobnum - 1;
      chan->output.rtp.timestamp = (int32_t)(first_block * (chan->output.samprate / block_rate));
      if(Verbose > 0)
	fprintf(stderr,"demod_linear: ssrc %u starting at FFT jobum %u, preset RTP TS to %u\n",chan->output.rtp.ssrc,first_block,chan->output.rtp.timestamp);
      first_run = true;
    }

    // First pass over sample block.
    // Run the PLL (if enabled)
    // Apply post-downconversion shift (if enabled, e.g. for CW)
    // Measure energy
    // Apply PLL & frequency shift, measure energy
    double signal = 0; // PLL only
    double noise = 0;  // PLL only

    if(chan->pll.enable){
      // Update PLL state, if active
      set_pll_params(&chan->pll.pll,chan->pll.loop_bw,damping);
      bool const square = chan->pll.square;
      for(int n=0; n<N; n++){
	double complex const s = buffer[n] * conj(pll_phasor(&chan->pll.pll));
	buffer[n] = s;
	double const phase = square ? carg(s*s) : carg(s);
	run_pll(&chan->pll.pll,phase);
	signal += creal(s) * creal(s); // signal in phase with VCO is signal + noise power
	noise += cimag(s) * cimag(s);  // signal in quadrature with VCO is assumed to be noise power
      }
      if(noise != 0){
	chan->pll.snr = (signal / noise) - 1; // S/N as power ratio; meaningful only in coherent modes
	if(chan->pll.snr < 0)
	  chan->pll.snr = 0; // Clamp to 0 so it'll show as -Inf dB
      } else
	chan->pll.snr = NAN;

      // Loop lock detector with hysteresis
      // If there's more I signal than Q signal, declare it locked
      // The squelch settings are really for FM, not for us
      if(chan->pll.snr < chan->squelch_close){
	chan->pll.lock_count -= N;
	if(chan->pll.lock_count <= -lock_limit){
	  chan->pll.lock_count = -lock_limit;
	  chan->pll.lock = false;
	}
      } else if(chan->pll.snr > chan->squelch_open){
	chan->pll.lock_count += N;
	if(chan->pll.lock_count >= lock_limit){
	  chan->pll.lock_count = lock_limit;
	  chan->pll.lock = true;
	}
      }
      double const phase = carg(pll_phasor(&chan->pll.pll));
      if(chan->pll.snr > chan->squelch_close){
	// Try to avoid counting cycle slips during loss of lock
	double const phase_diff = phase - chan->pll.cphase;
	if(phase_diff > M_PI)
	  chan->pll.rotations--;
	else if(phase_diff < -M_PI)
	  chan->pll.rotations++;
      }
      chan->pll.cphase = phase;
      chan->sig.foffset = pll_freq(&chan->pll.pll);
    } else {
      chan->pll.rotations = 0;
      chan->pll.pll.integrator = 0; // reset oscillator when coming back on
      chan->pll.lock_count = -lock_limit;
      chan->pll.lock = false;
    }
    // Apply frequency shift
    // Must be done after PLL, which operates only on DC
    assert(isfinite(chan->tune.shift));
    set_osc(&chan->shift,chan->tune.shift/chan->output.samprate,0);
    if(chan->shift.freq != 0){
      for(int n=0; n < N; n++)
	buffer[n] *= step_osc(&chan->shift);
    }

    // Run AGC on a block basis to do some forward averaging
    // Lots of people seem to have strong opinions on how AGCs should work
    // so there's probably a lot of work to do here
    double gain_change = 1; // default to constant gain
    if(chan->linear.agc){
      double const bw = fabs(chan->filter.min_IF - chan->filter.max_IF);
      double const bn = sqrt(bw * chan->sig.n0); // Noise amplitude
      double const ampl = sqrt(chan->sig.bb_power);

      /* Old comment: Per-sample gain change is required to avoid sudden gain changes at block boundaries that can
	 cause clicks and pops when a strong signal straddles a block boundary
	 the new gain setting is applied exponentially over the block
	 gain_change is per sample and close to 1, so be careful with numerical precision!
	 When the gain is allowed to vary, the average gain won't be exactly consistent with the
	 average baseband (input) and output powers. But I still try to make it meaningful.

	 Experiment prompted by monitoring ARRL 10m contest 13-14 Dec 2025 and hearing K6AM 1km away
	 Find peak level among 2ms subblocks, to handle start of an extremely loud signal
	 At 12 kHz, 2 ms is 24 samples which should be large enough to give a good average
      */
      double peak_level = 0;
      {
	// Divide into 2 ms slices. Hopefully divides evenly (it does for the usual sampling rates and block times)
	// Should handle fractions if that ever happens
	int samples_per_slice = (int)round(N * .002 / Blocktime);
	int n = 0;
	while(n < N){
	  double energy = 0;
	  for(int i = 0; i < samples_per_slice; i++)
	    energy += cnrmf(buffer[n++]);
	  if(energy > peak_level)
	    peak_level = energy;
	}
	peak_level = sqrt(peak_level/samples_per_slice); // RMS power -> amplitude
      }
      if(peak_level * chan->output.gain > M_SQRT2 * chan->output.headroom){
	// Peak is more than 3 dB above the headroom, immediately reduce gain but only for 80 ms
	double const newgain = M_SQRT2 * chan->output.headroom / peak_level;
	gain_change = 1;
	chan->output.gain = newgain;
	chan->linear.hangcount = (int)round(0.08 * chan->output.samprate);
      } else if(ampl * chan->output.gain > chan->output.headroom){
	// Strong signal, reduce gain
	// Don't do it instantly, but by the end of this block
	double const newgain = chan->output.headroom / ampl;
	// N-th root of newgain / gain
	// Should this be in double precision to avoid imprecision when gain = - epsilon dB?
	if(newgain > 0)
	  gain_change = pow(newgain/chan->output.gain, 1.0/N); // can newgain ever <= 0?
	chan->linear.hangcount = (int)round(chan->linear.hangtime * chan->output.samprate);
      } else if(bn * chan->output.gain > chan->linear.threshold * chan->output.headroom){
	// Reduce gain to keep noise < threshold, same as for strong signal
	// but don't touch hang timer
	double const newgain = chan->linear.threshold * chan->output.headroom / bn;
	if(newgain > 0)
	  gain_change = pow(newgain/chan->output.gain, 1.0/N);
      } else if(chan->linear.hangcount > 0){
	// Waiting for AGC hang time to expire before increasing gain
	chan->linear.hangcount -= N;
      } else {
	// Allow gain to increase at configured rate
	// This needs to be sped up when there's a lot of gain to be recovered
	// Maybe something like:
	// if amplitude < headroom - threshold - 20 dB, increase gain 20 dB immediately?
	gain_change = pow(chan->linear.recovery_rate, 1.0/chan->output.samprate);
      }
      assert(gain_change != 0 && isfinite(gain_change));
    }
    // Final pass over signal block
    // Demodulate, apply gain changes, compute output energy
    double output_power = 0;
    if(chan->output.channels == 1){
      /* Complex input buffer is I0 Q0 I1 Q1 ...
	 Real output will be R0 R1 R2 R3 ...
	 Help cache use by overlaying output on input; ok as long as we index it from low to high
      */
      float *samples = (float *)buffer;
      if(chan->linear.env){
	// AM envelope detection
	double gain = chan->output.gain;
	for(int n=0; n < N; n++){
	  double s = gain * M_SQRT1_2 * cabsf(buffer[n]); // Power from both I&Q
	  gain *= gain_change;
	  output_power += s*s;

	  // Estimate and remove carrier (DC)
	  if(chan->linear.dc_tau != 0){
	    am_dc += chan->linear.dc_tau * (s - am_dc);
	    s -= am_dc;
	  }
	  samples[n] = (float)s;
	}
	chan->output.gain = gain;
      } else {
	// I channel only (SSB, CW, etc)
	double gain = chan->output.gain;
	for(int n=0; n < N; n++){
	  double const s = gain * crealf(buffer[n]);
	  gain *= gain_change;
	  output_power += s*s;
	  samples[n] = (float)s;
	}
	chan->output.gain = gain;
      }
    } else {
      // Complex input buffer is I0 Q0 I1 Q1 ...
      // Real output will be L0 R0 L1 R1  ...
      // Overlay input with output
      if(chan->linear.env){
	// I on left, envelope/AM on right (for experiments in fine SSB tuning)
	double gain = chan->output.gain;
	for(int n=0; n < N; n++){
	  double complex s = gain * M_SQRT1_2 * (crealf(buffer[n]) + I * cabsf(buffer[n]));
	  gain *= gain_change;
	  output_power += cnrm(s);

	  // Estimate and remove DC
	  if(chan->linear.dc_tau != 0){
	    am_dc += chan->linear.dc_tau * (__imag__ s - am_dc);
	    __imag__ s -= am_dc;
	  }
	  buffer[n] = (float complex)s;
	}
	chan->output.gain = gain;
      } else {
	// Simplest case: I/Q output with I on left, Q on right
	double gain = chan->output.gain;
	for(int n=0; n < N; n++){
	  double complex const s = gain * buffer[n];
	  gain *= gain_change;
	  output_power += cnrm(s);
	  buffer[n] = (float complex)s;
	}
	chan->output.gain = gain;
      }
    }
    output_power /= N; // Per sample
    if(chan->output.channels == 1)
      output_power *= 2; // +3 dB for mono since 0 dBFS = 1 unit peak, not RMS
    chan->output.power = output_power;

    // If snr squelch is enabled, it takes precedence. Otherwise PLL lock, if it's on
    double snr = +INFINITY;
    if(chan->snr_squelch_enable)
      snr = (chan->sig.bb_power / (chan->sig.n0 * fabs(chan->filter.max_IF - chan->filter.min_IF))) - 1.0;
    else if(chan->pll.enable)
      snr = chan->pll.snr;

    if(chan->snr_squelch_enable || chan->pll.enable){
      if(snr < chan->squelch_close)
	squelch_open = false;
      else if(!squelch_open && snr > chan->squelch_open){
	squelch_open = true;
	am_dc = 0; // try to remove the opening thump caused by the carrier
      }
    } else
      squelch_open = true; // neither squelch enabled
    // otherwise leave it be

    // Mute if no signal (e.g., outside front end coverage)
    // or if zero frequency
    // or if squelch is closed
    bool const mute = output_power == 0 || !squelch_open || chan->tune.freq == 0;

    // send_output() knows if the buffer is mono or stereo
    if(send_output(chan,(float *)buffer,N,mute) == -1)
      break; // No output stream!

  } while(true);

  // clean up
  flush_output(chan,false,true); // if still set, marker won't get sent since it wasn't sent last time
  mirror_free((void *)&chan->output.queue,chan->output.queue_size * sizeof(float)); // Nails pointer
  FREE(chan->status.command);
  if(chan->output.opus != NULL){
    opus_encoder_destroy(chan->output.opus);
    chan->output.opus = NULL;
  }
  delete_filter_output(&chan->filter.out);
  delete_filter_output(&chan->filter2.out);
  delete_filter_input(&chan->filter2.in);
  chan->baseband = NULL;
  return 0;
}
