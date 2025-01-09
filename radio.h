// Internal structures and functions of the 'ka9q-radio' package
// Nearly all internal state is in the 'demod' structure
// More than one can exist in the same program,
// but so far it seems easier to just run separate instances of the 'radio' program.
// Copyright 2018-2023, Phil Karn, KA9Q
#ifndef _RADIO_H
#define _RADIO_H 1

#include <pthread.h>
#include <complex.h>

#include <sys/socket.h>
#include <stdint.h>
#include <stdbool.h>
#include <limits.h>
#include <iniparser/iniparser.h>
#include <opus/opus.h>

#include "multicast.h"
#include "osc.h"
#include "status.h"
#include "filter.h"
#include "iir.h"

// The four demodulator types
enum demod_type {
  LINEAR_DEMOD = 0,     // Linear demodulation, i.e., everything else: SSB, CW, DSB, CAM, IQ
  FM_DEMOD,             // Frequency/phase demodulation
  WFM_DEMOD,            // wideband frequency modulation (broadcast stereo)
  SPECT_DEMOD,          // Spectrum analysis pseudo-demod
  N_DEMOD,              // Dummy equal to number of valid entries
};

struct demodtab {
  enum demod_type type;
  char name[16];
};

extern struct demodtab Demodtab[];

char const *demod_name_from_type(enum demod_type type);
int demod_type_from_name(char const *name);

/**
@brief Front end control block, one per radiod instance
*/
struct frontend {

  // Stuff we maintain about our upstream source
  uint64_t samples;     // Count of raw I/Q samples received
  uint64_t overranges;  // Count of full scale A/D samples
  uint64_t samp_since_over; // Samples since last overrange

  int M;            // Impulse length of input filter
  int L;            // Block length of input filter

  // Stuff maintained by our upstream source and filled in by the status daemon
  char *description;  // free-form text, must be unique per radiod instance
  int samprate;      // Nominal (requested) sample rate on raw input data stream, needs to be integer for filter stuff
  int64_t timestamp; // Nanoseconds since GPS epoch 6 Jan 1980 00:00:00 UTC
  double frequency;
  double calibrate;  // Clock frequency error ratio, e.g, +1e-6 means 1 ppm high
  // R820T/828 tuner gains, dB. Informational only; total is reported in rf_gain
  uint8_t lna_gain;
  uint8_t mixer_gain;
  uint8_t if_gain;

  float rf_atten;         // dB (RX888 only, pretty useless)
  float rf_gain;          // dB gain (RX888) or lna_gain + mixer_gain + if_gain for R820/828 tuners
  bool rf_agc;            // Front end AGC of some sort is active
  float rf_level_cal;      // adjust to make 0 dBm give 0 dBFS: when zero, 0dBm gives "rf_gain_cal" dBFS
  bool direct_conversion; // Try to avoid DC spike if set
  bool isreal;            // Use real->complex FFT (otherwise complex->complex)
  int bitspersample;      // 1, 8, 12 or 16
  bool lock;              // Tuning is locked; clients cannot change

  // Limits on usable IF due to aliasing, filtering, etc
  // Less than or equal to +/- samprate/2
  // Straddles 0 Hz for complex, will have same sign for real output from a low IF tuner
  // Usually negative for the 820/828 tuners, which are effectively wideband LSB radios
  float min_IF;
  float max_IF;

  /* For efficiency, signal levels now scaled to full A/D range, e.g.,
     16 bit real:    0 dBFS = +87.2984 dB = 32767/sqrt(2) units RMS
     16 bit complex: 0 dBFS = +90.3087 dB = 32767 units RMS
     12 bit real:    0 dBFS = +63.2121 dB = 2047/sqrt(2) units RMS
     12 bit complex: 0 dBFS = +66.2224 dB = 2047 units RMS
      8 bit complex: 0 dBFS = +42.0761 dB = 127 units RMS

      so full A/D range now corresponds to different levels internally, and are scaled
      in radio_status.c when sending status messages
  */
  float if_power_instant; // instantaneous receive power
  float if_power;   // Exponentially smoothed power measurement in A/D units (not normalized)
  float if_power_max;

  // This structure is updated asynchronously by the front end thread, so it's protected
  pthread_mutex_t status_mutex;
  pthread_cond_t status_cond;     // Signalled whenever status changes

  // Entry points for local front end driver
  void *context;         // Stash hardware-dependent control block
  int (*setup)(struct frontend *,dictionary *,char const *); // Get front end ready to go
  int (*start)(struct frontend *);          // Start front end sampling
  double (*tune)(struct frontend *,double); // Tune front end, return actual frequency
  float (*gain)(struct frontend *,float);
  float (*atten)(struct frontend *,float);
  struct filter_in in; // Input half of fast convolver, shared with all channels
};

extern struct frontend Frontend; // Only one per radio instance

/**
@brief  radiod channel state block

This is primarily for radiod, but it is also used by 'control' and 'monitor' to shadow
radiod's state, encoded for network transmission by send_radio_status() and decoded by decode_radio_status().
The transfer protocol uses a series of TLV-encoded tuples that do *not* send every element of this
structure, so shadow copies can be incomplete.

Be careful with memcpy(): there are a few pointers (filter.energies, spectrum.bin_data, status.command, etc)
If you use these in shadow copies you must malloc these arrays yourself.
*/
struct channel {
  bool inuse;
  int lifetime;          // Remaining lifetime, frames
  // Tuning parameters
  struct {
    double freq;         // Desired carrier frequency (settable)
    double shift;        // Post-demod frequency shift (settable)
    double second_LO;
    double doppler;      // (settable)
    double doppler_rate; // (settable)
  } tune;

  struct osc fine,shift;

  // Zero IF pre-demod filter params
  struct {
    struct filter_out out;
    float min_IF;          // Edges of filter (settable)
    float max_IF;         // (settable)
    // Window shape factor for Kaiser window
    float kaiser_beta;  // settable
    bool isb;           // Independent sideband mode (settable, currently unimplemented)
    float *energies;    // Vector of smoothed bin energies
    int bin_shift;      // FFT bin shift for frequency conversion
    double remainder;   // Frequency remainder for fine tuning
    complex double phase_adjust; // Block rotation of phase
  } filter;

  enum demod_type demod_type;  // Index into demodulator table (Linear, FM, FM Stereo, Spectrum)
  char preset[32];       // name of last mode preset

  struct {               // Used only in linear demodulator
    bool env;            // Envelope detection in linear mode (settable)
    bool agc;            // Automatic gain control enabled (settable)
    float hangtime;      // AGC hang time, samples (settable)
    float recovery_rate; // AGC recovery rate, amplitude ratio/sample  (settable)
    float threshold;     // AGC threshold above noise, amplitude ratio
  } linear;
  int hangcount;      // AGC hang timer before gain recovery starts

  struct {
    struct pll pll;
    bool was_on;
    int lock_count;
    bool enable;         // Linear mode PLL tracking of carrier (settable)
    bool square;      // Squarer on PLL input (settable)
    bool lock;    // PLL is locked
    float loop_bw;    // Loop bw (coherent modes)
    float cphase;     // Carrier phase change radians (DSB/PSK)
    int64_t rotations; // Integer counts of cphase wraps through -PI, +PI
  } pll;

  // Signal levels & status, common to all demods
  struct {
    float bb_power;   // Average power of signal after filter but before digital gain, power ratio
    float bb_energy;  // Integrated power, reset by poll
    float foffset;    // Frequency offset Hz (FM, coherent AM, dsb)
    float snr;        // From PLL in linear, moments in FM
    float n0;         // per-demod N0 (experimental)
  } sig;

  struct {                   // Used only in FM demodulator
    float pdeviation;        // Peak frequency deviation Hz (FM)
    float tone_freq;         // PL tone squelch frequency
    float tone_deviation;    // Measured deviation of tone
    bool threshold;          // Threshold extension
    float squelch_open;      // squelch open threshold, power ratio
    float squelch_close;     // squelch close threshold
    int squelch_tail;        // Frames to hold open after loss of SNR
    float gain;              // Empirically set to match overall gain with deemphasis to that without
    float rate;              // de-emphasis filter coefficient computed from expf(-1.0 / (tc * output.samprate));
                             // tc = 75e-6 sec for North American FM broadcasting
                             // tc = 1 / (2 * M_PI * 300.) = 530.5e-6 sec for NBFM (300 Hz corner freq)
  } fm;

  // Used by spectrum analysis only
  // Coherent bin bandwidth = block rate in Hz
  // Coherent bin spacing = block rate * 1 - ((M-1)/(L+M-1))
  struct {
    float bin_bw;     // Requested bandwidth (hz) of noncoherent integration bin
    int bin_count;    // Requested bin count
    float *bin_data;  // Array of real floats with bin_count elements
  } spectrum;

  // Output
  struct {
    unsigned int samprate;      // Audio D/A sample rate
    float gain;        // Audio gain to normalize amplitude
    float sum_gain_sq; // Sum of squared gains, for averaging
    float headroom;    // Audio level headroom, amplitude ratio (settable)
    // RTP network streaming
    bool silent;       // last packet was suppressed (used to generate RTP mark bit)
    struct rtp_state rtp;

    struct sockaddr_storage source_socket;    // Source address of our data output
    struct sockaddr_storage dest_socket;      // Dest of our data output (typically multicast)
    char dest_string[_POSIX_HOST_NAME_MAX+20]; // Allow room for :portnum

    unsigned int channels;   // 1 = mono, 2 = stereo (settable)
    float energy;   // Output energy since last poll

    float deemph_state_left;
    float deemph_state_right;
    uint64_t samples;
    bool pacing;     // Pace output packets
    enum encoding encoding;
    OpusEncoder *opus;
    unsigned int opus_channels;
    unsigned int opus_bitrate;
    float *queue; // Mirrored ring buffer
    size_t queue_size; // Size of allocation, in floats
    unsigned wp,rp; // Queue write and read indices
    unsigned minpacket;  // minimum output packet size in blocks (0-3)
                         // i.e, no minimum or at least 20ms, 40ms or 60ms/packet
  } output;

  struct {
    uint64_t packets_in;
    uint32_t tag;
    pthread_mutex_t lock;       // Protect statistics during updates and reads
    uint64_t blocks_since_poll; // Used for averaging signal levels
    int global_timer;
    int output_timer;
    int output_interval;
    uint64_t packets_out;
    struct sockaddr_storage dest_socket; // Local status output; same IP as output.dest_socket but different port
    uint8_t *command;          // Incoming command
    int length;
  } status;

  struct {
    struct sockaddr_storage dest_socket;
    pthread_t thread;
  } rtcp;

  struct {
    struct sockaddr_storage dest_socket;
    pthread_t thread;
  } sap;

  pthread_t demod_thread;
  uint64_t options;
  float tp1,tp2; // Spare test points that can be read on the status channel
};


extern struct channel *Channel_list;
extern struct channel Template;
extern int Channel_list_length;
extern int const Channel_alloc_quantum;
extern pthread_mutex_t Channel_list_mutex;
extern int Channel_idle_timeout;
extern int Ctl_fd;     // File descriptor for receiving user commands
extern int Output_fd;
extern struct sockaddr_storage Metadata_dest_socket; // Socket for main metadata
extern int Verbose;
extern float Blocktime; // Common to all receiver slices. NB! Milliseconds, not seconds

// Channel initialization & manipulation
struct channel *create_chan(uint32_t ssrc);
struct channel *lookup_chan(uint32_t ssrc);
int close_chan(struct channel *);
int set_defaults(struct channel *chan);
int loadpreset(struct channel *chan,dictionary const *table,char const *preset);
int start_demod(struct channel * restrict chan);
double set_freq(struct channel * restrict ,double);
double set_first_LO(struct channel const * restrict, double);

// Routines common to the internals of all channel demods
int compute_tuning(int N, int M, int samprate,int *shift,double *remainder, double freq);
int downconvert(struct channel *chan);

// extract front end scaling factors (depends on width of A/D sample)
float scale_voltage_out2FS(struct frontend *frontend);
float scale_AD(struct frontend const *frontend);
float scale_ADpower2FS(struct frontend const *frontend);

// Helper threads
void *sap_send(void *);
void *radio_status(void *);

// Demodulator thread entry points
void *demod_fm(void *);
void *demod_wfm(void *);
void *demod_linear(void *);
void *demod_spectrum(void *);

int send_output(struct channel * restrict ,const float * restrict,int,bool);
int send_radio_status(struct sockaddr const *,struct frontend const *, struct channel *);
int reset_radio_status(struct channel *chan);
bool decode_radio_commands(struct channel *chan,uint8_t const *buffer,int length);
int decode_radio_status(struct frontend *frontend,struct channel *channel,uint8_t const *buffer,int length);
int flush_output(struct channel *chan,bool marker,bool complete);


unsigned int round_samprate(unsigned int x);
#endif
