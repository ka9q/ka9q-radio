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

  int const L = Frontend.L;
  int const M = Frontend.M;
  int const N = L + M - 1;

  float const fe_fft_bin_spacing = blockrate * (float)L/N; // Hz between FFT bins (less than actual FFT bin width due to FFT overlap)
  int binsperbin = 0;

  // experiment - make array largest possible to temp avoid memory corruption
  chan->spectrum.bin_data = calloc(Frontend.in.bins,sizeof *chan->spectrum.bin_data);


  enum mode { idle, direct, fft } mode = idle;

  fftwf_plan plan = NULL;
  complex float *fft_in = NULL;
  complex float *fft_out = NULL;
  float gain = 0;
  int fft_index = 0;
  int old_bin_count = 0;
  float old_bin_bw = 0;
  float const alpha = 1.0;

  while(1){
    // Check user params
    int bin_count = chan->spectrum.bin_count <= 0 ? 64 : chan->spectrum.bin_count;
    float bin_bw = chan->spectrum.bin_bw <= 0 ? 1000 : chan->spectrum.bin_bw;

    if(bin_bw != old_bin_bw || bin_count != old_bin_count){
      // Params have changed, set everything up again
      if(Verbose > 1)
	fprintf(stdout,"spectrum %d: freq %'lf bin_bw %'f binsperbin %'d bin_count %'d\n",chan->output.rtp.ssrc,chan->tune.freq,bin_bw,binsperbin,bin_count);

      // we could realloc() chan->spectrum.bin_data here
      // assert(chan->spectrum_bin_data != NULL);


      // Should we invoke front end tuning? Don't want to kill other channels if we are too far off here

      // Set mode & clear any previous state
      mode = (bin_bw >= 50) ? direct : fft;
      delete_filter_output(&chan->filter.out);
      if(plan != NULL){
	fftwf_destroy_plan(plan);
	plan = NULL;
      }
      FREE(fft_in);
      FREE(fft_out);
      FREE(chan->filter.energies);
      FREE(chan->status.command);
      switch(mode){
      case direct:
	{
	  // Parameters set by user, constrained by system input
	  int const t = roundf(bin_bw / fe_fft_bin_spacing);
	  binsperbin = (t == 0) ? 1 : t;  // Force reasonable value
	  bin_bw = binsperbin * fe_fft_bin_spacing; // Force to integer multiple of fe_fft_bin_spacing
	  int fe_fft_bins = bin_count * binsperbin;
	  if(fe_fft_bins > Frontend.in.bins){
	    // Too many, limit to total available
	    fe_fft_bins = Frontend.in.bins;
	    bin_count = fe_fft_bins / binsperbin;
	  }
	  // so radio.c:set_freq() will set the front end tuner properly
	  chan->filter.max_IF = (bin_count * bin_bw)/2;
	  chan->filter.min_IF = -chan->filter.max_IF;

	  create_filter_output(&chan->filter.out,&Frontend.in,NULL,0,SPECTRUM);
	  // Compute power (not amplitude) scale factor
	  gain = 1.0f / (float) N;   // scale each bin value for our FFT
	  gain *= gain;              // squared because the we're scaling the output of complex norm, not the input bin values
	  if(chan->filter.out.master->in_type == REAL)
	    gain *= 2;               // we only see one side of the spectrum for real inputs
#if SPECTRUM_DEBUG
	  fprintf(stdout,"direct mode binsperbin %d bin_bw %.1f bin_count %d gain %.1f dB\n",
		  binsperbin,bin_bw,bin_count,power2dB(gain));
#endif
	}
	break;
      case fft:
	{
	  // For fine resolution better than the ~40 Hz from the main FFT, create an ordinary IQ channel
	  // and feed it to a FFT. This also has parameter restrictions, mainly on the sample rate of the IQ channel
	  // It will take several blocks to fill each FFT

#if SPECTRUM_DEBUG
	  fprintf(stdout,"spectrum creating IQ/FFT channel, requested bw = %.1f bin_count = %d\n",bin_bw,bin_count);
#endif
	  int samprate = bin_bw * bin_count;
	  int valid_samprates = lcm(blockrate,L*blockrate/N);
	  if(samprate % valid_samprates != 0){
	    // round up
	    samprate += valid_samprates - samprate % valid_samprates;
	  }
	  // Should also round up to an efficient FFT size
	  int frame_len = ceilf(samprate * Blocktime / 1000.);
	  bin_count = ceilf(samprate / bin_bw);
	  assert(bin_count >= bin_count);

	  chan->filter.min_IF = -samprate/2 + 200;
	  chan->filter.max_IF = samprate/2 - 200;

	  // The channel filter already normalizes for the size of the forward input FFT, we just handle our own FFT gain
	  gain = 1.0f / (float) bin_count;
	  gain *= gain;                     // squared because the we're scaling the output of complex norm, not the input bin values

	  chan->filter.remainder = INFINITY; // Force init of downconverter
	  if(create_filter_output(&chan->filter.out,&Frontend.in,NULL,frame_len,COMPLEX) != 0)
	    break;

	  set_filter(&chan->filter.out,chan->filter.min_IF,chan->filter.max_IF,11.0);

	  // Should round FFT block size up to an efficient number
	  fft_in = lmalloc(bin_count * sizeof(complex float));
	  fft_out = lmalloc(bin_count * sizeof(complex float));
	  fft_index = 0;
	  assert(fft_in != NULL && fft_out != NULL);
#if SPECTRUM_DEBUG
	  fprintf(stdout,"frame_len %d, bin count %d samprate %d, bin_bw %.1f gain %.1f dB\n",
		  frame_len,bin_count,samprate,bin_bw,power2dB(gain));
#endif
	  pthread_mutex_lock(&FFTW_planning_mutex);
	  fftwf_plan_with_nthreads(1);
	  if((plan = fftwf_plan_dft_1d(bin_count, fft_in, fft_out, FFTW_FORWARD, FFTW_WISDOM_ONLY|FFTW_planning_level)) == NULL){
	    suggest(FFTW_planning_level,bin_count,FFTW_FORWARD,COMPLEX);
	    plan = fftwf_plan_dft_1d(bin_count,fft_in,fft_out,FFTW_FORWARD,FFTW_MEASURE);
	  }
	  pthread_mutex_unlock(&FFTW_planning_mutex);
	  if(fftwf_export_wisdom_to_filename(Wisdom_file) == 0)
	    fprintf(stdout,"fftwf_export_wisdom_to_filename(%s) failed\n",Wisdom_file);
	}
	break;
      case idle:
	assert(0); // shouldn't be possible
	sleep(1);  // but in case it happens, don't loop
	break;
      }
    }
    // Force args to actual values so they'll be reported
    old_bin_bw = bin_bw;
    old_bin_count = bin_count;

    chan->spectrum.bin_count = bin_count;
    chan->spectrum.bin_bw = bin_bw;

    // We're now set up in whatever mode we're using, wait for data
    if(downconvert(chan) != 0)
      break;

    if(mode == direct){
      // Look at downconverter's frequency bins directly
      //      chan->spectrum.bin_data = reallocf(&chan->spectrum.bin_data, bin_count * sizeof *chan->spectrum.bin_data);
      // Output flter is already waiting for the next job, so subtract 1 to get the current one
      unsigned int jobnum = (chan->filter.out.next_jobnum - 1) % ND;
      struct filter_in const * const master = chan->filter.out.master;
      complex float *fdomain = master->fdomain[jobnum];

      // Read the master's frequency bins directly
      // The layout depends on the master's time domain input:
      // 1. Complex 2. Real, upright spectrum 3. Real, inverted spectrum
      if(master->in_type == COMPLEX){
	int binp = -chan->filter.bin_shift - binsperbin * bin_count/2;
	if(binp < 0)
	  binp += master->bins; // Start in negative input region
	int i = bin_count/2; // lowest frequency in output
	do {
	  float p = 0;
	  for(int j=0; j < binsperbin; j++){ // Add energy of each fft bin that's part of this user integration bin
	    p += cnrmf(fdomain[binp++]);
	    if(binp == master->bins)
	      binp = 0; // cross from negative to positive in input spectrum
	  }
	  // Accumulate energy until next poll
	  if(!isfinite(chan->spectrum.bin_data[i]))
	    chan->spectrum.bin_data[i] = 0;
	  chan->spectrum.bin_data[i] += (p * gain - chan->spectrum.bin_data[i]) * alpha;
	  if(++i == bin_count)
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
	  for(int j=0; j < binsperbin && binp < master->bins; j++){ // Add energy of each fft bin that's part of this user integration bin
	    p += cnrmf(fdomain[binp++]);
	  }
	  // Accumulate energy until next poll
	  if(!isfinite(chan->spectrum.bin_data[i]))
	    chan->spectrum.bin_data[i] = 0;
	  chan->spectrum.bin_data[i] += (p * gain - chan->spectrum.bin_data[i]) * alpha;
	  if(++i == bin_count)
	    i = 0; // Wrap
	} while(i != bin_count/2 && binp < master->bins);
      } else { // Real input spectrum is inverted, read in reverse order
	int binp = chan->filter.bin_shift + binsperbin * bin_count/2;
	int i = bin_count/2;
	if(binp >= master->bins){
	  // Requested range starts above top; skip
	  i += (master->bins - binp - 1) / binsperbin;
	  if(i >= bin_count)
	    i -= bin_count;
	  binp = master->bins - 1;
	}
	do {
	  float p = 0;
	  for(int j=0; j < binsperbin && binp >= 0; j++){ // Add energy of each fft bin that's part of this user integration bin
	    if(binp < master->bins)
	      p += cnrmf(fdomain[binp]); // Actually cnrmf(conjf(fdomain[binp])) but it doesn't matter to cnrmf()
	    binp--;
	  }
	  // Accumulate energy until next poll
	  if(!isfinite(chan->spectrum.bin_data[i]))
	    chan->spectrum.bin_data[i] = 0;
	  chan->spectrum.bin_data[i] += (p * gain - chan->spectrum.bin_data[i]) * alpha;
	  if(++i == bin_count)
	    i = 0; // Wrap
	} while(i != bin_count/2 && binp >= 0);
      }
    } else {
      // FFT mode for more precision
      // Need to add overlapping windowed buffers here
      for(int i = 0; i < chan->sampcount; i++){
	assert(fft_index >= 0 && fft_index < bin_count);
	fft_in[fft_index] = chan->baseband[i];
	if(++fft_index >= bin_count){
	  // Time domain buffer is full, run the FFT
	  fft_index = 0;
	  fftwf_execute_dft(plan,fft_in,fft_out);
	  for(int j = 0; j < bin_count; j++){
	    float p = gain * cnrmf(fft_out[j]); // Take power spectrum
	    assert(p != 0);
#if SPECTRUM_DEBUG
	    if(p == 0)
	      fprintf(stdout,"spectrum[%d] = 0\n",j);
#endif

	    assert(j >= 0 && j < Frontend.in.bins);
	    if(!isfinite(chan->spectrum.bin_data[j]))
	      chan->spectrum.bin_data[j] = 0;

	    chan->spectrum.bin_data[j] += (p - chan->spectrum.bin_data[j]) * alpha;
	  }
	  // Nyquist bin?
	}
      }
    }
  }
  if(plan != NULL){
    fftwf_destroy_plan(plan);
    plan = NULL;
  }
  FREE(fft_in);
  FREE(fft_out);
  FREE(chan->spectrum.bin_data);
  FREE(chan->status.command);
  FREE(chan->filter.energies);
  delete_filter_output(&chan->filter.out);
  return 0;
}
