// $Id: radio.h,v 1.137 2022/04/14 10:50:43 karn Exp karn $
// Internal structures and functions of the 'radio' program
// Nearly all internal state is in the 'demod' structure
// More than one can exist in the same program,
// but so far it seems easier to just run separate instances of the 'radio' program.
// Copyright 2018, Phil Karn, KA9Q
#ifndef _RADIO_H
#define _RADIO_H 1

#include <pthread.h>
#include <complex.h>

#include <sys/socket.h>
#include <stdint.h>
#include <stdbool.h>
#include <limits.h>

#include "modes.h"
#include "multicast.h"
#include "osc.h"
#include "status.h"
#include "filter.h"

// Multicast network connection with front end hardware
// Only one off these, shared with all demod instances
struct frontend {

  // Stuff we maintain about our upstream source
  struct {
    int data_fd;           // Socket for raw incoming I/Q data
    int ctl_fd;            // Socket for commands to front end
    int status_fd;         // Socket for status from front end

    struct sockaddr_storage metadata_source_address;    // Source of SDR metadata
    struct sockaddr_storage metadata_dest_address;      // Dest of metadata (typically multicast)
    char metadata_dest_string[_POSIX_HOST_NAME_MAX+20]; // Allow room for :portnum
    uint64_t metadata_packets;
    
    struct sockaddr_storage data_source_address;     // Source of I/Q data
    struct sockaddr_storage data_dest_address;       // Dest of I/Q data (typically multicast)
    char data_dest_string[_POSIX_HOST_NAME_MAX+20];  // Allow room for :portnum
    struct rtp_state rtp; // State of the I/Q RTP receiver
    uint64_t samples;     // Count of raw I/Q samples received
  } input;

  int M;            // Impulse length of input filter
  int L;            // Block length of input filter
  float n0;         // Noise spectral density esimate (experimemtal), power/Hz ratio

  // Stuff maintained by our upstream source and filled in by the status daemon
  struct {
    char description[256]; // Free-form text
    uint64_t commands;     // Command counter
    uint32_t command_tag;   // Last received command tag
    int samprate;           // Sample rate on data stream
    long long timestamp; // Nanoseconds since GPS epoch 6 Jan 1980 00:00:00 UTC
    double frequency;
    uint8_t lna_gain;
    uint8_t mixer_gain;
    uint8_t if_gain;
    bool direct_conversion; // Avoid 0 Hz if set
    bool isreal;            // Front end stream is real-only
    int bitspersample; // 8, 12 or 16
    bool lock;              // Tuning is locked; clients cannot change
    
    // Limits on usable IF due to aliasing, filtering, etc
    // Less than or equal to +/- samprate/2
    // Straddles 0 Hz for complex, will have same sign for real output from a low IF tuner
    float min_IF;
    float max_IF;

    float gain;
    float output_level;

    // 'status' is written by the input thread and read by set_first_LO, etc, so it's protected by a mutex
    pthread_mutex_t status_mutex;
    pthread_cond_t status_cond;     // Signalled whenever status changes
  } sdr;
  float tp1;        // Spare test points
  float tp2;
  struct filter_in * restrict in;
  pthread_t status_thread;
};

extern struct frontend Frontend; // Only one per radio instance


// Demodulator state block; there can be many of these
struct demod {
  int inuse;
  int lifetime;          // Remaining lifetime, seconds
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
    struct filter_out *out;
    float min_IF;          // Edges of filter (settable)
    float max_IF;         // (settable)
    // Window shape factor for Kaiser window
    float kaiser_beta;  // settable
    bool isb;           // Independent sideband mode (settable, currently unimplemented)
  } filter;

  enum demod_type demod_type;  // Index into demodulator table (AM, FM, Linear)

  struct {               // Used only in linear demodulator
    bool env;            // Envelope detection in linear mode (settable)
    bool agc;            // Automatic gain control enabled (settable)
    float hangtime;      // AGC hang time, samples (settable)
    float recovery_rate; // AGC recovery rate, amplitude ratio/sample  (settable)
    float threshold;     // AGC threshold above noise, amplitude ratio

    bool pll;         // Linear mode PLL tracking of carrier (settable)
    bool square;      // Squarer on PLL input (settable)
    float lock_timer; // PLL lock timer
    bool pll_lock;    // PLL is locked
    float loop_bw;    // Loop bw (coherent modes)
    float cphase;     // Carrier phase change radians (DSB/PSK)
  } linear;
  int hangcount;

  struct {
    struct pll pll;
    bool was_on;
    int lock_count;
  } pll;

  // Signal levels & status, common to all demods
  struct {
    float bb_power;   // Average power of signal after filter but before digital gain, power ratio
    float foffset;    // Frequency offset Hz (FM, coherent AM, dsb)
    float snr;        // From PLL in linear, moments in FM
  } sig;
  
  float squelch_open;  // squelch open threshold, power ratio
  float squelch_close; // squelch close threshold
  int squelchtail;     // Frames to hold open after loss of SNR

  struct {               // Used only in FM demodulator
    float pdeviation;    // Peak frequency deviation Hz (FM)
  } fm;

  // Output
  struct {
    int samprate;      // Audio D/A sample rate (usually 48 kHz)
    float gain;        // Audio gain to normalize amplitude
    float headroom;    // Audio level headroom, amplitude ratio (settable)
    // RTP network streaming
    bool silent;       // last packet was suppressed (used to generate RTP mark bit)
    struct rtp_state rtp;
    
    struct sockaddr_storage data_source_address;    // Source address of our data output
    struct sockaddr_storage data_dest_address;      // Dest of our data outputg (typically multicast)
    char data_dest_string[_POSIX_HOST_NAME_MAX+20]; // Allow room for :portnum
    
    int data_fd;    // File descriptor for multicast output
    int rtcp_fd;    // File descriptor for RTP control protocol
    int sap_fd;     // Session announcement protocol (SAP) - experimental
    int channels;   // 1 = mono, 2 = stereo (settable)
    float level;    // Output level
    float deemph_state_left;
    float deemph_state_right;
    uint64_t samples;
  } output;

  // Used only when FM deemphasis is enabled
  struct {
    complex float state; // stereo filter state
    float gain;     // Empirically set
    // 'rate' computed from expf(-1.0 / (tc * output.samprate));
    // tc = 75e-6 sec for North American FM broadcasting
    // tc = 1 / (2 * M_PI * 300.) = 530.5e-6 sec for NBFM (300 Hz corner freq)
    float rate;
  } deemph;

  pthread_t sap_thread;
  pthread_t rtcp_thread;
  pthread_t demod_thread;
  // Set this flag to ask demod_thread to terminate.
  // pthread_cancel() can't be used because we're usually waiting inside of a mutex, and deadlock will occur
  int terminate;
  float tp1,tp2; // Spare test points
};

extern struct demod *Demod_list;
extern int Demod_list_length;
extern int Active_demod_count;
extern int const Demod_alloc_quantum;
extern pthread_mutex_t Demod_mutex;

extern int Status_fd;  // File descriptor for receiver status
extern int Ctl_fd;     // File descriptor for receiving user commands

extern char const *Libdir;
extern char const *Modefile;
extern int Verbose;
extern float Blocktime; // Common to all receiver slices
extern uint64_t Metadata_packets;
extern uint64_t Commands;
extern uint32_t Command_tag; // Echoed in responses to commands (settable)

// Functions/methods to control a demod instance
struct demod *alloc_demod(void);
void free_demod(struct demod **);
int init_demod(struct demod * restrict demod);
double set_freq(struct demod * restrict ,double);
int preset_mode(struct demod * restrict,const char * restrict);
int compute_tuning(int N, int M, int samprate,int *flip,int *rotate,double *remainder, double freq);
//int compute_tuning(const struct demod * restrict const demod,int * restrict flip,int * restrict rotate,double * restrict remainder, double freq);
int start_demod(struct demod * restrict demod);
int kill_demod(struct demod ** restrict demod);
int init_demod_streams(struct demod * restrict demod);
double set_first_LO(struct demod const * restrict, double);

void *proc_samples(void *);
void *estimate_n0(void *);
void *rtcp_send(void *);
void *sap_send(void *);
void *radio_status(void *);
void *sdr_status(void *);
void *demod_reaper(void *);

const float compute_n0(struct demod const * restrict);

// Used by demods to save CPU by bypassing various calculations when we're running without a status channel
// Demodulator thread entry points
void *demod_fm(void *);
void *demod_wfm(void *);
void *demod_linear(void *);
void *demod_null(void *);

int send_mono_output(struct demod * restrict ,const float * restrict,int,int);
int send_stereo_output(struct demod * restrict ,const float * restrict,int,int);
void output_cleanup(void *);


#endif
