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
  chan->status.output_interval = 0; // No automatic status updates
  chan->status.output_timer = 0; // No automatic status updates
  chan->output.silent = true; // we don't send anything there

  pthread_mutex_unlock(&chan->status.lock);

  // Parameters set by system input side
  float const blockrate = 1000.0f / Blocktime; // Width in Hz of frequency bins (greater than FFT bin spacing due to forward FFT overlap)
  int const N = Frontend.L + Frontend.M - 1;
  float const fft_bin_spacing = blockrate * (float)Frontend.L/N; // Hz between FFT bins (less than actual FFT bin width due to FFT overlap)

  // Still need to clean up code to force radio freq to be multiple of FFT bin spacing
  int old_fft_bins = -1;

  // experiment - make array largest possible to temp avoid memory corruption
  chan->spectrum.bin_data = calloc(Frontend.in.bins,sizeof *chan->spectrum.bin_data);

  // Special filter without a response curve or IFFT
  delete_filter_output(&chan->filter.out);
  if(create_filter_output(&chan->filter.out,&Frontend.in,NULL,0,SPECTRUM) != 0)
    assert(0);

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
    if(fft_bins != old_fft_bins){
      if(Verbose > 1)
	fprintf(stdout,"spectrum %d: freq %'lf bin_bw %'f binsperbin %'d bin_count %'d\n",chan->output.rtp.ssrc,chan->tune.freq,bin_bw,binsperbin,bin_count);

      old_fft_bins = fft_bins;
      // we could realloc() chan->spectrum.bin_data here
      //      chan->spectrum.bin_data = reallocf(&chan->spectrum.bin_data, bin_count * sizeof *chan->spectrum.bin_data);
      // assert(chan->spectrum_bin_data != NULL);

      // Although we don't use filter_output, chan->filter.min_IF and max_IF still need to be set
      // so radio.c:set_freq() will set the front end tuner properly
      chan->filter.max_IF = (bin_count * bin_bw)/2;
      chan->filter.min_IF = -chan->filter.max_IF;
      // Should we invoke front end tuning? Don't want to kill other channels if we are too far off here
    }
    // Output flter is already waiting for the next job, so subtract 1 to get the current one
    unsigned int jobnum = (chan->filter.out.next_jobnum - 1) % ND;
    complex float *fdomain = chan->filter.out.master->fdomain[jobnum];
    float gain = 2.0f / (float) N;   // scale each bin value by 2/N (and hope N isn't 0!)
    gain *= gain;                     // squared because the we're scaling the output of complex norm, not the input bin values

    // Read the master's frequency bins directly
    // The layout depends on the master's time domain input:
    // 1. Complex 2. Real, upright spectrum 3. Real, inverted spectrum
    if(chan->filter.out.master->in_type == COMPLEX){
      int binp = -chan->filter.bin_shift - binsperbin * bin_count/2;
      if(binp < 0)
	binp += chan->filter.out.master->bins; // Start in negative input region
      int i = bin_count/2; // lowest frequency in output
      do {
	float p = 0;
	for(int j=0; j < binsperbin; j++){ // Add energy of each fft bin that's part of this user integration bin
	  p += cnrmf(fdomain[binp++]);
	  if(binp == chan->filter.out.master->bins)
	    binp = 0; // cross from negative to positive in input spectrum
	}
	// Accumulate energy until next poll
	chan->spectrum.bin_data[i++] += (p * gain);
	if(i == bin_count)
	  i = 0; // Wrap
      } while(i != bin_count/2);
    } else if(chan->filter.bin_shift <= 0){	// Real input right side up
      int binp = -chan->filter.bin_shift - binsperbin * bin_count/2;
      int i = bin_count/2; // lowest frequency in output
      if(binp < 0){
	// Requested range starts below DC; skip
	i += binp / binsperbin;
	if(i >= bin_count)
	  i -= bin_count;
	binp = 0;
      }
      do {
	float p = 0;
	for(int j=0; j < binsperbin && binp < chan->filter.out.master->bins; j++){ // Add energy of each fft bin that's part of this user integration bin
	  p += cnrmf(fdomain[binp++]);
	}
	// Accumulate energy until next poll
	chan->spectrum.bin_data[i++] += (p * gain);
	if(i == bin_count)
	  i = 0; // Wrap
      } while(i != bin_count/2 && binp < chan->filter.out.master->bins);
    } else { // Real input spectrum is inverted, read in reverse order
      int binp = chan->filter.bin_shift + binsperbin * bin_count/2;
      int i = bin_count/2;
      if(binp >= chan->filter.out.master->bins){
	// Requested range starts above top; skip
	i += (chan->filter.out.master->bins - binp - 1) / binsperbin;
	if(i >= bin_count)
	  i -= bin_count;
	binp = chan->filter.out.master->bins - 1;
      }
      do {
	float p = 0;
	for(int j=0; j < binsperbin && binp >= 0; j++){ // Add energy of each fft bin that's part of this user integration bin
	  if(binp < chan->filter.out.master->bins)
	    p += cnrmf(fdomain[binp]); // Actually cnrmf(conjf(fdomain[binp])) but it doesn't matter to cnrmf()
	  binp--;
	}
	// Accumulate energy until next poll
	chan->spectrum.bin_data[i++] += (p * gain);
	if(i == bin_count)
	  i = 0; // Wrap
      } while(i != bin_count/2 && binp >= 0);
    }
  } while(downconvert(chan) == 0);
  FREE(chan->spectrum.bin_data);
  FREE(chan->status.command);
  FREE(chan->filter.energies);
  delete_filter_output(&chan->filter.out);
  return 0;
}
