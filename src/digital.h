// Receive pieces shared by the digital mode decoders (p25d now; DMR and others later)
// All work on a complex baseband (IQ) stream from radiod:
// receive filter, level control, frequency tracking, symbol timing, soft symbols
// Copyright 2026, Matthew Kaufman, KA6SQG
#ifndef _DIGITAL_H
#define _DIGITAL_H 1

#include <complex.h>
#include <stdbool.h>
#include <stdint.h>

// Lowpass FIR with real taps for complex baseband
// Raised-cosine shaped response: flat to cutoff * (1 - rolloff), -6 dB at the cutoff,
// zero beyond cutoff * (1 + rolloff); rolloff 0 is a plain windowed sinc. Hamming window, unity DC gain
// A second response may share the delay line, centred on it, so both have the same delay and
// either may be selected between any two samples without a step in timing or phase
enum { RXFILTER_RESPONSES = 2 };
struct rxfilter {
  int ntaps;              // delay line length, that of the first response
  int pos;
  float *taps;            // the response in use, ntaps long
  int first,count;        // its nonzero taps
  int response;           // which one is in use
  int nresponses;
  float *responses[RXFILTER_RESPONSES];
  int firsts[RXFILTER_RESPONSES],counts[RXFILTER_RESPONSES];
  float complex *history; // Stored twice so each output is one contiguous dot product
};
int init_rxfilter(struct rxfilter *f,double samprate,double cutoff,int ntaps,double rolloff);
// Another response, centred on the first one's delay line: no more taps, and odd if it is odd (even if
// even). Returns its number, or -1
int add_rxfilter_response(struct rxfilter *f,double samprate,double cutoff,int ntaps,double rolloff);
void select_rxfilter(struct rxfilter *f,int response); // from the next sample
// Root raised cosine matched filter for a linear modulation the transmitter shapes with the same
// response (P25 Phase 2 H-DQPSK): rolloff alpha, 'span' symbols each side of the centre, unity DC gain
// Runs with run_rxfilter()
int init_rrcfilter(struct rxfilter *f,double samprate,double symrate,double alpha,double span);
void free_rxfilter(struct rxfilter *f);
void run_rxfilter(struct rxfilter *f,float complex *buffer,int count); // in place

// Envelope ripple at the symbol rate: |mean of |x|^2 e^(-j 2 pi symrate t)| / mean of |x|^2
// A linear modulation (pi/4 DQPSK, QPSK) dips between symbols and so has a strong component at
// the symbol rate; a constant-envelope one (C4FM, 4FSK) has none, and neither has white noise.
// So unlike the spread of |x| this does not rise as the signal weakens: noise only adds a floor
// of about the spread of |x|^2 over the square root of the samples averaged
struct envelope {
  float alpha;            // per-sample smoothing factor
  float complex rotor,step; // e^(-j 2 pi symrate t)
  float complex ripple;   // smoothed |x|^2 e^(-j 2 pi symrate t)
  float power;            // smoothed |x|^2
  uint64_t samples;       // seen since init
};
void init_envelope(struct envelope *e,double samprate,double symrate,double time_constant);
void run_envelope(struct envelope *e,float complex const *buffer,int count);
float envelope_ripple(struct envelope const *e); // 0 = constant envelope

// Level control: scale to unity RMS with a slow power estimate
// The timing loop's gain depends on amplitude, so this is needed whatever radiod's own AGC is doing
struct level {
  float alpha; // per-sample smoothing factor
  float power; // smoothed |x|^2
};
void init_level(struct level *l,double samprate,double time_constant);
void prime_level(struct level *l,float complex const *buffer,int count); // start at this block's level
void run_level(struct level *l,float complex *buffer,int count); // in place

// Frequency tracking, updated once per block
// The block is rotated by the correction decided after the previous block, the residual is
// estimated from the lag-one product sum (unambiguous to +/- samprate/2), and the estimate is
// smoothed with a one-pole corner. Blocks that look like white noise leave the estimate alone
struct lagone {
  double re,im,power;
};
struct freqtrack {
  double samprate;
  double sample_factor;   // per-sample smoothing factor for the chosen corner
  float estimate;         // Hz, the loop state: signal's offset from the centre of the stream
  float applied;          // Hz, the correction used for the current block
  float max_offset;       // Hz
  float coherence;        // |sum| / power of the last unfiltered block, 0..1
  bool seed_pending;
  bool unfiltered_pending;
  struct lagone unfiltered;
  uint64_t phase;         // rotation oscillator
};
// corner: the loop's bandwidth, Hz (the -3 dB point of its one-pole response)
void init_freqtrack(struct freqtrack *ft,double samprate,double corner);
void set_freqtrack_corner(struct freqtrack *ft,double corner);
void seed_freqtrack(struct freqtrack *ft,float offset);
void correct_freqtrack(struct freqtrack *ft,float complex *buffer,int count); // rotate by -applied, in place
void measure_freqtrack(struct freqtrack *ft,float complex const *buffer,int count); // corrected, before the receive filter
// Residual offset (Hz) of a corrected and receive-filtered block from its lag-one products.
// Returns false if the block has no usable signal
bool freqtrack_residual(struct freqtrack const *ft,float complex const *buffer,int count,float *residual);
// Close the loop for this block of 'count' samples with a residual from freqtrack_residual()
// or from the decoder's own decisions; valid false = no estimate, hold
void update_freqtrack(struct freqtrack *ft,bool valid,float residual,int count);
// For a decoder that corrects each block by an estimate updated from that same block (feed-forward):
// rotate by -offset Hz rather than by the correction decided after the previous block
void shift_freqtrack(struct freqtrack *ft,float complex *buffer,int count,float offset);
// Move the estimate by delta Hz, e.g. a residual from the decoder's decisions
void adjust_freqtrack(struct freqtrack *ft,float delta);

// Symbol timing: polyphase interpolator to two samples per symbol, Gardner detector, PI loop
// Wide loop until told the signal is locked (e.g. by frame sync), narrow after
enum { SYMSYNC_TAPS = 8, SYMSYNC_HISTORY = 1024, SYMSYNC_OUTPUTS = 512 }; // history sizes are powers of two

typedef void (*symsync_output)(void *arg,float complex sample,bool is_symbol);

struct symsync {
  int nphases;
  float *taps;                 // nphases x SYMSYNC_TAPS, oldest sample first
  float complex window[2 * SYMSYNC_TAPS];
  int wpos;
  float base_ratio;            // outputs per input sample (2 x symbol rate / sample rate)
  float ratio;
  float accumulator;
  // Gardner detector
  bool expect_midpoint;
  float complex prev,mid;
  float error_ema;             // telemetry: smoothed |e| / energy
  // loop filter
  float kp,ki;
  float integral;
  float out_min,out_max,int_min,int_max;
  float acquire_bw,track_bw,damping,error_gain;
  bool locked;
  bool ignore_next_error;
  // history, for re-sampling past outputs at other timing
  uint64_t samples_in,outputs_out;
  float complex history[2 * SYMSYNC_HISTORY];
  struct {
    uint64_t newest;
    uint16_t arm;
  } outputs[SYMSYNC_OUTPUTS];
};
// nphases: interpolator arms per input sample. acquire_bw, track_bw: the loop's natural
// frequency, radians per symbol (time constant about 1 / bw symbols at damping 1). damping: zeta.
// error_gain: the timing detector's slope, error per input sample of timing offset (see
// set_gains() in dsync.c). rate_limit: the fraction by which the symbol rate estimate may
// depart from nominal
int init_symsync(struct symsync *s,double samprate,double symrate,int nphases,
		 float acquire_bw,float track_bw,float damping,float error_gain,float rate_limit);
void free_symsync(struct symsync *s);
void run_symsync(struct symsync *s,float complex const *buffer,int count,symsync_output out,void *arg);
void lock_symsync(struct symsync *s,bool locked); // narrow or widen the loop
void set_symsync_bandwidth(struct symsync *s,float bw); // until the next lock_symsync() change
void reset_symsync_rate(struct symsync *s);       // forget the symbol rate error estimate
// Output 'output' (0-based count) re-interpolated 'arms' interpolator steps later (negative: earlier)
bool symsync_near(struct symsync const *s,uint64_t output,int arms,float complex *result);
void nudge_symsync(struct symsync *s,float symbols); // move the sampling instant, |symbols| < 0.5
void slip_symsync(struct symsync *s);                // take the midpoints as the symbols from now on
static inline float symsync_arms_per_symbol(struct symsync const *s){
  return 2.0f * (float)s->nphases / s->base_ratio;
}

// One 4-level symbol after one-symbol differential detection
// dibit 0 = 00 at +45 deg, 1 = 01 at +135, 2 = 10 at -45, 3 = 11 at -135
// (C4FM levels +1, +3, -1, -3; the same for pi/4 DQPSK)
struct softdibit {
  uint8_t dibit;
  float confidence; // 0 on a decision boundary, 1 at an ideal point
  float phase;      // radians
};
struct softdibit make_softdibit(float complex z);

#endif
