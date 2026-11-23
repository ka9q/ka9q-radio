// P25 Phase 2 (TDMA) receiver for a complex baseband (IQ) stream
// H-DQPSK at 6000 symbols/s, two timeslots, bursts out with their error correction done
// Copyright 2026, Matthew Kaufman, KA6SQG
#ifndef _P25P2_H
#define _P25P2_H 1

#include <complex.h>
#include <stdbool.h>
#include <stdint.h>

#include "digital.h"
#include "p25p2unit.h"

enum {
  P25P2_SYMRATE = 6000,
  P25P2_FRAGMENT_DIBITS = 720, // I-ISCH, A, I-ISCH, B, S-ISCH, C, S-ISCH, D: 120 ms
  P25P2_ISCH_DIBITS = 20,
  P25P2_SUPERFRAME_SLOTS = 12, // three fragments: 360 ms
};

// One burst, as delivered once per timeslot of every fragment while frame sync is held.
// Valid only during the callback. A scrambled burst may be delivered twice: first undecoded,
// then with 'reparsed' set, if the scrambling parameters were set from a callback of its fragment
// (see p25p2.c for the receiver, the TDMA structure and the scrambling)
struct p25p2_burst {
  int timeslot;            // TDMA channel, 0 or 1
  int duid;                // enum p25p2_duid
  int duid_errors;         // bits corrected in the DUID, -1 if unknown
  int superframe_slot;     // 0-11: fragment * 4 + position in the fragment (A-D), the scrambling segment
  int fragment;            // 0-2, the fragment's position in the superframe
  bool position_from_isch; // the fragment's position came from its own I-ISCH (else carried on or guessed)
  bool position_known;     // an I-ISCH has decoded since frame sync was acquired
  int isch;                // bursts A and B: the I-ISCH before them, 9 info bits (LCH type 2, channel 2,
                           // location 2, flag 1, superframe count 2), or -1 (C, D, or not decoded)
  int isch_errors;         // bits it corrected
  int ultraframe;          // the superframe's place in the ultraframe, 0-3 (3 the last), from an I-ISCH
                           // of the superframe or carried on from one before; -1 unknown
  int color_code;          // the NAC the scrambling parameters were set with, -1 if none: a
                           // MAC_END_PTT with another color code is another system's (see p25p2.c)
  int sync_errors;         // bit errors in the fragment's two S-ISCH syncs
  bool descrambled;        // a scrambled burst: false until the scrambling parameters are set
  bool reparsed;           // delivered again, descrambled, after parameters set during this fragment
  uint64_t dibit;          // stream index of the burst's first dibit (6000 per second)
  uint8_t const *bits;     // 320, descrambled if 'descrambled'
  float const *confidence; // 160, per dibit
  // Error-corrected contents, NULL when the burst has none of that kind (or a scrambled one
  // could not be descrambled)
  int nframes;
  struct p25p2_ambe const *voice; // 4V: 4 frames, 2V: 2
  struct p25p2_ess const *ess;    // 2V: the encryption sync decode (check ess->ok)
  struct p25p2_mac const *mac;    // SACCH, FACCH, LCCH: the MAC PDU (check mac->crc_ok)
};
typedef void (*p25p2_burst_handler)(void *arg,struct p25p2_burst const *burst);

struct p25p2_stats {
  uint64_t dibits;
  uint64_t acquisitions;      // frame sync acquired
  uint64_t sync_losses;
  uint64_t fragments;         // with sync
  uint64_t missed_fragments;  // expected but no sync where it should be
  uint64_t slips;             // fragments re-aligned by one or two dibits
  uint64_t position_isch,position_flywheel; // fragments placed by an I-ISCH, or carried on
  uint64_t isch[2];           // I-ISCH decodes per timeslot
  uint64_t rotated_syncs;     // syncs found turned by a quadrant: frequency moved 1500 Hz
  uint64_t rotation_reverts;
  uint64_t reparsed;
  uint64_t bursts[2][16];     // per timeslot and DUID
  uint64_t unknown[2];        // DUID not decoded
  uint64_t undescrambled[2];  // scrambled bursts that could not be descrambled
  uint64_t mac_ok[2],mac_bad[2];
  uint64_t voice_frames[2];
  uint64_t ambe_errors[2];    // bits corrected in c0 and c1
  uint64_t ess_ok[2],ess_bad[2];
  uint64_t dd_updates;        // blocks whose frequency came from the decisions
  bool locked;                // frame sync held
  float frequency_offset;     // Hz
  float timing_error;         // smoothed normalized timing detector output
};

struct p25p2;

// Every soft dibit, before framing (tests and diagnostics)
typedef void (*p25p2_dibit_tap)(void *arg,struct softdibit const *sd);

struct p25p2 *create_p25p2(double samprate); // at least 4 samples per symbol (24 kHz)
void delete_p25p2(struct p25p2 *p);
void p25p2_set_burst_handler(struct p25p2 *p,p25p2_burst_handler handler,void *arg);
void p25p2_set_dibit_tap(struct p25p2 *p,p25p2_dibit_tap tap,void *arg);
// Scrambling parameters: from the control channel's network status broadcast (Phase 1 or the
// MAC's). Until set, scrambled bursts are delivered undecoded. Set from a burst callback (e.g.
// on a network status broadcast heard on this channel), the scrambled bursts of the fragment
// in progress are delivered again, descrambled; otherwise they apply from the next fragment
void p25p2_set_scrambling(struct p25p2 *p,uint32_t wacn,uint16_t sysid,uint16_t nac);
void p25p2_clear_scrambling(struct p25p2 *p);
// Feed 'count' samples at the rate given to create_p25p2(). Each call is one block of the
// frequency tracking (blocks over 0.1 s are split); 20 ms is typical
void p25p2_samples(struct p25p2 *p,float complex const *iq,int count);
// Feed soft dibits directly to the framer (tests): bypasses the receive chain
void p25p2_dibit(struct p25p2 *p,struct softdibit const *sd);
void p25p2_get_stats(struct p25p2 const *p,struct p25p2_stats *stats);
// A known carrier offset, e.g. the control channel's when a voice channel is granted. With
// 'hold' the offset is kept (not re-estimated) until frame sync gives its own
// (or for 30 seconds without frame sync); once frame sync has given one, a held seed is ignored
void p25p2_seed_frequency(struct p25p2 *p,float offset,bool hold);
float p25p2_frequency_offset(struct p25p2 const *p); // Hz, estimated carrier offset

// Options, for measurement; the defaults are the tested chain
enum p25p2_dd {
  P25P2_DD_FOURTH_POWER = 0, // default: residual from the 4th power of the differential symbols, power weighted
  P25P2_DD_DECISIONS,        // mean phase error against each symbol's decision, unweighted
  P25P2_DD_OFF,              // no frequency from the symbols: the block estimate keeps running (1 Hz corner)
};
void p25p2_set_decision_directed(struct p25p2 *p,enum p25p2_dd mode);
// Timing loop bandwidths: 'acquire' without frame sync, 'track' once it is held, 'settled'
// after 'settle_time' seconds of held sync (settled = track by default: one stage).
// A negative value keeps the current one
void p25p2_set_loop_bandwidths(struct p25p2 *p,float acquire,float track,float settled,float settle_time);
void p25p2_set_matched_filter(struct p25p2 *p,bool enable); // false: no receive filter at all

// The voice framing a MAC PDU's offset field (bits 3-5) gives, as the voice position
// (superframe_slot / 2, 0-4) of the timeslot's 2V burst, for a PDU in the burst at
// 'superframe_slot'; -1 if the field gives none (%110, %111)
int p25p2_offset_2v(int superframe_slot,int offset);

// The 12 scrambling segments of a superframe (320 bits each, DUID bits left clear) for these
// parameters (TIA-102.BBAC 7.2.5): a 44-bit LFSR seeded with WACN << 24 | SYSID << 12 | NAC
void p25p2_scrambling_sequence(uint8_t seq[P25P2_SUPERFRAME_SLOTS][P25P2_BURST_BITS],uint32_t wacn,
                               uint16_t sysid,uint16_t nac);
#endif
