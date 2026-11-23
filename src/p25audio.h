// Voice output for the P25 decoder: an IMBE and AMBE+2 vocoder behind a small interface, and per
// channel two RTP streams, each optional: speech (8 kHz mono s16be) and the vocoder frames
// themselves, with the status packets radiod sends with its own streams
// Copyright 2026, Matthew Kaufman, KA6SQG
#ifndef _P25AUDIO_H
#define _P25AUDIO_H 1

#include <stdbool.h>
#include <stdint.h>

#include "p25p1.h"
#include "p25p2.h"

enum {
  P25_AUDIO_SAMPRATE = 8000,
  P25_AUDIO_FRAME = 160,   // samples per vocoder frame (20 ms)
};

// What p25_vocoder_decode() did with a frame
enum p25_voice {
  P25_VOICE_SPEECH = 0,    // synthesized from the frame
  P25_VOICE_REPEAT,        // the previous frame's parameters repeated (errors, or the frame was lost)
  P25_VOICE_MUTED,         // silence: error rate too high, or too many repeats in a row
};

// Voice codecs
enum p25_codec {
  P25_CODEC_IMBE = 0,      // Phase 1 full rate: 88 information bits per frame (TIA-102.BABA)
  P25_CODEC_AMBE2,         // Phase 2 half rate: 49 information bits per frame (TIA-102.BABA-1)
};

// Vocoder. IMBE: the frame repeat and muting rules of TIA-102.BABA are applied here, from the
// error counts the channel decoder supplies, and synthesis is mbelib's. AMBE+2: mbelib's
// mbe_processAmbe2450Dataf() with its own rules. Built in only with HAVE_MBELIB
struct p25_vocoder;
char const *p25_vocoder_name(enum p25_codec codec); // e.g. "mbelib 1.3.0"; NULL when not built in
struct p25_vocoder *p25_vocoder_create(enum p25_codec codec); // NULL when not built in
void p25_vocoder_reset(struct p25_vocoder *v); // start of a transmission
// One frame: its information bits, one per byte (IMBE: u0-u7, struct p25_imbe u[]; AMBE+2:
// struct p25p2_ambe bits[]), and two error counts (IMBE: e0 in c0 and the total in c0-c6;
// AMBE+2: mbelib's errs and errs2, struct p25p2_ambe data_errors[0] and their sum).
// bits == NULL marks a lost frame. Writes 160 samples at 8 kHz
enum p25_voice p25_vocoder_decode(struct p25_vocoder *v,int16_t pcm[P25_AUDIO_FRAME],uint8_t const *bits,int e0,int total);
void p25_vocoder_destroy(struct p25_vocoder *v);

// Vocoder frame stream (see docs/p25d.md for the receiver's view). One RTP packet per 20 ms frame,
// payload type P25_FRAME_PT, RTP clock 8 kHz; its status says encoding P25_FRAME_ENCODING, a value
// outside rtp.h's list, and carries no RTP_PT, so pcmrecord and monitor never treat it as PCM
enum {
  P25_FRAME_PT = 97,           // dynamic; nothing in rtp.c's static table, and not 96 (AX.25)
  P25_FRAME_VERSION = 1,
  P25_FRAME_CODEC_IMBE = 1,    // payload codec octet
  P25_FRAME_CODEC_AMBE2 = 2,
  P25_FRAME_LOST = 0x01,       // flags: no frame was received here (counts, bits zero)
  P25_FRAME_ENCRYPTED = 0x02,  // the frame is from a voice unit known to be encrypted
  P25_FRAME_CRYPTO_FRESH = 0x04, // the ALGID, KID and MI were received just before this superframe
  P25_FRAME_HEADER = 21,       // octets before the information bits
  P25_FRAME_IMBE_SIZE = P25_FRAME_HEADER + 11 + 18, // 50
  P25_FRAME_AMBE2_SIZE = P25_FRAME_HEADER + 7 + 9,  // 37
};
#define P25_FRAME_ENCODING UNUSED_ENCODING // rtp.h's sentinel: no tool built from this tree takes it as PCM

// Output: one multicast destination shared by every channel, one stream (SSRC) per channel
// Data goes to port 5004 and status to 5006 of the same group, from one socket, as radiod sends them
struct p25_audio_group;
// 'target' is "group[:port][,iface]"; a name that is not a literal address is hashed into
// 239.0.0.0/8 with make_maddr() and, if service_name != NULL, advertised with avahi_start() as
// an _rtp._udp service, as radiod does. ttl 0 keeps the stream on this host. NULL on error
struct p25_audio_group *p25_audio_open(char const *target,char const *iface,int ttl,int tos,char const *service_name);
// The same for vocoder frames. 'encrypted': also send the frames of voice units known to be
// encrypted, flagged so (never decoded here)
struct p25_audio_group *p25_frames_open(char const *target,char const *iface,int ttl,int tos,char const *service_name,bool encrypted);
void p25_audio_close(struct p25_audio_group *g);

struct p25_audio_stats {
  uint64_t spurts;         // speech transmissions started
  uint64_t frames;         // vocoder frames decoded to speech (including lost ones filled in)
  uint64_t repeats;        // of those, repeated
  uint64_t muted;          // of those, muted
  uint64_t lost;           // frames filled in for missing voice units (speech stream)
  uint64_t encrypted;      // voice units not decoded because the call is encrypted
  uint64_t units;          // voice units sent as clear
  uint64_t held;           // voice units held while their encryption was not known
  uint64_t discarded;      // held units never confirmed clear, not sent as clear
  uint64_t packets;        // speech RTP packets sent
  uint64_t status;         // status packets sent (both streams)
  uint64_t errors;         // send failures
  uint64_t frame_packets;  // vocoder frame packets sent, lost ones included
  uint64_t frame_encrypted; // of those, flagged encrypted
};

// A transmission's voice is decoded to speech (and sent as clear frames) once a word that passed
// error correction has said unencrypted in it (a header or encryption sync; Phase 2: a MAC PTT or
// an ESS; ALGID 0x80, or the all-zero LDU2 sync), and from then until the transmission ends,
// unless something says encrypted (a header, sync, PTT or ESS with another ALGID, or link control
// or a MAC voice channel user structure that is protected or has the service options' encryption
// bit), which holds for the rest of the transmission. On a channel with trunking logic that logic
// must also label the unit clear: at once (P25_AUDIO_CLEAR) or later (P25_AUDIO_HOLD, then
// p25_audio_release(a,true)).
// A transmission ends at a terminator (TDU, TDULC), a header or MAC PTT (which starts the next), a
// MAC END PTT, IDLE or HANGTIME, the trunking logic ending the call, a voice unit overdue by more
// than half a second, or link control or a MAC voice channel user word naming another talkgroup,
// unit or talker. A shorter fade does not end it: if one hid the end of a clear transmission and
// the start of an encrypted one, up to one superframe (18 frames, 360 ms) of the new one may be
// decoded before its own sync or ESS says encrypted.
// Units waiting for either are kept (their frames copied, at most P25_AUDIO_MAX_HELD_FRAMES) and,
// once both allow, sent in order with their original timestamps. Held units are discarded if the
// transmission ends first, or if too many wait (the oldest go); if the call turns out encrypted
// they are discarded too, or, on a frame group opened with 'encrypted', sent as encrypted frames.
enum { P25_AUDIO_MAX_HELD_FRAMES = 72, P25_AUDIO_MAX_HELD = 18 }; // 1.44 s: 8 Phase 1 voice units
enum p25_audio_label {
  P25_AUDIO_UNLABELLED = 0, // no trunking logic on this channel: only this module's own rule applies
  P25_AUDIO_CLEAR,          // the trunking logic says clear
  P25_AUDIO_HOLD,           // the trunking logic does not know yet; see p25_audio_release()
  P25_AUDIO_ENCRYPTED,      // the trunking logic says encrypted: not clear, nor any unit held before it
  P25_AUDIO_NONE,           // the trunking logic knows no call on this channel: not sent at all
};

// One channel's streams; used only by that channel's decode thread
struct p25_audio;
// Speech to 'pcm' (NULL: none; also none when built without a vocoder), frames to 'frames'
// (NULL: none). NULL when there is neither
struct p25_audio *p25_audio_create(struct p25_audio_group *pcm,struct p25_audio_group *frames,uint32_t ssrc);
void p25_audio_destroy(struct p25_audio *a); // ends a transmission in progress
// Times ('when') are the IQ sender's clock, ns since the GPS epoch (0: not known, use our own clock).
// A transmission's first RTP timestamp is that clock at 8 kHz, so a recording lines up with the
// IQ stream; within a transmission timestamps advance 160 per frame. Both streams of a channel
// carry the same timestamps, marker bits and SSRC for the same frames
// Once per block of samples fed to the receiver: the channel frequency (for the status), and the
// time at the block, to end a transmission that stopped without a terminator
void p25_audio_poll(struct p25_audio *a,double frequency,int64_t when);
// What radiod measured on the channel this speech came from: power in its filter and noise density
// (power ratios, as radiod's status gives them once decoded) and the filter's edges, Hz. They go
// out in the status as radiod sends them, so monitor shows the channel's signal to noise ratio
// for the speech stream. Not sent until given, or when either power is not a positive number
void p25_audio_set_signal(struct p25_audio *a,float bb_power,float n0,float low,float high);
// Phase 1: every data unit from the receiver's frame callback, 'when' being the time at its end,
// with the trunking logic's label for a voice unit (P25_AUDIO_UNLABELLED on a channel without it):
// HDU and LDUs start a transmission (marker bit) and carry the talkgroup, source and encryption;
// LDUs are sent under the rule above, lost ones (counted from frame->dibit) filled with lost
// frames (repeated, then muted, in speech); TDU and TDULC end the transmission
void p25_audio_unit(struct p25_audio *a,struct p25_frame const *frame,int64_t when,enum p25_audio_label label);
// The same, unlabelled
void p25_audio_frame(struct p25_audio *a,struct p25_frame const *frame,int64_t when);
// Phase 2: every burst of one timeslot (one struct p25_audio per timeslot), 'when' being the time
// at its end, with the label of a voice burst as above. A MAC PTT starts a transmission, a MAC END
// PTT, IDLE or HANGTIME ends it; MAC PTT, ESS and MAC voice channel user structures carry the call
// and encryption, and a voice channel user naming another talkgroup or talker starts a new
// transmission; 4V and 2V bursts are the voice units, placed in time by burst->dibit and
// superframe_slot
void p25_audio_burst(struct p25_audio *a,struct p25p2_burst const *burst,int64_t when,enum p25_audio_label label);
// The trunking logic decided about the units it labelled P25_AUDIO_HOLD: clear (they are sent
// once this module's own rule also allows) or not (treated as encrypted)
void p25_audio_release(struct p25_audio *a,bool clear);
// The trunking logic ended the call on this channel: units still held are discarded and the
// transmission ends
void p25_audio_call_end(struct p25_audio *a);
// The streams' SSRC and the description their status carries (NULL: built from the voice
// channel's own link control). A change of SSRC ends a transmission in progress
void p25_audio_set_stream(struct p25_audio *a,uint32_t ssrc,char const *description);
// Diagnostics: called with each voice unit's time and position (Phase 1: dibit; Phase 2: its
// first frame's index) just before it is sent as clear
void p25_audio_set_unit_tap(struct p25_audio *a,void (*tap)(void *arg,int64_t when,uint64_t dibit),void *arg);
// Lower level: send PCM for the current transmission, starting one if needed (frames of 160)
void p25_audio_pcm(struct p25_audio *a,int16_t const *pcm,int count,int64_t when);
void p25_audio_end(struct p25_audio *a);
void p25_audio_get_stats(struct p25_audio const *a,struct p25_audio_stats *stats);

#endif
