// Ask radiod to create, keep alive and release receiver channels on behalf of a client program
// A channel is requested in the format (demod, sample rate, channels, encoding, filter, gain, destination)
// of an existing channel, learned from its status, on a new frequency
// How requests, keepalives and releases map onto radiod's commands, and why: see chanctl.c
// Copyright 2026, Matthew Kaufman, KA6SQG

#ifndef _CHANCTL_H
#define _CHANCTL_H 1
#include <stdint.h>
#include <stdbool.h>
#include <sys/socket.h>

#include "radio.h"

// Format of a channel, in the units of radiod's commands (dB, Hz, seconds)
// Filled from a channel's decoded status by chanctl_format()
struct chanctl_format {
  enum demod_type demod_type;
  int samprate;            // Hz
  int channels;            // 1 or 2
  enum encoding encoding;
  float low,high;          // filter edges, Hz
  float kaiser_beta;
  int filter2;             // filter2 blocking, 0 = off
  double shift;            // post-detection shift, Hz
  int maxdelay;            // output aggregation, blocks
  int status_interval;     // status on the data group every this many blocks; 0 = never
  // Linear demodulator only
  bool agc;
  float gain;              // dB; used when agc is off
  float headroom;          // dB; AGC parameters, used when agc is on
  float hangtime;          // s
  float threshold;         // dB
  float recovery_rate;     // dB/s
  bool pll,pll_square,envelope,isb;
  float pll_bw;            // Hz
  // All modes but spectrum
  bool snr_squelch;
  float squelch_open,squelch_close; // dB
  bool threshold_extend;   // FM only
  struct sockaddr_storage data; // output data destination (radiod uses port 5004 and 5006 on it regardless)
  int pt;                  // RTP payload type the channel uses for this format, 0 if unknown
};

enum chanctl_state {
  CHANCTL_NONE = 0,        // not ours, or gone
  CHANCTL_PENDING,         // asked for, not yet seen in status in the requested form
  CHANCTL_UP,              // seen in status with the requested frequency and format
  CHANCTL_RELEASING,       // released, may still be seen for a block or two
};

struct chanctl;

// Fill a format from a channel's decoded status (as decode_radio_status() leaves chan_t and struct frontend)
// Returns 0, or -1 if the status lacks the sample rate, channel count or data destination
int chanctl_format(struct chanctl_format *format,struct frontend const *frontend,chan_t const *chan);

// Open a control handle to the radiod whose command (= status) group is 'radio' (frontend.metadata_dest_socket,
// decoded from STATUS_DEST_SOCKET), sending on 'iface' (NULL for the default) with multicast 'ttl', as tune.c does
struct chanctl *chanctl_create(struct sockaddr_storage const *radio,char const *iface,int ttl);
// Release every channel the handle created, then close it
void chanctl_destroy(struct chanctl *ctl);

// Show the handle a status packet the caller has decoded. Optional, but without it the handle cannot confirm
// requests, notice channels it did not create, follow the front end tuning or retry lost commands
void chanctl_observe(struct chanctl *ctl,struct frontend const *frontend,chan_t const *chan);

// Ask for channel 'ssrc' on 'frequency' Hz in 'format', disappearing 'lifetime' seconds after the last request
// or refresh (0 = never). Cheap and idempotent: call it as often as you like, it sends only when needed and
// at most every 100 ms per SSRC. Returns 1 if a command was sent, 0 if none was needed, -1 on error with errno
// EEXIST (ssrc is someone else's channel in another form), ERANGE (outside the front end's coverage, radiod
// would retune it), EINVAL, ENOMEM, or a sendto() error
int chanctl_request(struct chanctl *ctl,uint32_t ssrc,double frequency,struct chanctl_format const *format,double lifetime);
// Keep a requested channel alive without changing it. Same returns; -1 with ENOENT if it isn't ours or has expired
int chanctl_refresh(struct chanctl *ctl,uint32_t ssrc);
// Let it go now. Channels the handle created are removed within a block; shared ones are left to expire
int chanctl_release(struct chanctl *ctl,uint32_t ssrc);

// State of 'ssrc'; optionally the tag of the last command sent for it (radiod echoes it as COMMAND_TAG in status)
// and the seconds since it was first requested without being confirmed (0 once confirmed)
enum chanctl_state chanctl_state(struct chanctl *ctl,uint32_t ssrc,uint32_t *tag,double *pending);

// SSRC for a frequency by radiod's own rule (kHz, rounded; radio.c), stepping past SSRCs that are someone
// else's channels on other frequencies or in other formats, as radiod steps past collisions
uint32_t chanctl_ssrc(struct chanctl *ctl,double frequency,struct chanctl_format const *format);

#endif
