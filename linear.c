// $Id: linear.c,v 1.100 2022/04/09 07:31:03 karn Exp karn $

// General purpose linear demodulator
// Handles USB/IQ/CW/etc, basically all modes but FM and envelope-detected AM
// Copyright Sept 20 2017 Phil Karn, KA9Q

#define DEFAULT_SHIFT (0.0)          // Post detection frequency shift, Hz
#define DEFAULT_HEADROOM (-10.0)     // Target average output level, dBFS
#define DEFAULT_HANGTIME (1.1)       // AGC gain hang time, sec
#define DEFAULT_RECOVERY_RATE (20.0)  // AGC recovery rate after hang expiration, dB/s
#define DEFAULT_GAIN (80.)           // Linear gain, dB
#define DEFAULT_THRESHOLD (-15.0)     // AGC threshold, dB (noise will be at HEADROOM + THRESHOLD)
#define DEFAULT_PLL_BW (500.0)         // PLL loop bandwidth, Hz
#define DEFAULT_PLL_DAMPING (M_SQRT1_2); // PLL loop damping factor; 1/sqrt(2) is "critical" damping
//#define DEFAULT_PLL_THRESHOLD (2.);  // PLL lock threshold, power ratio (2 = +3 dB SNR)
#define DEFAULT_PLL_LOCKTIME (.05);  // time, sec PLL stays above/below threshold SNR to lock/unlock

#define _GNU_SOURCE 1
#include <assert.h>
#include <complex.h>
#include <math.h>
#include <fftw3.h>
#include <pthread.h>
#include <string.h>

#include "misc.h"
#include "filter.h"
#include "radio.h"

void *demod_linear(void *arg){
  assert(arg != NULL);
  struct demod * demod = arg;

  {
    char name[100];
    snprintf(name,sizeof(name),"lin %u",demod->output.rtp.ssrc);
    pthread_setname(name);
  }
  demod->output.gain = dB2voltage(DEFAULT_GAIN); // AGC will bring it down

  int const blocksize = demod->output.samprate * Blocktime / 1000;
  if(demod->filter.out)
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
  
  // Coherent mode parameters
  //  float const snrthresh = DEFAULT_PLL_THRESHOLD;
  float const damping = DEFAULT_PLL_DAMPING;
  float const lock_time = DEFAULT_PLL_LOCKTIME;

  const int lock_limit = lock_time * demod->output.samprate;
  init_pll(&demod->pll.pll,(float)demod->output.samprate);

  while(!demod->terminate){
    const int N = demod->filter.out->olen; // Number of raw samples in filter output buffer

    // Force reasonable parameters if they get messed up or aren't set
#ifdef NDEBUG
    if(demod->output.channels != 1 && demod->output.channels != 2)
      demod->output.channels = 1;

    if(!isfinite(demod->tune.doppler_rate))
      demod->tune.doppler_rate = 0;

    if(!isfinite(demod->tune.shift))
      demod->tune.shift = DEFAULT_SHIFT;

    if(!isfinite(demod->output.headroom) || demod->output.headroom <= 0 )
      demod->output.headroom = dB2voltage(DEFAULT_HEADROOM);

    if(!isfinite(demod->linear.hangtime) || demod->linear.hangtime < 0)
      demod->linear.hangtime = DEFAULT_HANGTIME * (1000./Blocktime);

    if(!isfinite(demod->linear.recovery_rate) || demod->linear.recovery_rate <= 1)
      demod->linear.recovery_rate = dB2voltage(DEFAULT_RECOVERY_RATE * (Blocktime/1000.));
    
    if(!isfinite(demod->output.gain) || demod->output.gain <= 0)
      demod->output.gain = dB2voltage(DEFAULT_GAIN); // AGC will bring this down if it's too high

    if(!isfinite(demod->linear.threshold) || demod->linear.threshold <= 0)
      demod->linear.threshold = dB2voltage(DEFAULT_THRESHOLD);
    
    if(!isfinite(demod->linear.loop_bw) || demod->linear.loop_bw <= 0)
      demod->linear.loop_bw = DEFAULT_PLL_BW; // Only used outside the loop right now - fix this!!
#else
    assert(demod->output.channels == 1 || demod->output.channels == 2);
    assert(isfinite(demod->tune.doppler_rate));
    assert(isfinite(demod->tune.shift));
    assert(isfinite(demod->output.headroom));
    assert(demod->output.headroom > 0);
    assert(isfinite(demod->linear.hangtime));
    assert(demod->linear.hangtime >= 0);
    assert(isfinite(demod->linear.recovery_rate));
    assert(demod->linear.recovery_rate > 1);
    assert(isfinite(demod->output.gain));
    assert(demod->output.gain > 0);
    assert(isfinite(demod->linear.threshold));
    assert(demod->linear.threshold >= 0);
    assert(isfinite(demod->linear.loop_bw));
    assert(demod->linear.loop_bw > 0);
#endif
    double remainder;
    int rotate,flip;

    // To save CPU time when the front end is completely tuned away from us, block until the front
    // end status changes rather than process zeroes. We must still poll the terminate flag.
    pthread_mutex_lock(&Frontend.sdr.status_mutex);
    while(1){
      if(demod->terminate){
	// Note: relies on periodic front end status messages for polling
	pthread_mutex_unlock(&Frontend.sdr.status_mutex);
	return NULL;
      }
      demod->tune.second_LO = Frontend.sdr.frequency - demod->tune.freq;
      double const freq = demod->tune.doppler + demod->tune.second_LO; // Total logical oscillator frequency
      if(compute_tuning(Frontend.in->ilen + Frontend.in->impulse_length - 1,
			Frontend.in->impulse_length,
			Frontend.sdr.samprate,
			&flip,&rotate,&remainder,freq) == 0)
	break; // We can get at least part of the spectrum we want

      // No front end coverage of our passband; wait for it to retune
      demod->sig.bb_power = 0;
      demod->output.level = 0;
      struct timespec timeout; // Needed to avoid deadlock if no front end is available
      clock_gettime(CLOCK_REALTIME,&timeout);
      timeout.tv_sec += 1; // 1 sec in the future
      pthread_cond_timedwait(&Frontend.sdr.status_cond,&Frontend.sdr.status_mutex,&timeout);
    }
    pthread_mutex_unlock(&Frontend.sdr.status_mutex);

    demod->tp1 = rotate;
    demod->tp2 = remainder;
    set_pll_params(&demod->pll.pll,demod->linear.loop_bw,damping);

    // set these before execute_filter blocks
    set_osc(&demod->fine,remainder/demod->output.samprate,demod->tune.doppler_rate/(demod->output.samprate * demod->output.samprate));
    set_osc(&demod->shift,demod->tune.shift/demod->output.samprate,0);

    // Apply PLL & frequency shift, measure energy
    complex float * const buffer = demod->filter.out->output.c; // Working buffer
    float signal = 0; // PLL only
    float noise = 0;  // PLL only
    float energy = 0;

    execute_filter_output(demod->filter.out,-rotate);
    for(int n=0; n<N; n++){
      complex float s = buffer[n] * flip * step_osc(&demod->fine);
      
      if(demod->linear.pll){
	s *= conjf(pll_phasor(&demod->pll.pll));
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
      // Apply frequency shift
      // Must be done after PLL, which operates only on DC
      if(demod->shift.freq != 0)
	s *= step_osc(&demod->shift);

      energy += cnrmf(s);
      buffer[n] = s;
    }
    energy /= N;
    demod->sig.bb_power = energy;

    // Update PLL state, if active
    if(demod->linear.pll){
      if(!demod->pll.was_on){
	demod->pll.pll.integrator = 0; // reset oscillator when coming back on
	demod->pll.was_on = 1;
      }
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
      if(noise != 0){
	demod->sig.snr = (signal / noise) - 1; // S/N as power ratio; meaningful only in coherent modes
	if(demod->sig.snr < 0)
	  demod->sig.snr = 0; // Clamp to 0 so it'll show as -Inf dB
      } else
	demod->sig.snr = NAN;
    } else { // if PLL
      demod->pll.was_on = 0;
    }

    // Run AGC on a block basis to do some forward averaging
    // Lots of people seem to have strong opinions how AGCs should work
    // so there's probably a lot of work to do here
    float gain_change = 1; // default to constant gain
    if(demod->linear.agc){
      const float bw = fabsf(demod->filter.min_IF - demod->filter.max_IF);
      const float bn = sqrtf(bw * compute_n0(demod)); // Noise amplitude
      const float ampl = sqrtf(energy);

      // per-sample gain change is required to avoid sudden gain changes at block boundaries that can
      // cause clicks and pops when a strong signal straddles a block boundary
      // the new gain setting is applied exponentially over the block
      // gain_change is per sample and close to 1, so be careful with numerical precision!
      if(ampl * demod->output.gain > demod->output.headroom){
	// Strong signal - reduce gain to whatever gets it in range
	float const newgain = demod->output.headroom / ampl;
	// N-th root of newgain / gain
	// Do in double precision to avoid imprecision when gain = - epsilon dB
	gain_change = expf(logf(newgain/demod->output.gain) / N);
	demod->hangcount = demod->linear.hangtime;
      } else if(bn * demod->output.gain > demod->linear.threshold * demod->output.headroom){
	// Keep noise < threshold
	float const newgain = demod->linear.threshold * demod->output.headroom / bn;
	gain_change = expf(logf(newgain/demod->output.gain) / N);
	//	demod->hangcount = demod->linear.hangtime; // experimental removal
      } else if(demod->hangcount > 0){
	// Waiting for AGC hang time to expire before increasing gain
	gain_change = 1;
	demod->hangcount--;
      } else {
	// Allow gain to slowly recover
	// Use the set recovery rate unless that would be too much
	gain_change = expf(logf(demod->linear.recovery_rate) / N);
      }
    }

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
      int mute = 0;
      if(demod->output.level == 0)
	mute = 1;
      if(demod->linear.pll && !demod->linear.pll_lock)
	mute = 1;

      if(send_mono_output(demod,samples,N,mute) == -1)
	break; // No output stream!
    } else { // channels == 2, stereo
      if(demod->linear.env){
	// I on left, envelope/AM on right
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
	mute = 1;
      if(demod->linear.pll && !demod->linear.pll_lock)
	mute = 1; // AM carrier squelch

      if(send_stereo_output(demod,(float *)buffer,N,mute)){
	break; // No output stream! Terminate
      }
    }
  }
  delete_filter_output(&demod->filter.out);
  return NULL;
}
