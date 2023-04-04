// $Id: linear.c,v 1.116 2022/06/21 07:40:01 karn Exp $

// General purpose linear demodulator
// Handles USB/IQ/CW/etc, all modes but FM
// Copyright May 2022 Phil Karn, KA9Q

#define DEFAULT_SHIFT (0.0)          // Post detection frequency shift, Hz
#define DEFAULT_HEADROOM (-10.0)     // Target average output level, dBFS
#define DEFAULT_HANGTIME (1.1)       // AGC gain hang time, sec
#define DEFAULT_RECOVERY_RATE (20.0)  // AGC recovery rate after hang expiration, dB/s
#define DEFAULT_GAIN (0.)           // Linear gain, dB
#define DEFAULT_THRESHOLD (-15.0)     // AGC threshold, dB (noise will be at HEADROOM + THRESHOLD)
#define DEFAULT_PLL_DAMPING (M_SQRT1_2); // PLL loop damping factor; 1/sqrt(2) is "critical" damping
#define DEFAULT_PLL_LOCKTIME (.05);  // time, sec PLL stays above/below threshold SNR to lock/unlock

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

void *demod_linear(void *arg){
  assert(arg != NULL);
  struct demod * const demod = arg;

  {
    char name[100];
    snprintf(name,sizeof(name),"lin %u",demod->output.rtp.ssrc);
    pthread_setname(name);
  }
 
  int const blocksize = demod->output.samprate * Blocktime / 1000;
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
  
  // Coherent mode parameters
  float const damping = DEFAULT_PLL_DAMPING;
  float const lock_time = DEFAULT_PLL_LOCKTIME;

  int const lock_limit = lock_time * demod->output.samprate;
  init_pll(&demod->pll.pll,(float)demod->output.samprate);

  realtime();

  while(!demod->terminate){
    if(downconvert(demod) == -1) // received terminate
      break;

    int const N = demod->filter.out->olen; // Number of raw samples in filter output buffer

    // First pass over sample block.
    // Perform fine frequency downconversion & block phase correction
    // Run the PLL (if enabled)
    // Apply post-downconversion shift (if enabled, e.g. for CW)
    // Measure energy
    // Apply PLL & frequency shift, measure energy
    complex float * const buffer = demod->filter.out->output.c; // Working buffer
    float signal = 0; // PLL only
    float noise = 0;  // PLL only

    if(demod->linear.pll){
      // Update PLL state, if active
      if(!demod->pll.was_on){
	demod->pll.pll.integrator = 0; // reset oscillator when coming back on
	demod->pll.was_on = 1;
      }
      set_pll_params(&demod->pll.pll,demod->linear.loop_bw,damping);
      for(int n=0; n<N; n++){
	complex float const s = buffer[n] *= conjf(pll_phasor(&demod->pll.pll));
	float phase;
	if(demod->linear.square){
	  phase = cargf(s*s);
	} else {
	  phase = cargf(s);
	}
	run_pll(&demod->pll.pll,phase);
	signal += crealf(s) * crealf(s); // signal in phase with VCO is signal + noise power
	noise += cimagf(s) * cimagf(s);  // signal in quadrature with VCO is assumed to be noise power
      }
      if(noise != 0){
	demod->sig.snr = (signal / noise) - 1; // S/N as power ratio; meaningful only in coherent modes
	if(demod->sig.snr < 0)
	  demod->sig.snr = 0; // Clamp to 0 so it'll show as -Inf dB
      } else
	demod->sig.snr = NAN;

      // Loop lock detector with hysteresis
      // If the loop is locked, the SNR must fall below the threshold for a while
      // before we declare it unlocked, and vice versa
      if(demod->sig.snr < demod->squelch_close){
	demod->pll.lock_count -= N;
      } else if(demod->sig.snr > demod->squelch_open){
	demod->pll.lock_count += N;
      }
      if(demod->pll.lock_count >= lock_limit){
	demod->pll.lock_count = lock_limit;
	demod->linear.pll_lock = 1;
      }
      if(demod->pll.lock_count <= -lock_limit){
	demod->pll.lock_count = -lock_limit;
	demod->linear.pll_lock = 0;
      }
      demod->linear.lock_timer = demod->pll.lock_count;
      demod->linear.cphase = carg(pll_phasor(&demod->pll.pll));
      if(demod->linear.square)
	demod->linear.cphase /= 2; // Squaring doubles the phase
      
      demod->sig.foffset = pll_freq(&demod->pll.pll);
    } else { // if PLL
      demod->pll.was_on = 0;
    }
    // Apply frequency shift
    // Must be done after PLL, which operates only on DC
    set_osc(&demod->shift,demod->tune.shift/demod->output.samprate,0);
    if(demod->shift.freq != 0){
      for(int n=0; n < N; n++){
	buffer[n] *= step_osc(&demod->shift);
      }
    }
 
   // Run AGC on a block basis to do some forward averaging
    // Lots of people seem to have strong opinions how AGCs should work
    // so there's probably a lot of work to do here
    float gain_change = 1; // default to constant gain
    if(demod->linear.agc){
      float const bw = fabsf(demod->filter.min_IF - demod->filter.max_IF);
      float const bn = sqrtf(bw * demod->sig.n0); // Noise amplitude
      float const ampl = sqrtf(demod->sig.bb_power);

      // per-sample gain change is required to avoid sudden gain changes at block boundaries that can
      // cause clicks and pops when a strong signal straddles a block boundary
      // the new gain setting is applied exponentially over the block
      // gain_change is per sample and close to 1, so be careful with numerical precision!
      if(ampl * demod->output.gain > demod->output.headroom){
	// Strong signal, reduce gain
	// Don't do it instantly, but by the end of this block
	float const newgain = demod->output.headroom / ampl;
	// N-th root of newgain / gain
	// Should this be in double precision to avoid imprecision when gain = - epsilon dB?
	gain_change = powf(newgain/demod->output.gain, 1.0F/N);
	demod->hangcount = demod->linear.hangtime;
      } else if(bn * demod->output.gain > demod->linear.threshold * demod->output.headroom){
	// Reduce gain to keep noise < threshold, same as for strong signal
	float const newgain = demod->linear.threshold * demod->output.headroom / bn;
	gain_change = powf(newgain/demod->output.gain, 1.0F/N);
      } else if(demod->hangcount > 0){
	// Waiting for AGC hang time to expire before increasing gain
	gain_change = 1; // Constant gain
	demod->hangcount--;
      } else {
	// Allow gain to increase at configured rate, e.g. 20 dB/s
	gain_change = powf(demod->linear.recovery_rate, 1.0F/N);
      }
    }

    // Second pass over signal block
    // Demodulate, apply gain changes, compute output energy
    float output_level = 0;
    if(demod->output.channels == 1){
      float samples[N]; // for mono output
      // channels == 1, mono
      if(demod->linear.env){
	// AM envelope detection
	for(int n=0; n < N; n++){
	  samples[n] = cabsf(buffer[n]) * demod->output.gain;
	  output_level += samples[n] * samples[n];
	  demod->output.gain *= gain_change;
	}
      } else {
	// I channel only (SSB, CW, etc)
	for(int n=0; n < N; n++){
	  samples[n] = crealf(buffer[n]) * demod->output.gain;
	  output_level += samples[n] * samples[n];
	  demod->output.gain *= gain_change;
	}
      }
      demod->output.level = output_level / N;
      // Mute if no signal (e.g., outside front end coverage)
      bool mute = false;
      if(demod->output.level == 0)
	mute = true;
      if(demod->linear.pll && !demod->linear.pll_lock) // Use PLL for AM carrier squelch
	mute = true;

      if(send_mono_output(demod,samples,N,mute) == -1)
	break; // No output stream!
    } else { // channels == 2, stereo
      if(demod->linear.env){
	// I on left, envelope/AM on right (for experiments in fine SSB tuning)
	for(int n=0; n < N; n++){      
	  __imag__ buffer[n] = cabsf(buffer[n]) * 2; // empirical +6dB
	  buffer[n] *= demod->output.gain;
	  output_level += cnrmf(buffer[n]);
	  demod->output.gain *= gain_change;
	}
      } else {	// I/Q mode
	// I on left, Q on right
	for(int n=0; n < N; n++){      
	  buffer[n] *= demod->output.gain;
	  output_level += cnrmf(buffer[n]);
	  demod->output.gain *= gain_change;
	}
      }
      demod->output.level = output_level / (N * demod->output.channels);
      // Mute if no signal (e.g., outside front end coverage)
      int mute = 0;
      if(demod->output.level == 0)
	mute = true;
      if(demod->linear.pll && !demod->linear.pll_lock)
	mute = true; // AM carrier squelch

      if(send_stereo_output(demod,(float *)buffer,N,mute)){
	break; // No output stream! Terminate
      }
    }
  }
 quit:;
  FREE(demod->filter.energies);
  delete_filter_output(&demod->filter.out);
  return NULL;
}
