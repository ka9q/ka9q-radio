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

// Controls tradeoff between main lobe width and sidelobe level in small-bin (overlapped FFT) spectrum display
// 7 gives -70 dB sidelobes. Could be dynamically changed from peak-to-N0 ratios


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
  FREE(chan->spectrum.window);
  if(chan->spectrum.plan){
    fftwf_destroy_plan((fftwf_plan)chan->spectrum.plan);
    chan->spectrum.plan = NULL;
  }

  delete_filter_output(&chan->filter.out);
  delete_filter_output(&chan->filter2.out); // filter2 not used in spectrum mode
  delete_filter_input(&chan->filter2.in);
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

  float complex *fft0_in = NULL;
  float complex *fft1_in = NULL;
  float complex *fft_out = NULL;

  double gain = 0;
  int fft0_index = 0;
  int fft1_index = 0;
  int const bin_count = chan->spectrum.bin_count <= 0 ? 64 : chan->spectrum.bin_count;
  double const bin_bw = chan->spectrum.bin_bw <= 0 ? 1000 : chan->spectrum.bin_bw;

#if 1
  realtime(chan->prio - 10); // Drop below demods
#endif

  chan->spectrum.bin_data = calloc(bin_count,sizeof *chan->spectrum.bin_data);
  assert(chan->spectrum.bin_data != NULL);
  if(bin_bw > chan->spectrum.crossover){
    if(Verbose > 1)
      fprintf(stderr,"wide bin spectrum %d: freq %'lf bin_bw %'f bin_count %'d\n",chan->output.rtp.ssrc,chan->tune.freq,bin_bw,bin_count);

    // Direct Wideband mode. Setup FFT to work on raw A/D input
    // What can we do about unfriendly sizes? Anything?
    chan->spectrum.fft_n = frontend->samprate / bin_bw;

    // Generate normalized Kaiser window
    chan->spectrum.window = malloc(chan->spectrum.fft_n * sizeof *chan->spectrum.window);
    make_kaiser(chan->spectrum.window,chan->spectrum.fft_n,chan->spectrum.kaiser);
    double window_gain = 0;
    for(int i = 0; i < chan->spectrum.fft_n; i++)
      window_gain += chan->spectrum.window[i];

    window_gain = chan->spectrum.fft_n / window_gain;
    for(int i = 0; i < chan->spectrum.fft_n; i++)
      chan->spectrum.window[i] *= window_gain;

    if(frontend->isreal){
      float *in = fftwf_malloc(chan->spectrum.fft_n * sizeof *in);
      float complex *out = fftwf_malloc((chan->spectrum.fft_n/2 + 1) * sizeof *out); // N/2 + 1 output points for real->complex
      chan->spectrum.plan = plan_r2c(chan->spectrum.fft_n, in, out);
      fftwf_free(in);
      fftwf_free(out);
    } else {
      float complex *in = fftwf_malloc(chan->spectrum.fft_n * sizeof *in);
      float complex *out = fftwf_malloc(chan->spectrum.fft_n * sizeof *out);
      chan->spectrum.plan = plan_complex(chan->spectrum.fft_n, in, out, FFTW_FORWARD);
      fftwf_free(in);
      fftwf_free(out);
    }
    // Dummy just so downconvert() will process commands and signal need to restart
    int r = create_filter_output(&chan->filter.out,&frontend->in,NULL,0,SPECTRUM);
    assert(r == 0);
    (void)r;
  } else {
    // Set up downconvert mode
    double const margin = 400; // Allow for filter skirts at edge of I/Q receiver
    int const samprate_base = lcm(blockrate,L*blockrate/N); // Samprate must be allowed by receiver
    chan->spectrum.fft_n = bin_count + margin / bin_bw; // Minimum for search to avoid receiver filter skirt
    // This (int) cast should be cleaned up
    while(chan->spectrum.fft_n < 65536 && (!goodchoice(chan->spectrum.fft_n) || (int)round(chan->spectrum.fft_n * bin_bw) % samprate_base != 0))
      chan->spectrum.fft_n++;
    int samprate = chan->spectrum.fft_n * bin_bw;
    if(Verbose > 1)
      fprintf(stderr,"spectrum setup: bin count %d, bin_bw %.1lf, samprate %d fft size %d\n",
	      bin_count,bin_bw,samprate,chan->spectrum.fft_n);

    // The channel filter already normalizes for the size of the forward input FFT, we just handle our own FFT gain
    // squared because the we're scaling the output of complex norm, not the input bin values
    gain = 1.0/ ((double)chan->spectrum.fft_n * chan->spectrum.fft_n);

    int frame_len = samprate / blockrate;
    int r = create_filter_output(&chan->filter.out,&frontend->in,NULL,frame_len,COMPLEX);
    (void)r;
    assert(r == 0);

    chan->filter.max_IF = (double)(samprate - margin)/2;
    chan->filter.min_IF = -chan->filter.max_IF;
    chan->filter2.blocking = 0; // Not used in this mode, make sure it's 0
    set_filter(&chan->filter.out,chan->filter.min_IF,chan->filter.max_IF,KAISER_BETA);
    chan->filter.remainder = NAN; // Force init of downconverter
    chan->filter.bin_shift = 1010101010; // Unlikely - but a kludge, force init of phase rotator

    // Generate normalized Kaiser window
    chan->spectrum.window = malloc(chan->spectrum.fft_n * sizeof *chan->spectrum.window);
    make_kaiser(chan->spectrum.window,chan->spectrum.fft_n,SPECTRUM_KAISER_BETA);
    double window_gain = 0;
    for(int i = 0; i < chan->spectrum.fft_n; i++)
      window_gain += chan->spectrum.window[i];

    window_gain = chan->spectrum.fft_n / window_gain;
    for(int i = 0; i < chan->spectrum.fft_n; i++)
      chan->spectrum.window[i] *= window_gain;

    // Set up two 50% overlapping time-domain windows
    fft0_in = fftwf_malloc(chan->spectrum.fft_n * sizeof *fft0_in);
    fft1_in = fftwf_malloc(chan->spectrum.fft_n * sizeof *fft1_in);
    fft_out = fftwf_malloc(chan->spectrum.fft_n * sizeof *fft_out);
    assert(fft0_in != NULL && fft1_in != NULL && fft_out != NULL);
    memset(fft0_in,0,chan->spectrum.fft_n * sizeof *fft0_in);
    memset(fft1_in,0,chan->spectrum.fft_n * sizeof *fft1_in); // Odd buffer not full when first transformed
    memset(fft_out,0,chan->spectrum.fft_n * sizeof *fft_out); // Odd buffer not full when first transformed
    fft0_index = 0;
    fft1_index = chan->spectrum.fft_n/2;

    chan->spectrum.plan = plan_complex(chan->spectrum.fft_n, fft0_in, fft_out, FFTW_FORWARD);
    assert(chan->spectrum.plan != NULL);
  }
  // Main loop
  while(1){
    if(downconvert(chan) != 0) // Wait for new frame
      break; // restart required, e.g, due to parameter change

    if(bin_bw > chan->spectrum.crossover)
      continue; // Do the rest at poll time

    // narrowband mode from here on
    // For fine resolution better than the ~40 Hz from the main FFT, create an ordinary IQ channel
    // and feed it to a FFT. This also has parameter restrictions, mainly on the sample rate of the IQ channel
    // It will take several blocks to fill each FFT
    // Two 50% overlapping windows with Kaiser windows
    assert(chan->spectrum.window != NULL && fft0_in != NULL && fft1_in != NULL && fft_out != NULL);
    for(int i = 0; i < chan->sampcount; i++){
      bool did_fft = false;

      fft0_in[fft0_index] = chan->baseband[i] * chan->spectrum.window[fft0_index];
      fft1_in[fft1_index] = chan->baseband[i] * chan->spectrum.window[fft1_index];

      if(++fft0_index >= chan->spectrum.fft_n){
	fft0_index = 0;
	fftwf_execute_dft(chan->spectrum.plan,fft0_in,fft_out);
	did_fft = true;
      }
      if(++fft1_index >= chan->spectrum.fft_n){
	fft1_index = 0;
	fftwf_execute_dft(chan->spectrum.plan,fft1_in,fft_out);
	did_fft = true;
      }
      if(did_fft){
	// Copy requested number of bins to user
	// Should verify correctness for combinations of even and odd bin_count and actual_bin_count
	int k = 0;
	double const alpha = 0.5; // need to centralize this
	// Could probably eliminate exponential smoothing, just average a few blocks

	for(int j = 0; j < bin_count; j++,k++){
	  if(j == bin_count/2)
	    k += chan->spectrum.fft_n - bin_count; // jump to negative spectrum of FFT
	  double p = gain * cnrmf(fft_out[k]); // Take power spectrum
	  chan->spectrum.bin_data[j] += alpha * (p - chan->spectrum.bin_data[j]); // average it in
	  assert(isfinite(chan->spectrum.bin_data[j]));
	}
      }
    }
  } // end of main loop, break on restart
  delete_filter_output(&chan->filter.out);
  if(chan->spectrum.plan)
    fftwf_destroy_plan(chan->spectrum.plan);
  chan->spectrum.plan = NULL;
  fftwf_free(fft0_in);
  fftwf_free(fft1_in);
  fftwf_free(fft_out);
  fft0_in = fft1_in = fft_out = NULL;
  FREE(chan->spectrum.window);
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

  // These can happen if we're called too early, before allocations
  struct filter_in const * const master = chan->filter.out.master;
  if(master == NULL)
    return -1;

  if(chan->spectrum.bin_data == NULL)
    return -1;

  if(chan->spectrum.plan == NULL || chan->spectrum.fft_n <= 0 || chan->spectrum.window == NULL || chan->spectrum.bin_count <= 0
     || chan->spectrum.bin_bw <= 0 || chan->spectrum.window == NULL)
    return 0; // Not yet set up

  double const bin_bw = chan->spectrum.bin_bw <= 0 ? 1000 : chan->spectrum.bin_bw;

  if(bin_bw <= chan->spectrum.crossover)
    return 0; // Only in wide binmode

  // Parameters set by system input side
  int const bin_count = chan->spectrum.bin_count <= 0 ? 64 : chan->spectrum.bin_count;

  // Asynchronously read newest data from input buffer
  // Look back two FFT blocks from the most recent write pointer to allow room for overlapping windows
  float complex *fft_out = NULL;
  if(!frontend->isreal){
    fft_out = fftwf_malloc(chan->spectrum.fft_n * sizeof *fft_out);
    float complex const *input = frontend->in.input_write_pointer.c;
    float complex *buffer = fftwf_malloc(chan->spectrum.fft_n * sizeof *buffer);
    assert(buffer != NULL);
    assert(fft_out != NULL);
    // Find starting point to read in input A/D stream - 2 buffers back from current write point
    input -= 2 * chan->spectrum.fft_n * sizeof *input;
    if(input < (float complex *)frontend->in.input_buffer)
      input += frontend->in.input_buffer_size / sizeof *input; // backward wrap
    // Copy and window raw A/D
    for(int i=0; i < chan->spectrum.fft_n; i++)
      buffer[i] = chan->spectrum.window[i] * input[i];
    fftwf_execute_dft(chan->spectrum.plan,buffer,fft_out);
    fftwf_free(buffer);
  } else {
    fft_out = fftwf_malloc((chan->spectrum.fft_n/2 + 1) * sizeof *fft_out);
    float const *input = frontend->in.input_write_pointer.r;
    float *buffer = fftwf_malloc(chan->spectrum.fft_n * sizeof *buffer);
    assert(buffer != NULL);
    assert(fft_out != NULL);
    // Find starting point to read in input A/D stream - 2 buffers back from current write point
    input -= 2 * chan->spectrum.fft_n * sizeof *input;
    if(input < (float *)frontend->in.input_buffer)
      input += frontend->in.input_buffer_size / sizeof *input; // backward wrap
    // Copy and window raw A/D
    // Invert spectrum for inverted inputs?
    for(int i=0; i < chan->spectrum.fft_n; i++)
      buffer[i] = chan->spectrum.window[i] * input[i];
    fftwf_execute_dft_r2c(chan->spectrum.plan,buffer,fft_out);
    fftwf_free(buffer);
  }
  assert(fft_out != NULL);	
  // scale each bin value for our FFT
  // squared because the we're scaling the output of complex norm, not the input bin values
  // we only see one side of the spectrum for real inputs
  double const gain = (master->in_type == REAL ? 2.0f : 1.0f) / ((float)chan->spectrum.fft_n * (float)chan->spectrum.fft_n);
  double const alpha = 1; // for smoothing

  // scale fft bin shift down to size of analysis FFT, which is smaller than the input FFT
  int const shift = chan->filter.bin_shift * (int64_t)chan->spectrum.fft_n / master->bins;

  if(frontend->in.in_type == COMPLEX){
    // Copy requested bins to user
    int binp = shift - bin_count/2;
    if(binp < 0)
      binp += chan->spectrum.fft_n; // Start in negative input region

    // Form array of bin energies from lowest frequency to high
    // Lowest frequency in power_buffer[0] to simplify interpolation
    // Handle real inverted!
    for(int i = 0; i < bin_count; i++){
      double const p = gain * cnrmf(fft_out[binp]); // Take power
      chan->spectrum.bin_data[i] += alpha * (p - chan->spectrum.bin_data[i]); // average it in
      assert(isfinite(chan->spectrum.bin_data[i]));
      if(++binp == chan->spectrum.fft_n)
	binp = 0;
    }
  } else if(chan->filter.bin_shift > 0){
    // Real input right side up
    // Start with DC + positive frequencies
    int binp = shift;
    int i = 0;
    while(i < bin_count/2){
      if(binp >= chan->spectrum.fft_n/2 + 1)
	chan->spectrum.bin_data[i++] = 0;
      else {
	double const p = gain * cnrmf(fft_out[binp]); // Take power
	chan->spectrum.bin_data[i] += alpha * (p - chan->spectrum.bin_data[i]); // average it in
	i++;
      }
      binp++;
    }
    // Negative frequencies
    binp = shift - bin_count/2;
    while(i < bin_count){
      if(binp < 0)
	chan->spectrum.bin_data[i++] = 0;
      else {
	double const p = gain * cnrmf(fft_out[binp]); // Take power
	chan->spectrum.bin_data[i] += alpha * (p - chan->spectrum.bin_data[i]); // average it in
	i++;
      }
      binp++;
    }
  } else {
    // Real input inverted - broken, fix
    // Real input spectrum is inverted, read in reverse order
    int binp = -shift + bin_count/2;
    int i = 0;
    while(binp >= chan->spectrum.fft_n/2 + 1){
      binp--;
      chan->spectrum.bin_data[i++] = 0;
    }
    while(i < bin_count && binp >= 0){
      double const p = gain * cnrmf(fft_out[binp]); // Take power
      chan->spectrum.bin_data[i] += alpha * (p - chan->spectrum.bin_data[i]); // average it in
      i++;
      binp--;
    }
    while(i < bin_count)
      chan->spectrum.bin_data[i++] = 0;
  }
  if(fft_out != NULL)
    fftwf_free(fft_out);
  return 0;
}
