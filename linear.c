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

static __thread bool first_run;

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
  pthread_mutex_init(&chan->status.lock,NULL);
  pthread_mutex_lock(&chan->status.lock);
  FREE(chan->status.command);
  FREE(chan->spectrum.bin_data);
  if(chan->output.opus != NULL){
    opus_encoder_destroy(chan->output.opus);
    chan->output.opus = NULL;
  }

  unsigned int const blocksize = chan->output.samprate * Blocktime / 1000;
  delete_filter_output(&chan->filter.out);
  delete_filter_output(&chan->filter2.out);
  delete_filter_input(&chan->filter2.in);
  int status = create_filter_output(&chan->filter.out,&Frontend.in,NULL,blocksize,COMPLEX);
  if(status != 0){
    pthread_mutex_unlock(&chan->status.lock);
    return -1;
  }
  set_channel_filter(chan);
  chan->filter.remainder = NAN;   // Force re-init of fine downconversion osc
  set_freq(chan,chan->tune.freq); // Retune if necessary to accommodate edge of passband
  // Coherent mode parameters
  float const damping = DEFAULT_PLL_DAMPING;
  float const lock_time = DEFAULT_PLL_LOCKTIME;

  int const lock_limit = lock_time * chan->output.samprate;
  init_pll(&chan->pll.pll,(float)chan->output.samprate);
  pthread_mutex_unlock(&chan->status.lock);

  realtime(chan->prio);

  bool squelch_open = true; // memory for squelch hysteresis, starts open

  while(downconvert(chan) == 0){
    unsigned int N = chan->sampcount; // Number of raw samples in filter output buffer
    float complex * buffer = chan->baseband; // Working buffer

    if (!first_run){
      if(Frontend.L != 0){
        int block_rate = Frontend.samprate / Frontend.L;
        uint32_t first_block = chan->filter.out.next_jobnum - 1;
        chan->output.rtp.timestamp = first_block * (chan->output.samprate / block_rate);
        if(Verbose > 0)
          fprintf(stderr,"demod_linear: ssrc %u starting at FFT jobum %u, preset RTP TS to %u\n",chan->output.rtp.ssrc,first_block,chan->output.rtp.timestamp);
        first_run = true;
      }
    }

    // First pass over sample block.
    // Run the PLL (if enabled)
    // Apply post-downconversion shift (if enabled, e.g. for CW)
    // Measure energy
    // Apply PLL & frequency shift, measure energy
    float signal = 0; // PLL only
    float noise = 0;  // PLL only

    if(chan->pll.enable){
      // Update PLL state, if active
      set_pll_params(&chan->pll.pll,chan->pll.loop_bw,damping);
      for(unsigned int n=0; n<N; n++){
	float complex const s = buffer[n] *= conjf(pll_phasor(&chan->pll.pll));
	float phase;
	if(chan->pll.square){
	  phase = cargf(s*s);
	} else {
	  phase = cargf(s);
	}
	run_pll(&chan->pll.pll,phase);
	signal += crealf(s) * crealf(s); // signal in phase with VCO is signal + noise power
	noise += cimagf(s) * cimagf(s);  // signal in quadrature with VCO is assumed to be noise power
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
      double phase = carg(pll_phasor(&chan->pll.pll));
      if(chan->pll.snr > chan->squelch_close){
	// Try to avoid counting cycle slips during loss of lock
	double phase_diff = phase - chan->pll.cphase;
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
      for(unsigned int n=0; n < N; n++){
	buffer[n] *= step_osc(&chan->shift);
      }
    }

    // Run AGC on a block basis to do some forward averaging
    // Lots of people seem to have strong opinions on how AGCs should work
    // so there's probably a lot of work to do here
    float gain_change = 1; // default to constant gain
    if(chan->linear.agc){
      float const bw = fabsf(chan->filter.min_IF - chan->filter.max_IF);
      float const bn = sqrtf(bw * chan->sig.n0); // Noise amplitude
      float const ampl = sqrtf(chan->sig.bb_power);

      /* Per-sample gain change is required to avoid sudden gain changes at block boundaries that can
	 cause clicks and pops when a strong signal straddles a block boundary
	 the new gain setting is applied exponentially over the block
	 gain_change is per sample and close to 1, so be careful with numerical precision!
      */
      if(ampl * chan->output.gain > chan->output.headroom){
	// Strong signal, reduce gain
	// Don't do it instantly, but by the end of this block
	float const newgain = chan->output.headroom / ampl;
	// N-th root of newgain / gain
	// Should this be in double precision to avoid imprecision when gain = - epsilon dB?
	if(newgain > 0)
	  gain_change = powf(newgain/chan->output.gain, 1.0F/N);
	chan->linear.hangcount = chan->linear.hangtime * chan->output.samprate;
      } else if(bn * chan->output.gain > chan->linear.threshold * chan->output.headroom){
	// Reduce gain to keep noise < threshold, same as for strong signal
	float const newgain = chan->linear.threshold * chan->output.headroom / bn;
	if(newgain > 0)
	  gain_change = powf(newgain/chan->output.gain, 1.0F/N);
      } else if(chan->linear.hangcount > 0){
	// Waiting for AGC hang time to expire before increasing gain
	chan->linear.hangcount -= N;
      } else {
	// Allow gain to increase at configured rate
	gain_change = powf(chan->linear.recovery_rate, 1.0F/chan->output.samprate);
      }
      assert(gain_change != 0 && isfinite(gain_change));
    }
    // Final pass over signal block
    // Demodulate, apply gain changes, compute output energy
    float output_power = 0;
    if(chan->output.channels == 1){
      /* Complex input buffer is I0 Q0 I1 Q1 ...
	 Real output will be R0 R1 R2 R3 ...
	 Help cache use by overlaying output on input; ok as long as we index it from low to high
      */
      float *samples = (float *)buffer;
      if(chan->linear.env){
	// AM envelope detection
	for(unsigned int n=0; n < N; n++){
	  samples[n] = M_SQRT1_2 * cabsf(buffer[n]) * chan->output.gain; // Power from both I&Q
	  output_power += samples[n] * samples[n];
	  chan->output.gain *= gain_change;
	}
      } else {
	// I channel only (SSB, CW, etc)
	for(unsigned int n=0; n < N; n++){
	  samples[n] = crealf(buffer[n]) * chan->output.gain;
	  output_power += samples[n] * samples[n];
	  chan->output.gain *= gain_change;
	}
      }
    } else {
      // Complex input buffer is I0 Q0 I1 Q1 ...
      // Real output will be L0 R0 L1 R1  ...
      // Overlay input with output
      if(chan->linear.env){
	// I on left, envelope/AM on right (for experiments in fine SSB tuning)
	for(unsigned int n=0; n < N; n++){
	  __imag__ buffer[n] = M_SQRT1_2 * cabsf(buffer[n]);
	  buffer[n] *= chan->output.gain;
	  output_power += cnrmf(buffer[n]);
	  chan->output.gain *= gain_change;
	}
      } else {
	// Simplest case: I/Q output with I on left, Q on right
	for(unsigned int n=0; n < N; n++){
	  buffer[n] *= chan->output.gain;
	  output_power += cnrmf(buffer[n]);
	  chan->output.gain *= gain_change;
	}
      }
    }
    output_power /= N; // Per sample
    if(chan->output.channels == 1)
      output_power *= 2; // +3 dB for mono since 0 dBFS = 1 unit peak, not RMS
    chan->output.power = output_power;

    // If snr squelch is enabled, it takes precedence. Otherwise PLL lock, if it's on
    float snr = +INFINITY;
    if(chan->snr_squelch_enable)
      snr = (chan->sig.bb_power / (chan->sig.n0 * fabsf(chan->filter.max_IF - chan->filter.min_IF))) - 1.0f;
    else if(chan->pll.enable)
      snr = chan->pll.snr;

    if(snr < chan->squelch_close)
      squelch_open = false;
    else if(snr > chan->squelch_open)
      squelch_open = true;
    // otherwise leave it be

    // Mute if no signal (e.g., outside front end coverage)
    // or if zero frequency
    // or if squelch is closed
    bool mute = output_power == 0 || !squelch_open || chan->tune.freq == 0;

    // send_output() knows if the buffer is mono or stereo
    if(send_output(chan,(float *)buffer,N,mute) == -1)
      break; // No output stream!

    // When the gain is allowed to vary, the average gain won't be exactly consistent with the
    // average baseband (input) and output powers. But I still try to make it meaningful.
  }
  return 0; // Non-fatal exit, may be restarted
}
