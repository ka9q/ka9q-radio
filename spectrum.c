//#define SPECTRUM_DEBUG 1

// Spectral analysis service - far from complete - for ka9q-radio's radiod
// Copyright 2023-2025, Phil Karn, KA9Q
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

static float const SPECTRUM_KAISER_BETA = 5.0;
static float const Spectrum_crossover = 5000; // Switch to summing raw FFT bins above 5 kHz

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
  struct frontend * const frontend = chan->frontend;
  assert(frontend != NULL);
  pthread_mutex_init(&chan->status.lock,NULL);
  pthread_mutex_lock(&chan->status.lock);
  FREE(chan->status.command);
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
  double const blockrate = 1000.0 / Blocktime; // Typically 50 Hz

  int const L = frontend->L;
  int const M = frontend->M;
  int const N = L + M - 1;

  double const fe_fft_bin_spacing = blockrate * (float)L/N; // Input FFT bin spacing. Typically 40 Hz
  double binsperbin = 0; // can handle non-integer ratios

  chan->spectrum.bin_data = calloc(frontend->in.bins,sizeof *chan->spectrum.bin_data);

  fftwf_plan plan = NULL;
  float complex *fft0_in = NULL;
  float complex *fft1_in = NULL;
  float complex *fft_out = NULL;

  double gain = 0;
  int fft_size = 0;
  int fft0_index = 0;
  int fft1_index = 0;
  int old_bin_count = -1;
  double old_bin_bw = -1;
  float *kaiser = NULL;
  double kaiser_gain = 0;

#if 1
  realtime(chan->prio - 10); // Drop below demods
#endif

  while(1){
    // Check user params
    int bin_count = chan->spectrum.bin_count <= 0 ? 64 : chan->spectrum.bin_count;
    double bin_bw = chan->spectrum.bin_bw <= 0 ? 1000 : chan->spectrum.bin_bw;

    if(bin_bw != old_bin_bw || bin_count != old_bin_count){
      // Params have changed, set everything up again
      old_bin_bw = bin_bw;
      old_bin_count = bin_count;

      // Get rid of anything old
      delete_filter_output(&chan->filter.out);
      if(plan != NULL){
	fftwf_destroy_plan(plan);
	plan = NULL;
      }
      fftwf_free(fft0_in);
      fftwf_free(fft1_in);
      fftwf_free(fft_out);
      fft0_in = fft1_in = fft_out = NULL;
      FREE(chan->status.command);
      FREE(kaiser);

      if(bin_bw > Spectrum_crossover){
	// Set up wide bin mode
	binsperbin = bin_bw / fe_fft_bin_spacing;
	if(Verbose > 1)
	  fprintf(stderr,"wide bin spectrum %d: freq %'lf bin_bw %'f binsperbin %'.1f bin_count %'d\n",chan->output.rtp.ssrc,chan->tune.freq,bin_bw,binsperbin,bin_count);

	chan->filter.max_IF = (bin_count * bin_bw)/2;
	chan->filter.min_IF = -chan->filter.max_IF;
	create_filter_output(&chan->filter.out,&frontend->in,NULL,0,SPECTRUM);
	// Compute power (not amplitude) scale factor
#if SPECTRUM_DEBUG
	double const gain = (master->in_type == REAL ? 2.0 : 1.0) / ((double)N*N);
	fprintf(stderr,"direct mode binsperbin %'.1lf bin_bw %.1lf bin_count %d gain %.1lf dB\n",
		binsperbin,bin_bw,bin_count,(double)power2dB(gain));
#endif
      } else {
	// Set up FFT mode
	double const margin = 400; // Allow for filter skirts at edge of I/Q receiver
	int const samprate_base = lcm(blockrate,L*blockrate/N); // Samprate must be allowed by receiver
	fft_size = bin_count + margin / bin_bw; // Minimum for search to avoid receiver filter skirt
	// This (int) cast should be cleaned up
	while(fft_size < 65536 && (!goodchoice(fft_size) || (int)round(fft_size * bin_bw) % samprate_base != 0))
	    fft_size++;
	int samprate = fft_size * bin_bw;
	if(Verbose > 1)
	  fprintf(stderr,"spectrum setup: bin count %d, bin_bw %.1lf, samprate %d fft size %d\n",
		  bin_count,bin_bw,samprate,fft_size);

	// The channel filter already normalizes for the size of the forward input FFT, we just handle our own FFT gain
	// squared because the we're scaling the output of complex norm, not the input bin values
	gain = 1.0/ ((double)fft_size * fft_size);

	int frame_len = samprate / blockrate;
	int r = create_filter_output(&chan->filter.out,&frontend->in,NULL,frame_len,COMPLEX);
	(void)r;
	assert(r == 0);

	chan->filter.max_IF = (double)(samprate - margin)/2;
	chan->filter.min_IF = -chan->filter.max_IF;
	set_filter(&chan->filter.out,chan->filter.min_IF,chan->filter.max_IF,KAISER_BETA);
	chan->filter.remainder = NAN; // Force init of downconverter
	chan->filter.bin_shift = 1010101010; // Unlikely - but a kludge, force init of phase rotator

	// Generate normalized Kaiser window
	kaiser = malloc(fft_size * sizeof *kaiser);
	make_kaiser(kaiser,fft_size,SPECTRUM_KAISER_BETA);
	kaiser_gain = 0;
	for(int i = 0; i < fft_size; i++)
	  kaiser_gain += kaiser[i];

	kaiser_gain = fft_size / kaiser_gain;
	for(int i = 0; i < fft_size; i++)
	  kaiser[i] *= kaiser_gain;

	// Set up two 50% overlapping time-domain windows
	fft0_in = fftwf_malloc(fft_size * sizeof *fft0_in);
	fft1_in = fftwf_malloc(fft_size * sizeof *fft1_in);
	fft_out = fftwf_malloc(fft_size * sizeof *fft_out);
	assert(fft0_in != NULL && fft1_in != NULL && fft_out != NULL);
	memset(fft0_in,0,fft_size * sizeof *fft0_in);
	memset(fft1_in,0,fft_size * sizeof *fft1_in); // Odd buffer not full when first transformed
	memset(fft_out,0,fft_size * sizeof *fft_out); // Odd buffer not full when first transformed
	fft0_index = 0;
	fft1_index = fft_size/2;

	plan = plan_complex(fft_size, fft0_in, fft_out, FFTW_FORWARD);
	assert(plan != NULL);
      }
    }
    // Setup done (or nothing has changed)
    if(downconvert(chan) != 0) // Wait for new frame
      break;

    if(bin_bw > Spectrum_crossover)
      continue; // Do the rest at poll time

    // FFT mode from here on
    // For fine resolution better than the ~40 Hz from the main FFT, create an ordinary IQ channel
    // and feed it to a FFT. This also has parameter restrictions, mainly on the sample rate of the IQ channel
    // It will take several blocks to fill each FFT
    // Two 50% overlapping windows with Kaiser windows
    for(int i = 0; i < chan->sampcount; i++){
      bool did_fft = false;

      fft0_in[fft0_index] = chan->baseband[i] * kaiser[fft0_index];
      fft1_in[fft1_index] = chan->baseband[i] * kaiser[fft1_index];

      if(++fft0_index >= fft_size){
	fft0_index = 0;
	fftwf_execute_dft(plan,fft0_in,fft_out);
	did_fft = true;
      }
      if(++fft1_index >= fft_size){
	fft1_index = 0;
	fftwf_execute_dft(plan,fft1_in,fft_out);
	did_fft = true;
      }
      if(did_fft){
	// Copy requested number of bins to user
	// Should verify correctness for combinations of even and odd bin_count and actual_bin_count
	int k = 0;
	double alpha = 0.5;

	for(int j = 0; j < bin_count; j++,k++){
	  if(j == bin_count/2)
	    k += fft_size - bin_count; // jump to negative spectrum of FFT
	  double p = gain * cnrmf(fft_out[k]); // Take power spectrum
	  chan->spectrum.bin_data[j] += alpha * (p - chan->spectrum.bin_data[j]); // average it in
	  assert(isfinite(chan->spectrum.bin_data[j]));
	}
      }
    }
  }
  delete_filter_output(&chan->filter.out);
  if(plan != NULL)
    fftwf_destroy_plan(plan);
  plan = NULL;
  fftwf_free(fft0_in);
  fftwf_free(fft1_in);
  fftwf_free(fft_out);
  fft0_in = fft1_in = fft_out = NULL;
  FREE(kaiser);
  FREE(chan->status.command);
  FREE(chan->spectrum.bin_data);
  return 0;
}

// Called at poll time in wide bin mode
int spectrum_poll(struct channel *chan){

  if(chan == NULL)
    return -1;
  struct frontend const *frontend = chan->frontend;
  if(frontend == NULL)
    return -1;

  struct filter_in const * const master = chan->filter.out.master;
  if(master == NULL)
    return -1;

  if(chan->spectrum.bin_data == NULL)
    return -1;

  double const bin_bw = chan->spectrum.bin_bw <= 0 ? 1000 : chan->spectrum.bin_bw;

  if(bin_bw <= Spectrum_crossover)
    return 0; // Only in wide binmode

  // Parameters set by system input side
  int const L = frontend->L;
  int const M = frontend->M;
  int const N = L + M - 1;
  int const bin_count = chan->spectrum.bin_count <= 0 ? 64 : chan->spectrum.bin_count;

  double const blockrate = 1000.0f / Blocktime; // Typically 50 Hz
  double const fe_fft_bin_spacing = blockrate * (float)L/N; // Input FFT bin spacing. Typically 40 Hz
  double const binsperbin = bin_bw / fe_fft_bin_spacing;
  int const input_bins = ceilf(binsperbin * bin_count);

  // scale each bin value for our FFT
  // squared because the we're scaling the output of complex norm, not the input bin values
  // we only see one side of the spectrum for real inputs
  double const gain = (master->in_type == REAL ? 2.0f : 1.0f) / ((float)N*N);

  // Look at downconverter's frequency bins directly
  //      chan->spectrum.bin_data = reallocf(&chan->spectrum.bin_data, bin_count * sizeof *chan->spectrum.bin_data);
  // Output filter is already waiting for the next job, so subtract 1 to get the current one
  unsigned int jobnum = (chan->filter.out.next_jobnum - 1) % ND;
  float complex const * const fdomain = master->fdomain[jobnum];
  if(fdomain == NULL)
    return -1;

  // Read the master's frequency bins directly
  // The layout depends on the master's time domain input:
  // 1. Complex 2. Real, upright spectrum 3. Real, inverted spectrum
  double *power_buffer = malloc((input_bins + 10) * sizeof *power_buffer);
  if(master->in_type == COMPLEX){
    int binp = chan->filter.bin_shift - input_bins/2;
    if(binp < 0)
      binp += master->bins; // Start in negative input region

    // Form array of bin energies from lowest frequency to high
    // Lowest frequency in power_buffer[0] to simplify interpolation
    for(int i = 0; i < input_bins; i++){
      power_buffer[i] = cnrmf(fdomain[binp]);
      if(++binp == master->bins)
	binp = 0;
    }
  } else if(chan->filter.bin_shift > 0){
    // Real input right side up
    int binp = chan->filter.bin_shift - input_bins/2;
    int i = 0;
    while(binp < 0 && i < input_bins){
      binp++;
      power_buffer[i++] = 0;
    }
    while(i < input_bins && binp < master->bins){
      power_buffer[i++] = cnrmf(fdomain[binp++]);
    }
    while(i < input_bins)
      power_buffer[i++] = 0;
  } else {
    // Real input spectrum is inverted, read in reverse order
    int binp = -chan->filter.bin_shift + input_bins/2;
    int i = 0;
    while(binp >= master->bins){
      binp--;
      power_buffer[i++] = 0;
    }
    while(i < input_bins && binp >= 0)
      power_buffer[i++] = cnrmf(fdomain[binp--]);

    while(i < input_bins)
      power_buffer[i++] = 0;
  }
  // Merge the bins, negative output frequencies first
  double ratio = (double)bin_count / input_bins;

  int out = bin_count/2;
  double outf;
  int in = 0;
  outf = out;
  int x = 0;
  while(out < bin_count && in < input_bins){
    double p = 0;
    while((int)outf == out && in < input_bins){
      assert(in >= 0 && in < input_bins);
      p += power_buffer[in++];
      outf = (++x * ratio) + (bin_count/2);
    }
    chan->spectrum.bin_data[out++] = (p * gain);
  }
  // Positive output frequencies
  out = 0;
  in = input_bins/2;
  outf = out;
  x = 0;
  while(out < bin_count/2 && in < input_bins){
    double p = 0;
    while((int)outf == out && in < input_bins){
      assert(in >= 0 && in < input_bins);
      p += power_buffer[in++];
      outf = (++x * ratio);
    }
    chan->spectrum.bin_data[out++] = (p * gain);
  }
  FREE(power_buffer);
  return 0;
}
