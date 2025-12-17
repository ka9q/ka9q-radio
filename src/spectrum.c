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

#define OVERLAP (0.0)
// Use at RBW <= 10 Hz to reduce bouncing on pulsed signals like WWV's tones
#define VERY_NARROWBAND_OVERLAP (0.5)
int const point_budget = 512 * 1024; // tunable, experimental

static int spectrum_poll(struct channel *chan);

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
  assert(Blocktime != 0);
  double const blockrate = 1. / Blocktime; // Typically 50 Hz

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
      fprintf(stderr,"wide bin spectrum %u: freq %'lf bin_bw %'f chan->spectrum.bin_count %'d\n",chan->output.rtp.ssrc,chan->tune.freq,chan->spectrum.bin_bw,chan->spectrum.bin_count);

    chan->spectrum.fft_n = (int)round(frontend->samprate / chan->spectrum.bin_bw);
    chan->output.samprate = 0; // Not meaningful
    chan->output.channels = 0;
    if(chan->spectrum.fft_avg <= 0){
      // Experiment: vary the number of averaged blocks of A/D samples with the resolution bandwidth to keep the
      // total FFT points processed roughly constant and the CPU load also roughly constant
      // This helps with noise variance at wider spans, not as much with medium spans
      chan->spectrum.fft_avg = (int)ceil((double)point_budget / chan->spectrum.fft_n);
    }
    // Dummy just so downconvert() will block on each frame
    int r = create_filter_output(&chan->filter.out,&frontend->in,NULL,0,SPECTRUM);
    assert(r == 0);
    (void)r;
  } else {
    // Set up downconvert mode
    double const margin = 400; // Allow 400 Hz for filter skirts at edge of I/Q receiver
    unsigned long const samprate_base = lcm((unsigned long)blockrate,(unsigned long)(L*blockrate/N)); // Samprate must be allowed by receiver
    chan->spectrum.fft_n = (int)round(chan->spectrum.bin_count + margin / chan->spectrum.bin_bw); // Minimum for search to avoid receiver filter skirt
    // This (int) cast should be cleaned up
    while(chan->spectrum.fft_n < 65536 && (!goodchoice(chan->spectrum.fft_n) || (int)round(chan->spectrum.fft_n * chan->spectrum.bin_bw) % samprate_base != 0))
      chan->spectrum.fft_n++;
    chan->output.samprate = (int)round(chan->spectrum.fft_n * chan->spectrum.bin_bw);
    chan->output.channels = 2; // IQ mode
    if(Verbose > 1)
      fprintf(stderr,"narrow bin spectrum: bin count %d, bin_bw %.1lf, samprate %d fft size %d\n",
	      chan->spectrum.bin_count,chan->spectrum.bin_bw,chan->output.samprate,chan->spectrum.fft_n);

    int blocklen = (int)round(chan->output.samprate/blockrate);

    int r = create_filter_output(&chan->filter.out,&frontend->in,NULL,blocklen,COMPLEX);
    (void)r;
    assert(r == 0);

    chan->filter.max_IF = (double)(chan->output.samprate - margin)/2;
    chan->filter.min_IF = -chan->filter.max_IF;
    chan->filter2.blocking = 0; // Not used in this mode, make sure it's 0
    set_filter(&chan->filter.out,chan->filter.min_IF,chan->filter.max_IF,chan->filter.kaiser_beta);
    chan->filter.remainder = NAN; // Force init of downconverter
    chan->filter.bin_shift = 1010101010; // Unlikely - but a kludge, force init of phase rotator

    if(chan->spectrum.fft_avg <= 0)
      chan->spectrum.fft_avg = 2; // generalize this

    // Set up ring buffer for demod output - CHAN->SPECTRUM.FFT_AVG times the analysis FFT length
    chan->spectrum.ring_size = chan->spectrum.fft_avg * chan->spectrum.fft_n;
    assert(chan->spectrum.ring_size > 0);
    chan->spectrum.ring = calloc(chan->spectrum.ring_size,sizeof *chan->spectrum.ring);
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
  switch(chan->spectrum.window_type){
  default:
  case KAISER_WINDOW: // If beta == 0, same as rectangular
    make_kaiserf(chan->spectrum.window,chan->spectrum.fft_n,chan->spectrum.shape);
    break;
  case RECT_WINDOW:
    for(int i=0; i < chan->spectrum.fft_n; i++)
      chan->spectrum.window[i] = 1;
    break;
  case BLACKMAN_WINDOW:
    for(int i=0; i < chan->spectrum.fft_n; i++)
      chan->spectrum.window[i] = blackman_window(i,chan->spectrum.fft_n);
    break;
  case EXACT_BLACKMAN_WINDOW:
    for(int i=0; i < chan->spectrum.fft_n; i++)
      chan->spectrum.window[i] = exact_blackman_window(i,chan->spectrum.fft_n);
    break;
  case GAUSSIAN_WINDOW:
    // Reuse kaiser β as σ parameter
    // note σ = 0 is a pathological value for gaussian, it's an impulse with infinite sidelobes
    for(int i=0; i < chan->spectrum.fft_n; i++)
      chan->spectrum.window[i] = gaussian_window(i,chan->spectrum.fft_n,chan->spectrum.shape);
    break;
  case HANN_WINDOW:
    for(int i=0; i < chan->spectrum.fft_n; i++)
      chan->spectrum.window[i] = hann_window(i,chan->spectrum.fft_n);
    break;
  case HAMMING_WINDOW:
    for(int i=0; i < chan->spectrum.fft_n; i++)
      chan->spectrum.window[i] = hamming_window(i,chan->spectrum.fft_n);
  }
  normalize_windowf(chan->spectrum.window,chan->spectrum.fft_n);

  // Compute noise bandwidth of each bin in bins
  chan->spectrum.noise_bw = 0;
  for(int i=0; i < chan->spectrum.fft_n; i++)
    chan->spectrum.noise_bw += (double)chan->spectrum.window[i] * chan->spectrum.window[i];

  // Scale to the actual bin bandwidth
  // This also has to be divided by the square of the sum of the window values, but that's already normalized to 1
  chan->spectrum.noise_bw *= chan->spectrum.bin_bw / chan->spectrum.fft_n;

  bool restart_needed = false;
  bool response_needed = true;

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
	if(chan->spectrum.ring_idx == chan->spectrum.ring_size)
	  chan->spectrum.ring_idx = 0; // wrap around
      }
    }
    if(response_needed)
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
  chan->spectrum.fft_avg = 0; // cause it to be regenerated next time (temp hack)
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


  memset(chan->spectrum.bin_data,0, chan->spectrum.bin_count * sizeof *chan->spectrum.bin_data); // zero output data

  if(chan->spectrum.bin_bw <= chan->spectrum.crossover){
    // Narrowband mode

    if(chan->spectrum.ring == NULL)
      return 0; // Needed
    // Most recent data from receive ring buffer
    int rp = chan->spectrum.ring_idx - chan->spectrum.fft_n;
    if(rp < 0)
      rp += chan->spectrum.ring_size;

    float complex *fft_in = fftwf_alloc_complex(chan->spectrum.fft_n);
    assert(fft_in != NULL);
    float complex *fft_out = fftwf_alloc_complex(chan->spectrum.fft_n);
    assert(fft_out != NULL);

    // scale each bin value for our FFT
    // squared because the we're scaling the output of complex norm, not the input bin values
    // we only see one side of the spectrum for real inputs
    double const gain = 1./chan->spectrum.fft_avg * (frontend->isreal ? 2.0 : 1.0) / ((double)chan->spectrum.fft_n * chan->spectrum.fft_n);

    for(int iter=0; iter < chan->spectrum.fft_avg; iter++){
      // Copy and window raw baseband
      for(int i = 0; i < chan->spectrum.fft_n; i++){
	assert(rp >= 0 && rp < chan->spectrum.ring_size);
	fft_in[i] = chan->spectrum.ring[rp++] * chan->spectrum.window[i];
	if(rp >= chan->spectrum.ring_size)
	  rp -= chan->spectrum.ring_size;
      }
      fftwf_execute_dft(chan->spectrum.plan,fft_in,fft_out);
      // DC to Nyquist-1, then -Nyquist to -1
      int fr = 0;
      for(int i=0; i < chan->spectrum.bin_count; i++){
	if(i == chan->spectrum.bin_count/2)
	  fr = chan->spectrum.fft_n - i; // skip over excess FFT bins at edges
	assert(fr >= 0 && fr < chan->spectrum.fft_n);
	double const p = chan->spectrum.bin_data[i] + gain * cnrmf(fft_out[fr++]);
	assert(isfinite(p));
	chan->spectrum.bin_data[i] = (float)p;
      }
      // rp now points to *next* buffer, so move it back between 1 and 2 buffers depending on overlap
      if(chan->spectrum.bin_bw <= 10)
	rp -= (int)round(chan->spectrum.fft_n * (2. - VERY_NARROWBAND_OVERLAP));
      else
	rp -= (int)round(chan->spectrum.fft_n * (2. - OVERLAP));

      if(rp < 0)
	rp += chan->spectrum.ring_size;
    }
    fftwf_free(fft_in);
    fftwf_free(fft_out);
    return 0;
  }

  // Wideband mode
  // Asynchronously read newest data from input buffer
  // Look back two FFT blocks from the most recent write pointer to allow room for overlapping windows
  // scale fft bin shift down to size of analysis FFT, which is smaller than the input FFT
  int shift = (int)(chan->filter.bin_shift * (int64_t)chan->spectrum.fft_n / master->points);

  // scale each bin value for our FFT
  // squared because the we're scaling the output of complex norm, not the input bin values
  // we only see one side of the spectrum for real inputs
  double const gain = 1./chan->spectrum.fft_avg * (frontend->isreal ? 2.0 : 1.0) / ((double)chan->spectrum.fft_n * chan->spectrum.fft_n);

  if(frontend->isreal){
    // Point into raw SDR A/D input ring buffer
    // We're reading from a mirrored buffer so it will automatically wrap back to the beginning
    // as long as it doesn't go past twice the buffer length
    float const *input = frontend->in.input_write_pointer.r - chan->spectrum.fft_n; // 1 FFT buffer back
    if(input < (float *)frontend->in.input_buffer)
      input += frontend->in.input_buffer_size / sizeof *input; // wrap backward

    float *fft_in = fftwf_alloc_real(chan->spectrum.fft_n);
    assert(fft_in != NULL);
    float complex *fft_out = fftwf_alloc_complex(chan->spectrum.fft_n/2 + 1); // r2c has only the positive frequencies
    assert(fft_out != NULL);

    for(int iter=0; iter < chan->spectrum.fft_avg; iter++){
      // Copy and window raw A/D
      if(shift >= 0){
	// Upright spectrum
	for(int i=0; i < chan->spectrum.fft_n; i++)
	  fft_in[i] = chan->spectrum.window[i] * input[i];
      } else {
	// Invert spectrum by flipping sign of every other sample - UNTESTED
	// equivalent to multiplication by a sinusoid at the Nyquist rate
	// If FFT N is odd, just forget the odd last sample.
	// We don't have to track the sign flip phase because we're only summing energy
	for(int i=0; i < chan->spectrum.fft_n-1; i += 2){
	  fft_in[i] = chan->spectrum.window[i] * input[i];
	  fft_in[i+1] = -chan->spectrum.window[i+1] * input[i+1];
	}
	if(chan->spectrum.fft_n & 1)
	  fft_in[chan->spectrum.fft_n-1] = 0;
	shift = -shift; // make positive
      }
      fftwf_execute_dft_r2c(chan->spectrum.plan,fft_in,fft_out);

      // Spectrum is always right side up so shift is never negative
      // Start with DC + positive frequencies, then wrap to negative
      int binp = shift;
      assert(binp >= 0);
      for(int i=0;i < chan->spectrum.bin_count && binp < chan->spectrum.fft_n/2+1 ; i++,binp++){
	if(i == chan->spectrum.bin_count/2)
	  binp -= chan->spectrum.bin_count/2; // crossed into negative output rang, Wrap input back to lowest frequency requested

	double const p = chan->spectrum.bin_data[i] + gain * cnrmf(fft_out[binp]); // Accumulate power
	assert(isfinite(p));
	chan->spectrum.bin_data[i] = (float)p; // Accumulate power
      }
      input -= (int)round(chan->spectrum.fft_n * (1. - OVERLAP)); // move back fraction of a buffer
      if(input < (float *)frontend->in.input_buffer)
	input += frontend->in.input_buffer_size / sizeof *input; // wrap backward
    }
    fftwf_free(fft_in);
    fftwf_free(fft_out);
  } else {
    // Complex front end (frontend->isreal == false)
    // Find starting points to read in input A/D stream
    // UNTESTED
    float complex const *input = frontend->in.input_write_pointer.c - chan->spectrum.fft_n; // 1 buffer back
    input += (input < (float complex *)frontend->in.input_buffer) ? frontend->in.input_buffer_size / sizeof *input : 0; // backward wrap

    float complex *fft_in = fftwf_alloc_complex(chan->spectrum.fft_n);
    assert(fft_in != NULL);
    float complex *fft_out = fftwf_alloc_complex(chan->spectrum.fft_n);
    assert(fft_out != NULL);

    for(int iter=0; iter < chan->spectrum.fft_avg; iter++){
      // Copy and window raw A/D
      for(int i=0; i < chan->spectrum.fft_n; i++)
	fft_in[i] = chan->spectrum.window[i] * input[i];

      fftwf_execute_dft(chan->spectrum.plan,fft_in,fft_out);

      // Copy requested bins to user, starting with requested frequency
      int binp;
      int i = 0;

      if(shift >= 0){
	// Starts in positive spectrum
	if(shift >= chan->spectrum.fft_n/2) // starts past end of spectrum, nothing to return
	  goto done;
	binp = shift;
      } else {
	// shift < 0, starts in negative spectrum
	if(-shift >= chan->spectrum.fft_n)
	  goto done; // Nothing overlaps, quit
	if(-shift >= chan->spectrum.fft_n/2){ // before start of input spectrum
	  i = -shift - chan->spectrum.fft_n/2;
	  binp = chan->spectrum.fft_n/2; // start input at lowest negative frequency
	} else {
	  binp = chan->spectrum.fft_n + shift;
	}
      }
      do {
	assert(binp >= 0 && binp < chan->spectrum.fft_n && i >= 0 && i < chan->spectrum.bin_count);
	double const p = chan->spectrum.bin_data[i] + gain * cnrmf(fft_out[binp]);
	assert(isfinite(p));
	chan->spectrum.bin_data[i] = (float)p;
	// Increment and wrap indices
	if(++i == chan->spectrum.bin_count)
	  i = 0; // wrap to DC
	if(++binp == chan->spectrum.fft_n)
	  binp = 0; // wrap to DC
      } while(i != chan->spectrum.bin_count/2 && binp != chan->spectrum.fft_n/2); // upper ends of positive frequncies
    done:;

      // Back to previous buffer
      input -= (int)round(chan->spectrum.fft_n * (1. - OVERLAP));
      if(input < (float complex *)frontend->in.input_buffer)
	input += frontend->in.input_buffer_size / sizeof *input; // backward wrap
    }
    fftwf_free(fft_in);
    fftwf_free(fft_out);
  }

  return 0;
}
