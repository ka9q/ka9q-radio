// Spectral analysis service - far from complete - for ka9q-radio's radiod
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
  struct channel * const chan = arg;  
  
  {
    char name[100];
    snprintf(name,sizeof(name),"spect %u",chan->output.rtp.ssrc); // SSRC is dummy for ID, there's no RTP stream
    pthread_setname(name);
  }

  if(chan->filter.out)
    delete_filter_output(&chan->filter.out);

  if(chan->spectrum.bin_count <= 0)
    chan->spectrum.bin_count = 64; // Force a reasonable number of bins

  if(chan->spectrum.bin_bw <= 0)
    chan->spectrum.bin_bw = 10000; // Force reasonable value of 10 kHz

  // Parameters set by system input side
  float const blockrate = 1000.0f / Blocktime; // Width in Hz of frequency bins (greater than FFT bin spacing due to forward FFT overlap)
  int const N = Frontend.L + Frontend.M - 1;
  float const fft_bin_spacing = blockrate * (float)Frontend.L/N; // Hz between FFT bins (less than actual FFT bin width due to FFT overlap)

  // Parameters set by user, constrained by system input
  int const t = roundf(chan->spectrum.bin_bw / fft_bin_spacing);
  int const binsperbin = (t == 0) ? 1 : t;  // Force reasonable value
  chan->spectrum.bin_bw = binsperbin * fft_bin_spacing; // Force to integer multiple of fft_bin_spacing
  int const fft_bins = chan->spectrum.bin_count * binsperbin;

  // Although we don't use filter_output, chan->filter.min_IF and max_IF still need to be set
  // so radio.c:set_freq() will set the front end tuner properly
  chan->filter.max_IF = (chan->spectrum.bin_count * chan->spectrum.bin_bw)/2;
  chan->filter.min_IF = -chan->filter.max_IF;

  // Special filter without a response curve or IFFT
  // the output size arg to create_filter_output refers only to usable output time points. We want to access the frequency domain
  // points directly, so we decrease to correct for the overlap factor
  // I know all this can be simplified
  int const olen = fft_bins * (float)Frontend.L / N;
  chan->filter.out = create_filter_output(Frontend.in,NULL,olen,SPECTRUM);
  if(chan->filter.out == NULL){
    fprintf(stdout,"unable to create filter for ssrc %lu\n",(unsigned long)chan->output.rtp.ssrc);
    goto quit;
  }

  // If it's already allocated (why should it be?) we don't know how big it is
  if(chan->spectrum.bin_data != NULL)
    FREE(chan->spectrum.bin_data);
  chan->spectrum.bin_data = calloc(chan->spectrum.bin_count,sizeof(*chan->spectrum.bin_data));

  set_freq(chan,chan->tune.freq); // retune front end if needed to cover requested bandwidth

  // Still need to clean up code to force radio freq to be multiple of FFT bin spacing
  while(!chan->terminate){
    if(downconvert(chan) == -1)
      break; // received terminate

    int binp = 0; 
    for(int i=0; i < chan->spectrum.bin_count; i++){ // For each noncoherent integration bin above center freq
      float p = 0;
      for(int j=0; j < binsperbin; j++){ // Add energy of each fft bin that's part of this user integration bin
	p += cnrmf(chan->filter.out->fdomain[binp++]);
      }
      // Accumulate energy until next poll
      chan->spectrum.bin_data[i] += p;
    }
  }
 quit:;
  close_chan(chan);
  return NULL;
}
