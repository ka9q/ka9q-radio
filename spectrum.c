// Spectral analysis service - far from complete - for ka9q-radio's radiod
// Copyright 2023-2024, Phil Karn, KA9Q
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
    snprintf(name,sizeof(name),"spect %u",chan->output.rtp.ssrc);
    pthread_setname(name);
  }
  pthread_mutex_init(&chan->status.lock,NULL);
  pthread_mutex_lock(&chan->status.lock);
  FREE(chan->status.command);
  FREE(chan->filter.energies);
  FREE(chan->spectrum.bin_data);
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
  int fft_bins = chan->spectrum.bin_count * binsperbin;
  if(fft_bins > Frontend.in.bins){
    // Too many, limit to total available
    fft_bins = Frontend.in.bins;
    chan->spectrum.bin_count = fft_bins / binsperbin;
  }

  // Special filter without a response curve or IFFT
  if(create_filter_output(&chan->filter.out,&Frontend.in,NULL,fft_bins,SPECTRUM) == NULL)
    assert(0);

  // Although we don't use filter_output, chan->filter.min_IF and max_IF still need to be set
  // so radio.c:set_freq() will set the front end tuner properly
  chan->filter.max_IF = (chan->spectrum.bin_count * chan->spectrum.bin_bw)/2;
  chan->filter.min_IF = -chan->filter.max_IF;

  // If it's already allocated (why should it be?) we don't know how big it is
  //  chan->spectrum.bin_data = calloc(chan->spectrum.bin_count,sizeof(*chan->spectrum.bin_data));
  // experiment - make array largest possible to temp avoid memory corruption
  chan->spectrum.bin_data = calloc(Frontend.in.bins,sizeof(*chan->spectrum.bin_data));

  set_freq(chan,chan->tune.freq); // retune front end if needed to cover requested bandwidth
  pthread_mutex_unlock(&chan->status.lock);

  if(Verbose > 1)
    fprintf(stdout,"spectrum %d: freq %'lf bin_bw %'f binsperbin %'d bin_count %'d\n",chan->output.rtp.ssrc,chan->tune.freq,chan->spectrum.bin_bw,binsperbin,chan->spectrum.bin_count);

  // Still need to clean up code to force radio freq to be multiple of FFT bin spacing
  while(downconvert(chan) == 0){
    int binp = 0;
    for(int i=0; i < chan->spectrum.bin_count; i++){ // For each noncoherent integration bin above center freq
      double p = 0;
      for(int j=0; j < binsperbin; j++) // Add energy of each fft bin that's part of this user integration bin
	p += cnrmf(chan->filter.out.fdomain[binp++]);

      // Accumulate energy until next poll
      chan->spectrum.bin_data[i] += p;
    }
  }
  FREE(chan->spectrum.bin_data);
  FREE(chan->status.command);
  FREE(chan->filter.energies);
  delete_filter_output(&chan->filter.out);
  return NULL;
}
