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
int demod_spectrum(void *arg){
  struct channel * const chan = arg;
  assert(chan != NULL);
  if(chan == NULL)
    return -1;
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
  if(chan->output.opus != NULL){
    opus_encoder_destroy(chan->output.opus);
    chan->output.opus = NULL;
  }

  pthread_mutex_unlock(&chan->status.lock);

  // Parameters set by system input side
  float const blockrate = 1000.0f / Blocktime; // Width in Hz of frequency bins (greater than FFT bin spacing due to forward FFT overlap)
  int const N = Frontend.L + Frontend.M - 1;
  float const fft_bin_spacing = blockrate * (float)Frontend.L/N; // Hz between FFT bins (less than actual FFT bin width due to FFT overlap)

  // Still need to clean up code to force radio freq to be multiple of FFT bin spacing
  int old_bins = -1;

  // experiment - make array largest possible to temp avoid memory corruption
  chan->spectrum.bin_data = calloc(Frontend.in.bins,sizeof(*chan->spectrum.bin_data));

  do {
    int bin_count = chan->spectrum.bin_count <= 0 ? 64 : chan->spectrum.bin_count;
    float bin_bw = chan->spectrum.bin_bw <= 0 ? 10000 : chan->spectrum.bin_bw;

    // Parameters set by user, constrained by system input
    int const t = roundf(bin_bw / fft_bin_spacing);
    int const binsperbin = (t == 0) ? 1 : t;  // Force reasonable value
    bin_bw = binsperbin * fft_bin_spacing; // Force to integer multiple of fft_bin_spacing
    int fft_bins = bin_count * binsperbin;
    if(fft_bins > Frontend.in.bins){
      // Too many, limit to total available
      fft_bins = Frontend.in.bins;
      bin_count = fft_bins / binsperbin;
    }
    if(fft_bins != old_bins){
      if(Verbose > 1)
	fprintf(stdout,"spectrum %d: freq %'lf bin_bw %'f binsperbin %'d bin_count %'d\n",chan->output.rtp.ssrc,chan->tune.freq,bin_bw,binsperbin,bin_count);

      delete_filter_output(&chan->filter.out);
      old_bins = fft_bins;

      // Special filter without a response curve or IFFT
      if(create_filter_output(&chan->filter.out,&Frontend.in,NULL,fft_bins,SPECTRUM) != 0)
	assert(0);

      // Although we don't use filter_output, chan->filter.min_IF and max_IF still need to be set
      // so radio.c:set_freq() will set the front end tuner properly
      chan->filter.max_IF = (bin_count * bin_bw)/2;
      chan->filter.min_IF = -chan->filter.max_IF;
    } else {
      int binp = 0;
      for(int i=0; i < bin_count; i++){ // For each noncoherent integration bin above center freq
	double p = 0;
	for(int j=0; j < binsperbin; j++) // Add energy of each fft bin that's part of this user integration bin
	  p += cnrmf(chan->filter.out.fdomain[binp++]);

	// Accumulate energy until next poll
	chan->spectrum.bin_data[i] += p;
      }
    }
  } while(downconvert(chan) == 0);
  FREE(chan->spectrum.bin_data);
  FREE(chan->status.command);
  FREE(chan->filter.energies);
  delete_filter_output(&chan->filter.out);
  return 0;
}
