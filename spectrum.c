// $Id$
// Spectral analysis service - far from complete!
// Copyright 2023, Phil Karn, KA9Q
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

// Spectrum analysis thread
void *demod_spectrum(void *arg){
  assert(arg != NULL);
  struct demod * const demod = arg;  
  
  {
    char name[100];
    snprintf(name,sizeof(name),"spect %u",demod->output.rtp.ssrc); // SSRC is dummy for ID, there's no RTP stream
    pthread_setname(name);
  }

  if(demod->filter.out)
    delete_filter_output(&demod->filter.out);

  if(demod->spectrum.bin_count <= 0)
    demod->spectrum.bin_count = 64; // Force a reasonable number of bins: 64

  if(demod->spectrum.bin_bw <= 0)
    demod->spectrum.bin_bw = 10000; // Force reasonable value of 10 kHz

  float const blockrate = 1000.0f / Blocktime; // Width in Hz of frequency bins (> FFT bin spacing)
  int const N = Frontend.L + Frontend.M - 1;
  float const fft_bin_spacing = blockrate * (float)Frontend.L/N; // Hz between FFT bins (< FFT bin width)
  int const fft_bins = demod->spectrum.bin_count * demod->spectrum.bin_bw / fft_bin_spacing;

  int const binsperbin = demod->spectrum.bin_bw / fft_bin_spacing;

  // Although we don't use filter_output, demod->filter.min_IF and max_IF still need to be set
  // so radio.c:set_freq() will set the front end tuner properly
  demod->filter.max_IF = (demod->spectrum.bin_count * demod->spectrum.bin_bw)/2;
  demod->filter.min_IF = -demod->filter.max_IF;

  // Special filter without a response curve or IFFT
  // the output size arg to create_filter_output refers only to usable output time points. We want to access the frequency domain
  // points directly, so we decrease to correct for the overlap factor
  // I know all this can be simplified
  int const olen = fft_bins * (float)Frontend.L / N;

  demod->filter.out = create_filter_output(Frontend.in,NULL,olen,SPECTRUM);
  if(demod->filter.out == NULL){
    fprintf(stdout,"unable to create filter for ssrc %lu\n",(unsigned long)demod->output.rtp.ssrc);
    goto quit;
  }

  if(demod->spectrum.bin_data == NULL)
    demod->spectrum.bin_data = calloc(demod->spectrum.bin_count,sizeof(*demod->spectrum.bin_data));

  set_freq(demod,demod->tune.freq); // retune front end if needed to cover requested bandwidth

  while(!demod->terminate){
    
    if(downconvert(demod) == -1)
      break; // received terminate

    if(demod->spectrum.integrate_tc <= 0)
      demod->spectrum.integrate_tc = 5; // Force reasonable value of 5 sec
      
    // https://en.wikipedia.org/wiki/Exponential_smoothing#Time constant
    // smooth = 1 - exp(-blocktime/tc)
    // expm1(x) = exp(x) - 1 (to preserve precision)
    float const smooth = -expm1f(-Blocktime / (1000 * demod->spectrum.integrate_tc)); // Blocktime is in millisec!
    int binp = 0; 
    for(int i=0; i < demod->spectrum.bin_count; i++){ // For each noncoherent integration bin
      float p = 0;
      for(int j=0; j < binsperbin; j++) // Add energy of each fft bin that's part of this user integration bin
	p += cnrmf(demod->filter.out->fdomain[binp++]);

      // Exponential smoothing
      demod->spectrum.bin_data[i] += smooth * (p - demod->spectrum.bin_data[i]);
    }
  }
 quit:;
  FREE(demod->spectrum.bin_data);
  FREE(demod->filter.energies);
  delete_filter_output(&demod->filter.out);
  return NULL;
}
