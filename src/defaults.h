// Various default parameters for radiod
// Some are global, most apply to individual channels
// Copyright 2026 Phil Karn KA9Q

#ifndef _DEFAULTS_H
#define _DEFAULTS_H 1
#include "radio.h"
#define Nchannels 2000                          // Maximum number of channels (static array)

static int const DEFAULT_PRIO = 1;              // Default real-time priority for channel thread (Linux only);
#define N_SUBFILES (100)                        // Maximum number of files in a /etc/radio/radiod@foo.conf.d directory. Macro to suppress compiler warning.
static int const DEFAULT_TTL = 0;               // Don't blast cheap switches and access points unless the user says so
static int const DEFAULT_IP_TOS = 46 << 2;      // Expedited Forwarding
static int const DEFAULT_UPDATE = 25;           // 2 Hz for a 20 ms frame time

// FFT and fast convolution global parameters
static double const DEFAULT_BLOCKTIME = .02;     // Fundamental operating period of radiod in seconds; .02 = 20 ms
                                                 // Should be a size supported by Opus: 2.5, 5, 10, 20, 40, 60, 80, 100, 120 ms
static int const DEFAULT_OVERLAP = 5;            // Default fast convolution overlap factor; 5 => 1/5 ie, 20% overlap. Limits filter impulse duration

static int const DEFAULT_FFTW_THREADS = 1;       // number of independent FFT execution threads working on different frames
static int const DEFAULT_FFTW_INTERNAL_THREADS = 1;// number of threads *INTERNAL* to FFTW (usually doesn't help much)

static enum demod_type const DEFAULT_DEMOD = IDLE_DEMOD;
static int    const DEFAULT_LINEAR_SAMPRATE = 12000; // good for SSB; should be supported by Opus (8k, 12k, 16k, 24k, 48k)
static double const DEFAULT_LIFETIME = 0;         // frame times; really should be specified by every dynamic channel creation

// Channel filter
static double const DEFAULT_KAISER_BETA = 11.0;   // reasonable tradeoff between skirt sharpness and sidelobe height, gives ~100 dB sidelobes
static double const DEFAULT_LOW = -5000.0;        // Hz; Ballpark numbers for linear, should be properly set for each mode
static double const DEFAULT_HIGH = 5000.0;

// Squelch
static double const DEFAULT_SQUELCH_OPEN = 8.0;   // open when SNR > 8 dB
static double const DEFAULT_SQUELCH_CLOSE = 7.0;  // close when SNR < 7 dB
static bool   const DEFAULT_SNR_SQUELCH = false;  // enables squelch when true, so don't enable except in modes that use squelch

// Linear demod per-channel AGC
static double const DEFAULT_HEADROOM = -15.0;     // Target output level in dBFS; keep gaussian signals from clipping
static double const DEFAULT_RECOVERY_RATE = 20.0; // 20 dB/s gain increase after hang timer expiration
static double const DEFAULT_THRESHOLD = -15.0;    // dB; don't let noise rise above -15dB relative to headroom
static double const DEFAULT_GAIN = 50.0;          // dB; Unused in FM, usually adjusted automatically in linear, sets starting point for AGC
static double const DEFAULT_HANGTIME = 1.1;       // sec; hang before gain increase
static double const DEFAULT_DC_CUT = 0;           // high pass cutoff for AM carrier removal, default 0 (off)

// PLL
static double const DEFAULT_PLL_BW = 10.0;     // 10 Hz is reasonable for acquiring AM carrier. Automatically drops to 1/10 on lock

// FM
static int    const DEFAULT_SQUELCH_TAIL = 1;        // length in frames after going below threshold, may let partial frame noise through
static int    const DEFAULT_NBFM_SAMPRATE = 24000;   // Carson's rule: 2 * (5 kHz modulation + 5 kHz deviation); standard Opus rate
static double const DEFAULT_NBFM_TC = 530.5e-6;      // Assumed de-facto standard time constant for NBFM de-emphasis (300 Hz), microseconds
static double const DEFAULT_NBFM_DEEMPH_GAIN = 12.0; // +12 dB to give subjectively equal loudness with de-emphasis

// WFM (broadcast stereo FM)
static int    const DEFAULT_WFM_SAMPRATE = 48000;
static double const DEFAULT_WFM_TC = 75.0e-6;        // Time constant for FM broadcast. Outside America/Korea, use 50e-6 microseconds
static double const DEFAULT_WFM_DEEMPH_GAIN = 0.0;   // dB (unity gain)

// Noise (N0) estimator
static double const N0_alpha = 0.10;    // per block time smoother: tc = ~190 ms for 20 ms frame; alpha = -expm1(-frametime / tau)
static double const N0_NQ = 0.10;       // look for energy in 10th quartile, hopefully contains only noise
static double const N_cutoff = 1.5;     // Average (all noise, hopefully) bins up to 1.5x the energy in the 10th quartile
static int const Min_noise_bins = 1000; // Always examine at least this many FFT bins (typically 40 Hz each)

// Spectrum analyzer
static double const DEFAULT_CROSSOVER = 200;            // About where the two spectral analysis algorithms use equal CPU
static double const DEFAULT_SPECTRUM_KAISER_BETA = 7.0; // Default for spectral analysis window
static enum window_type const DEFAULT_WINDOW_TYPE = HANN_WINDOW; // seems to be the favorite, especially when overlapping
static int    const DEFAULT_FFT_AVG = 10;               // number of FFTs averaged per spectrum display
static double const DEFAULT_FFT_OVERLAP = 0;            // 0-1; really useful only for small RBW

// Opus encoder defaults
static int  const DEFAULT_OPUS_APPLICATION = OPUS_APPLICATION_AUDIO;  // Could be OPUS_APPLICATION_VOIP
static int  const DEFAULT_OPUS_BITRATE = 0;                           // automatic, let Opus pick a rate
static int  const DEFAULT_OPUS_BANDWIDTH = OPUS_BANDWIDTH_FULLBAND;   // 20 kHz is maximum, dynamically set later
static int  const DEFAULT_OPUS_SIGNAL = OPUS_AUTO;                    // could be OPUS_SIGNAL_VOICE, OPUS_SIGNAL_MUSIC
static bool const DEFAULT_OPUS_DTX = false;                           // isn't probably useful except at a source
static int  const DEFAULT_OPUS_FEC = 0; // disabled                   // Not sure if this is useful in a 1-way multicast
#endif
