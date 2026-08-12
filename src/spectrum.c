//#define SPECTRUM_DEBUG 1
// Spectral analysis service for ka9q-radio's radiod
// Copyright 2023-2026, Phil Karn, KA9Q
#include <assert.h>
#include <pthread.h>
#include <string.h>
#include <math.h>
#include <complex.h>
#include <fftw3.h>
#include <unistd.h>
#include "misc.h"
#include "iir.h"
#include "filter.h"
#include "radio.h"
#include "window.h"

//#define RICE 1
//#define SPECTRUM_CLIP 1
//#define FIXED_STEP  1

static void generate_window(chan_t *);
static void setup_real_fft(chan_t *);
static void setup_complex_fft(chan_t *);
static void setup_wideband(chan_t *);
static void setup_narrowband(chan_t *);
static void narrowband_poll(chan_t *);
static void wideband_poll(chan_t *);
#if RICE
static void rice(chan_t *);
#endif

// Spectrum analysis thread
int demod_spectrum(void *arg){
  chan_t * const chan = arg;
  assert(chan != NULL);
  if(chan == NULL)
    return -1;

  pthread_mutex_lock(&chan->status.lock);
  chan->status.output_interval = 0; // No automatic status updates
  chan->status.output_timer = 0; // No automatic status updates
  chan->output.silent = true; // we don't send anything there
  pthread_mutex_unlock(&chan->status.lock);

  // Parameters set by system input side
  assert(Blocktime != 0);
  int const prio = chan->prio >= 10 ? chan->prio - 10 : 0; // don't let it go negative
  realtime(prio); // Drop below demods
  if(chan->spectrum.fft_avg <= 0)
    chan->spectrum.fft_avg = 1;     // force legal

  bool restart_needed = false;
  bool response_needed = true;
  // Watch for parameter changes and do them in the loop so we don't have to force a restart
  enum window_type window_type = -1; // force generation on first loop
  double rbw = -1;
  int bin_count = -1;
  int crossover = -1;
  double shape = -1;
  int timeout = 0;

  // Main loop
  do {
    response(chan,response_needed);
    response_needed = false;

    // Look on the single-entry command queue, grab it atomically and execute it
    pthread_mutex_lock(&chan->status.lock);
    // Look on the command queue and grab just one atomically
    for(int i=0;i < CQLEN; i++){
      if(chan->commands[i].buffer != NULL){
	restart_needed = decode_radio_commands(chan,chan->commands[i].buffer,
					       chan->commands[i].length);
	FREE(chan->commands[i].buffer);
	chan->commands[i].length = 0;
	response_needed = true;
	break;
      }
    }
    pthread_mutex_unlock(&chan->status.lock);
    // Must handle possible parameter changes from decode_radio_commands() BEFORE executing the downconverter,
    // which will act immediately on those changes. Otherwise segfaults occur when crossing between wideband and narrowband
    // modes because things are not properly set up for the poll when it comes
    if(chan->spectrum.bin_count < 1 || chan->spectrum.rbw == 0){
      // can't do anything yet; wait for another command
      useconds_t s = 1e6 * Blocktime;
      usleep(s);
      continue;
    }
    if((chan->spectrum.rbw > chan->spectrum.crossover) != (rbw > crossover)) // note nested booleans
      chan->spectrum.fft_n = -1; // force setup

    if(chan->spectrum.rbw != rbw || chan->spectrum.bin_count != bin_count)
      chan->spectrum.fft_n = -1; // force setup;

    // fairly major reinitialization required
    if(chan->spectrum.fft_n <= 0){
      FREE(chan->spectrum.window); // force regeneration on first poll
      if(chan->spectrum.rbw > chan->spectrum.crossover)
	setup_wideband(chan);
      else
	setup_narrowband(chan);
      // Remember the new values
      rbw = chan->spectrum.rbw;
      bin_count = chan->spectrum.bin_count;
      crossover = chan->spectrum.crossover;
      shape = chan->spectrum.shape;
      window_type = chan->spectrum.window_type;
    } else if(chan->spectrum.window_type != window_type
	      || (chan->spectrum.shape != shape && (chan->spectrum.window_type == KAISER_WINDOW
						    || chan->spectrum.window_type == GAUSSIAN_WINDOW))){
      FREE(chan->spectrum.window); // force regeneration
      shape = chan->spectrum.shape;
      window_type = chan->spectrum.window_type;
    }
    // End of parameter checking and (re)initialization
    int r;
    if(restart_needed || (r = downconvert(chan)) == -1)
      break; // restart or terminate
    else if(r == 1)
      continue; // channel inactive; poll for commands

    // r == 0 is normal return
    // Process receiver data only in narrowband mode
    if(chan->spectrum.rbw <= chan->spectrum.crossover && chan->baseband != NULL){
      if(chan->spectrum.ring == NULL || chan->spectrum.ring_size < chan->spectrum.fft_avg * chan->spectrum.fft_n){

	// Need a new or bigger baseband ring buffer
	if(chan->spectrum.ring == NULL)
	  chan->spectrum.ring_idx = 0; // ? no need to reset on growth vs start?

	int old_ring_size = chan->spectrum.ring_size;
	chan->spectrum.ring_size = chan->spectrum.fft_avg * chan->spectrum.fft_n;
	assert(chan->spectrum.ring_size > 0);
	void *old = chan->spectrum.ring;
	int newsize =  chan->spectrum.ring_size * sizeof *chan->spectrum.ring;
	assert(newsize > 0);
	chan->spectrum.ring = realloc(chan->spectrum.ring,newsize);
	if(chan->spectrum.ring == NULL){
	  fprintf(stderr,"spectrum: realloc(%d) failed\n",newsize);
	  FREE(old); // emulate reallocf() in case the realloc fails
	  goto quit;
	}
	// Clear the new space to avoid display glitches
	assert(newsize >= old_ring_size);
	memset(chan->spectrum.ring + old_ring_size, 0, (chan->spectrum.ring_size - old_ring_size) * sizeof *chan->spectrum.ring);
      }
      assert(chan->spectrum.ring != NULL);
      for(int i=0; i < chan->sampcount; i++){
	chan->spectrum.ring[chan->spectrum.ring_idx++] = chan->baseband[i];
	if(chan->spectrum.ring_idx == chan->spectrum.ring_size)
	  chan->spectrum.ring_idx = 0; // wrap around
      }
      timeout -= chan->sampcount;
      if(timeout < 0)
	timeout = 0;
    }
    if(response_needed){      // Generate new bin data for the next response
      // Make sure output frequency bin data buffers exist
      if(chan->spectrum.bin_data == NULL || chan->spectrum.bin_count != bin_count){
	void *old = chan->spectrum.ring;
	chan->spectrum.bin_data = realloc(chan->spectrum.bin_data, chan->spectrum.bin_count * sizeof *chan->spectrum.bin_data);
	if(chan->spectrum.bin_data == NULL){
	  FREE(old); // emulate reallocf()
	  goto quit;
	}
      }
      if(chan->spectrum.rbw <= chan->spectrum.crossover){
#if 0
	// Don't run FFT more often than one FFT's worth of samples
        if(timeout <= 0){
	  narrowband_poll(chan);
	  timeout = chan->spectrum.fft_n;
	}
#else
	narrowband_poll(chan);
#endif
      } else
	wideband_poll(chan);
#ifdef RICE
      rice(chan); // experiment with rice encoding
#endif
    }
    // Remember new values in case they change next time
    rbw = chan->spectrum.rbw;
    bin_count = chan->spectrum.bin_count;
    crossover = chan->spectrum.crossover;
    window_type = chan->spectrum.window_type;
    shape = chan->spectrum.shape;
  } while(true);
  response(chan,response_needed); // in case one is pending as we're restarting

 quit:;
  if(Verbose > 1)
    fprintf(stderr,"%s returning\n",chan->name);

  chan->spectrum.fft_n = 0;
  delete_filter_output(&chan->filter.out);
  chan->baseband = NULL;
  if(chan->spectrum.plan)
    fftwf_destroy_plan(chan->spectrum.plan);
  chan->spectrum.plan = NULL;
  FREE(chan->spectrum.window);
  FREE(chan->spectrum.bin_data);
  FREE(chan->spectrum.ring);
  chan->spectrum.ring_size = 0;
  return chan->demod_type == INVALID_DEMOD ? -1 : 0;
}

static void narrowband_poll(chan_t *chan){
  // Narrowband mode poll

  if(chan->spectrum.ring == NULL)
      return; // Needed

  struct frontend const * restrict const frontend = chan->frontend;
  if(frontend == NULL)
    return;

  int const bin_count = chan->spectrum.bin_count;
  float * restrict const bin_data = chan->spectrum.bin_data;
  assert(bin_data != NULL); // allocated just before we're called
  memset(bin_data,0, bin_count * sizeof *bin_data); // zero output data

  if(chan->spectrum.plan == NULL)
    setup_complex_fft(chan); // narrowband always uses complex

  fftwf_plan restrict const plan = chan->spectrum.plan;
  assert(plan != NULL);

  if(chan->spectrum.window == NULL)
    generate_window(chan);

  float const * restrict const window = chan->spectrum.window;

  // Most recent data from receive ring buffer
  float complex const * restrict const ring = chan->spectrum.ring;
  int const ring_size = chan->spectrum.ring_size;
  int const fft_n = chan->spectrum.fft_n;
  assert(fft_n > 0); // should be set by narrowband_setup()
  float complex * restrict fft_in = fftwf_alloc_complex(fft_n);
  assert(fft_in != NULL);
  float complex * restrict fft_out = fftwf_alloc_complex(fft_n);
  assert(fft_out != NULL);

  // This check actually isn't necessary because ring_size is calculated from fft_avg assuming no overlap
  assert(chan->spectrum.fft_avg >= 1);
  double const avg_limit = floor(1 + (( ring_size / fft_n) - 1) / (1-chan->spectrum.overlap));
  assert(chan->spectrum.fft_avg <= avg_limit);  // so the assertion shouldn't fail
  int const fft_avg = chan->spectrum.fft_avg > avg_limit ? lrint(avg_limit) : chan->spectrum.fft_avg;
  int rp = chan->spectrum.ring_idx - lrint(fft_n * (1 + (fft_avg - 1)*(1-chan->spectrum.overlap)));
  if(rp < 0)
    rp += ring_size;
  assert(rp >= 0); // with limit, shouldn't wrap more than once

  // scale each bin value for our FFT
  // squared because the we're scaling the output of complex norm, not the input bin values
  // Unlike wideband, no adjustment for a real front end because the downconverter corrects the gain
  double const gain = 1.0 / ((double)fft_n * fft_n * fft_avg);

  for(int iter=0; iter < fft_avg; iter++){
    // Copy and window raw baseband
    for(int i = 0; i < fft_n; i++){
      assert(rp >= 0 && rp < ring_size);
      fft_in[i] = ring[rp++] * window[i];
      if(rp >= ring_size)
	rp -= ring_size;
    }
    fftwf_execute_dft(plan,fft_in,fft_out);
    // DC to Nyquist-1, then -Nyquist to -1
    int fr = 0;
    for(int i=0; i < bin_count; i++){
      if(i == bin_count/2)
	fr = fft_n - i; // skip over excess FFT bins at edges
      assert(fr >= 0 && fr < fft_n);
      double const p = cnrm((double complex)fft_out[fr++]); // use double for improved accuracy when summing?
      assert(isfinite(p));
      if(isfinite(p))
	bin_data[i] += gain * p; // Don't pollute with infinities or NANs
    }
    // rp now points to *next* buffer, so move it back if there's overlap
    rp -= lrint(fft_n * chan->spectrum.overlap);
    if(rp < 0)
      rp += ring_size;
  }
  fftwf_free(fft_in);
  fftwf_free(fft_out);
  double min_power = INFINITY;
  double max_power = 0;

  // scaling for byte bin format
  for(int i=0; i < bin_count; i++){
    if(bin_data[i] < min_power)
      min_power = bin_data[i];
    if(bin_data[i] > max_power)
      max_power = bin_data[i];
  }
  if(max_power > 0 && min_power > 0){
#if SPECTRUM_CLIP
    chan->spectrum.base = power2dB(chan->sig.n0 * chan->spectrum.noise_bw);
#else
    chan->spectrum.base = power2dB(min_power);
#endif
#if FIXED_STEP
    chan->spectrum.step = 0.5; // 0.5 dB fixed
#else
    chan->spectrum.step = (1./256.) * (power2dB(max_power) - chan->spectrum.base); // dB range
#endif
  }
}

static void wideband_poll(chan_t *chan){
  if(chan == NULL)
    return;

  // Wideband mode poll
  struct frontend const * restrict const frontend = chan->frontend;
  if(frontend == NULL)
    return;

  // These can happen if we're called too early, before allocations
  struct filter_in const * const restrict master = chan->filter.out.master;
  if(master == NULL)
    return;

  int const fft_n = chan->spectrum.fft_n;
  assert(fft_n > 0); // should be set by setup_wideband()

  if(chan->spectrum.plan == NULL){
    if(chan->frontend->isreal)
      setup_real_fft(chan);
    else
      setup_complex_fft(chan);
  }
  fftwf_plan restrict const plan = chan->spectrum.plan;
  assert(plan != NULL);

  // should be set up just before we were called
  int const bin_count = chan->spectrum.bin_count;
  float * restrict const bin_data = chan->spectrum.bin_data;
  assert(bin_count > 0 && bin_data != NULL);
  memset(bin_data,0, bin_count * sizeof *bin_data); // zero output data

  if(chan->spectrum.window == NULL)
    generate_window(chan);
  float const * restrict const window = chan->spectrum.window;
  assert(window != NULL);

  // Asynchronously read newest data from input buffer
  // scale fft bin shift down to size of analysis FFT, which is smaller than the input FFT
  int const shift = (int)(chan->filter.bin_shift * (int64_t)fft_n / master->points);

  // scale each bin value for our FFT
  // squared because the we're scaling the output of complex norm, not the input bin values
  // we only see one side of the spectrum for real inputs
  // Limit averaging to the data on hand

  if(frontend->isreal){
    // Point into raw SDR A/D input ring buffer
    // We're reading from a mirrored buffer so it will automatically wrap back to the beginning
    // as long as it doesn't go past twice the buffer length
    // Limit averaging to amount on hand. also done in radio_status.c, belt and suspenders for now
    double const avg_limit = floor(1 + ((frontend->in.input_buffer_size / (sizeof (float) * fft_n)) - 1) / (1-chan->spectrum.overlap));
    if((double)chan->spectrum.fft_avg > avg_limit)
      chan->spectrum.fft_avg = (int)avg_limit;
    int const fft_avg = chan->spectrum.fft_avg;
    assert(fft_avg >= 1);
    int const adjust = lrint(fft_n * (1 + (fft_avg - 1)*(1-chan->spectrum.overlap)));
    chan->filter.out.sample_index = frontend->samples - adjust; // since it's not done by a filter output route
    float const *input = frontend->in.input_write_pointer.r - adjust;
    while(input < (float *)frontend->in.input_buffer)
      input += frontend->in.input_buffer_size / sizeof *input; // bring it back into the buffer
    float * restrict fft_in = fftwf_alloc_real(fft_n);
    assert(fft_in != NULL);
    float complex * restrict fft_out = fftwf_alloc_complex(fft_n/2 + 1); // r2c has only the positive frequencies
    assert(fft_out != NULL);
    double const gain = 2./(double)((int64_t)fft_avg * fft_n * fft_n); // +3dB to include the virtual conjugate spectrum
    for(int iter=0; iter < fft_avg; iter++){
      // Copy and window raw A/D
      if(shift >= 0){
	// Upright spectrum
	for(int i=0; i < fft_n; i++)
	  fft_in[i] = window[i] * input[i];
      } else {
	// Invert spectrum by flipping sign of every other sample - UNTESTED
	// equivalent to multiplication by a sinusoid at the Nyquist rate
	// If FFT N is odd, just forget the odd last sample.
	// We don't have to track the sign flip phase because we're only summing energy
	for(int i=0; i < fft_n-1; i += 2){
	  fft_in[i] = window[i] * input[i];
	  fft_in[i+1] = -window[i+1] * input[i+1];
	}
	if(fft_n & 1)
	  fft_in[fft_n-1] = 0;
      }
      fftwf_execute_dft_r2c(plan,fft_in,fft_out);

      // Spectrum is always right side up
      // Start with DC + positive frequencies, then wrap to negative
      int binp = shift >= 0 ? shift : fft_n/2 + shift; // what if fft_n is odd?
      assert(binp >= 0);
      for(int i=0;i < bin_count && binp < fft_n/2+1 ; i++,binp++){
	if(i == bin_count/2)
	  binp -= bin_count; // crossed into negative output rang, Wrap input back to lowest frequency requested

	double const p = cnrm(fft_out[binp]);
	assert(isfinite(p));
	if(isfinite(p))
	  bin_data[i] += gain * p;
      }
      input += lrint(fft_n * (1. - chan->spectrum.overlap)); // move forward fraction of a buffer
      if(input > (float *)frontend->in.input_buffer + frontend->in.input_buffer_size / sizeof *input)
	input -= frontend->in.input_buffer_size / sizeof *input;
    }
    fftwf_free(fft_in);
    fftwf_free(fft_out);
  } else {
    // Complex front end (frontend->isreal == false)
    // Find starting points to read in input A/D stream
    // Limit averaging to amount on hand
    double const avg_limit = floor(1 + ((frontend->in.input_buffer_size / (sizeof (float complex) * fft_n)) - 1) / (1-chan->spectrum.overlap));
    if((double)chan->spectrum.fft_avg > avg_limit)
      chan->spectrum.fft_avg = (int)avg_limit;
    int const fft_avg = chan->spectrum.fft_avg;
    assert(fft_avg >= 1);
    int const adjust = lrint(fft_n * (1 + (fft_avg - 1)*(1-chan->spectrum.overlap)));
    chan->filter.out.sample_index = frontend->samples - adjust; // since it's not done by a filter output route
    float complex const * restrict input = frontend->in.input_write_pointer.c - adjust;
    chan->filter.out.sample_index = frontend->samples; // since it's not done by a filter output route
    input += (input < (float complex *)frontend->in.input_buffer) ? frontend->in.input_buffer_size / sizeof *input : 0; // backward wrap
    float complex * restrict fft_in = fftwf_alloc_complex(fft_n);
    assert(fft_in != NULL);
    float complex * restrict fft_out = fftwf_alloc_complex(fft_n);
    assert(fft_out != NULL);
    double const gain = 1./(double)((int64_t)fft_avg * fft_n * fft_n); // check this
    for(int iter=0; iter < fft_avg; iter++){
      // Copy and window raw A/D
      for(int i=0; i < fft_n; i++)
	fft_in[i] = window[i] * input[i];

      fftwf_execute_dft(plan,fft_in,fft_out);

#if 0 // another KA9Q fix attempt
      int i = bin_count/2; // most negative output bin
      int binp = shift - bin_count/2; // most negative input bin needed
      if(binp > fft_n/2)
	break; // Already past upper end of input
      if(binp < 0)
	binp += fft_n; // starts in negative input frequencies

      if(binp < 0){
	// below lowest input frequency. Skip a bit, brother
	i -= binp;
	binp = 0;
	if(i >= bin_count)
	  i -= bin_count; // crossed into positive output range
	if(i >= bin_count)
	  break; // still too high
      }
      do {
	assert(binp >=0 && binp < fft_n);
	assert(i >= 0 && i < bin_count);
	double const p = cnrm(fft_out[binp]);
	assert(isfinite(p));
	if(isfinite(p)) // Don't pollute integrator with bad data
	  bin_data[i] += gain * p;
	if(++binp == fft_n)
	  binp = 0; // wrap round to DC
	if(++i == bin_count)
	  i = 0;    // wrap to DC
      } while (binp != fft_n/2 && i != bin_count/2);
#else  // KE5GDB
      /* Copy requested bins to user. Input and output are both in FFT order:
	 bins 0 ... n/2-1 are DC and the positive frequencies, bins n/2 ... n-1 are the
	 negative frequencies, most negative first. So output bin i sits at signed offset
	 'offset' from the requested center frequency, which is itself at signed input bin
	 'shift' from the front end's center frequency.
	 Bins falling outside the front end's coverage are left at zero.
	 (fixed by KE5GDB)
      */
      for(int i=0; i < bin_count; i++){
	int const offset = i < bin_count/2 ? i : i - bin_count; // signed output frequency, bins
	int const b = shift + offset;                           // signed input frequency, bins
	if(b < -fft_n/2 || b >= (fft_n+1)/2)
	  continue; // Outside the front end passband
	int const binp = b >= 0 ? b : b + fft_n; // back to FFT order
	assert(binp >= 0 && binp < fft_n);
	double const p = cnrm(fft_out[binp]);
	assert(isfinite(p));
	if(isfinite(p))
	  bin_data[i] += gain * p;
      }
#endif
      // Back to previous buffer
      input -= lrint(fft_n * (1. - chan->spectrum.overlap));
      if(input < (float complex *)frontend->in.input_buffer)
	input += frontend->in.input_buffer_size / sizeof *input; // backward wrap
    }
    fftwf_free(fft_in);
    fftwf_free(fft_out);
  }
  double min_power = INFINITY;
  double max_power = 0;

  // scaling for byte bin format
  for(int i=0; i < bin_count; i++){
    if(bin_data[i] < min_power)
      min_power = bin_data[i];
    if(bin_data[i] > max_power)
      max_power = bin_data[i];
  }
  if(max_power > 0 && min_power > 0){
#if SPECTRUM_CLIP
    chan->spectrum.base = power2dB(chan->sig.n0 * chan->spectrum.noise_bw);

#else
    chan->spectrum.base = power2dB(min_power);
#endif
#if FIXED_STEP
    chan->spectrum.step = 0.5; // 0.25 dB fixed
#else
    chan->spectrum.step = ldexp(power2dB(max_power) - chan->spectrum.base,-8); // dB range
#endif
  }

}

// Fill a buffer with compact frequency bin data, 1 byte each
// Each unsigned byte in the output gives the bin power above 'base' decibels, in steps of 'step' dB
// Unlike the regular float32 format, these bins run uniformly from lowest frequency to highest frequency
void encode_byte_data(chan_t const *chan, uint8_t *buffer){
  assert(chan != NULL && buffer != NULL);

  int const bin_count = chan->spectrum.bin_count;
  double scale = 1./chan->spectrum.step;

  int wbin = bin_count/2; // nyquist freq is most negative
  for(int i=0; i < bin_count; i++){
    double x = scale * (power2dB(chan->spectrum.bin_data[wbin++]) - chan->spectrum.base);
    if(x < 0)
      x = 0;
    else if(x > 255)
      x = 255;

    buffer[i] = (uint8_t)llrint(x);
    if(wbin == bin_count)
      wbin = 0;  // Continuing through dc and positive frequencies
  }
}
// Generate normalized sampling window
// the generation functions are symmetric so lengthen them by one point to make them periodic
static void generate_window(chan_t *chan){
  assert(chan != NULL);
  if(chan->spectrum.fft_n == 0)
    return; // can't do anything until we know the size

  int const fft_n = chan->spectrum.fft_n;
  FREE(chan->spectrum.window);
  chan->spectrum.window = malloc((1 + fft_n) * sizeof *chan->spectrum.window);
  assert(chan->spectrum.window != NULL);
  switch(chan->spectrum.window_type){
  default:
  case KAISER_WINDOW: // If β == 0, same as rectangular
    make_kaiserf(chan->spectrum.window,fft_n+1,chan->spectrum.shape);
    break;
  case RECT_WINDOW:
    for(int i=0; i < fft_n; i++)
      chan->spectrum.window[i] = 1;
    break;
  case BLACKMAN_WINDOW:
    for(int i=0; i < fft_n; i++)
      chan->spectrum.window[i] = blackman_window(i,fft_n+1);
    break;
  case EXACT_BLACKMAN_WINDOW:
    for(int i=0; i < fft_n; i++)
      chan->spectrum.window[i] = exact_blackman_window(i,fft_n+1);
    break;
  case BLACKMAN_HARRIS_WINDOW:
    for(int i=0; i < fft_n; i++)
      chan->spectrum.window[i] = blackman_harris_window(i,fft_n+1);
    break;
  case GAUSSIAN_WINDOW:
    // Reuse kaiser β as σ parameter
    // note σ = 0 is a pathological value for gaussian, it's an impulse with infinite sidelobes
    gaussian_window_alpha(chan->spectrum.window, fft_n+1,chan->spectrum.shape, false); // we normalize them all below
    break;
  case HANN_WINDOW:
    for(int i=0; i < fft_n; i++)
      chan->spectrum.window[i] = hann_window(i,fft_n+1);
    break;
  case HAMMING_WINDOW:
    for(int i=0; i < fft_n; i++)
      chan->spectrum.window[i] = hamming_window(i,fft_n+1);
    break;
  case HP5FT_WINDOW:
    for(int i=0; i < fft_n; i++)
      chan->spectrum.window[i] = hp5ft_window(i,fft_n+1);
    break;
  }
  normalize_windowf(chan->spectrum.window,fft_n);

  // Compute noise bandwidth of each bin in bins
  chan->spectrum.noise_bw = 0;
  for(int i=0; i < fft_n; i++)
    chan->spectrum.noise_bw += (double)chan->spectrum.window[i] * chan->spectrum.window[i];

  // Scale to the actual bin bandwidth
  // This also has to be divided by the square of the sum of the window values, but that's already normalized to 1
  chan->spectrum.noise_bw *= chan->spectrum.rbw / fft_n;
}

// Direct Wideband mode. Setup FFT to work on raw A/D input
// What can we do about unfriendly sizes? Anything?
static void setup_wideband(chan_t *chan){
  assert(chan != NULL);
  if(chan->frontend->samprate == 0 || chan->spectrum.rbw <= 0)
    return; // avoid divide by zero
  chan->spectrum.fft_n = lrint(chan->frontend->samprate / chan->spectrum.rbw); // should limit to a sane value
  chan->output.samprate = 0; // Not meaningful
  chan->output.channels = 0;
  if(Verbose > 1)
    fprintf(stderr,"%s wide spectrum: center %'.3lf Hz bin count %u, rbw %.1lf Hz, samprate %u Hz fft size %u\n",
	    chan->name,chan->tune.freq,chan->spectrum.bin_count,chan->spectrum.rbw,chan->output.samprate,chan->spectrum.fft_n);

  FREE(chan->spectrum.ring); // not needed
  chan->spectrum.ring_size = 0;
  // Dummy just so downconvert() will block on each frame
  delete_filter_output(&chan->filter.out);
  int r = create_filter_output(&chan->filter.out,&chan->frontend->in,NULL,0,SPECTRUM);
  assert(r == 0);
  (void)r;
  // Wideband mode with real front end; use real->complex FFT
  if(chan->frontend->isreal)
    setup_real_fft(chan);
  else
    setup_complex_fft(chan);
}
// Set up narrow band (downconvert) mode
static void setup_narrowband(chan_t *chan){
  assert(chan != NULL);
  if(Blocktime == 0)
    return; // avoid divide by zero
  if(chan->spectrum.rbw <= 0 || chan->spectrum.bin_count < 1)
    return; // Can't do anything yet

  int const L = chan->frontend->L;
  int const M = chan->frontend->M;
  int const N = L + M - 1;
  if(N == 0)
    return; // avoid divide by zero (can this happen with -1?
  double const blockrate = 1. / Blocktime; // Typically 50 Hz
  unsigned long const samprate_base = lcm(lrint(blockrate),lrint(L*blockrate/N)); // Samprate must be allowed by receiver
  assert(samprate_base != 0);
  if(samprate_base == 0)
    return;
  double const margin = 400; // Allow 400 Hz for filter skirts at edge of I/Q receiver - calculate this
  chan->spectrum.fft_n = lrint(chan->spectrum.bin_count + margin / chan->spectrum.rbw); // Minimum for search to avoid receiver filter skirt
  // This (int) cast should be cleaned up
  while(chan->spectrum.fft_n < 65536 && (!goodchoice(chan->spectrum.fft_n) || lrint(chan->spectrum.fft_n * chan->spectrum.rbw) % samprate_base != 0))
    chan->spectrum.fft_n++;
  chan->output.samprate = lrint(chan->spectrum.fft_n * chan->spectrum.rbw);
  chan->output.channels = 2; // IQ mode
  if(Verbose > 1)
    fprintf(stderr,"%s narrow spectrum: center %'.3lf Hz bin count %u, rbw %.1lf Hz, samprate %u Hz fft size %u\n",
	    chan->name,chan->tune.freq,chan->spectrum.bin_count,chan->spectrum.rbw,chan->output.samprate,chan->spectrum.fft_n);
  int blocklen = lrint(chan->output.samprate * Blocktime);
  // Set up downconverter
  delete_filter_output(&chan->filter.out);
  int r = create_filter_output(&chan->filter.out,&chan->frontend->in,NULL,blocklen,COMPLEX);
  (void)r;
  assert(r == 0);
  chan->filter.max_IF = (double)(chan->output.samprate - margin)/2;
  chan->filter.min_IF = -chan->filter.max_IF;
  chan->filter2.blocking = 0; // Not used in this mode, make sure it's 0
  set_filter(&chan->filter.out,chan->filter.min_IF,chan->filter.max_IF,chan->filter.kaiser_beta);
  chan->filter.remainder = NAN; // Force init of downconverter
  chan->filter.bin_shift = 1010101010; // Unlikely - but a kludge, force init of phase rotator
  setup_complex_fft(chan);
}
// Wideband mode with real front end
static void setup_real_fft(chan_t *chan){
  assert(chan != NULL);
  if(chan->spectrum.fft_n < 1)
    return; // Can't do it yet
  if(chan->spectrum.plan != NULL)
    fftwf_destroy_plan(chan->spectrum.plan);
  float *in = fftwf_alloc_real(chan->spectrum.fft_n);
  assert(in != NULL);
  float complex *out = fftwf_alloc_complex(chan->spectrum.fft_n/2+1); // N/2 + 1 output points for real->complex
  assert(out != NULL);
  chan->spectrum.plan = plan_r2c(chan->spectrum.fft_n, in, out);
  fftwf_free(in);
  fftwf_free(out);
  assert(chan->spectrum.plan != NULL);
}
// Wideband mode with complex front end, or narrowband mode with either front end
static void setup_complex_fft(chan_t *chan){
  assert(chan != NULL);
  if(chan->spectrum.fft_n < 1)
    return; // Can't do anything yet
  if(chan->spectrum.plan != NULL)
    fftwf_destroy_plan(chan->spectrum.plan);
  float complex *in = fftwf_alloc_complex(chan->spectrum.fft_n);
  assert(in != NULL);
  float complex *out = fftwf_alloc_complex(chan->spectrum.fft_n);
  assert(out != NULL);
  chan->spectrum.plan = plan_complex(chan->spectrum.fft_n, in, out, FFTW_FORWARD);
  fftwf_free(in);
  fftwf_free(out);
  assert(chan->spectrum.plan != NULL);
}

#if RICE
// Experiment with rice coding of delta values in bin data
static void rice(chan_t *chan){
  assert(chan != NULL);
  if(chan->spectrum.step == 0 || chan->spectrum.bin_count)
    return;

  // make vector of quantized measurements
  int const bin_count = chan->spectrum.bin_count;
  double scale = 1./chan->spectrum.step;
  int data[bin_count];
  int wbin = bin_count/2; // nyquist freq is most negative
  for(int i=0; i < bin_count; i++){
    double x = scale * (power2dB(chan->spectrum.bin_data[wbin++]) - chan->spectrum.base);
    if(x < 0)
      x = 0;
    data[i] = llrint(x);
    if(wbin == bin_count)
      wbin = 0;  // Continuing through dc and positive frequencies
  }
  int best_k = -1;
  int best_bits = 8 * bin_count;
  bool delta = false;

  // Test non-delta encoding
  for(int k=1;k < 6; k++){
    int bits = 0;
    int M = 1 << k;

    for(int i=0;i < bin_count; i++){
      int value = data[i]; // values are non-negative, no sign bit needed

      // Rice encode
      int q = value / M;
      int r = value % M;
      (void)r;

      // emit q 0-bits and a 1 bit
      bits += q + 1;
      // Emit r using k bits
      bits += k;
    }
    if(bits < best_bits){
      best_bits = bits;
      best_k = k;
    }
  }
  // Now test delta encoding
  for(int k=1;k < 6; k++){
    int bits = 0;
    int M = 1 << k;
    int prev = 0;

    for(int i=0;i < bin_count; i++){
      int delta = data[i] - prev;
      prev = data[i];
      int value = (abs(delta) << 1) | (delta < 0); // zig-zag encoding

      // Rice encode
      int q = value / M;
      int r = value % M;
      (void)r;

      // emit q 0-bits and a 1 bit
      bits += q + 1;
      // Emit r using k bits
      bits += k;
    }
    if(bits < best_bits){
      best_bits = bits;
      best_k = k;
      delta = true;
    }
  }
  fprintf(stderr,"rice encoding, delta %d,  k=%d: %d bits (%d bytes), %.1lf%%\n",delta,best_k,best_bits,best_bits/8,
	  100. * best_bits / (8 * bin_count));
}
#endif
