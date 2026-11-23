// P25 Phase 2 (TDMA) receiver: IQ samples in, bursts out
// Level, frequency correction of each block by its own estimate, root raised cosine matched
// filter, symbol timing, the phase change over one symbol (H-DQPSK is differentially encoded
// pi/4 QPSK), then TDMA framing: fragment sync, the ISCH, descrambling, the two timeslots
// Copyright 2026, Matthew Kaufman, KA6SQG
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "digital.h"
#include "p25p2.h"

// Theory of operation
//
// The signal. A Phase 2 carrier holds two TDMA channels (timeslots 0 and 1) at 6000 symbols/s,
// 12 kbit/s. This receiver takes the outbound direction, from the site, which is a continuous
// carrier in H-DQPSK; the subscribers' inbound bursts are H-CPM and are not decoded here
// (TSB-102.BBAA 6). H-DQPSK puts the information in the phase change from one symbol to the next,
// +-45 or +-135 degrees, as Phase 1's linear simulcast modulation does, so the same one-symbol
// differential detector and the same soft dibit (dsync.c) serve both. The receive filter is a
// root raised cosine of rolloff 0.2 (Rrc_alpha), taken to be the transmitter's shaping, for which
// the signal occupies +-3600 Hz.
//
// The chain. p25p2_samples() takes each block (split at Max_block_time) through, in order: level
// control to unity RMS; the frequency estimate and the correction of this same block by it
// (below); the matched filter, the transmitter's root raised cosine again, so that the two
// together are a raised cosine, with no intersymbol interference at the symbol instants and the
// best signal to noise ratio there; and the timing loop (interpolator, Gardner detector, two
// outputs per symbol), which calls sync_output() for each. There the phase change over one
// symbol, z = y[k] conj(y[k-1]), becomes a soft dibit and adds to the block's frequency error
// sums. After the block the frequency loop is closed from those sums and the soft dibits go to
// the framer one at a time. The loops want 4 samples per symbol, so the receiver needs at least
// 24 kHz: create_p25p2() refuses less.
//
// Frequency. Each block is corrected by an estimate that already includes that block's own
// measurement (feed-forward, shift_freqtrack()), so a carrier's first timeslot, decoded before
// any sync has been seen, is corrected too. The measurement is the lag-one residual of a copy of
// the block rotated by the current estimate and lowpassed (Measure_cutoff), smoothed with a 20 Hz
// corner while searching. Once frame sync is held that estimate is frozen and the decisions move
// it instead (dd_mode): by default the fourth power of each differential symbol, which takes
// the modulation off (the fourth power of an ideal +-45 or +-135 degree step is -1), weighted by
// power; a quarter of its angle is the residual turn per symbol, unambiguous to +-750 Hz. Each
// block moves the estimate by Dd_gain of its residual, from the next block. When sync is lost
// the estimate is held for Afc_hold_time before the block estimate runs again, since a carrier
// does not move between transmissions; a held seed (the control channel's offset, given when a
// voice channel is granted) is kept the same way, until frame sync or Afc_hold_time without it.
//
// Quadrant errors. The fourth power is blind to an error of a whole quadrant per symbol, 1500 Hz
// (or two, 3000 Hz): every differential symbol is then turned by 90 (180) degrees, every
// decision is wrong, and the residual is still zero; a frozen or held estimate does not move by
// itself either. So the framer also looks for the sync pattern with every dibit turned by one,
// two or three quadrants (hunt_rotated()). When the same turn is found again one fragment later
// the estimate is moved by that many quadrants, and moved back unless frame sync follows within
// four fragments (Rotation_revert, 0.48 s).
//
// Timing. The loop is very wide while searching (Acquire_bw, a time constant of about 2.5
// symbols) so that the first timeslot of a carrier decodes whatever the starting phase, and
// narrow at frame sync (Track_bw, about 83 symbols, 14 ms). A second, narrower stage after
// Settle_time can be set (p25p2_set_loop_bandwidths()) but is off by default. When sync is lost
// the loop widens again and forgets the symbol rate estimate, which noise would otherwise walk.
//
// The TDMA structure. A superframe is 12 timeslots of 30 ms, 360 ms (TIA-102.BBAC 3.2.1). Each
// timeslot is a 20-dibit ISCH (inter-slot signalling channel) followed by a 160-dibit (320-bit)
// burst; strictly the ISCH straddles two slots, half at the end of one and half at the start of
// the next, and a superframe begins halfway through the ISCH before its slot 0 (TIA-102.BBAC 3.3.4,
// 3.4.2). This file works in fragments of four timeslots, 720 dibits or 120 ms, three to a
// superframe. In dibits:
//
//   0 I-ISCH, 20 burst A, 180 I-ISCH, 200 burst B, 360 S-ISCH, 380 burst C, 540 S-ISCH, 560 burst D
//
// The ISCHs go in pairs, two I-ISCHs then two S-ISCHs, so those before slots 0, 1, 4, 5, 8 and 9
// are I-ISCHs and those before 2, 3, 6, 7, 10 and 11 are S-ISCHs (TIA-102.BBAC 3.3.4, figure 5-3).
// An S-ISCH is the fixed 40-bit sync pattern (TIA-102.BBAC 5.1, the voice channel's). An I-ISCH is
// a (40,9) codeword (p25p2unit.c) whose fields include the channel of the burst after it and the
// fragment's position in the superframe, its ISCH location 0, 1 or 2 (TIA-102.BBAC 5.5). Bursts A,
// B, C, D belong to channels 0, 1, 0, 1, except in a superframe's last fragment, where C belongs
// to 1 and D to 0: slots 10 and 11 are swapped so that a subscriber has time to turn round between
// sending and listening on its SACCH (TIA-102.BBAC 3.2.1). So each channel has six bursts in a
// superframe: five voice positions, then its SACCH (slot 10 for channel 1, 11 for channel 0).
// Each burst says what it is in its DUID (4 bits in an (8,4,4) code spread over the burst, never
// scrambled): 4V and 2V voice, SACCH, FACCH and LCCH signalling, scrambled or not, and DATCH data
// (not decoded). Their layouts and codes are in p25p2unit.c.
//
// Sync search (hunt()). Each new dibit, the last 20 are compared with the S-ISCH pattern in bit
// errors. A close match may be either S-ISCH of a fragment, so acquisition waits for the other one
// too, 180 dibits before or after. The fragment found is delivered at once.
//
// Tracking (process_fragment()). With sync, a fragment is taken every 720 dibits. Both syncs clean
// where expected: it is aligned. Otherwise offsets of one and two dibits either way are tried too
// (best_offset()), and the fragment is moved to the best: the timing loop can slip a symbol, and
// this follows it. A fragment with no sync near is dropped, since noise decodes as plausible
// DUIDs; it still takes its place in the superframe, and MISSES of them in a row lose sync.
//
// Position (fragment_position()). Each I-ISCH that decodes gives the fragment's position in the
// superframe, which fixes the channel of bursts C and D, the scrambling segment of every burst,
// and the voice positions the ESS is assembled by. With neither I-ISCH decoded the position
// carries on from the last fragment; until one decodes after acquisition it is a guess (0), and
// bursts say so (position_known).
//
// Ultraframe. Four superframes make an ultraframe (TIA-102.BBAC-A 3.1.2), and every I-ISCH of a
// voice channel carries the place of its superframe in it, the Ultraframe Count in its bits 1-0,
// %11 the last (5.5.1.4). It matters for the SACCH: while a subscriber transmits, the outbound
// SACCH of the first three superframes names the source of the outbound audio and that of the
// last superframe the inbound talker, who may be another unit, so a listener takes the talker
// only from the first three (3.4). A SACCH never has an I-ISCH of its own: it is in the inverted
// timeslot, whose ISCH is always an S-ISCH (3.2), slot 10 or 11, bursts C and D of the
// superframe's last fragment. So the count is the superframe's, from whichever of its six
// I-ISCHs decoded (the best of the fragment's two, as for the position), carried on from the
// fragment before when none did, and advanced by one when a new superframe begins (position 0).
// The count means something only where the LCH Type (bits 8-7) says voice channel, %00 (on an
// LCCH bit 2 and the count are other fields, 5.5.1.1); a decode of another type gives none.
//
// Scrambling (TIA-102.BBAC 7.2.5). Its purpose is that a receiver does not take the voice or
// messages of another P25 TDMA system on the same frequency for its own. Voice bursts are always
// scrambled; signalling bursts are too, except those carrying a network status broadcast (which
// gives the parameters) or a MAC_END_PTT (whose color code identifies an interfering system).
// The DUID says which (p25p2_duid_scrambled()); the ISCH, the DUID and sync sequences are never
// scrambled. The sequence comes from a 44-bit shift register seeded with the WACN, system ID and
// color code (the NAC), restarted every superframe: 4320 bits, one 360-bit stretch per timeslot,
// starting halfway through the ISCH before slot 0, of which a burst uses the 320 bits under it.
// So the segment is chosen by the burst's superframe slot. The parameters come from outside
// (p25d: the control channel's network status and NAC) and are taken up at the start of a
// fragment. Until they are set, scrambled bursts are delivered with 'descrambled'
// false and nothing decoded from them, rather than decoded with a guessed sequence. Unscrambled
// bursts are decoded regardless, so a handler may learn the parameters from one (a network status
// broadcast on this channel): set during a fragment's delivery, they cause its scrambled bursts
// to be delivered again, descrambled and marked 'reparsed'.
//
// What is delivered. For each timeslot of each synced fragment, one struct p25p2_burst (p25p2.h):
// its place, DUID, sync and ISCH quality, the (descrambled) bits and confidences, and what was
// decoded from them: a signalling burst's MAC PDU, a voice burst's AMBE+2 frames, and with a 2V
// the ESS assembled from the timeslot's 4Vs before it. Each timeslot's ESS collector is emptied
// at a MAC PTT, END PTT, IDLE or HANGTIME, at a change of audio source and when sync is lost, so
// that no ESS is assembled from two transmissions or two sources. A change of source within a
// transmission (a console interrupting, or another unit repeated) is signalled by a MAC PDU whose
// offset field gives a voice framing other than the one in force (TIA-102.BBAD-A 4.2): the
// offset field of every SACCH and FACCH PDU names the 4V that starts the next voice sequence,
// so where the 2V falls (p25p2_offset_2v()). An END PTT is the exception to that list when its
// color code (bits 12-23) is not the NAC the scrambling was set with: it is sent unscrambled so
// that a receiver can tell another system's (TIA-102.BBAD-A 4.10; TIA-102.BBAC 7.2.5, 7.3.3),
// and another system's END PTT ends nothing here. Before the scrambling parameters are set no
// color code is known and an END PTT is taken as this system's, as before; voice is scrambled,
// so until then no voice is decoded and no ESS assembled anyway. Each burst carries that color
// code (color_code) for the handler's own check, and its superframe's ultraframe count. A burst
// whose DUID did not decode is delivered with nothing decoded.

// Timing loop bandwidths (normalized, per symbol). Acquisition is wide so that the first
// timeslot of a carrier (dibits 20-179, before its first S-ISCH) decodes whatever the initial
// timing phase; with frame sync the loop narrows. One stage by default
static float const Acquire_bw = 0.4f;
static float const Track_bw = 0.012f;
static float const Settle_time = 0.5f; // seconds of held sync before an optional second stage
static float const Loop_damping = 1.0f;
// The timing detector's slope, error per input sample (set_gains() in dsync.c): Phase 1's
// tuning constant (p25p1.c), which there is for 4 samples per symbol, as 24 kHz is here. It is
// not scaled for other sample rates as Phase 1's is
static float const Error_gain = 0.42f;
// Symbol rate error the timing loop may integrate: transmitter plus receiver clock errors are tens of ppm
static float const Rate_limit = 0.0003f;
static int const Interp_phases = 32;
// Frequency: the block estimate's corner while acquiring; with frame sync it is frozen and the
// decided symbols move it, by this fraction of each block's residual (a time constant of about
// seven blocks). The 1 Hz corner is for running without that
static double const Afc_acquire_corner = 20.0; // Hz
static double const Afc_track_corner = 1.0;    // Hz
static float const Dd_gain = 0.15f;
// After frame sync is lost the frequency is held this long before the block estimate takes
// over again: a carrier does not move between transmissions
static float const Afc_hold_time = 30.0f; // seconds
// The block estimate's residual is measured after this lowpass, flat over the signal (+-3600 Hz)
// so it adds no data dependent bias, and narrow enough that noise across a wider stream does
// not pull the estimate toward the stream's centre
static double const Measure_cutoff = 4000.0; // Hz
static double const Measure_span = 95.0 / 24000.0; // seconds
// Receive matched filter: the transmitter's root raised cosine, rolloff 0.2, +-8 symbols
static double const Rrc_alpha = 0.2;
static int const Rrc_span = 8; // symbols each side
// Level: per-sample power smoothing, 1/2000 at 24 kHz
static double const Level_time = 1.0 / 12.0; // seconds
// A sync found only turned by +-90 or 180 degrees means a frequency error of +-1500 or 3000 Hz
// (each symbol turned a quadrant); the move is undone if frame sync does not follow within
// four fragments
static float const Quadrant_hz = P25P2_SYMRATE / 4.0f;
static int const Rotation_revert = 4 * P25P2_FRAGMENT_DIBITS;
// Longest block processed at once: the block's buffers, and its dibit list (with room for the
// timing loop running fast), are sized from it
static double const Max_block_time = 0.1; // seconds

// Framing (fragment layout and sync thresholds, in bit errors of the 40-bit S-ISCH sync)
// A one-dibit slip shifts the pattern against itself: its 7 changes between 01 and 11 cost a bit
// each, and the dibit shifted in, a DUID dibit of the burst beside it, 0 to 2 more. So at offset
// 0 a slipped sync can pass TRACK, and best_offset() takes the offset with the fewest errors
// rather than the first that passes. While searching the framer needs a pair's partner 180
// dibits back and a fragment start up to 540 back: it trims its history to two fragments when it
// holds four (SEARCH_TRIM, SEARCH_KEEP). Synced, it keeps one fragment before the next one's
// start, for the negative offsets
enum {
  SYNC_DIBITS = 20,
  FRAGMENT = P25P2_FRAGMENT_DIBITS,
  SYNC1 = 360,             // the two S-ISCHs, before bursts C and D
  SYNC2 = 540,
  PAIR = SYNC2 - SYNC1,
  ISCH1 = 0,               // the two I-ISCHs, before bursts A and B
  ISCH2 = 180,
  HUNT = 4,                // acquire on a sync this close ...
  HUNT_PARTNER = 6,        // ... with the other one 180 dibits away this close
  TRACK = 7,               // a fragment keeps sync with either sync this close (a one-dibit slip scores 7 to 9)
  CLEAN = 4,               // both this close: aligned, no slip search
  SLIP1 = 6,               // both syncs this close at +-1 dibit
  SLIP2 = 5,               // ... at +-2
  MAX_SLIP = 2,
  NO_SYNC = 99,
  MISSES = 2,              // fragments without sync before sync is lost
  ISCH_TRUST = 3,          // an I-ISCH corrected by more bits than this does not override the flywheel
  ROTATION_TOLERANCE = 2,  // dibits: a rotated sync repeats one fragment later
  RING = 4096,             // dibit history, a power of two above the four fragments the framer may keep
  SEARCH_TRIM = 4 * FRAGMENT,
  SEARCH_KEEP = 2 * FRAGMENT,
};
static uint64_t const NONE = ~(uint64_t)0;
// First dibit of bursts A-D in the fragment, each after its 20-dibit ISCH
static int const Burst_offset[4] = { 20, 200, 380, 560 };
// S-ISCH sync, as dibits (01 = 1, 11 = 3): the voice channel's pattern of TIA-102.BBAC 5.1,
// +3 and -3 symbols
static uint8_t const Sync_pattern[SYNC_DIBITS] = { 1,1,1,3, 1,1,3,1, 1,1,1,3, 3,3,1,3, 3,3,3,3 };
// The ideal phase change of each dibit, as make_softdibit() decides them (digital.h)
static float const Nominal_phase[4] = { M_PI_4, 3 * M_PI_4, -M_PI_4, -3 * M_PI_4 };

struct slot {
  struct p25p2_burst b;
  uint8_t bits[P25P2_BURST_BITS];
  float conf[P25P2_BURST_DIBITS];
};

struct p25p2 {
  double samprate;
  struct level level;
  struct freqtrack afc;      // the block estimate; rotates the measurement copy
  struct freqtrack shift;    // rotates the signal by the updated estimate
  struct rxfilter measure;
  struct rxfilter rrc;
  struct symsync sync;
  bool matched;
  bool primed;
  float complex prev_symbol;
  int max_block;
  float complex *block,*copy;
  struct softdibit *dibits;
  int ndibits,max_dibits;
  uint64_t rx_dibits;        // dibits out of the receive chain, counted a block at a time

  // frequency control
  bool frame_synced;
  bool seed_pending,seed_hold;
  float seed_value;
  bool afc_held;             // the block estimate is frozen (frame sync or a held seed)
  bool afc_from_sync;        // the estimate came from frame sync on this carrier
  uint64_t last_sync_dibit;
  uint64_t hold_dibits;
  bool rotation_pending;
  float rotation_hz;
  uint64_t rotation_deadline;
  enum p25p2_dd dd_mode;
  double dd_re,dd_im;        // sum of -d^4 / |d|^2 over the block's differential symbols
  double dd_error;           // sum of (phase - nominal phase of the decision)
  int dd_symbols;

  // timing loop stages
  float settled_bw;
  uint64_t settle_dibits;
  bool settled;
  uint64_t synced_since;

  // framer
  bool synced;
  struct softdibit ring[RING];
  uint64_t buf_start;        // oldest dibit the framer still holds
  uint64_t total;            // dibits in
  uint64_t frag_start;       // next fragment's first dibit (synced)
  uint64_t pending_first;    // a sync that may be the first of a pair (searching)
  int last_position;         // superframe position of the last fragment, -1 unknown
  bool position_known;
  bool position_from_isch;
  bool just_acquired;
  int misses;
  int isch_info[2],isch_errors[2];
  int ultraframe;            // of the last fragment's superframe, -1 unknown
  int framing[2];            // per timeslot: the 2V's voice position by the last offset field, -1 none
  uint64_t last_rotation_report,rotation_candidate;
  int rotation_q;

  // scrambling
  bool scr_valid;
  uint64_t scr_seed;
  uint8_t seq[P25P2_SUPERFRAME_SLOTS][P25P2_BURST_BITS];
  bool scr_pending,scr_pending_valid;
  uint64_t scr_pending_seed;
  bool delivering,reparse;

  // the fragment being delivered and its decoded contents
  struct slot slots[4];
  struct p25p2_ess_collector ess[2];
  struct p25p2_ambe frames[P25P2_MAX_FRAMES];
  struct p25p2_ess ess_out;
  struct p25p2_mac mac;

  struct p25p2_stats stats;
  p25p2_burst_handler handler;
  void *handler_arg;
  p25p2_dibit_tap tap;
  void *tap_arg;
};

static void frame_dibit(struct p25p2 *p,struct softdibit const *sd);

// ---------------- scrambling ----------------

// The seed: WACN * 2^24 + system ID * 2^12 + color code, 20 + 12 + 12 bits, the shift register's
// length (TIA-102.BBAC 7.2.5; the color code is the NAC of the Phase 1 control channel)
static uint64_t scr_seed(uint32_t wacn,uint16_t sysid,uint16_t nac){
  return (uint64_t)(wacn & 0xfffff) << 24 | (uint64_t)(sysid & 0xfff) << 12 | (nac & 0xfff);
}
// 44-bit register shifting left, output its bit 43, feedback 43 ^ 33 ^ 19 ^ 14 ^ 8 ^ 3; a zero
// seed is all ones. Superframe timeslot k is xored with stream bits 20 + 360 k on: the stream
// starts halfway through the superframe's first ISCH, and each timeslot is 320 of every 360
// That feedback is the external form of G(x) = x^44 + x^40 + x^35 + x^29 + x^24 + x^10 + 1, and a
// zero seed is replaced by 2^44 - 1 (TIA-102.BBAC 7.2.5). Every burst bit but the DUID's is xored,
// including the 42-bit sync in the middle of a scrambled FACCH (bits 138-179), which the standard
// leaves unscrambled; nothing decodes those bits
static void sequence_from_seed(uint8_t seq[P25P2_SUPERFRAME_SLOTS][P25P2_BURST_BITS],uint64_t seed){
  uint64_t const mask = ((uint64_t)1 << 44) - 1;
  uint64_t reg = seed & mask;
  if(reg == 0)
    reg = mask;
  for(int i=0; i < 20 + 360 * P25P2_SUPERFRAME_SLOTS; i++){
    uint64_t const out = (reg >> 43) & 1;
    int const k = (i - 20) / 360,b = (i - 20) % 360;
    if(i >= 20 && b < P25P2_BURST_BITS)
      seq[k][b] = (uint8_t)out;
    uint64_t const fb = out ^ (reg >> 33) ^ (reg >> 19) ^ (reg >> 14) ^ (reg >> 8) ^ (reg >> 3);
    reg = ((reg << 1) | (fb & 1)) & mask;
  }
  // The DUID is sent in the clear
  static int const duid_bits[8] = { 0, 1, 74, 75, 244, 245, 318, 319 };
  for(int k=0; k < P25P2_SUPERFRAME_SLOTS; k++)
    for(int i=0; i < 8; i++)
      seq[k][duid_bits[i]] = 0;
}
// Offset n (TIA-102.BBAD-A 4.2, Table 10): the first 4V of the next voice sequence is the
// (n+1)th non-SACCH burst on this timeslot after this one, %000 to %100 outbound and %101 too
// (the 6th, outbound only); %110 is reserved outbound and %111 means no voice framing or none
// known. The non-SACCH bursts of a timeslot are its five voice positions in each superframe,
// superframe_slot / 2 = 0-4, and its SACCH is the sixth (slot 10 or 11, 5): counted in
// non-SACCH bursts the one after position k is k + 1, the one after position 4 or the SACCH is
// position 0 of the next superframe. The voice bursts go 4V 4V 4V 4V 2V without regard to the
// SACCH (TIA-102.BBAC-A 6.1), so the 2V is the non-SACCH burst before the first 4V. On the
// real recording this agrees with the 2Vs seen, after every PTT
int p25p2_offset_2v(int superframe_slot,int offset){
  if(offset < 0 || offset > 5 || superframe_slot < 0 || superframe_slot >= P25P2_SUPERFRAME_SLOTS)
    return -1;
  int const k = superframe_slot / 2;
  int const next = k < 5 ? k + 1 : 0;
  return (next + offset + 4) % 5;
}
void p25p2_scrambling_sequence(uint8_t seq[P25P2_SUPERFRAME_SLOTS][P25P2_BURST_BITS],uint32_t wacn,
			       uint16_t sysid,uint16_t nac){
  sequence_from_seed(seq,scr_seed(wacn,sysid,nac));
}
// Take up parameters set since the last fragment
static void refresh_scrambling(struct p25p2 *p){
  if(!p->scr_pending)
    return;
  p->scr_pending = false;
  if(p->scr_pending_valid && (!p->scr_valid || p->scr_pending_seed != p->scr_seed))
    sequence_from_seed(p->seq,p->scr_pending_seed);
  p->scr_valid = p->scr_pending_valid;
  p->scr_seed = p->scr_pending_seed;
}
// Only the pending copy changes here, so all the bursts of a fragment are taken with one sequence.
// Set during a fragment's delivery with new parameters, it has the fragment's scrambled bursts
// that went out undescrambled delivered again (process_fragment()); bursts already descrambled
// with other parameters are not
void p25p2_set_scrambling(struct p25p2 *p,uint32_t wacn,uint16_t sysid,uint16_t nac){
  uint64_t const seed = scr_seed(wacn,sysid,nac);
  if(p->delivering && (!p->scr_valid || seed != p->scr_seed))
    p->reparse = true;
  p->scr_pending = true;
  p->scr_pending_valid = true;
  p->scr_pending_seed = seed;
}
void p25p2_clear_scrambling(struct p25p2 *p){
  p->scr_pending = true;
  p->scr_pending_valid = false;
}

// ---------------- frequency and timing loops driven by frame sync ----------------

static void reset_to_acquisition(struct p25p2 *p){
  p->frame_synced = false;
  p->settled = false;
  lock_symsync(&p->sync,false);
  reset_symsync_rate(&p->sync); // drop the rate estimate noise has walked
  // Hold the frequency where frame sync left it; the block estimate only acquires a carrier
  // not yet synced (or not for Afc_hold_time)
  set_freqtrack_corner(&p->afc,p->afc_held ? 0 : Afc_acquire_corner);
}
static void frame_sync(struct p25p2 *p,bool synced){
  if(synced == p->frame_synced)
    return;
  if(!synced){
    reset_to_acquisition(p);
    return;
  }
  p->frame_synced = true;
  p->rotation_pending = false;
  // From here the decided symbols move the estimate; the block estimate is frozen
  set_freqtrack_corner(&p->afc,p->dd_mode == P25P2_DD_OFF ? Afc_track_corner : 0);
  p->afc_held = p->dd_mode != P25P2_DD_OFF;
  p->afc_from_sync = true;
  p->last_sync_dibit = p->rx_dibits;
  p->synced_since = p->rx_dibits;
  p->settled = false;
  lock_symsync(&p->sync,true);
}
// The framer found the sync with every dibit turned by 'q' quadrants (+90 degrees each)
static void rotated_sync(struct p25p2 *p,int q){
  q = ((q % 4) + 4) % 4;
  if(q == 0)
    return;
  float const move = (q == 2 ? 2.0f : q == 1 ? 1.0f : -1.0f) * Quadrant_hz;
  seed_freqtrack(&p->afc,p->afc.estimate + move);
  p->stats.rotated_syncs++;
  p->rotation_pending = true;
  p->rotation_hz = move;
  p->rotation_deadline = p->rx_dibits + Rotation_revert;
}

// ---------------- framer ----------------

static inline struct softdibit const *at(struct p25p2 const *p,uint64_t index){
  return &p->ring[index & (RING - 1)];
}
// Bit errors between the sync pattern, each dibit turned by q * 90 degrees, and the 20 dibits
// from stream index 'index'
static int sync_errors_at(struct p25p2 const *p,uint64_t index,int q){
  if(index < p->buf_start || index + SYNC_DIBITS > p->total)
    return 99;
  static int const turn[4] = { 1, 3, 0, 2 }; // +90 degrees: 00 -> 01 -> 11 -> 10 -> 00
  int errors = 0;
  for(int i=0; i < SYNC_DIBITS; i++){
    int expected = Sync_pattern[i];
    for(int k=0; k < q; k++)
      expected = turn[expected];
    errors += __builtin_popcount((unsigned)(at(p,index + i)->dibit ^ expected));
  }
  return errors;
}
static void trim_search(struct p25p2 *p){
  if(p->total - p->buf_start > SEARCH_TRIM)
    p->buf_start = p->total - SEARCH_KEEP;
}
static void trim_synced(struct p25p2 *p){
  if(p->frag_start > p->buf_start + FRAGMENT)
    p->buf_start = p->frag_start - FRAGMENT;
}
static void lose_sync(struct p25p2 *p){
  p->synced = false;
  p->misses = 0;
  p->last_position = -1;
  p->position_known = false;
  p->ultraframe = -1;
  p->pending_first = NONE;
  p->stats.sync_losses++;
  for(int ts=0; ts < 2; ts++){
    p25p2_ess_reset(&p->ess[ts]);
    p->framing[ts] = -1;
  }
  frame_sync(p,false);
  trim_search(p);
}

// The superframe position of the fragment at 'fs': from I-ISCH 1 (channel 0) or I-ISCH 2
// (channel 1), whichever needed fewer corrections; if neither, one on from the last; until one
// decodes after acquisition, a guess (0). A heavily corrected I-ISCH that contradicts the
// position carried on is more likely a mis-decode
// The ultraframe count goes with it (see the top of the file): from the best decoded I-ISCH of
// a voice channel, if the position was taken from an I-ISCH; else carried on
static int fragment_position(struct p25p2 *p,uint64_t fs){
  int const predicted = p->last_position >= 0 ? (p->last_position + 1) % 3 : -1;
  int position = -1,corrected = 99;
  int ultraframe = -1,uf_corrected = 99;
  for(int ch=0; ch < 2; ch++){
    uint64_t word = 0;
    uint64_t const base = fs + (ch == 0 ? ISCH1 : ISCH2);
    for(int d=0; d < P25P2_ISCH_DIBITS; d++)
      word = (word << 2) | at(p,base + d)->dibit;
    int info = -1;
    int const e = isch_decode_word(word,ch,&info);
    p->isch_info[ch] = e >= 0 ? info : -1;
    p->isch_errors[ch] = e;
    if(e < 0)
      continue;
    p->stats.isch[ch]++;
    int const seq = (info >> 3) & 3;
    if(seq < 3 && e < corrected){
      position = seq;
      corrected = e;
    }
    if(seq < 3 && ((info >> 7) & 3) == 0 && e < uf_corrected){ // LCH Type %00: a voice channel
      ultraframe = info & 3;
      uf_corrected = e;
    }
  }
  if(position >= 0 && predicted >= 0 && position != predicted && corrected > ISCH_TRUST)
    position = -1;
  int const carried = p->ultraframe < 0 ? -1 : (position >= 0 ? position : predicted) == 0 ? (p->ultraframe + 1) % 4
    : p->ultraframe;
  if(position < 0 || (carried >= 0 && ultraframe != carried && uf_corrected > ISCH_TRUST))
    ultraframe = -1; // no I-ISCH taken, or a heavily corrected one contradicting the count carried on
  p->ultraframe = ultraframe >= 0 ? ultraframe : carried;
  p->position_from_isch = position >= 0;
  if(position >= 0){
    p->position_known = true;
    p->stats.position_isch++;
  } else if(predicted >= 0){
    position = predicted;
    p->stats.position_flywheel++;
  } else
    position = 0;
  p->last_position = position;
  return position;
}

// A timeslot's running voice position, five per superframe, or -1 when the superframe
// position is not known (the sixth burst of a timeslot in a superframe is its SACCH)
// The superframe is numbered by its first dibit's stream index over 2160, rounded. The stream
// index is not aligned to superframes, so this counts them one apart as long as slips do not move
// a superframe's start across a rounding point (1080 mod 2160). The only use is the ESS decode,
// which matches a 2V with the four 4Vs before it in this count. Those may be partly in the
// superframe before: voice bursts repeat four 4Vs and a 2V from the start of the transmission,
// wherever that falls among the voice positions (TIA-102.BBAC 6.1)
static int64_t voice_position(struct p25p2_burst const *b){
  if(!b->position_known)
    return -1;
  int const n = b->superframe_slot >> 1;
  if(n > 4)
    return -1;
  int64_t const start = (int64_t)b->dibit - 20 - 180 * b->superframe_slot;
  int64_t const superframe = (start + 1080) / 2160;
  return superframe * 5 + n;
}

// Decode one burst's contents and hand it up
static void deliver(struct p25p2 *p,struct slot *s){
  struct p25p2_burst * const b = &s->b;
  int const ts = b->timeslot;
  b->bits = s->bits;
  b->confidence = s->conf;
  b->nframes = 0;
  b->voice = NULL;
  b->ess = NULL;
  b->mac = NULL;
  bool const readable = b->descrambled || !p25p2_duid_scrambled(b->duid);
  if(!b->reparsed){
    if(b->duid >= 0)
      p->stats.bursts[ts][b->duid]++;
    else
      p->stats.unknown[ts]++;
    if(!readable)
      p->stats.undescrambled[ts]++;
  }
  if(readable && p25p2_duid_mac_chan(b->duid) >= 0){
    p25p2_mac_decode_burst(&p->mac,b->duid,s->bits,s->conf);
    b->mac = &p->mac;
    if(p->mac.crc_ok){
      p->stats.mac_ok[ts]++;
      // A new transmission (or none), or a new audio source: its ESS must not be assembled from
      // the last one's (see the top of the file)
      int const type = p->mac.bits[0] << 2 | p->mac.bits[1] << 1 | p->mac.bits[2];
      int const offset = p->mac.bits[3] << 2 | p->mac.bits[4] << 1 | p->mac.bits[5];
      int const framing = b->position_known ? p25p2_offset_2v(b->superframe_slot,offset) : -1;
      int cc = 0;
      for(int i=12; i < 24; i++)
	cc = cc << 1 | p->mac.bits[i];
      // Another system's END PTT ends nothing of ours
      bool const foreign = type == P25_MAC_END_PTT && b->color_code >= 0 && cc != b->color_code;
      if(!foreign && (type == P25_MAC_PTT || type == P25_MAC_END_PTT || type == P25_MAC_IDLE || type == P25_MAC_HANGTIME)){
	p25p2_ess_reset(&p->ess[ts]);
	p->framing[ts] = type == P25_MAC_PTT ? framing : -1;
      } else if(!foreign && framing >= 0){
	if(p->framing[ts] >= 0 && framing != p->framing[ts])
	  p25p2_ess_reset(&p->ess[ts]); // the audio source changed
	p->framing[ts] = framing;
      }
    } else
      p->stats.mac_bad[ts]++;
  } else if(readable && p25p2_duid_voice(b->duid)){
    int64_t const vi = voice_position(b);
    if(b->duid == P25P2_DUID_4V)
      p25p2_ess_add(&p->ess[ts],s->bits,vi);
    else {
      if(p25p2_ess_decode(&p->ess[ts],&p->ess_out,s->bits,vi))
	p->stats.ess_ok[ts]++;
      else
	p->stats.ess_bad[ts]++;
      b->ess = &p->ess_out;
    }
    b->nframes = p25p2_voice_decode(p->frames,b->duid,s->bits);
    b->voice = p->frames;
    for(int i=0; i < b->nframes; i++){
      p->stats.voice_frames[ts]++;
      p->stats.ambe_errors[ts] += (uint64_t)(p->frames[i].errors[0] + p->frames[i].errors[1]);
    }
  }
  if(p->handler != NULL)
    p->handler(p->handler_arg,b);
}

// The dibit offset (0, +-1, +-2) at which the fragment's syncs fit best, or NO_SYNC
static int best_offset(struct p25p2 const *p,uint64_t fs){
  static int const offsets[5] = { 0, 1, -1, 2, -2 };
  int best = NO_SYNC,best_score = 1000;
  for(int i=0; i < 5; i++){
    int const k = offsets[i];
    uint64_t const shifted = (uint64_t)((int64_t)fs + k);
    int const e1 = sync_errors_at(p,shifted + SYNC1,0);
    int const e2 = sync_errors_at(p,shifted + SYNC2,0);
    bool ok;
    if(k == 0)
      ok = e1 <= TRACK || e2 <= TRACK;
    else {
      int const threshold = (k == 1 || k == -1) ? SLIP1 : SLIP2;
      ok = e1 <= threshold && e2 <= threshold;
    }
    if(ok && e1 + e2 < best_score){ // ties keep the smaller offset
      best = k;
      best_score = e1 + e2;
    }
  }
  return best;
}

// One fragment while synced: both S-ISCHs where expected (or a dibit or two away: the timing
// loop can slip), the position from the I-ISCHs, then the four bursts. Timeslots A, B, C, D
// carry channels 0, 1, 0, 1, except in the superframe's last fragment, where C carries 1 and
// D carries 0; the scrambling segment follows the burst's place in the superframe
static void process_fragment(struct p25p2 *p){
  // The fragment acquisition found, and one whose syncs are both clean where expected, are
  // aligned and delivered at once; others wait for the dibits a +2 offset needs
  bool const aligned = p->just_acquired || (sync_errors_at(p,p->frag_start + SYNC1,0) <= CLEAN
					    && sync_errors_at(p,p->frag_start + SYNC2,0) <= CLEAN);
  if(!aligned && p->total < p->frag_start + FRAGMENT + MAX_SLIP)
    return;
  p->just_acquired = false;
  int const slip = aligned ? 0 : best_offset(p,p->frag_start);
  uint64_t fs = p->frag_start;
  bool const synced = slip != NO_SYNC;
  if(synced && slip != 0){
    fs = (uint64_t)((int64_t)fs + slip);
    p->stats.slips++;
  }
  int const sync_errors = sync_errors_at(p,fs + SYNC1,0) + sync_errors_at(p,fs + SYNC2,0);
  p->frag_start = fs + FRAGMENT; // the next fragment follows whether or not this one is usable
  if(!synced){
    // No sync where one must be: the carrier has gone or the timing has slipped. Drop the
    // fragment (noise decodes as plausible DUIDs)
    p->stats.missed_fragments++;
    if(++p->misses >= MISSES)
      lose_sync(p);
    else {
      if(p->last_position >= 0)
	p->last_position = (p->last_position + 1) % 3; // it still occupies a position
      if(p->last_position == 0 && p->ultraframe >= 0)
	p->ultraframe = (p->ultraframe + 1) % 4;       // and may begin a superframe
      trim_synced(p);
    }
    return;
  }
  p->misses = 0;
  p->stats.fragments++;
  int const position = fragment_position(p,fs);
  bool const last = position == 2;
  int const channel[4] = { 0, 1, last ? 1 : 0, last ? 0 : 1 };
  refresh_scrambling(p);
  p->delivering = true;
  for(int i=0; i < 4; i++){
    struct slot * const s = &p->slots[i];
    struct p25p2_burst * const b = &s->b;
    memset(b,0,sizeof *b);
    b->timeslot = channel[i];
    b->fragment = position;
    b->superframe_slot = position * 4 + i;
    b->position_from_isch = p->position_from_isch;
    b->position_known = p->position_known;
    b->isch = i < 2 ? p->isch_info[i] : -1;
    b->isch_errors = i < 2 ? p->isch_errors[i] : -1;
    b->ultraframe = p->ultraframe;
    b->color_code = p->scr_valid ? (int)(p->scr_seed & 0xfff) : -1;
    b->sync_errors = sync_errors;
    b->dibit = fs + Burst_offset[i];
    for(int d=0; d < P25P2_BURST_DIBITS; d++){
      struct softdibit const *sd = at(p,b->dibit + d);
      s->bits[2*d] = (sd->dibit >> 1) & 1;
      s->bits[2*d+1] = sd->dibit & 1;
      s->conf[d] = sd->confidence;
    }
    b->duid = p25p2_duid_decode(s->bits,s->conf,&b->duid_errors);
    if(p->scr_valid && p25p2_duid_scrambled(b->duid)){
      for(int k=0; k < P25P2_BURST_BITS; k++)
	s->bits[k] ^= p->seq[b->superframe_slot][k];
      b->descrambled = true;
    }
    deliver(p,s);
  }
  // Parameters that arrived during this fragment (a network status broadcast in one of its
  // unscrambled bursts): deliver again the scrambled bursts that went out undescrambled
  if(p->reparse){
    p->reparse = false;
    refresh_scrambling(p);
    if(p->scr_valid){
      for(int i=0; i < 4; i++){
	struct slot * const s = &p->slots[i];
	if(s->b.descrambled || !p25p2_duid_scrambled(s->b.duid))
	  continue;
	for(int k=0; k < P25P2_BURST_BITS; k++)
	  s->bits[k] ^= p->seq[s->b.superframe_slot][k];
	s->b.descrambled = true;
	s->b.reparsed = true;
	p->stats.reparsed++;
	deliver(p,s);
      }
    }
  }
  p->delivering = false;
  trim_synced(p);
}

// 'start' is signed: a sync pair in the first 540 dibits of the stream belongs to a fragment
// that began before it. One that began before the dibits held cannot be extracted: start at
// the next
static void acquire(struct p25p2 *p,int64_t start){
  while(start < (int64_t)p->buf_start)
    start += FRAGMENT;
  p->synced = true;
  p->frag_start = (uint64_t)start;
  p->misses = 0;
  p->last_position = -1;
  p->position_known = false;
  p->just_acquired = true;
  p->stats.acquisitions++;
  frame_sync(p,true);
  if(p->total >= p->frag_start + FRAGMENT)
    process_fragment(p);
}

// A sync pair with every dibit turned by the same quadrant: the frequency is off by +-1500 Hz
// (or 3000) and differential detection turns each symbol by 90 (180) degrees. Reported at most
// once a fragment, and only when the same rotation is found again one fragment later: single
// rotated pairs turn up in other signals now and then, a rotated TDMA carrier repeats every
// fragment
static void hunt_rotated(struct p25p2 *p,uint64_t hit){
  if(hit < p->buf_start + PAIR)
    return;
  if(p->last_rotation_report != NONE && hit < p->last_rotation_report + FRAGMENT)
    return;
  for(int q=1; q < 4; q++){
    if(sync_errors_at(p,hit,q) <= HUNT && sync_errors_at(p,hit - PAIR,q) <= HUNT_PARTNER){
      uint64_t const expected = p->rotation_candidate + FRAGMENT;
      if(p->rotation_candidate != NONE && q == p->rotation_q
	 && hit + ROTATION_TOLERANCE >= expected && hit <= expected + ROTATION_TOLERANCE){
	p->rotation_candidate = NONE;
	p->last_rotation_report = hit;
	rotated_sync(p,q);
      } else if(p->rotation_candidate == NONE || hit > expected + ROTATION_TOLERANCE){
	p->rotation_candidate = hit;
	p->rotation_q = q;
      }
      return;
    }
  }
}

// Acquisition: a sync is either the fragment's first S-ISCH or its second. Acquire only when
// the other is there too: 180 dibits earlier (the hit was the second), else 180 later (it was
// the first). Taking a lone sync as the first locks one timeslot late three times in four
static void hunt(struct p25p2 *p){
  if(p->pending_first != NONE && p->total >= p->pending_first + PAIR + SYNC_DIBITS){
    uint64_t const first = p->pending_first;
    p->pending_first = NONE;
    if(sync_errors_at(p,first + PAIR,0) <= HUNT_PARTNER){
      acquire(p,(int64_t)first - SYNC1);
      return;
    }
  }
  if(p->total < SYNC_DIBITS)
    return;
  uint64_t const hit = p->total - SYNC_DIBITS;
  int const e = sync_errors_at(p,hit,0);
  if(e > HUNT)
    hunt_rotated(p,hit);
  if(e <= HUNT){
    if(hit >= p->buf_start + PAIR && sync_errors_at(p,hit - PAIR,0) <= HUNT_PARTNER){
      p->pending_first = NONE;
      acquire(p,(int64_t)hit - SYNC2);
      return;
    }
    if(p->pending_first == NONE)
      p->pending_first = hit;
  }
  trim_search(p);
}

static void frame_dibit(struct p25p2 *p,struct softdibit const *sd){
  p->ring[p->total & (RING - 1)] = *sd;
  p->total++;
  p->stats.dibits++;
  if(!p->synced)
    hunt(p);
  else if(p->total >= p->frag_start + FRAGMENT)
    process_fragment(p);
}

// ---------------- receive chain ----------------

// Called by the timing loop for every interpolated sample: symbols and the midpoints between them
static void sync_output(void *arg,float complex y,bool is_symbol){
  struct p25p2 * const p = arg;
  if(!is_symbol)
    return;
  float complex const z = y * conjf(p->prev_symbol);
  p->prev_symbol = y;
  struct softdibit const sd = make_softdibit(z);
  // Residual frequency from the symbols: the fourth power of each differential symbol takes
  // out H-DQPSK's +-45 and +-135 degree steps (power weighted, unambiguous to +-750 Hz), or
  // the phase error against each decision (unweighted, the same span)
  double const re = crealf(z),im = cimagf(z);
  double const m2 = re * re + im * im;
  if(m2 > 0){
    double const r2 = re * re - im * im,i2 = 2 * re * im; // z^2
    p->dd_re -= (r2 * r2 - i2 * i2) / m2;
    p->dd_im -= 2 * r2 * i2 / m2;
  }
  p->dd_error += sd.phase - Nominal_phase[sd.dibit];
  p->dd_symbols++;
  if(p->ndibits < p->max_dibits)
    p->dibits[p->ndibits++] = sd;
}

// One block through the chain (see the top of the file). A seed can be given at any time, even
// from a burst handler running inside this function, so it waits for the start of the next block
static void run_block(struct p25p2 *p,float complex const *iq,int count){
  if(!p->primed){
    // From its default the level estimate would take a tenth of a second to reach a weak stream's level
    prime_level(&p->level,iq,count);
    p->primed = true;
  }
  if(p->seed_pending){
    p->seed_pending = false;
    seed_freqtrack(&p->afc,p->seed_value);
    p->rotation_pending = false;
    if(p->seed_hold && !p->afc_from_sync){
      // A held seed (a grant): the block estimate stays frozen until frame sync
      p->afc_held = true;
      p->last_sync_dibit = p->rx_dibits;
      set_freqtrack_corner(&p->afc,0);
    }
    p->seed_hold = false;
  }
  if(p->rotation_pending && p->rx_dibits >= p->rotation_deadline){
    p->rotation_pending = false;
    if(!p->frame_synced){
      seed_freqtrack(&p->afc,p->afc.estimate - p->rotation_hz);
      p->stats.rotation_reverts++;
    }
  }
  float complex * const buf = p->block;
  memcpy(buf,iq,(size_t)count * sizeof *buf);
  run_level(&p->level,buf,count);

  // Frequency: the residual is measured on a copy corrected by the current estimate and
  // lowpassed, the estimate updated, and this same block corrected by the update, so a
  // carrier's first timeslot is corrected too. The unfiltered copy gives the coherence (the
  // filter would make white noise look coherent) and a large residual the filter under-reads
  memcpy(p->copy,buf,(size_t)count * sizeof *buf);
  shift_freqtrack(&p->afc,p->copy,count,p->afc.estimate);
  measure_freqtrack(&p->afc,p->copy,count);
  run_rxfilter(&p->measure,p->copy,count);
  float residual = 0;
  bool const valid = freqtrack_residual(&p->afc,p->copy,count,&residual);
  update_freqtrack(&p->afc,valid,residual,count);
  shift_freqtrack(&p->shift,buf,count,p->afc.estimate);

  if(p->matched)
    run_rxfilter(&p->rrc,buf,count);

  p->ndibits = 0;
  p->dd_re = p->dd_im = 0;
  p->dd_error = 0;
  p->dd_symbols = 0;
  run_symsync(&p->sync,buf,count,sync_output,p);
  p->rx_dibits += (uint64_t)p->ndibits;

  // With frame sync the decisions move the estimate, from the next block
  if(p->frame_synced && p->dd_mode != P25P2_DD_OFF){
    p->last_sync_dibit = p->rx_dibits;
    double hz = 0;
    bool ok = false;
    if(p->dd_mode == P25P2_DD_FOURTH_POWER && (p->dd_re != 0 || p->dd_im != 0)){
      hz = atan2(p->dd_im,p->dd_re) * P25P2_SYMRATE / (8 * M_PI);
      ok = true;
    } else if(p->dd_mode == P25P2_DD_DECISIONS && p->dd_symbols > 0){
      hz = p->dd_error / p->dd_symbols * P25P2_SYMRATE / (2 * M_PI);
      ok = true;
    }
    if(ok){
      adjust_freqtrack(&p->afc,Dd_gain * (float)hz);
      p->stats.dd_updates++;
    }
  } else if(p->frame_synced){
    p->last_sync_dibit = p->rx_dibits;
  } else if(p->afc_held && p->rx_dibits - p->last_sync_dibit > p->hold_dibits){
    p->afc_held = false;
    p->afc_from_sync = false;
    set_freqtrack_corner(&p->afc,Afc_acquire_corner);
  }

  if(p->tap != NULL)
    for(int i=0; i < p->ndibits; i++)
      p->tap(p->tap_arg,&p->dibits[i]);
  for(int i=0; i < p->ndibits; i++)
    frame_dibit(p,&p->dibits[i]);

  // Optional second stage of the timing loop once frame sync has been held a while
  if(p->frame_synced && !p->settled && p->settled_bw != p->sync.track_bw
     && p->rx_dibits - p->synced_since >= p->settle_dibits){
    p->settled = true;
    set_symsync_bandwidth(&p->sync,p->settled_bw);
  }
}

struct p25p2 *create_p25p2(double samprate){
  if(samprate < 4 * P25P2_SYMRATE) // the loops want at least 4 samples per symbol
    return NULL;
  struct p25p2 * const p = calloc(1,sizeof *p);
  if(p == NULL)
    return NULL;
  p->samprate = samprate;
  p->prev_symbol = 1;
  p->matched = true;
  p->dd_mode = P25P2_DD_FOURTH_POWER;
  p->pending_first = NONE;
  p->last_position = -1;
  p->ultraframe = -1;
  p->framing[0] = p->framing[1] = -1;
  p->last_rotation_report = NONE;
  p->rotation_candidate = NONE;
  p->hold_dibits = (uint64_t)(Afc_hold_time * P25P2_SYMRATE);
  p->settled_bw = Track_bw;
  p->settle_dibits = (uint64_t)(Settle_time * P25P2_SYMRATE);
  p->max_block = (int)ceil(Max_block_time * samprate);
  // A block's symbols at the nominal rate, with a tenth and 16 more to spare
  p->max_dibits = (int)(p->max_block * 1.1 * P25P2_SYMRATE / samprate) + 16;
  p->block = calloc((size_t)p->max_block,sizeof *p->block);
  p->copy = calloc((size_t)p->max_block,sizeof *p->copy);
  p->dibits = calloc((size_t)p->max_dibits,sizeof *p->dibits);
  init_level(&p->level,samprate,Level_time);
  init_freqtrack(&p->afc,samprate,Afc_acquire_corner);
  init_freqtrack(&p->shift,samprate,0);
  int const measure_taps = (int)lrint(Measure_span * samprate) | 1;
  if(p->block == NULL || p->copy == NULL || p->dibits == NULL
     || init_rxfilter(&p->measure,samprate,Measure_cutoff,measure_taps,0) != 0
     || init_rrcfilter(&p->rrc,samprate,P25P2_SYMRATE,Rrc_alpha,Rrc_span) != 0
     || init_symsync(&p->sync,samprate,P25P2_SYMRATE,Interp_phases,Acquire_bw,Track_bw,Loop_damping,Error_gain,Rate_limit) != 0){
    delete_p25p2(p);
    return NULL;
  }
  return p;
}
void delete_p25p2(struct p25p2 *p){
  if(p == NULL)
    return;
  free_rxfilter(&p->measure);
  free_rxfilter(&p->rrc);
  free_symsync(&p->sync);
  free(p->block);
  free(p->copy);
  free(p->dibits);
  free(p);
}
void p25p2_set_burst_handler(struct p25p2 *p,p25p2_burst_handler handler,void *arg){
  p->handler = handler;
  p->handler_arg = arg;
}
void p25p2_set_dibit_tap(struct p25p2 *p,p25p2_dibit_tap tap,void *arg){
  p->tap = tap;
  p->tap_arg = arg;
}
void p25p2_samples(struct p25p2 *p,float complex const *iq,int count){
  assert(p != NULL);
  while(count > 0){
    int const n = count > p->max_block ? p->max_block : count;
    run_block(p,iq,n);
    iq += n;
    count -= n;
  }
}
void p25p2_dibit(struct p25p2 *p,struct softdibit const *sd){
  frame_dibit(p,sd);
}
void p25p2_seed_frequency(struct p25p2 *p,float offset,bool hold){
  if(hold && p->afc_from_sync)
    return; // frame sync on this carrier has given it its own estimate
  p->seed_pending = true;
  p->seed_value = offset;
  p->seed_hold = hold;
}
float p25p2_frequency_offset(struct p25p2 const *p){
  return p->afc.estimate;
}
void p25p2_set_decision_directed(struct p25p2 *p,enum p25p2_dd mode){
  p->dd_mode = mode;
}
void p25p2_set_loop_bandwidths(struct p25p2 *p,float acquire,float track,float settled,float settle_time){
  if(acquire >= 0)
    p->sync.acquire_bw = acquire;
  if(track >= 0){
    if(p->settled_bw == p->sync.track_bw && settled < 0)
      p->settled_bw = track; // still one stage
    p->sync.track_bw = track;
  }
  if(settled >= 0)
    p->settled_bw = settled;
  if(settle_time >= 0)
    p->settle_dibits = (uint64_t)(settle_time * P25P2_SYMRATE);
  if(acquire >= 0 && !p->sync.locked)
    set_symsync_bandwidth(&p->sync,p->sync.acquire_bw);
}
void p25p2_set_matched_filter(struct p25p2 *p,bool enable){
  p->matched = enable;
}
void p25p2_get_stats(struct p25p2 const *p,struct p25p2_stats *stats){
  *stats = p->stats;
  stats->locked = p->synced;
  stats->frequency_offset = p->afc.estimate;
  stats->timing_error = p->sync.error_ema;
}
