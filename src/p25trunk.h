// P25 trunking: which channel grants to follow, what a call is and when it ends, from a
// site's control channel messages and the voice channels being received (TIA-102.AABC, .AABD)
// Copyright 2026, Matthew Kaufman, KA6SQG

#ifndef _P25TRUNK_H
#define _P25TRUNK_H 1

#include <stdbool.h>
#include <stdint.h>

#include "p25msg.h"

// Nothing here does I/O, keeps a clock or allocates after create_p25_trunk(): the caller passes
// every message and the time (nanoseconds, any epoch, never decreasing), and is told what
// happened through one handler. One struct p25_trunk per control channel (site); it is not
// thread safe.
//
// How a caller uses it. Every control channel message goes to p25_trunk_control() (an LCCH's to
// p25_trunk_lcch()). A CALL_START with P25_TRUNK_F_FOLLOW asks for its voice channel; from then
// on each data unit received there goes in, word first, and the label returned says what to do
// with its voice frames: decode, do not, or hold until a CALL_UPDATE with P25_TRUNK_F_RELEASE
// says what the held ones were. A CALL_UPDATE without F_FOLLOW means stop receiving the call (it
// turned encrypted, say), and with it again that the call may now be received. CALL_END means
// the call is over and its channel no longer needed for it. Calls time out in any entry point,
// but control loss is noticed only in p25_trunk_tick(), and with no messages arriving only it
// ends calls: call it a few times a second. The times are the caller's to choose; p25d gives
// the sender's time of each sample, so the timeouts are judged on the signal's own time, however
// far behind it each decoding thread runs. The top of p25trunk.c describes a call's life and how
// its voice is labelled.

// Table sizes. When one is full:
enum {
  P25_TRUNK_MAX_CALLS = 64,      // a grant for a new call is refused (P25_TRUNK_NOT_TABLE_FULL)
  P25_TRUNK_MAX_PATCHES = 32,    // a new supergroup is not tracked
  P25_TRUNK_MAX_MEMBERS = 16,    // further member talkgroups of a supergroup are not recorded
  P25_TRUNK_MAX_SIGHTINGS = 32,  // further unconfirmed member names of a supergroup are dropped
  P25_TRUNK_MAX_ADJACENT = 64,   // further neighbour sites are not recorded
  P25_TRUNK_MAX_SECONDARY = 16,  // further secondary control channels are not recorded
  P25_TRUNK_HOLD_UNITS = 4,      // voice units held at most while a call's encryption is unknown
};

// Times, nanoseconds
#define P25_TRUNK_CALL_TIMEOUT (3000LL * 1000000)    // no voice and no grant: the call has ended
#define P25_TRUNK_CONTROL_LOSS (15LL * 1000000000)   // no control channel broadcast: control lost
#define P25_TRUNK_PATCH_FRESH (30LL * 1000000000)    // a patch neither granted nor added is over
#define P25_TRUNK_MEMBER_CONFIRM (30LL * 1000000000) // a member must be named twice within this
#define P25_TRUNK_MEMBER_STALE (300LL * 1000000000)  // a member the adds stop naming leaves

// Policy. Zero-initialised: follow every call that is not encrypted, data channels not asked for
struct p25_trunk_config {
  bool follow_encrypted;      // follow calls treated as encrypted
  bool follow_data;           // follow data channel grants as calls of type P25_TRUNK_DATA
  bool ignore_private;        // do not follow unit-to-unit and telephone calls
  int max_calls;              // most calls followed at once; 0: no limit but the table's
  uint16_t nac;               // control channel messages with another NAC are dropped; 0: any
  uint16_t const *allow;      // group calls on only these talkgroups are followed (copied)
  int nallow;                 // 0: every talkgroup
  uint16_t const *deny;       // group calls on these talkgroups are never followed (copied)
  int ndeny;
  // Quiet time, ns (0: off, the default). Systems go on naming a channel in grant updates for a
  // second or two after a call's last voice, reply or not, and an update carries no talker, so an
  // update alone can't tell a reply from that hang time; without this each one starts a call
  // with no voice. For this long after a call ends that was followed (and is received:
  // p25_trunk_set_heard()), an update naming the same call on the same channel starts no call; an
  // explicit grant, voice there, or an update naming another call still do. Only for a caller
  // that keeps receiving the channel at least this long after the call: a reply announced by
  // updates alone is then started from its own voice there. See begin_call() in p25trunk.c
  int64_t update_quiet;
};

enum p25_trunk_event_type {
  P25_TRUNK_CALL_START,   // a call: freq_hz (and slot) wanted now if P25_TRUNK_F_FOLLOW
  P25_TRUNK_CALL_UPDATE,  // still going on that channel (a grant), or its attributes changed;
                          // with P25_TRUNK_F_RELEASE: what to do with the voice units held
  P25_TRUNK_CALL_TALKER,  // the source of a call became known or changed
  P25_TRUNK_CALL_END,     // the call ended (end_reason): the channel may be released
  P25_TRUNK_SITE,         // site or system identity, or a neighbour site, learned or changed
  P25_TRUNK_CONTROL,      // a control channel of this site became known, or control was lost
  P25_TRUNK_PATCH,        // a patch or regroup changed
  P25_TRUNK_OTHER,        // emergency alarm, data grant and the like, for the log
};

enum p25_trunk_call_type {
  P25_TRUNK_GROUP,
  P25_TRUNK_UNIT,         // unit to unit: target is the called unit
  P25_TRUNK_TELEPHONE,    // telephone interconnect: target is the subscriber unit
  P25_TRUNK_DATA,         // a data channel (only with follow_data): talkgroup or target
};

enum p25_trunk_end {
  P25_TRUNK_END_NONE = 0,
  P25_TRUNK_END_TERMINATOR,  // TDU or TDULC after voice
  P25_TRUNK_END_TIMEOUT,     // no voice and no grant for P25_TRUNK_CALL_TIMEOUT
  P25_TRUNK_END_NEW_CALL,    // the channel was granted to another call, or a header named one
  P25_TRUNK_END_LC,          // the voice channel's link control named another call
};

enum p25_trunk_why {         // why a call is not followed
  P25_TRUNK_FOLLOWED = 0,
  P25_TRUNK_NOT_ENCRYPTED,
  P25_TRUNK_NOT_TALKGROUP,   // allow or deny list
  P25_TRUNK_NOT_PRIVATE,     // ignore_private
  P25_TRUNK_NOT_MAX_CALLS,
  P25_TRUNK_NOT_TABLE_FULL,  // CALL_START only, with call_id 0: the call is not tracked at all
};

// Event flags
#define P25_TRUNK_F_FOLLOW      0x0001  // the caller should receive this call
#define P25_TRUNK_F_EMERGENCY   0x0002
#define P25_TRUNK_F_ENCRYPTED   0x0004  // treated as encrypted (service options or encryption sync;
                                        // RELEASE: also a header or sync confirmed it and none
                                        // has said clear since, whatever the service options say)
#define P25_TRUNK_F_ENC_KNOWN   0x0008  // encryption confirmed by a header or encryption sync
#define P25_TRUNK_F_UNCONFIRMED 0x0010  // encrypted, not confirmed (held units): on the service
                                        // options alone, or on a header or sync they contradicted
#define P25_TRUNK_F_SVC_KNOWN   0x0020  // service options came from a message that carries them
#define P25_TRUNK_F_TDMA        0x0040  // a Phase 2 slot (slot >= 0)
#define P25_TRUNK_F_PATCHED     0x0080  // on a supergroup: members[] are the talkgroups it carries
#define P25_TRUNK_F_GRANT       0x0100  // UPDATE: a grant or grant update named the call
#define P25_TRUNK_F_NEW_TALKER  0x0200  // TALKER: another unit keyed up in the same call
#define P25_TRUNK_F_RELEASE     0x0400  // UPDATE: held_units voice units are released
#define P25_TRUNK_F_SEED        0x0800  // START: seed_offset_hz is the control channel's offset
#define P25_TRUNK_F_LOST        0x1000  // CONTROL: control lost (else acquired)
#define P25_TRUNK_F_SECONDARY   0x2000  // CONTROL: a secondary control channel (else primary)
#define P25_TRUNK_F_NETWORK     0x4000  // SITE: WACN and system (else RFSS and site)
#define P25_TRUNK_F_ADJACENT    0x8000  // SITE: a neighbour site
#define P25_TRUNK_F_JOIN        0x10000 // PATCH: talkgroup joined supergroup
#define P25_TRUNK_F_LEAVE       0x20000 // PATCH: talkgroup left supergroup
#define P25_TRUNK_F_PATCH_NEW   0x40000 // PATCH: supergroup first named
#define P25_TRUNK_F_PATCH_END   0x80000 // PATCH: supergroup ended
#define P25_TRUNK_F_SIMULSELECT 0x100000 // PATCH: L3Harris simulselect (one way)

struct p25_trunk_event {
  enum p25_trunk_event_type type;
  int64_t time;
  uint32_t call_id;          // calls: unique while this p25_trunk lives
  enum p25_trunk_call_type call_type;
  uint64_t freq_hz;          // calls: downlink; CONTROL and SITE: the control channel
  int slot;                  // TDMA slot, -1 FDMA
  struct p25_chan ch;        // as granted ({0,0} for a call found on a voice channel)
  uint16_t talkgroup;        // group calls; PATCH: the member; OTHER: the alarm's talkgroup
  uint32_t source;           // 0 until known
  uint32_t prev_source;      // TALKER
  uint32_t target;           // unit-to-unit, telephone, data
  uint32_t flags;            // P25_TRUNK_F_*
  enum p25_trunk_why why_not;
  uint8_t svc;               // service options
  uint8_t algid;             // 0x80 clear; 0 unknown, unless F_ENC_KNOWN: then as a header or
                             // sync sent it (0x00 is an encrypted algorithm there)
  uint16_t keyid;
  enum p25_trunk_end end_reason;
  int held_units;            // RELEASE: units released (they returned P25_TRUNK_VOICE_HOLD)
  float seed_offset_hz;      // START with P25_TRUNK_F_SEED
  uint16_t supergroup;       // patched calls and PATCH
  uint16_t members[P25_TRUNK_MAX_MEMBERS + 1]; // the patch's talkgroups (supergroup included
  int nmembers;              // for a patched call)
  uint32_t wacn;             // SITE, CONTROL
  uint16_t sysid;
  uint8_t rfss;
  uint8_t site;
  uint8_t svc_class;
  char text[200];            // one log line, no newline
};
typedef void (*p25_trunk_handler)(void *arg,struct p25_trunk_event const *event);

// What to do with one voice unit's frames (p25_trunk_voice_unit)
enum p25_trunk_voice {
  P25_TRUNK_VOICE_NONE = 0,  // no call on that channel: drop them
  P25_TRUNK_VOICE_CLEAR,     // decode
  P25_TRUNK_VOICE_ENCRYPTED, // encrypted: do not decode
  P25_TRUNK_VOICE_HOLD,      // encryption not known yet: keep them until a RELEASE for the call.
                             // A released unit is encrypted if its own label said so or the
                             // release has P25_TRUNK_F_ENCRYPTED; unconfirmed unless F_ENC_KNOWN.
                             // A call a header or encryption sync confirmed encrypted is not
                             // labelled CLEAR, nor its units released clear, on the service
                             // options alone: only once a header or sync says clear
};
// How a voice unit's frames are labelled: identity, and encryption as decided
struct p25_trunk_label {
  enum p25_trunk_voice decision;
  uint32_t call_id;
  enum p25_trunk_call_type call_type;
  uint32_t destination;      // talkgroup, or the unit of a unit-to-unit or telephone call
  uint32_t source;
  uint8_t svc;
  bool emergency;
  bool encrypted;
  bool unconfirmed;          // encrypted, not confirmed: on the service options alone, or on a
                             // header or sync they contradicted
  bool carried;              // the link control did not decode: identity carried over
  uint8_t algid;
  uint16_t keyid;
  uint16_t members[P25_TRUNK_MAX_MEMBERS + 1]; // a patched group call's talkgroups
  int nmembers;
};

// A call as it stands (p25_trunk_calls)
struct p25_trunk_call {
  uint32_t id;
  enum p25_trunk_call_type type;
  uint64_t freq_hz;
  int slot;
  struct p25_chan ch;
  uint16_t talkgroup;
  uint32_t source;
  uint32_t target;
  uint8_t svc;
  bool svc_known;
  bool emergency;
  bool encrypted;            // the service options' or link control's encryption bit
  bool enc_confirmed;
  uint8_t algid;
  uint16_t keyid;
  bool pending_ess;          // an encryption sync contradicting the call waits for a repeat (or
                             // a confirmed encryption the service options contradicted)
  bool had_voice;
  int held_units;
  bool followed;
  enum p25_trunk_why why_not;
  bool patched;
  uint16_t supergroup;
  uint16_t members[P25_TRUNK_MAX_MEMBERS + 1];
  int nmembers;
  int64_t started;
  int64_t last_activity;     // last voice, or start
  int64_t last_grant;        // INT64_MIN if never granted
};

// A patch (supergroup) as it stands (p25_trunk_patches)
struct p25_trunk_patch {
  uint16_t supergroup;
  uint16_t members[P25_TRUNK_MAX_MEMBERS]; // confirmed members, ascending
  int nmembers;
  bool harris;               // an L3Harris regroup
  bool simulselect;
  uint8_t sequence;
};

// The site as broadcast
struct p25_trunk_adjacent {
  uint16_t sysid;
  uint8_t rfss;
  uint8_t site;
  struct p25_chan ch;
  uint8_t svc_class;
};
struct p25_trunk_site {
  bool known;                // RFSS status seen
  uint16_t sysid;
  uint8_t rfss;
  uint8_t site;
  uint64_t control_hz;       // the control channel the RFSS status names (0: band not known)
  bool network_known;        // network status seen
  uint32_t wacn;
  uint16_t net_sysid;
  bool control_up;           // control channel broadcasts are arriving
  bool control_lost;         // and were lost (P25_TRUNK_CONTROL_LOSS)
  int64_t last_control;
  int64_t last_message;      // any message, any channel
  int nsecondary;
  uint64_t secondary_hz[P25_TRUNK_MAX_SECONDARY];
  int nadjacent;
  struct p25_trunk_adjacent adjacent[P25_TRUNK_MAX_ADJACENT];
};

struct p25_trunk;

struct p25_trunk *create_p25_trunk(struct p25_trunk_config const *config,p25_trunk_handler handler,void *arg);
void delete_p25_trunk(struct p25_trunk *t);

// A decoded message from the control channel (a TSBK or multi-block trunking message).
// Trunking messages that turn up on a voice channel go here too, and so do a Phase 2 voice
// channel's MAC structures (P25_FAMILY_MAC: grants and updates as the TSBKs they abbreviate).
void p25_trunk_control(struct p25_trunk *t,struct p25_msg const *msg,uint16_t nac,int64_t now);
// A MAC structure from a Phase 2 control channel's LCCH (P25_FAMILY_MAC): as p25_trunk_control(),
// and it counts as control channel activity as a control channel's TSBK does
void p25_trunk_lcch(struct p25_trunk *t,struct p25_msg const *msg,uint16_t nac,int64_t now);

// From a voice channel the caller is receiving, identified by its frequency and slot (-1 FDMA),
// as given in CALL_START. For each data unit, first the word it carried if that decoded (LDU1
// or TDULC link control, LDU2 encryption sync, HDU header) ...
void p25_trunk_voice_msg(struct p25_trunk *t,uint64_t freq_hz,int slot,struct p25_msg const *msg,int64_t now);
// ... or the same with the Reed-Solomon erasure count of its decode (inner codewords that could
// not be corrected; p25_trunk_voice_msg means 0). A link control with erasures may not end a call.
void p25_trunk_voice_word(struct p25_trunk *t,uint64_t freq_hz,int slot,struct p25_msg const *msg,int erasures,
                          int64_t now);
// ... then the data unit itself (P25_DUID_LDU1, _LDU2, _TDU, _TDULC, _HDU; _PDU for a data call),
// so silence and terminators are known. Returns what to do with an LDU's voice frames
// (enum p25_trunk_voice); p25_trunk_label() then describes them
int p25_trunk_voice_unit(struct p25_trunk *t,uint64_t freq_hz,int slot,int duid,int64_t now);
struct p25_trunk_label const *p25_trunk_label(struct p25_trunk const *t);

// Call regularly (a few times a second) so calls time out and control loss is noticed
void p25_trunk_tick(struct p25_trunk *t,int64_t now);

// The control channel's frequency as the caller receives it (voice there means the control
// channel has moved) and its carrier offset, seeded into newly granted voice channels
void p25_trunk_set_control(struct p25_trunk *t,uint64_t freq_hz,float offset_hz);
// A Phase 2 control channel whose LCCH is on one timeslot (0 or 1) of its carrier: voice on the
// other slot is traffic, not a sign that control has moved. -1 (the default): FDMA, or LCCH on both
void p25_trunk_set_control_slot(struct p25_trunk *t,int slot);
// Whether the caller receives a call the logic follows (default yes). One it doesn't (a limit or
// a refusal of its own) leaves nothing listening on the channel, so the quiet time does not apply
// after it (update_quiet)
void p25_trunk_set_heard(struct p25_trunk *t,uint32_t call_id,bool heard);

// The band plan this site has broadcast (for the caller's log lines)
struct p25_bandplan const *p25_trunk_bandplan(struct p25_trunk const *t);
struct p25_trunk_site const *p25_trunk_site(struct p25_trunk const *t);
// Copy up to max calls (patches) as they stand; returns how many there are
int p25_trunk_calls(struct p25_trunk const *t,struct p25_trunk_call *out,int max);
int p25_trunk_patches(struct p25_trunk const *t,struct p25_trunk_patch *out,int max);

char const *p25_trunk_event_name(enum p25_trunk_event_type type);
char const *p25_trunk_end_name(enum p25_trunk_end reason);
char const *p25_trunk_why_name(enum p25_trunk_why why);

#endif
