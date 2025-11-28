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

#define RING_SIZE (2 * chan->spectrum.fft_n)


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
  FREE(chan->spectrum.ring);
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
    chan->output.samprate = 0; // Not meaningful
    chan->output.channels = 0;

    // Generate normalized Kaiser window
    chan->spectrum.window = malloc(chan->spectrum.fft_n * sizeof *chan->spectrum.window);
    assert(chan->spectrum.window != NULL);
    make_kaiser(chan->spectrum.window,chan->spectrum.fft_n,chan->spectrum.kaiser_beta);
    double window_gain = 0;
    for(int i = 0; i < chan->spectrum.fft_n; i++)
      window_gain += chan->spectrum.window[i];

    window_gain = chan->spectrum.fft_n / window_gain;
    for(int i = 0; i < chan->spectrum.fft_n; i++)
      chan->spectrum.window[i] *= window_gain;

    if(frontend->isreal){
      float *in = fftwf_alloc_real(chan->spectrum.fft_n);
      assert(in != NULL);
      float complex *out = fftwf_alloc_complex(chan->spectrum.fft_n/2+1); // N/2 + 1 output points for real->complex
      assert(out != NULL);
      chan->spectrum.plan = plan_r2c(chan->spectrum.fft_n, in, out);
      fftwf_free(in);
      fftwf_free(out);
    } else {
      float complex *in = fftwf_alloc_complex(chan->spectrum.fft_n);
      assert(in != NULL);
      float complex *out = fftwf_alloc_complex(chan->spectrum.fft_n);
      assert(out != NULL);
      chan->spectrum.plan = plan_complex(chan->spectrum.fft_n, in, out, FFTW_FORWARD);
      fftwf_free(in);
      fftwf_free(out);
    }
    assert(chan->spectrum.plan != NULL);
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
    chan->output.samprate = samprate;
    chan->output.channels = 2; // IQ mode
    if(Verbose > 1)
      fprintf(stderr,"spectrum setup: bin count %d, bin_bw %.1lf, samprate %d fft size %d\n",
	      bin_count,bin_bw,samprate,chan->spectrum.fft_n);

    // The channel filter already normalizes for the size of the forward input FFT, we just handle our own FFT gain
    // squared because the we're scaling the output of complex norm, not the input bin values
    //    double const gain = 1.0/ ((double)chan->spectrum.fft_n * chan->spectrum.fft_n);

    int frame_len = samprate / blockrate;
    int r = create_filter_output(&chan->filter.out,&frontend->in,NULL,frame_len,COMPLEX);
    (void)r;
    assert(r == 0);

    chan->filter.max_IF = (double)(samprate - margin)/2;
    chan->filter.min_IF = -chan->filter.max_IF;
    chan->filter2.blocking = 0; // Not used in this mode, make sure it's 0
    set_filter(&chan->filter.out,chan->filter.min_IF,chan->filter.max_IF,chan->filter.kaiser_beta);
    chan->filter.remainder = NAN; // Force init of downconverter
    chan->filter.bin_shift = 1010101010; // Unlikely - but a kludge, force init of phase rotator

    // Generate normalized Kaiser window
    chan->spectrum.window = malloc(chan->spectrum.fft_n * sizeof *chan->spectrum.window);
    make_kaiser(chan->spectrum.window,chan->spectrum.fft_n,chan->spectrum.kaiser_beta);
    double window_gain = 0;
    for(int i = 0; i < chan->spectrum.fft_n; i++)
      window_gain += chan->spectrum.window[i];

    window_gain = chan->spectrum.fft_n / window_gain;
    for(int i = 0; i < chan->spectrum.fft_n; i++)
      chan->spectrum.window[i] *= window_gain;

    // Set up ring buffer for demod output - twice the analysis FFT lengthxg
    chan->spectrum.ring = calloc(RING_SIZE,sizeof *chan->spectrum.ring);
    chan->spectrum.ring_ptr = 0;

    // Set up analysis FFT
    float complex *in = fftwf_alloc_complex(chan->spectrum.fft_n);
    float complex *out = fftwf_alloc_complex(chan->spectrum.fft_n);
    chan->spectrum.plan = plan_complex(chan->spectrum.fft_n, in, out, FFTW_FORWARD);
    assert(chan->spectrum.plan != NULL);
    fftwf_free(in);
    fftwf_free(out);
  }
  // Main loop
  while(downconvert(chan) == 0){ // Wait for new frame
    if(bin_bw > chan->spectrum.crossover)
      continue;

    // Copy received signal to ring buffer
    for(int i=0; i < chan->sampcount; i++){
      chan->spectrum.ring[chan->spectrum.ring_ptr++] = chan->baseband[i];
      if(chan->spectrum.ring_ptr == RING_SIZE)
	chan->spectrum.ring_ptr = 0; // wrap around
    }
  }

  chan->spectrum.fft_n = 0;
  delete_filter_output(&chan->filter.out);
  if(chan->spectrum.plan)
    fftwf_destroy_plan(chan->spectrum.plan);
  chan->spectrum.plan = NULL;
  FREE(chan->spectrum.window);
  FREE(chan->status.command);
  FREE(chan->spectrum.bin_data);
  FREE(chan->spectrum.ring);
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
  // Parameters set by system input side
  int const bin_count = chan->spectrum.bin_count <= 0 ? 64 : chan->spectrum.bin_count;
  // scale each bin value for our FFT
  // squared because the we're scaling the output of complex norm, not the input bin values
  // we only see one side of the spectrum for real inputs
  double const gain = (frontend->isreal ? 2.0f : 1.0f) / ((float)chan->spectrum.fft_n * (float)chan->spectrum.fft_n);
  double const alpha = 0.5; // -3dB/cycle, or -30 dB/sec @ 10 Hz pollrate

  if(bin_bw <= chan->spectrum.crossover){
    // Narrowband mode
    // Copy most recent data from receive ring buffer
    float complex *buffer = fftwf_alloc_complex(chan->spectrum.fft_n);
    assert(buffer != NULL);
    int rp = chan->spectrum.ring_ptr - chan->spectrum.fft_n;
    if(rp < 0)
      rp += RING_SIZE; // back wrap
    for(int i = 0; i < chan->spectrum.fft_n; i++){
      buffer[i] = chan->spectrum.ring[rp++] * chan->spectrum.window[i];
      if(rp == RING_SIZE)
	rp = 0;
    }
    float complex *fft_out = fftwf_alloc_complex(chan->spectrum.fft_n);
    fftwf_execute_dft(chan->spectrum.plan,buffer,fft_out);
    fftwf_free(buffer);
    // DC, positive frequencies, then negative
    int fr = 0;
    for(int i=0; i < bin_count; i++){
      double const p = gain * cnrmf(fft_out[fr++]); // Take power
      chan->spectrum.bin_data[i] += alpha * (p - chan->spectrum.bin_data[i]); // average it in
      assert(isfinite(chan->spectrum.bin_data[i]));
      if(i == bin_count/2)
	fr = chan->spectrum.fft_n - bin_count/2; // skip excess FFT bins
    }
    fftwf_free(fft_out);
    return 0;
  }

  // Wideband mode
  // Asynchronously read newest data from input buffer
  // Look back two FFT blocks from the most recent write pointer to allow room for overlapping windows
  // scale fft bin shift down to size of analysis FFT, which is smaller than the input FFT
  int shift = chan->filter.bin_shift * (int64_t)chan->spectrum.fft_n / master->points;
  if(frontend->isreal){
    float complex *fft_out = fftwf_alloc_complex(chan->spectrum.fft_n/2 + 1);
    assert(fft_out != NULL);
    float *buffer = fftwf_alloc_real(chan->spectrum.fft_n);
    assert(buffer != NULL);

    // Find starting point to read in input A/D stream - 2 buffers back from current write point
    float const *input = frontend->in.input_write_pointer.r;
    input -= 2 * chan->spectrum.fft_n;
    if(input < (float *)frontend->in.input_buffer)
      input += frontend->in.input_buffer_size / sizeof *input; // backward wrap
    // Copy and window raw A/D
    if(shift >= 0){
      // Upright spectrum
      for(int i=0; i < chan->spectrum.fft_n; i++)
	buffer[i] = chan->spectrum.window[i] * input[i];
    } else {
      // Invert spectrum by flipping sign of every other sample
      // equivalent to multiplication by a sinusoid at the Nyquist rate
      for(int i=0; i < chan->spectrum.fft_n; i += 2){
	buffer[i] = chan->spectrum.window[i] * input[i];
	buffer[i+1] = -chan->spectrum.window[i+1] * input[i+1];
      }
      shift = -shift;
    }
    fftwf_execute_dft_r2c(chan->spectrum.plan,buffer,fft_out);
    fftwf_free(buffer);
    // Real input right side up
    // Start with DC + positive frequencies
    int binp = shift;
    for(int i=0;i < bin_count;i++,binp++){
      if(i == bin_count/2)
	binp -= bin_count; // Wrap input to lowest frequency

      if(binp < 0)
	chan->spectrum.bin_data[i] = 0;  // Can't be negative for a real signal
      else if(binp >= chan->spectrum.fft_n/2 + 1) // Nor does it wrap at the top
	chan->spectrum.bin_data[i] = 0;
      else {
	double const p = gain * cnrmf(fft_out[binp]); // Take power
	chan->spectrum.bin_data[i] += alpha * (p - chan->spectrum.bin_data[i]); // average it in
      }
    }
    fftwf_free(fft_out);
  } else {
    // Complex front end (!frontend->isreal)
    float complex *fft_out = fftwf_alloc_complex(chan->spectrum.fft_n);
    float complex const *input = frontend->in.input_write_pointer.c;
    float complex *buffer = fftwf_alloc_complex(chan->spectrum.fft_n);
    assert(buffer != NULL);
    assert(fft_out != NULL);
    // Find starting point to read in input A/D stream - 2 buffers back from current write point
    input -= 2 * chan->spectrum.fft_n;
    if(input < (float complex *)frontend->in.input_buffer)
      input += frontend->in.input_buffer_size / sizeof *input; // backward wrap
    // Copy and window raw A/D
    for(int i=0; i < chan->spectrum.fft_n; i++)
      buffer[i] = chan->spectrum.window[i] * input[i];
    fftwf_execute_dft(chan->spectrum.plan,buffer,fft_out);
    fftwf_free(buffer);
    // Copy requested bins to user
    // Unlike the narrowband downconvert mode, this FFT has to be the same size as the user's requested bin count
    int binp = shift - bin_count/2;
    if(binp < 0)
      binp += chan->spectrum.fft_n; // Start in negative input region

    // Form array of bin energies from lowest frequency to high
    // Lowest frequency in power_buffer[0] to simplify interpolation
    // Handle real inverted!
    for(int i = 0; i < bin_count; i++){
      if(i == bin_count/2){
	binp -= bin_count; // Wrap input to lowest frequency
	if(binp < 0)
	  binp += chan->spectrum.fft_n;
      }
      double const p = gain * cnrmf(fft_out[binp]); // Take power
      chan->spectrum.bin_data[i] += alpha * (p - chan->spectrum.bin_data[i]); // average it in
      assert(isfinite(chan->spectrum.bin_data[i]));
      if(++binp == chan->spectrum.fft_n)
	binp = 0;
    }
    fftwf_free(fft_out);
  }
  return 0;
}
