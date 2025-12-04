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

static int spectrum_poll(struct channel *chan);

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
  chan->status.output_interval = 0; // No automatic status updates
  chan->status.output_timer = 0; // No automatic status updates
  chan->output.silent = true; // we don't send anything there
  pthread_mutex_unlock(&chan->status.lock);

  // Parameters set by system input side
  double const blockrate = 1000.0 / Blocktime; // Typically 50 Hz

  int const L = frontend->L;
  int const M = frontend->M;
  int const N = L + M - 1;

#if 1
  realtime(chan->prio - 10); // Drop below demods
#endif

  chan->spectrum.bin_data = calloc(chan->spectrum.bin_count,sizeof *chan->spectrum.bin_data);
  assert(chan->spectrum.bin_data != NULL);
  if(chan->spectrum.bin_bw > chan->spectrum.crossover){
    // Direct Wideband mode. Setup FFT to work on raw A/D input
    // What can we do about unfriendly sizes? Anything?
    if(Verbose > 1)
      fprintf(stderr,"wide bin spectrum %d: freq %'lf bin_bw %'f chan->spectrum.bin_count %'d\n",chan->output.rtp.ssrc,chan->tune.freq,chan->spectrum.bin_bw,chan->spectrum.bin_count);

    chan->spectrum.fft_n = frontend->samprate / chan->spectrum.bin_bw;
    chan->output.samprate = 0; // Not meaningful
    chan->output.channels = 0;

    // Dummy just so downconvert() will block on each frame
    int r = create_filter_output(&chan->filter.out,&frontend->in,NULL,0,SPECTRUM);
    assert(r == 0);
    (void)r;
  } else {
    // Set up downconvert mode
    double const margin = 400; // Allow 400 Hz for filter skirts at edge of I/Q receiver
    int const samprate_base = lcm(blockrate,L*blockrate/N); // Samprate must be allowed by receiver
    chan->spectrum.fft_n = chan->spectrum.bin_count + margin / chan->spectrum.bin_bw; // Minimum for search to avoid receiver filter skirt
    // This (int) cast should be cleaned up
    while(chan->spectrum.fft_n < 65536 && (!goodchoice(chan->spectrum.fft_n) || (int)round(chan->spectrum.fft_n * chan->spectrum.bin_bw) % samprate_base != 0))
      chan->spectrum.fft_n++;
    chan->output.samprate = chan->spectrum.fft_n * chan->spectrum.bin_bw;
    chan->output.channels = 2; // IQ mode
    if(Verbose > 1)
      fprintf(stderr,"narrow bin spectrum: bin count %d, bin_bw %.1lf, samprate %d fft size %d\n",
	      chan->spectrum.bin_count,chan->spectrum.bin_bw,chan->output.samprate,chan->spectrum.fft_n);

    // The channel filter already normalizes for the size of the forward input FFT, we just handle our own FFT gain
    // squared because the we're scaling the output of complex norm, not the input bin values
    //    double const gain = 1.0/ ((double)chan->spectrum.fft_n * chan->spectrum.fft_n);

    int r = create_filter_output(&chan->filter.out,&frontend->in,NULL,chan->output.samprate/blockrate,COMPLEX);
    (void)r;
    assert(r == 0);

    chan->filter.max_IF = (double)(chan->output.samprate - margin)/2;
    chan->filter.min_IF = -chan->filter.max_IF;
    chan->filter2.blocking = 0; // Not used in this mode, make sure it's 0
    set_filter(&chan->filter.out,chan->filter.min_IF,chan->filter.max_IF,chan->filter.kaiser_beta);
    chan->filter.remainder = NAN; // Force init of downconverter
    chan->filter.bin_shift = 1010101010; // Unlikely - but a kludge, force init of phase rotator

    // Set up ring buffer for demod output - twice the analysis FFT lengthxg
    chan->spectrum.ring = calloc(RING_SIZE,sizeof *chan->spectrum.ring);
    chan->spectrum.ring_idx = 0;
  }
  // Set up analysis FFT
  if(chan->spectrum.bin_bw > chan->spectrum.crossover && frontend->isreal){
    // Wideband mode with real front end; use real->complex FFT
    float *in = fftwf_alloc_real(chan->spectrum.fft_n);
    assert(in != NULL);
    float complex *out = fftwf_alloc_complex(chan->spectrum.fft_n/2+1); // N/2 + 1 output points for real->complex
    assert(out != NULL);
    chan->spectrum.plan = plan_r2c(chan->spectrum.fft_n, in, out);
    fftwf_free(in);
    fftwf_free(out);
  } else {
    // Wideband mode with complex front end, or narrowband mode with either front end
    float complex *in = fftwf_alloc_complex(chan->spectrum.fft_n);
    assert(in != NULL);
    float complex *out = fftwf_alloc_complex(chan->spectrum.fft_n);
    assert(out != NULL);
    chan->spectrum.plan = plan_complex(chan->spectrum.fft_n, in, out, FFTW_FORWARD);
    fftwf_free(in);
    fftwf_free(out);
  }
  assert(chan->spectrum.plan != NULL);

  // Generate normalized Kaiser window
  chan->spectrum.window = malloc(chan->spectrum.fft_n * sizeof *chan->spectrum.window);
  assert(chan->spectrum.window != NULL);
  make_kaiserf(chan->spectrum.window,chan->spectrum.fft_n,chan->spectrum.kaiser_beta);
  bool restart_needed = true;
  bool response_needed = false;

  // Main loop
  do {
    response(chan,response_needed);
    response_needed = false;

    // Look on the single-entry command queue and grab it atomically
    pthread_mutex_lock(&chan->status.lock);
    if(chan->status.command != NULL){
      restart_needed = decode_radio_commands(chan,chan->status.command,chan->status.length);
      FREE(chan->status.command);
      response_needed = true;
    }
    pthread_mutex_unlock(&chan->status.lock);

    if(restart_needed || downconvert(chan) != 0)
      break; // No response sent this time

    if(chan->spectrum.bin_bw <= chan->spectrum.crossover){
      // Narrowband mode
      // Copy received signal to ring buffer
      assert(chan->baseband != NULL);
      assert(chan->spectrum.ring != NULL);
      for(int i=0; i < chan->sampcount; i++){
	chan->spectrum.ring[chan->spectrum.ring_idx++] = chan->baseband[i];
	if(chan->spectrum.ring_idx == RING_SIZE)
	  chan->spectrum.ring_idx = 0; // wrap around
      }
    }
    spectrum_poll(chan);
  } while(true);

  chan->spectrum.fft_n = 0;
  delete_filter_output(&chan->filter.out);
  chan->baseband = NULL;
  if(chan->spectrum.plan)
    fftwf_destroy_plan(chan->spectrum.plan);
  chan->spectrum.plan = NULL;
  FREE(chan->spectrum.window);
  FREE(chan->status.command);
  FREE(chan->spectrum.bin_data);
  FREE(chan->spectrum.ring);
  return 0;
}

// Called at poll time
// Runs FFTs, updates chan->spectrum.bin_data[]
static int spectrum_poll(struct channel *chan){

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

  // scale each bin value for our FFT
  // squared because the we're scaling the output of complex norm, not the input bin values
  // we only see one side of the spectrum for real inputs
  double const gain = (frontend->isreal ? 2.0f : 1.0f) / ((float)chan->spectrum.fft_n * (float)chan->spectrum.fft_n);

  if(chan->spectrum.bin_bw <= chan->spectrum.crossover){
    // Narrowband mode
    if(chan->spectrum.ring == NULL)
      return 0; // Needed
    // Copy most recent data from receive ring buffer, 50% overlap
    int rp0 = chan->spectrum.ring_idx - 3 * chan->spectrum.fft_n / 2;
    rp0 += (rp0 < 0) ? RING_SIZE : 0;
    int rp1 = chan->spectrum.ring_idx - chan->spectrum.fft_n;
    rp1 += (rp1 < 0) ? RING_SIZE : 0;

    float complex *buffer0 = fftwf_alloc_complex(chan->spectrum.fft_n);
    float complex *buffer1 = fftwf_alloc_complex(chan->spectrum.fft_n);
    assert(buffer0 != NULL); assert(buffer1 != NULL);

    for(int i = 0; i < chan->spectrum.fft_n; i++){
      buffer0[i] = chan->spectrum.ring[rp0++] * chan->spectrum.window[i];
      rp0 -= (rp0 >= RING_SIZE) ? RING_SIZE : 0;
      buffer1[i] = chan->spectrum.ring[rp1++] * chan->spectrum.window[i];
      rp1 -= (rp1 >= RING_SIZE) ? RING_SIZE : 0;
    }
    float complex *fft_out0 = fftwf_alloc_complex(chan->spectrum.fft_n);
    fftwf_execute_dft(chan->spectrum.plan,buffer0,fft_out0);
    fftwf_free(buffer0);

    float complex *fft_out1 = fftwf_alloc_complex(chan->spectrum.fft_n);
    fftwf_execute_dft(chan->spectrum.plan,buffer1,fft_out1);
    fftwf_free(buffer1);

    // DC, then positive frequencies, then negative
    int fr = 0;
    for(int i=0; i < chan->spectrum.bin_count; i++){
      double const p0 = cnrmf(fft_out0[fr]); // Take power
      double const p1 = cnrmf(fft_out1[fr++]); // Take power
      chan->spectrum.bin_data[i] = 0.5 * gain * (p0 + p1); // Average the overlapping window spectra (could fold 0.5 into gain)
      assert(isfinite(chan->spectrum.bin_data[i]));
      if(i == chan->spectrum.bin_count/2)
	fr = chan->spectrum.fft_n - chan->spectrum.bin_count/2; // skip over excess FFT bins
    }
    fftwf_free(fft_out0);
    fftwf_free(fft_out1);
    return 0;
  }

  // Wideband mode
  // Asynchronously read newest data from input buffer
  // Look back two FFT blocks from the most recent write pointer to allow room for overlapping windows
  // scale fft bin shift down to size of analysis FFT, which is smaller than the input FFT
  int shift = chan->filter.bin_shift * (int64_t)chan->spectrum.fft_n / master->points;
  if(frontend->isreal){
    // Find starting point to read in input A/D stream - two FFTs with 50% overlap
    // We're reading from a mirrored buffer so it will automatically wrap back to the beginning
    // as long as it doesn't go past twice the buffer length
    float const *input0 = frontend->in.input_write_pointer.r - 3 * chan->spectrum.fft_n/2; // 1.5 buffers back
    input0 += (input0 < (float *)frontend->in.input_buffer) ? frontend->in.input_buffer_size / sizeof *input0 : 0; // backward wrap
    float const *input1 = input0 + chan->spectrum.fft_n/2;

    // Copy and window raw A/D
    float *buffer0 = fftwf_alloc_real(chan->spectrum.fft_n);
    assert(buffer0 != NULL);

    float *buffer1 = fftwf_alloc_real(chan->spectrum.fft_n);
    assert(buffer1 != NULL);

    if(shift >= 0){
      // Upright spectrum
      for(int i=0; i < chan->spectrum.fft_n; i++){
	buffer0[i] = chan->spectrum.window[i] * input0[i];
	buffer1[i] = chan->spectrum.window[i] * input1[i];
      }
    } else {
      // Invert spectrum by flipping sign of every other sample
      // equivalent to multiplication by a sinusoid at the Nyquist rate
      for(int i=0; i < chan->spectrum.fft_n; i += 2){
	buffer0[i] = chan->spectrum.window[i] * input0[i];
	buffer0[i+1] = -chan->spectrum.window[i+1] * input0[i+1];
	buffer1[i] = chan->spectrum.window[i] * input1[i];
	buffer1[i+1] = -chan->spectrum.window[i+1] * input1[i+1];
      }
      shift = -shift;
    }
    float complex *fft_out0 = fftwf_alloc_complex(chan->spectrum.fft_n/2 + 1); // r2c has only the positive frequencies
    assert(fft_out0 != NULL);

    fftwf_execute_dft_r2c(chan->spectrum.plan,buffer0,fft_out0);
    fftwf_free(buffer0);

    float complex *fft_out1 = fftwf_alloc_complex(chan->spectrum.fft_n/2 + 1);
    assert(fft_out1 != NULL);

    fftwf_execute_dft_r2c(chan->spectrum.plan,buffer1,fft_out1);
    fftwf_free(buffer1);

    // Real input right side up
    // Start with DC + positive frequencies, then wrap to negative
    int binp = shift;
    for(int i=0;i < chan->spectrum.bin_count;i++,binp++){
      if(i == chan->spectrum.bin_count/2)
	binp -= chan->spectrum.bin_count; // Wrap input to lowest frequency

      if(binp < 0)
	chan->spectrum.bin_data[i] = 0;  // Can't be negative for a real signal
      else if(binp >= chan->spectrum.fft_n/2 + 1) // Nor does it wrap at the top
	chan->spectrum.bin_data[i] = 0;
      else {
	assert(binp >= 0 && binp < chan->spectrum.fft_n);
	double const p0 = cnrmf(fft_out0[binp]); // Take power
	double const p1 = cnrmf(fft_out1[binp]); // Take power
	chan->spectrum.bin_data[i] = 0.5 * gain * (p0 + p1); // Average the two
	assert(isfinite(chan->spectrum.bin_data[i]));
      }
    }
    fftwf_free(fft_out0);
    fftwf_free(fft_out1);
  } else {
    // Complex front end (frontend->isreal == false)
    // Find starting points to read in input A/D stream
    float complex const *input0 = frontend->in.input_write_pointer.c - 3 * chan->spectrum.fft_n/2; // 1.5 buffers back
    input0 += (input0 < (float complex *)frontend->in.input_buffer) ? frontend->in.input_buffer_size / sizeof *input0 : 0; // backward wrap
    float complex const *input1 = input0 + chan->spectrum.fft_n/2; // 1.0 buffers backa

    float complex *buffer0 = fftwf_alloc_complex(chan->spectrum.fft_n);
    assert(buffer0 != NULL);
    float complex *buffer1 = fftwf_alloc_complex(chan->spectrum.fft_n);
    assert(buffer1 != NULL);

    // Copy and window raw A/D
    for(int i=0; i < chan->spectrum.fft_n; i++){
      buffer0[i] = chan->spectrum.window[i] * input0[i];
      buffer1[i] = chan->spectrum.window[i] * input1[i];
    }
    float complex *fft_out0 = fftwf_alloc_complex(chan->spectrum.fft_n);
    assert(fft_out0 != NULL);
    fftwf_execute_dft(chan->spectrum.plan,buffer0,fft_out0);
    fftwf_free(buffer0);

    float complex *fft_out1 = fftwf_alloc_complex(chan->spectrum.fft_n);
    assert(fft_out1 != NULL);
    fftwf_execute_dft(chan->spectrum.plan,buffer1,fft_out1);
    fftwf_free(buffer1);

    // Copy requested bins to user
    // Unlike the narrowband downconvert mode, this FFT has to be the same size as the user's requested bin count
    int binp = shift - chan->spectrum.bin_count/2;
    binp += (binp < 0) ? chan->spectrum.fft_n : 0; // Start in negative input region

    // Form array of bin energies from lowest frequency to high
    // Lowest frequency in power_buffer[0] to simplify interpolation
    // Handle real inverted!
    for(int i = 0; i < chan->spectrum.bin_count; i++,binp++){
      binp = (binp == chan->spectrum.fft_n) ? 0 : binp;
      if(i == chan->spectrum.bin_count/2){
	binp -= chan->spectrum.bin_count; // Wrap input to lowest frequency
	binp += (binp < 0) ? chan->spectrum.fft_n : 0;
      }
      assert(binp >= 0 && binp < chan->spectrum.fft_n);
      double const p0 = gain * cnrmf(fft_out0[binp]); // Take power
      double const p1 = gain * cnrmf(fft_out1[binp]); // Take power
      chan->spectrum.bin_data[i] = (p0 + p1)/2;       // average it in
      assert(isfinite(chan->spectrum.bin_data[i]));
    }
    fftwf_free(fft_out0);
    fftwf_free(fft_out1);
  }
  return 0;
}
