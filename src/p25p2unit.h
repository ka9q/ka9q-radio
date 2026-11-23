// Error correction and field extraction for one P25 Phase 2 burst (one timeslot):
// the DUID, voice bursts (4V, 2V) and their AMBE+2 frames, the encryption sync (ESS),
// and the signalling bursts (SACCH, FACCH, LCCH) to corrected MAC PDU octets
// Copyright 2026, Matthew Kaufman, KA6SQG
#ifndef _P25P2UNIT_H
#define _P25P2UNIT_H 1

#include <stdbool.h>
#include <stdint.h>

#include "p25p2mac.h"

// Conventions:
// A burst is the 320 bits (160 dibits) of one timeslot, in transmitted order, descrambled,
// one bit per uint8_t. conf[] has one confidence (0-1) per dibit and may be NULL (hard).
// Every field is msb first. Nothing allocates; the only global is the ISCH code table, built
// once and then only read.

enum {
  P25P2_BURST_BITS = 320,
  P25P2_BURST_DIBITS = 160,
  P25P2_AMBE_BITS = 72,        // one AMBE+2 frame as sent
  P25P2_AMBE_INFO_BITS = 49,   // its information bits (vocoder input)
  P25P2_MAX_FRAMES = 4,        // AMBE+2 frames in a burst (4V)
  P25P2_MAC_MAX_BITS = 180,    // SACCH, LCCH; a FACCH PDU is 156
  P25P2_MAC_MAX_OCTETS = (P25P2_MAC_MAX_BITS + 7) / 8,
  P25P2_ESS_SEGMENTS = 8,      // ESS-B segments an ESS collector keeps
};

// The four information bits of the DUID (TIA-102.BBAC 5.4), sent as an (8,4,4) codeword at burst
// bits 0, 1, 74, 75, 244, 245, 318 and 319, never scrambled. The original TIA-102.BBAC (5.4.1)
// defines 0, 3, 6, 9, 12 and 15, the voice channel's bursts; DATCH and LCCH are not in it
enum p25p2_duid {
  P25P2_DUID_UNKNOWN = -1,
  P25P2_DUID_4V = 0,         // four voice frames and ESS-B
  P25P2_DUID_SACCH_S = 3,    // slow associated control, scrambled
  P25P2_DUID_2V = 6,         // two voice frames and ESS-A
  P25P2_DUID_FACCH_S = 9,    // fast associated control, scrambled
  P25P2_DUID_DATCH_S = 10,   // data channel, scrambled (not decoded)
  P25P2_DUID_SACCH = 12,     // slow associated control, not scrambled
  P25P2_DUID_LCCH = 13,      // logical control channel, not scrambled
  P25P2_DUID_FACCH = 15,     // fast associated control, not scrambled
};

// DUID to the nearest codeword: one bit error is corrected; two only when conf[] makes one of
// the candidates at distance two clearly the cheapest; a reserved value is P25P2_DUID_UNKNOWN.
// *errors (may be NULL) gets the bits corrected, -1 when unknown
int p25p2_duid_decode(uint8_t const bits[P25P2_BURST_BITS],float const conf[P25P2_BURST_DIBITS],int *errors);
uint8_t p25p2_duid_codeword(int duid);  // the 8 bits as sent, value << 4 | parity
bool p25p2_duid_scrambled(int duid);    // voice and the scrambled signalling and data bursts
bool p25p2_duid_voice(int duid);
int p25p2_duid_mac_chan(int duid);      // enum p25_mac_chan of a signalling burst, else -1
char const *p25p2_duid_name(int duid);

// Phase 2 ISCH (I-ISCH): (40,9) code, minimum distance 16, nearest codeword up to 7 errors
// Only codewords whose channel field is this timeslot (0 or 1) and whose ISCH location is
// not the reserved value 3 are candidates; a tie goes to the lowest info value.
// *info gets the 9 info bits, msb first: LCH type (2), channel (2), ISCH location (2),
// LCH flag (1), superframe sequence (2). Returns bits corrected (0-7) or -1
// (The original TIA-102.BBAC, 5.5, names those fields reserved, channel number, ISCH location,
// FR (the next inbound SACCH is free for listeners) and ultraframe count, and adds an offset
// vector to every codeword, as here)
int isch_decode(uint8_t const bits[40],int timeslot,int *info);
int isch_decode_word(uint64_t word,int timeslot,int *info); // word bit 39 = first bit sent

// One AMBE+2 (half rate, 3600 x 2450) frame (TIA-102.BABA-1): four vectors read from the 72
// bits as sent, column by column (bit 4k + c is entry 18c + k of the list, the list split
// 24 / 23 / 11 / 14): c0 Golay(24,12), c1 Golay(23,12) masked by a sequence seeded from u0,
// c2 and c3 unprotected
struct p25p2_ambe {
  uint8_t raw[P25P2_AMBE_BITS];
  // u0 (12) u1 (12) u2 (11) u3 (14), each msb first: the 49 information bits in the order a
  // vocoder takes them, e.g. mbelib's ambe_d[49] for mbe_processAmbe2450Dataf()
  uint8_t bits[P25P2_AMBE_INFO_BITS];
  uint16_t u[4];
  // Bits corrected in c0 (0-3, or 4: a fourth error seen by its parity bit; u0 is then the
  // Golay(23,12) correction regardless) and in c1 (0-3): epsilon 0 and 1 of TIA-102.BABA-1
  int errors[2];
  // Of those, the ones in u0 and u1. mbelib counts only these: for mbe_processAmbe2450Dataf()
  // its errs is data_errors[0] and its errs2 data_errors[0] + data_errors[1]
  int data_errors[2];
};
void p25p2_ambe_decode(struct p25p2_ambe *f,uint8_t const raw[P25P2_AMBE_BITS]);
// The frames of a voice burst (4 in a 4V, at bits 2, 76, 172, 246; 2 in a 2V, at 2, 76).
// Returns the number of frames, 0 if duid is not a voice burst
int p25p2_voice_decode(struct p25p2_ambe frames[P25P2_MAX_FRAMES],int duid,uint8_t const bits[P25P2_BURST_BITS]);

// Encryption sync: ALGID (8), KID (16), MI (72) in RS(44,16,29). ESS-B 1-4 (four hexbits each,
// burst bits 148-171) come in the four 4V bursts before a 2V, ESS-A (28 parity hexbits, bits
// 148-243 and 246-317) in the 2V. A missing ESS-B is four erasures: ESS-A alone recovers the
// ESS with up to six errors
struct p25p2_ess {
  bool ok;             // Reed-Solomon decoded
  uint8_t algid;       // 0x80 clear
  uint16_t kid;
  uint8_t mi[9];
  int erasures;        // ESS-B hexbits missing (4 per segment)
  int rs_fixed;        // hexbits Reed-Solomon changed, erasures included
};
// One timeslot's ESS-B segments until its next 2V. A 2V decode, or a reset, empties it; if a 2V
// is missed the segments go on collecting, the oldest dropped once P25P2_ESS_SEGMENTS are held
struct p25p2_ess_collector {
  struct {
    uint8_t sym[4];
    int64_t voice_index;
    uint64_t arrival;
  } seg[P25P2_ESS_SEGMENTS];
  int nseg;
  uint64_t arrivals;
  uint64_t arrivals_at_2v;
};
// voice_index: the burst's voice position, a running count of the timeslot's five voice
// bursts per superframe, or -1 if not known (segments are then taken in arrival order)
void p25p2_ess_reset(struct p25p2_ess_collector *c); // new transmission: forget the segments
void p25p2_ess_add(struct p25p2_ess_collector *c,uint8_t const bits[P25P2_BURST_BITS],int64_t voice_index); // 4V
bool p25p2_ess_decode(struct p25p2_ess_collector *c,struct p25p2_ess *ess,uint8_t const bits[P25P2_BURST_BITS],
                      int64_t voice_index); // 2V: returns ess->ok

// A signalling burst's MAC PDU. SACCH and LCCH: 30 information and 22 parity hexbits, a
// RS(52,30) punctured from RS(63,35,29) by six parity symbols; FACCH: 26 and 19, RS(45,26),
// nine punctured. Punctured symbols are erasures; if a decode fails, the 4, 8 and then 12
// weakest received hexbits (by dibit confidence) are erased too and the decode retried.
// The PDU is then checked by its CRC: CRC-12 over 168 (SACCH) or 144 (FACCH) bits,
// CRC-16 CCITT over 164 (LCCH)
struct p25p2_mac {
  int chan;            // enum p25_mac_chan
  bool rs_ok;
  bool crc_ok;         // implies rs_ok: the PDU is good
  int nbits;           // 180 SACCH, LCCH; 156 FACCH (CRC included)
  uint8_t bits[P25P2_MAC_MAX_BITS];
  uint8_t octets[P25P2_MAC_MAX_OCTETS]; // msb first, the last zero filled: p25_mac_decode()'s input
  int corrected;       // received hexbits Reed-Solomon changed (punctured ones not counted)
  int erasures;        // erasures used in the last attempt, punctured included
  int attempts;        // 1-4
};
// Returns m->crc_ok; false (m->rs_ok false) if duid is not a signalling burst
bool p25p2_mac_decode_burst(struct p25p2_mac *m,int duid,uint8_t const bits[P25P2_BURST_BITS],
                            float const conf[P25P2_BURST_DIBITS]);

#endif
