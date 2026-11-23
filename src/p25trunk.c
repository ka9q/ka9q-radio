// P25 trunking: which channel grants to follow, what a call is and when it ends, from a
// site's control channel messages and the voice channels being received (TIA-102.AABC, .AABD)
// Copyright 2026, Matthew Kaufman, KA6SQG

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "p25p1.h"
#include "p25trunk.h"

// Theory of operation
//
// The model. One site's calls, built from what its control channel broadcasts and what the voice
// channels being received carry. Nothing here does I/O or keeps a clock (p25trunk.h). Every entry
// point does the same three things: first the calls that have timed out end (begin(), expire());
// then the message is taken in; then finish() reports what changed. A call started by a message
// is reported only at the end, once the whole message has been taken in, so that its START has
// every field the message gave and its follow decision is made on the call as it then stands.
//
// A call. One call on one voice channel: a frequency and, on a TDMA carrier, a timeslot (struct
// key). A channel carries one call at a time, so the table holds at most one per channel
// (find_call()). A call has its identity (type; talkgroup, or the called unit or subscriber; the
// talker once known), its service options, its encryption as far as it is known, and its patch
// if it is on a supergroup. Its id is a counter, unique while the struct p25_trunk lives, and is
// how the caller keeps track of it. The comments below name the steps of its life: START, a
// grant or update refreshing it, CONTINUE (each voice unit), and END.
//
// START (begin_call()). A grant or grant update for a channel with no call on it; voice on a
// channel with no call, whose link control decodes and names one (late entry: the grant was
// missed, or the voice is on a channel the caller was already receiving); or a header naming a
// talkgroup other than the channel's call's (a header always starts a group call). A grant or
// update naming the call already on the channel only refreshes it, and a grant naming another
// talker there is a new talker in the same call. One naming another call ends the call on the
// channel first (P25_TRUNK_END_NEW_CALL): the system has given the channel away. After a call the
// caller received ends, the quiet time may keep an update from starting one (see begin_call()).
//
// Refreshes. A grant or grant update naming the call (last_grant); each voice unit
// (last_activity); packet data, for a data call. A header, a terminator or a link control alone
// is not activity: only the voice behind it is.
//
// END. A terminator (TDU, TDULC; a Phase 2 END PTT) once the call has had voice: one before any
// voice belongs to the transmission before, which the system may still be ending on the channel
// when the next grant comes. A grant, update or header naming another call on the channel. A link
// control decoded without erasures naming another call: a terminator was missed, or the grant
// was taken wrongly. Or P25_TRUNK_CALL_TIMEOUT (3 s) with neither voice nor a grant (expire()):
// while a call goes on, the system keeps naming its channel in grant updates, so the call is held
// through a fade or a few lost updates. An ended call is gone: the next call on its channel
// starts from nothing (begin_call() clears the entry), so nothing of one call's encryption or
// identity carries into the next.
//
// Phase 2. The caller gives a TDMA timeslot's signalling in Phase 1's terms (p25d.c): a MAC PTT
// as a header, a MAC END PTT as a TDU, a voice channel user structure ahead of a voice burst as
// an LDU1's link control, a 2V burst's ESS as an LDU2's encryption sync, and any other voice
// burst as an LDU2 without one. Grants and updates in MAC structures come as the TSBKs they
// abbreviate. So one set of rules serves both phases, and each timeslot is a channel of its own.
//
// Encryption. Three sources, in rising weight: the service options' encryption bit (in grants
// and channel user link control), the encryption sync (LDU2, ESS) and the header (HDU, PTT). Any
// ALGID but 0x80 counts as encrypted, 0x00 included; an all-zero Phase 1 encryption sync is
// already given as 0x80 by its decoder (p25p1msg.c), so it is clear here. The header opens the
// transmission and is Golay and Reed-Solomon protected, so it confirms the encryption
// (c->confirmed), whatever it says. A sync confirms when it agrees with the service options or
// repeats the sync before it (with no service options known, any sync agrees); one that
// contradicts them waits (pending_ess) for a repeat, so a single miscorrected sync cannot turn
// the label of a call whose service options are known either way (accept_sync()). Service options
// that contradict a confirmation withdraw it (note_svc()), since a talker may key up encrypted in
// a clear call, or the reverse; but one bit cannot make a call confirmed encrypted known clear:
// it is marked contradicted and waits for a header or sync to say clear.
//
// Labels (voice_frames()). Each voice unit is labelled for the caller CLEAR, ENCRYPTED, or HOLD
// where the encryption is not known well enough to say: not confirmed, and the service options
// say encrypted or are not known, or a protected link control was seen, or a confirmation was
// contradicted. The caller keeps held units until a RELEASE says what they were (release()):
// when a header or sync confirms, when the service options say clear and nothing confirmed says
// otherwise, when another talker keys up, when the call ends, or after P25_TRUNK_HOLD_UNITS
// units, by what is known then (encrypted if the service options say so or a confirmation was
// contradicted; otherwise clear, even with the service options not known). Holding costs only
// delay, while a unit wrongly labelled clear would be decoded as speech, so doubt holds. On
// Phase 1 four units are two LDU2s, two chances at an encryption sync; on Phase 2, where a 2V
// comes after every four 4V bursts, four bursts may hold none. This label is one of two gates:
// the voice output (p25audio.c) decodes a transmission only once its own header or sync has said
// unencrypted, and holds units itself until then, so a unit labelled clear here on the service
// options alone is still not decoded before that. A new talker does not withdraw a confirmation
// here; the voice output starts a new transmission for one, which waits for its own word.
//
// Who the call is (voice_ldu1()). A link control that needed erasures to decode may name a wrong
// talkgroup or unit and still pass its Reed-Solomon code, so only one decoded without erasures
// may end the call or name a new talker. Even then a link control does not replace the grant's
// talkgroup in a call it names (through a patch, say): the grant came through a CRC. A group
// link control naming talkgroup 0 or 0xff00 and up (taken here as no one and as supergroups)
// never ends a call.
//
// Patches. See the section below: the calls of a supergroup carry its members' talkgroups, and
// the allow list is matched against all of them.
//
// Site and control channel. RFSS, network and adjacent site status, secondary control channels
// and the band plan are kept as broadcast; a grant is turned into a frequency through the band
// plan, so one on a band not yet announced is only logged. Control is up once a message only a
// control channel sends arrives, and lost after P25_TRUNK_CONTROL_LOSS (15 s) without one
// (p25_trunk_tick()), or at once when an LDU1 or TDULC is heard on the control channel's
// frequency: control has moved, and that frequency is now a voice channel.
//
// Following (decide()). Made again for every call at the end of every entry point, so it tracks
// the call as it changes: the talkgroup lists, private calls, encryption (the service options'
// bit or a confirmed encryption), the limit on calls followed at once. A call already followed is
// not dropped for the limit, so a later call cannot take the channel from one under way. Unless
// encrypted calls are followed, a followed call that turns encrypted is no longer followed, and
// one that turns clear again is followed again; the caller hears of either as a CALL_UPDATE.
//
// Events. CALL_START when a call starts, followed or not (with F_FOLLOW if it should be
// received); CALL_UPDATE when the attributes reported or the decision change (signature()), when
// a grant refreshes the call (F_GRANT), and when held units are released (F_RELEASE);
// CALL_TALKER when the talker becomes known or changes; CALL_END with its reason. SITE, CONTROL,
// PATCH and OTHER are for the log.

#define ALG_UNKNOWN 0x00   // ALGID not known yet (a grant said encrypted)
#define ALG_CLEAR 0x80     // ALGID of an unencrypted call
#define FAR_PAST INT64_MIN

// How a call got onto its channel: what may refresh it, and whether its service options count
enum origin { ORIGIN_GRANT, ORIGIN_UPDATE, ORIGIN_LATE, ORIGIN_HDU };
// The message that granted it
enum grant_kind {
  KIND_GROUP, KIND_GROUP_UPDATE, KIND_GROUP_UPDATE_EXP, KIND_UU, KIND_UU_UPDATE, KIND_TEL,
  KIND_TEL_UPDATE, KIND_PATCH, KIND_PATCH_UPDATE, KIND_MBT, KIND_DATA, KIND_LATE,
};

struct call {
  bool used;
  uint32_t id;
  uint64_t freq;
  int slot;                  // -1 FDMA
  struct p25_chan ch;
  enum p25_trunk_call_type type;
  uint16_t tg;               // group calls (0 for unit-to-unit and telephone)
  uint32_t src;
  uint32_t unit;             // unit-to-unit callee, telephone subscriber, data target
  bool encrypted;            // the service options' (or link control's) encryption bit
  bool emergency;
  uint8_t prio;
  uint8_t svc;
  uint8_t alg;
  uint16_t kid;
  enum origin origin;
  int64_t started;
  int64_t last_activity;
  int64_t last_grant;
  bool had_voice;
  // Encryption: confirmed by a header or an accepted encryption sync
  bool confirmed;
  bool svc_known;            // the service options came from a message that carries them
  bool pending_ess;
  uint8_t pend_alg;
  uint16_t pend_kid;
  bool contradicted;         // confirmed encrypted, then service options said clear: not known clear
  bool protected_seen;       // a protected link control came before the encryption was confirmed
  bool carried;              // the last LDU1's link control did not decode (or was protected)
  int held;                  // voice units held while the encryption is not known
  // Patch: the talkgroups the call carries, as the patch stood when the call became patched
  bool patched;
  uint16_t patch_sg;
  uint16_t files[P25_TRUNK_MAX_MEMBERS + 1];
  int nfiles;
  bool members_known;
  // Reporting
  bool start_pending;
  bool refreshed;            // a grant named it during this call into the library
  bool followed;
  bool unheard;              // followed, but the caller does not receive it (p25_trunk_set_heard())
  enum p25_trunk_why why;
  uint8_t sig[64];           // attributes last reported
};

struct sighting {
  uint16_t g;
  int count;
  int64_t last;
};

// A supergroup. Its parts have their own lifetimes: the membership (in_map), the last grant or
// add (has_update), the last add (has_add), the L3Harris regroup, the members named but not
// confirmed (sightings). The slot is free when none is left.
struct patch {
  bool used;
  uint16_t sg;
  bool in_map;
  uint16_t members[P25_TRUNK_MAX_MEMBERS]; // ascending
  int nmembers;
  bool has_update;
  int64_t last_update;
  bool has_add;
  int64_t last_add;
  bool harris;
  uint8_t seq;
  bool simulselect;
  struct sighting sight[P25_TRUNK_MAX_SIGHTINGS];
  int nsight;
};

struct p25_trunk {
  struct p25_trunk_config config;
  p25_trunk_handler handler;
  void *arg;
  struct p25_bandplan plan;
  struct call calls[P25_TRUNK_MAX_CALLS];
  uint32_t next_id;
  struct patch patches[P25_TRUNK_MAX_PATCHES];
  struct p25_trunk_site site;
  uint64_t control_freq;
  float control_offset;
  int control_slot;          // the control channel's timeslot on a TDMA carrier (LCCH), -1 FDMA or both
  // The word of the voice unit about to be passed
  bool pending;
  uint64_t pend_freq;
  int pend_slot;
  int pend_erasures;
  struct p25_msg pend_msg;
  struct p25_trunk_label label;
  int64_t now;
  struct call ended[P25_TRUNK_MAX_CALLS]; // calls ended lately, for config.update_quiet
  int64_t ended_at[P25_TRUNK_MAX_CALLS];
  int next_ended;
};

// Link control data: who a voice unit belongs to
struct lcd {
  enum p25_trunk_call_type type;
  uint16_t dest;             // talkgroup of a group call
  uint32_t unit;
  uint32_t src;
  bool enc;
  bool emerg;
  uint8_t prio;
  uint8_t svc;
};

// Log lines
struct out {
  char *p;
  size_t left;
};
static void put(struct out *o, char const *fmt, ...){
  if(o->left == 0)
    return;
  va_list ap;
  va_start(ap, fmt);
  int k = vsnprintf(o->p, o->left, fmt, ap);
  va_end(ap);
  if(k < 0)
    return;
  size_t const used = (size_t)k < o->left ? (size_t)k : o->left - 1;
  o->p += used;
  o->left -= used;
}
static void put_mhz(struct out *o, char const *key, uint64_t hz){
  put(o, " %s=%llu.%06llu", key, (unsigned long long)(hz / 1000000), (unsigned long long)(hz % 1000000));
}

static char const *const Type_names[] = { "group", "unit", "telephone", "data" };

static void emit(struct p25_trunk *t, struct p25_trunk_event *e){
  e->time = t->now;
  if(t->handler)
    t->handler(t->arg, e);
}

// Encrypted as a header or encryption sync confirmed it: any ALGID but the unencrypted one.
// 0x00 received in a header or sync is an algorithm like any other (ALG_UNKNOWN stands for
// "not known yet" only in a call nothing has confirmed)
static bool confirmed_encrypted(struct call const *c){
  return c->confirmed && c->alg != ALG_CLEAR;
}

static bool in_list(uint16_t const *list, int n, uint16_t g){
  for(int i = 0; i < n; i++)
    if(list[i] == g)
      return true;
  return false;
}

static int followed_count(struct p25_trunk const *t, struct call const *except){
  int n = 0;
  for(int i = 0; i < P25_TRUNK_MAX_CALLS; i++)
    if(t->calls[i].used && &t->calls[i] != except && t->calls[i].followed && !t->calls[i].start_pending)
      n++;
  return n;
}

// The caller's policy for one call. Only a call not yet followed is held to the limit, so one
// under way keeps its place. The allow list is matched against any talkgroup of a patched call,
// the deny list only against the call's own
static enum p25_trunk_why decide(struct p25_trunk const *t, struct call const *c){
  struct p25_trunk_config const *cf = &t->config;
  if(c->type == P25_TRUNK_GROUP){
    bool allowed = cf->nallow == 0 || in_list(cf->allow, cf->nallow, c->tg);
    // A patched call carries its members' traffic: any allowed member will do
    for(int i = 0; !allowed && c->patched && i < c->nfiles; i++)
      allowed = in_list(cf->allow, cf->nallow, c->files[i]);
    if(!allowed || in_list(cf->deny, cf->ndeny, c->tg))
      return P25_TRUNK_NOT_TALKGROUP;
  } else if(c->type != P25_TRUNK_DATA && cf->ignore_private)
    return P25_TRUNK_NOT_PRIVATE;
  if((c->encrypted || confirmed_encrypted(c)) && !cf->follow_encrypted)
    return P25_TRUNK_NOT_ENCRYPTED;
  if(cf->max_calls > 0 && !c->followed && followed_count(t, c) >= cf->max_calls)
    return P25_TRUNK_NOT_MAX_CALLS;
  return P25_TRUNK_FOLLOWED;
}

static uint32_t call_flags(struct call const *c){
  uint32_t f = 0;
  if(c->followed)
    f |= P25_TRUNK_F_FOLLOW;
  if(c->emergency)
    f |= P25_TRUNK_F_EMERGENCY;
  if(c->encrypted || confirmed_encrypted(c))
    f |= P25_TRUNK_F_ENCRYPTED;
  if(c->confirmed)
    f |= P25_TRUNK_F_ENC_KNOWN;
  if(c->svc_known)
    f |= P25_TRUNK_F_SVC_KNOWN;
  if(c->slot >= 0)
    f |= P25_TRUNK_F_TDMA;
  if(c->patched)
    f |= P25_TRUNK_F_PATCHED;
  return f;
}

// The attributes an UPDATE reports, packed for comparison
static void signature(struct call const *c, uint8_t sig[64]){
  memset(sig, 0, 64);
  struct out o = { (char *)sig, 64 };
  put(&o, "%d %u %u %x %x %u %u %u %u %d %d", c->type, c->tg, c->unit, c->svc, call_flags(c),
      c->alg, c->kid, c->patch_sg, c->nfiles, c->why, c->members_known);
  for(int i = 0; i < c->nfiles; i++)
    put(&o, ",%u", c->files[i]);
}

static void put_call(struct out *o, struct call const *c){
  put(o, " call=%u %s", c->id, Type_names[c->type]);
  if(c->type == P25_TRUNK_GROUP || (c->type == P25_TRUNK_DATA && c->tg))
    put(o, " tg=%u", c->tg);
  else
    put(o, " tgt=%u", c->unit);
  put(o, " src=%u", c->src);
  put_mhz(o, "freq", c->freq);
  if(c->slot >= 0)
    put(o, " slot=%d", c->slot);
  put(o, " svc=0x%02x", c->svc);
  if(c->emergency)
    put(o, " emerg=1");
  if(c->encrypted || confirmed_encrypted(c))
    put(o, " enc=1");
  if(c->confirmed)
    put(o, " alg=0x%02x kid=0x%04x", c->alg, c->kid);
  if(c->patched){
    put(o, " sg=%u members=", c->patch_sg);
    for(int i = 0; i < c->nfiles; i++)
      put(o, "%s%u", i ? "," : "", c->files[i]);
  }
  if(c->followed)
    put(o, " follow");
  else
    put(o, " skip=%s", p25_trunk_why_name(c->why));
}

static void call_event(struct p25_trunk *t, struct call *c, struct p25_trunk_event *e, enum p25_trunk_event_type type);

// Report a call that has started, now that the message that started it has been taken in full
static void flush_start(struct p25_trunk *t, struct call *c){
  if(!c->start_pending)
    return;
  c->start_pending = false;
  c->why = decide(t, c);
  c->followed = c->why == P25_TRUNK_FOLLOWED;
  signature(c, c->sig);
  struct p25_trunk_event e;
  call_event(t, c, &e, P25_TRUNK_CALL_START);
  // A new voice channel's carrier is near the control channel's (same transmitter site)
  if(t->site.control_up && c->freq != t->control_freq){
    e.flags |= P25_TRUNK_F_SEED;
    e.seed_offset_hz = t->control_offset;
  }
  emit(t, &e);
}

static void call_event(struct p25_trunk *t, struct call *c, struct p25_trunk_event *e, enum p25_trunk_event_type type){
  if(type != P25_TRUNK_CALL_START)
    flush_start(t, c);
  memset(e, 0, sizeof *e);
  e->type = type;
  e->call_id = c->id;
  e->call_type = c->type;
  e->freq_hz = c->freq;
  e->slot = c->slot;
  e->ch = c->ch;
  e->talkgroup = c->tg;
  e->source = c->src;
  e->target = c->unit;
  e->flags = call_flags(c);
  e->why_not = c->why;
  e->svc = c->svc;
  e->algid = c->alg;
  e->keyid = c->kid;
  if(c->patched){
    e->supergroup = c->patch_sg;
    e->nmembers = c->nfiles;
    memcpy(e->members, c->files, sizeof c->files);
  }
  struct out o = { e->text, sizeof e->text };
  put(&o, "%s", type == P25_TRUNK_CALL_START ? "start" : type == P25_TRUNK_CALL_UPDATE ? "update"
      : type == P25_TRUNK_CALL_TALKER ? "talker" : "end");
  put_call(&o, c);
}

// At the end of each call into the library: report calls started, and calls whose attributes
// or follow decision changed or that a grant refreshed
static void finish(struct p25_trunk *t){
  for(int i = 0; i < P25_TRUNK_MAX_CALLS; i++){
    struct call *c = &t->calls[i];
    if(!c->used)
      continue;
    if(c->start_pending){
      flush_start(t, c);
      c->refreshed = false;
      continue;
    }
    c->why = decide(t, c);
    c->followed = c->why == P25_TRUNK_FOLLOWED;
    uint8_t sig[64];
    signature(c, sig);
    bool const changed = memcmp(sig, c->sig, sizeof sig) != 0;
    if(changed || c->refreshed){
      memcpy(c->sig, sig, sizeof sig);
      struct p25_trunk_event e;
      call_event(t, c, &e, P25_TRUNK_CALL_UPDATE);
      if(c->refreshed){
        e.flags |= P25_TRUNK_F_GRANT;
        size_t n = strlen(e.text);
        snprintf(e.text + n, sizeof e.text - n, " grant");
      }
      emit(t, &e);
    }
    c->refreshed = false;
  }
}

static void other(struct p25_trunk *t, char const *fmt, ...){
  struct p25_trunk_event e;
  memset(&e, 0, sizeof e);
  e.type = P25_TRUNK_OTHER;
  e.slot = -1;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(e.text, sizeof e.text, fmt, ap);
  va_end(ap);
  emit(t, &e);
}

// ---- Patches and regroups ----
// A Motorola supergroup (group regroup) or L3Harris regroup carries the calls of its member
// talkgroups. Members are listed by regroup add commands (TSBK, link control) and leave by
// deletes; grants and voice name the supergroup.
//
// What a patch does for a call. A call on a supergroup is marked patched (mark_patched()) and
// takes a snapshot of the patch's talkgroups, the supergroup among them (files[]): the allow list
// is matched against all of them (decide()), a grant or link control naming any of them names
// the same call (same_call()), and they are reported with the call and in its voice labels. The
// snapshot is taken again only while the members are not yet known.
//
// Lifetimes. A talkgroup joins once adds have named it twice within P25_TRUNK_MEMBER_CONFIRM
// (30 s; patch_add()). A member leaves when a delete names it, or when adds that go on naming the
// patch have not named it for P25_TRUNK_MEMBER_STALE (300 s; patch_prune()); a patch that is no
// longer announced at all keeps its members. The whole patch is over on a delete naming no
// member or leaving it with none, or after P25_TRUNK_PATCH_FRESH (30 s) with neither a grant
// nor an add for it (control_activity(), so only while control channel broadcasts arrive).

static struct patch *patch_find(struct p25_trunk *t, uint16_t sg){
  for(int i = 0; i < P25_TRUNK_MAX_PATCHES; i++)
    if(t->patches[i].used && t->patches[i].sg == sg)
      return &t->patches[i];
  return NULL;
}
static struct patch *patch_get(struct p25_trunk *t, uint16_t sg){
  struct patch *p = patch_find(t, sg);
  if(p != NULL)
    return p;
  for(int i = 0; i < P25_TRUNK_MAX_PATCHES; i++){
    if(!t->patches[i].used){
      p = &t->patches[i];
      memset(p, 0, sizeof *p);
      p->used = true;
      p->sg = sg;
      return p;
    }
  }
  return NULL;
}
static void patch_gc(struct patch *p){
  if(p != NULL && !p->in_map && !p->has_update && !p->has_add && !p->harris && p->nsight == 0)
    p->used = false;
}
static bool patch_has(struct patch const *p, uint16_t g){
  for(int i = 0; i < p->nmembers; i++)
    if(p->members[i] == g)
      return true;
  return false;
}
static void patch_event(struct p25_trunk *t, struct patch const *p, uint32_t flag, uint16_t g, char const *what){
  struct p25_trunk_event e;
  memset(&e, 0, sizeof e);
  e.type = P25_TRUNK_PATCH;
  e.slot = -1;
  e.flags = flag | (p->harris && p->simulselect ? P25_TRUNK_F_SIMULSELECT : 0);
  e.supergroup = p->sg;
  e.talkgroup = g;
  e.nmembers = p->nmembers;
  memcpy(e.members, p->members, sizeof p->members);
  struct out o = { e.text, sizeof e.text };
  put(&o, "patch sg=%u %s", p->sg, what);
  if(g)
    put(&o, " tg=%u", g);
  put(&o, " members=");
  for(int i = 0; i < p->nmembers; i++)
    put(&o, "%s%u", i ? "," : "", p->members[i]);
  if(p->harris)
    put(&o, " harris%s seq=%u", p->simulselect ? " simulselect" : "", p->seq);
  emit(t, &e);
}
// The supergroup's membership, created empty if new
static struct patch *patch_ensure(struct p25_trunk *t, uint16_t sg){
  struct patch *p = patch_get(t, sg);
  if(p != NULL && !p->in_map){
    p->in_map = true;
    p->nmembers = 0;
    patch_event(t, p, P25_TRUNK_F_PATCH_NEW, 0, "new");
  }
  return p;
}
static void member_add(struct p25_trunk *t, struct patch *p, uint16_t g){
  if(patch_has(p, g) || p->nmembers >= P25_TRUNK_MAX_MEMBERS)
    return;
  int i = p->nmembers;
  while(i > 0 && p->members[i-1] > g){
    p->members[i] = p->members[i-1];
    i--;
  }
  p->members[i] = g;
  p->nmembers++;
  patch_event(t, p, P25_TRUNK_F_JOIN, g, "join");
}
static void member_del(struct p25_trunk *t, struct patch *p, uint16_t g){
  for(int i = 0; i < p->nmembers; i++){
    if(p->members[i] == g){
      memmove(&p->members[i], &p->members[i+1], (p->nmembers - i - 1) * sizeof p->members[0]);
      p->nmembers--;
      patch_event(t, p, P25_TRUNK_F_LEAVE, g, "leave");
      return;
    }
  }
}
static struct sighting *sighting(struct patch *p, uint16_t g, bool create){
  for(int i = 0; i < p->nsight; i++)
    if(p->sight[i].g == g)
      return &p->sight[i];
  if(!create || p->nsight >= P25_TRUNK_MAX_SIGHTINGS)
    return NULL;
  struct sighting *s = &p->sight[p->nsight++];
  memset(s, 0, sizeof *s);
  s->g = g;
  return s;
}
static void sighting_del(struct patch *p, uint16_t g){
  for(int i = 0; i < p->nsight; i++){
    if(p->sight[i].g == g){
      memmove(&p->sight[i], &p->sight[i+1], (p->nsight - i - 1) * sizeof p->sight[0]);
      p->nsight--;
      return;
    }
  }
}
// The membership, the unconfirmed names and the last add go; the regroup and grant times stay
static void patch_forget(struct p25_trunk *t, struct patch *p){
  if(p->in_map){
    p->nmembers = 0;
    patch_event(t, p, P25_TRUNK_F_PATCH_END, 0, "end");
    p->in_map = false;
  }
  p->nsight = 0;
  p->has_add = false;
}

// Regroup add: the listed talkgroups join. Zero and the supergroup itself are not members. A
// talkgroup joins only when adds name it twice within P25_TRUNK_MEMBER_CONFIRM: a link control
// that passed its Reed-Solomon check can still name random members, and the supergroup's adds
// repeat every second or so, so a single wrong name would otherwise stay for good.
static void patch_add(struct p25_trunk *t, uint16_t sg, uint16_t const *groups, int count){
  if(sg == 0)
    return;
  struct patch *p = patch_ensure(t, sg);
  if(p == NULL)
    return;
  for(int i = 0; i < count; i++){
    uint16_t const g = groups[i];
    if(g == 0 || g == sg)
      continue;
    struct sighting *s = sighting(p, g, true);
    if(s == NULL)
      continue;
    if(!patch_has(p, g) && s->count > 0 && t->now - s->last > P25_TRUNK_MEMBER_CONFIRM)
      s->count = 0;
    s->count++;
    s->last = t->now;
    if(s->count >= 2)
      member_add(t, p, g);
  }
  p->has_update = true;
  p->last_update = t->now;
  p->has_add = true;
  p->last_add = t->now;
}

// Regroup delete: the listed members leave; a delete listing none ends the patch, and so does
// a patch left without members
static void patch_delete(struct p25_trunk *t, uint16_t sg, uint16_t const *groups, int count){
  struct patch *p = patch_find(t, sg);
  if(p == NULL || !p->in_map)
    return;
  bool listed = false;
  for(int i = 0; i < count; i++){
    if(groups[i] == 0 || groups[i] == sg)
      continue;
    listed = true;
    member_del(t, p, groups[i]);
  }
  if(listed)
    for(int i = 0; i < count; i++)
      sighting_del(p, groups[i]);
  if(!listed || p->nmembers == 0){
    patch_forget(t, p);
    p->has_update = false;
    p->harris = false;
  }
  patch_gc(p);
}

// Members the supergroup's adds have stopped naming, and names never confirmed, go after
// P25_TRUNK_MEMBER_STALE; a patch announced once and then only granted keeps its members
static void patch_prune(struct p25_trunk *t){
  for(int k = 0; k < P25_TRUNK_MAX_PATCHES; k++){
    struct patch *p = &t->patches[k];
    if(!p->used || p->nsight == 0)
      continue;
    bool const announced = p->has_add && t->now - p->last_add <= P25_TRUNK_MEMBER_STALE;
    for(int i = 0; i < p->nsight;){
      struct sighting *s = &p->sight[i];
      bool const member = p->in_map && patch_has(p, s->g);
      bool const stale = t->now - s->last > P25_TRUNK_MEMBER_STALE;
      if(stale && (!member || announced)){
        if(member)
          member_del(t, p, s->g);
        sighting_del(p, s->g);
      } else
        i++;
    }
    patch_gc(p);
  }
}

// L3Harris group regroup (MFID 0xa4 opcode 0x30): one member per message, repeated while the
// regroup stands. A talkgroup target joins the supergroup, a radio target is not tracked; a
// deactivation ends the regroup whatever it names, and a new sequence number is a new version
// of the regroup whose members are announced again.
static void harris_regroup(struct p25_trunk *t, struct p25_msg const *m){
  uint16_t const sg = m->group;
  if(sg == 0)
    return;
  if(!(m->regroup_opts & 0x20)){ // deactivate
    patch_delete(t, sg, NULL, 0);
    struct patch *p = patch_find(t, sg);
    if(p != NULL){
      p->harris = false;
      patch_gc(p);
    }
    return;
  }
  if(!(m->regroup_opts & 0x40)) // a radio target
    return;
  uint8_t const seq = m->regroup_opts & 0x1f;
  struct patch *p = patch_find(t, sg);
  if(p != NULL && p->harris && p->seq != seq)
    patch_forget(t, p);
  p = patch_get(t, sg);
  if(p == NULL)
    return;
  p->harris = true;
  p->seq = seq;
  p->simulselect = (m->regroup_opts & 0x80) != 0;
  uint16_t const g = m->group_b;
  patch_add(t, sg, &g, 1);
}

// Both talkgroups in one patch. A one-way simulselect carries the supergroup's calls to its
// members, but the members' own calls stay apart.
static bool in_same_patch(struct p25_trunk *t, uint16_t a, uint16_t b){
  for(int i = 0; i < P25_TRUNK_MAX_PATCHES; i++){
    struct patch const *p = &t->patches[i];
    if(!p->used || !p->in_map)
      continue;
    bool const has_a = a == p->sg || patch_has(p, a);
    bool const has_b = b == p->sg || patch_has(p, b);
    if(!has_a || !has_b)
      continue;
    if(p->harris && p->simulselect && a != p->sg && b != p->sg)
      continue;
    return true;
  }
  return false;
}

// The patch a regroup link control's talkgroup belongs to: itself if a supergroup, else the
// (lowest) supergroup that has it as a member, else the talkgroup as a supergroup not yet known
static uint16_t supergroup_for(struct p25_trunk *t, uint16_t g){
  struct patch const *p = patch_find(t, g);
  if(p != NULL && p->in_map)
    return g;
  bool found = false;
  uint16_t best = 0;
  for(int i = 0; i < P25_TRUNK_MAX_PATCHES; i++){
    p = &t->patches[i];
    if(p->used && p->in_map && patch_has(p, g) && (!found || p->sg < best)){
      best = p->sg;
      found = true;
    }
  }
  return found ? best : g;
}

// A supergroup (0xff00 and up) with known members stands for its lowest member
static uint16_t resolve_tg(struct p25_trunk *t, uint16_t g){
  if(g < 0xff00)
    return g;
  struct patch const *p = patch_find(t, g);
  if(p != NULL && p->in_map && p->nmembers > 0)
    return p->members[0];
  return g;
}
static bool special_tg(uint16_t g){
  return g == 0 || g >= 0xff00;
}

static void lookup_files(struct p25_trunk *t, struct call *c){
  struct patch const *p = patch_find(t, c->patch_sg);
  c->nfiles = 0;
  if(p != NULL && p->in_map && p->nmembers > 0){
    bool sg_in = false;
    for(int i = 0; i < p->nmembers; i++){
      if(!sg_in && c->patch_sg < p->members[i]){
        c->files[c->nfiles++] = c->patch_sg;
        sg_in = true;
      }
      c->files[c->nfiles++] = p->members[i];
    }
    if(!sg_in)
      c->files[c->nfiles++] = c->patch_sg;
    c->members_known = true;
  } else {
    c->files[c->nfiles++] = c->patch_sg;
    c->members_known = false;
  }
}
// The call's talkgroups, looked up again until the members are known
static void patch_files(struct p25_trunk *t, struct call *c){
  if(c->patched && !c->members_known)
    lookup_files(t, c);
}
// The call is on a supergroup: snapshot its members, unless already patched there
static void mark_patched(struct p25_trunk *t, struct call *c, uint16_t sg){
  if(c->patched && c->patch_sg == sg)
    return;
  c->patched = true;
  c->patch_sg = sg;
  lookup_files(t, c);
}
// L3Harris grants a regroup's calls with standard group grants naming the supergroup
static void mark_harris(struct p25_trunk *t, struct call *c){
  if(c->type != P25_TRUNK_GROUP)
    return;
  struct patch const *p = patch_find(t, c->tg);
  if(p != NULL && p->harris)
    mark_patched(t, c, c->tg);
}

// ---- Calls ----
// A call's life is described at the top of the file. Calls are found by channel (frequency and
// timeslot: find_call()) and recognised by type, talkgroup and unit (same_call()).

static struct call *find_call(struct p25_trunk *t, uint64_t freq, int slot){
  for(int i = 0; i < P25_TRUNK_MAX_CALLS; i++)
    if(t->calls[i].used && t->calls[i].freq == freq && t->calls[i].slot == slot)
      return &t->calls[i];
  return NULL;
}

// Does (talkgroup, type, unit) name the call? For a group call, any talkgroup of its patch does
static bool same_call(struct p25_trunk *t, struct call const *c, uint16_t tg, enum p25_trunk_call_type type,
                      uint32_t unit){
  if(c->type != type)
    return false;
  if(type == P25_TRUNK_DATA)
    return c->tg == tg && c->unit == unit;
  if(type != P25_TRUNK_GROUP)
    return c->unit == unit;
  if(c->tg == tg)
    return true;
  if(c->patched){
    if(tg == c->patch_sg)
      return true;
    for(int i = 0; i < c->nfiles; i++)
      if(c->files[i] == tg)
        return true;
  }
  return in_same_patch(t, tg, c->tg);
}

static void set_source(struct p25_trunk *t, struct call *c, uint32_t src, bool new_talker){
  if(src == c->src && !new_talker)
    return;
  uint32_t const prev = c->src;
  c->src = src;
  if(c->start_pending)
    return; // the start reports it
  struct p25_trunk_event e;
  call_event(t, c, &e, P25_TRUNK_CALL_TALKER);
  e.prev_source = prev;
  if(new_talker)
    e.flags |= P25_TRUNK_F_NEW_TALKER;
  size_t n = strlen(e.text);
  snprintf(e.text + n, sizeof e.text - n, " prev=%u%s", prev, new_talker ? " new" : "");
  emit(t, &e);
}

// Voice units held while the encryption was not known: tell the caller what they were. Each is
// encrypted if its own label said so or the release does (F_ENCRYPTED: the service options say
// encrypted, or a header or sync confirmed it and none has said clear since), and then
// unconfirmed unless F_ENC_KNOWN. With keep, the last held unit is the one being passed now and
// is described by its label instead.
static void release(struct p25_trunk *t, struct call *c, bool keep){
  if(c->held == 0)
    return;
  int const n = c->held - (keep ? 1 : 0);
  c->held = 0;
  if(c->type == P25_TRUNK_GROUP)
    patch_files(t, c);
  bool const enc = c->encrypted || confirmed_encrypted(c) || c->contradicted;
  if(n > 0){
    struct p25_trunk_event e;
    call_event(t, c, &e, P25_TRUNK_CALL_UPDATE);
    e.flags |= P25_TRUNK_F_RELEASE;
    e.flags &= ~P25_TRUNK_F_ENCRYPTED;
    if(enc)
      e.flags |= P25_TRUNK_F_ENCRYPTED;
    if(enc && !c->confirmed)
      e.flags |= P25_TRUNK_F_UNCONFIRMED;
    e.held_units = n;
    if(c->type != P25_TRUNK_GROUP){
      e.nmembers = 0;
      e.supergroup = 0;
    }
    size_t len = strlen(e.text);
    snprintf(e.text + len, sizeof e.text - len, " release=%d %s", n, enc ? "encrypted" : "clear");
    emit(t, &e);
  }
  if(keep){
    struct p25_trunk_label *l = &t->label;
    // A unit held as encrypted stays so; the release can only add it
    bool const e = l->encrypted || enc;
    l->encrypted = e;
    l->unconfirmed = e && !c->confirmed;
    l->algid = c->alg;
    l->keyid = c->kid;
    l->decision = enc ? P25_TRUNK_VOICE_ENCRYPTED : P25_TRUNK_VOICE_CLEAR;
    if(c->type == P25_TRUNK_GROUP && c->patched){
      l->nmembers = c->nfiles;
      memcpy(l->members, c->files, sizeof c->files);
    } else
      l->nmembers = 0;
  }
}

static void end_call(struct p25_trunk *t, struct call *c, enum p25_trunk_end reason){
  release(t, c, false); // the call ends: never leave its frames behind
  struct p25_trunk_event e;
  call_event(t, c, &e, P25_TRUNK_CALL_END);
  e.end_reason = reason;
  size_t n = strlen(e.text);
  snprintf(e.text + n, sizeof e.text - n, " reason=%s", p25_trunk_end_name(reason));
  emit(t, &e);
  if(t->config.update_quiet > 0){
    t->ended[t->next_ended] = *c;
    t->ended_at[t->next_ended] = t->now;
    t->next_ended = (t->next_ended + 1) % P25_TRUNK_MAX_CALLS;
  }
  c->used = false;
}

// Another unit keyed up in the same call: its frames are a new transmission
static void new_talker(struct p25_trunk *t, struct call *c, uint32_t src){
  release(t, c, false);
  set_source(t, c, src, true);
}

static bool carries_svc(enum origin origin, enum grant_kind kind){
  if(origin == ORIGIN_HDU)
    return false;
  switch(kind){
  case KIND_GROUP_UPDATE: // the update has no service options field
  case KIND_UU:           // nor have the unit-to-unit grant and update
  case KIND_UU_UPDATE:
  case KIND_PATCH_UPDATE:
    return false;
  default:
    return true;
  }
}
static enum origin origin_of(enum grant_kind kind){
  switch(kind){
  case KIND_GROUP_UPDATE:
  case KIND_GROUP_UPDATE_EXP:
  case KIND_UU_UPDATE:
  case KIND_TEL_UPDATE:
  case KIND_PATCH_UPDATE:
    return ORIGIN_UPDATE;
  case KIND_LATE:
    return ORIGIN_LATE;
  default:
    return ORIGIN_GRANT;
  }
}

// Service options from a grant or a channel user link control. Their encryption bit is the
// call's first word on encryption: if it contradicts what a header or encryption sync confirmed
// (a talker keying up encrypted in a clear call, or the reverse), the encryption is unconfirmed
// again until the next one. One bit cannot undo what a header or sync said, though: a call
// confirmed encrypted is not known clear until a header or sync says so. Its units are held
// meanwhile, the confirmed ALGID and key wait as the pending sync (so the next sync repeating
// them confirms the encryption again), and units still held when the hold limit runs out or the
// call ends are encrypted. Units held for a call now known clear are released.
static void note_svc(struct p25_trunk *t, struct call *c, uint8_t svc){
  c->svc = svc;
  c->svc_known = true;
  bool const enc = (svc & P25_SVC_ENCRYPTED) != 0;
  c->encrypted = enc;
  if(c->confirmed && enc != confirmed_encrypted(c)){
    if(confirmed_encrypted(c)){
      c->contradicted = true;
      c->pending_ess = true;
      c->pend_alg = c->alg;
      c->pend_kid = c->kid;
    } else
      c->pending_ess = false;
    c->confirmed = false;
  }
  if(!enc && c->held > 0 && !c->confirmed && !c->contradicted){
    c->alg = ALG_CLEAR;
    release(t, c, false);
  }
}

struct key {
  uint64_t freq;
  int slot;
  struct p25_chan ch;
};

// START a call, or refresh the call already on the channel. A grant or update for the same call
// refreshes it (a grant naming another unit is a new talker); anything else ends the call on the
// channel and starts this one. NULL if the table is full.
static struct call *begin_call(struct p25_trunk *t, struct key const *k, enum p25_trunk_call_type type, uint16_t tg,
                               uint32_t src, uint32_t unit, uint8_t svc, uint8_t alg, enum origin origin,
                               enum grant_kind kind){
  struct call *c = find_call(t, k->freq, k->slot);
  if(c != NULL && same_call(t, c, tg, type, unit)){
    if(origin == ORIGIN_GRANT || origin == ORIGIN_UPDATE){
      c->last_grant = t->now;
      c->refreshed = true;
    }
    if(!c->svc_known && carries_svc(origin, kind))
      note_svc(t, c, svc);
    if(src != 0){
      if(c->src == 0)
        set_source(t, c, src, false);
      else if(c->src != src && origin == ORIGIN_GRANT)
        new_talker(t, c, src);
    }
    mark_harris(t, c);
    return c;
  }
  if(c == NULL && origin == ORIGIN_UPDATE && t->config.update_quiet > 0){
    // The quiet time (p25trunk.h, update_quiet). A grant update naming the call that ended on
    // this channel a moment ago is most likely the system's own hang time after it, and an
    // update has no talker to say otherwise; starting a call from it would log a call that
    // never had voice, count it against the caller's limit and keep the channel in use for
    // nothing. So none is started. No reply is lost by it: the caller is still receiving the
    // channel (that is the condition for setting a quiet time), so a reply announced by updates
    // alone starts its call from its own header or first voice unit, with its talker, a few
    // tenths of a second later. Only after a call that was followed and received: after any
    // other nothing is listening there, and the update must start a call as before
    for(int i = 0; i < P25_TRUNK_MAX_CALLS; i++){
      struct call const *e = &t->ended[i];
      if(e->used && e->followed && !e->unheard && e->freq == k->freq && e->slot == k->slot
         && t->now - t->ended_at[i] < t->config.update_quiet && same_call(t, e, tg, type, unit))
        return NULL;
    }
  }
  if(c != NULL)
    end_call(t, c, P25_TRUNK_END_NEW_CALL);
  c = NULL;
  for(int i = 0; i < P25_TRUNK_MAX_CALLS; i++){
    if(!t->calls[i].used){
      c = &t->calls[i];
      break;
    }
  }
  if(c == NULL){
    struct p25_trunk_event e;
    memset(&e, 0, sizeof e);
    e.type = P25_TRUNK_CALL_START;
    e.call_type = type;
    e.freq_hz = k->freq;
    e.slot = k->slot;
    e.ch = k->ch;
    e.talkgroup = tg;
    e.source = src;
    e.target = unit;
    e.svc = svc;
    e.why_not = P25_TRUNK_NOT_TABLE_FULL;
    struct out o = { e.text, sizeof e.text };
    put(&o, "start %s tg=%u tgt=%u src=%u", Type_names[type], tg, unit, src);
    put_mhz(&o, "freq", k->freq);
    put(&o, " skip=%s", p25_trunk_why_name(e.why_not));
    emit(t, &e);
    return NULL;
  }
  memset(c, 0, sizeof *c);
  c->used = true;
  c->id = ++t->next_id;
  c->freq = k->freq;
  c->slot = k->slot;
  c->ch = k->ch;
  c->type = type;
  c->tg = tg;
  c->src = src;
  c->unit = unit;
  c->encrypted = (svc & P25_SVC_ENCRYPTED) != 0;
  c->prio = svc & P25_SVC_PRIORITY;
  c->emergency = (svc & P25_SVC_EMERGENCY) != 0;
  c->svc = svc;
  c->alg = alg;
  c->last_activity = t->now;
  c->origin = origin;
  c->started = t->now;
  c->svc_known = carries_svc(origin, kind);
  c->last_grant = (origin == ORIGIN_GRANT || origin == ORIGIN_UPDATE) ? t->now : FAR_PAST;
  c->start_pending = true;
  mark_harris(t, c);
  return c;
}

// The channel a grant names, as the band plan has it; false if its identifier is not known
static bool grant_key(struct p25_trunk *t, struct p25_chan ch, struct key *k){
  uint64_t dl;
  int slot;
  if(!p25_chan_freq(&t->plan, ch, &dl, NULL, &slot))
    return false;
  struct p25_band const *b = &t->plan.band[ch.iden];
  k->freq = dl;
  k->slot = (b->tdma && b->slots > 1) ? slot : -1;
  k->ch = ch;
  return true;
}

static void follow_grant(struct p25_trunk *t, enum grant_kind kind, struct p25_chan ch, uint16_t tg, uint32_t src,
                         uint8_t svc, enum p25_trunk_call_type type, uint32_t unit, uint16_t patch_sg){
  struct key k;
  if(!grant_key(t, ch, &k)){
    other(t, "grant on unknown channel %u-%u %s tg=%u tgt=%u src=%u", ch.iden, ch.num, Type_names[type], tg, unit,
          src);
    return;
  }
  bool const enc = (svc & P25_SVC_ENCRYPTED) != 0;
  struct call *c = begin_call(t, &k, type, tg, src, unit, svc, enc ? ALG_UNKNOWN : ALG_CLEAR, origin_of(kind), kind);
  if(c != NULL && patch_sg != 0)
    mark_patched(t, c, patch_sg);
}

// A group voice channel grant update: the call goes on, its service options not given
static void follow_update(struct p25_trunk *t, struct p25_chan ch, uint16_t tg){
  struct key k;
  if(!grant_key(t, ch, &k))
    return;
  begin_call(t, &k, P25_TRUNK_GROUP, tg, 0, 0, 0, ALG_CLEAR, ORIGIN_UPDATE, KIND_GROUP_UPDATE);
}

// Data channel grants carry packet data, not a voice call: followed only if asked for
static void data_grant(struct p25_trunk *t, struct p25_msg const *m){
  if(!t->config.follow_data){
    uint64_t dl = 0;
    p25_chan_freq(&t->plan, m->ch, &dl, NULL, NULL);
    struct p25_trunk_event e;
    memset(&e, 0, sizeof e);
    e.type = P25_TRUNK_OTHER;
    e.call_type = P25_TRUNK_DATA;
    e.freq_hz = dl;
    e.slot = -1;
    e.ch = m->ch;
    e.talkgroup = m->group;
    e.target = m->tgt;
    e.source = m->src;
    e.svc = m->svc;
    struct out o = { e.text, sizeof e.text };
    put(&o, "data grant %s ch=%u-%u", p25_kind_name(m->kind), m->ch.iden, m->ch.num);
    if(dl)
      put_mhz(&o, "freq", dl);
    put(&o, " tg=%u tgt=%u src=%u", m->group, m->tgt, m->src);
    emit(t, &e);
    return;
  }
  struct key k;
  if(!grant_key(t, m->ch, &k))
    return;
  begin_call(t, &k, P25_TRUNK_DATA, m->group, m->src, m->tgt, m->svc, ALG_CLEAR, ORIGIN_GRANT, KIND_DATA);
}

// FADE and END by time: activity is the last voice, the start, or the last grant
static void expire(struct p25_trunk *t){
  for(int i = 0; i < P25_TRUNK_MAX_CALLS; i++){
    struct call *c = &t->calls[i];
    if(!c->used)
      continue;
    int64_t const since = c->last_activity > c->last_grant ? c->last_activity : c->last_grant;
    if(t->now - since > P25_TRUNK_CALL_TIMEOUT)
      end_call(t, c, P25_TRUNK_END_TIMEOUT);
  }
}

// ---- Site and control channel ----
// What the site broadcasts is kept as broadcast, and reported (SITE, CONTROL) when it is new or
// changes. Frequencies come from the band plan: this site's RFSS status heard before its band's
// identifier update is reported again once the band is known, a secondary control channel is
// recorded only then, and an adjacent site's is reported without a frequency.

static uint64_t chan_hz(struct p25_trunk *t, struct p25_chan ch){
  uint64_t dl = 0;
  if(!p25_chan_freq(&t->plan, ch, &dl, NULL, NULL))
    return 0;
  return dl;
}

static void site_event(struct p25_trunk *t, uint32_t flags, uint64_t freq, uint32_t wacn, uint16_t sysid,
                       uint8_t rfss, uint8_t site, uint8_t svc_class, struct p25_chan ch){
  struct p25_trunk_event e;
  memset(&e, 0, sizeof e);
  e.type = P25_TRUNK_SITE;
  e.flags = flags;
  e.freq_hz = freq;
  e.slot = -1;
  e.ch = ch;
  e.wacn = wacn;
  e.sysid = sysid;
  e.rfss = rfss;
  e.site = site;
  e.svc_class = svc_class;
  struct out o = { e.text, sizeof e.text };
  if(flags & P25_TRUNK_F_NETWORK)
    put(&o, "network wacn=0x%05x sysid=0x%03x", wacn, sysid);
  else {
    put(&o, "%s sysid=0x%03x rfss=%u site=%u ch=%u-%u", (flags & P25_TRUNK_F_ADJACENT) ? "adjacent" : "site", sysid,
        rfss, site, ch.iden, ch.num);
    if(freq)
      put_mhz(&o, "freq", freq);
    put(&o, " class=0x%02x", svc_class);
  }
  emit(t, &e);
}

static void control_event(struct p25_trunk *t, uint32_t flags, uint64_t freq, char const *what){
  struct p25_trunk_event e;
  memset(&e, 0, sizeof e);
  e.type = P25_TRUNK_CONTROL;
  e.flags = flags;
  e.freq_hz = freq;
  e.slot = -1;
  e.wacn = t->site.wacn;
  e.sysid = t->site.sysid;
  e.rfss = t->site.rfss;
  e.site = t->site.site;
  struct out o = { e.text, sizeof e.text };
  put(&o, "control %s", what);
  if(freq)
    put_mhz(&o, "freq", freq);
  emit(t, &e);
}

// RFSS status (TSBK, multi-block or link control): this site's identity and control channel
static void note_rfss(struct p25_trunk *t, uint16_t sysid, uint8_t rfss, uint8_t site, struct p25_chan ch,
                      uint8_t svc_class){
  struct p25_trunk_site *s = &t->site;
  uint64_t const cc = chan_hz(t, ch);
  if(s->known && s->sysid == sysid && s->rfss == rfss && s->site == site && s->control_hz == cc)
    return;
  bool const new_cc = cc != 0 && cc != s->control_hz;
  s->known = true;
  s->sysid = sysid;
  s->rfss = rfss;
  s->site = site;
  s->control_hz = cc;
  site_event(t, 0, cc, s->wacn, sysid, rfss, site, svc_class, ch);
  if(new_cc)
    control_event(t, 0, cc, "primary");
}

// Network status: the system's WACN and system ID
static void note_network(struct p25_trunk *t, uint32_t wacn, uint16_t sysid){
  struct p25_trunk_site *s = &t->site;
  if(s->network_known && s->wacn == wacn && s->net_sysid == sysid)
    return;
  s->network_known = true;
  s->wacn = wacn;
  s->net_sysid = sysid;
  struct p25_chan none = { 0, 0 };
  site_event(t, P25_TRUNK_F_NETWORK, 0, wacn, sysid, 0, 0, 0, none);
}

// Adjacent site status: a neighbour, new or changed
static void note_adjacent(struct p25_trunk *t, uint16_t sysid, uint8_t rfss, uint8_t site, struct p25_chan ch,
                          uint8_t svc_class){
  struct p25_trunk_site *s = &t->site;
  struct p25_trunk_adjacent *a = NULL;
  for(int i = 0; i < s->nadjacent; i++){
    if(s->adjacent[i].sysid == sysid && s->adjacent[i].rfss == rfss && s->adjacent[i].site == site){
      a = &s->adjacent[i];
      break;
    }
  }
  if(a != NULL && a->ch.iden == ch.iden && a->ch.num == ch.num && a->svc_class == svc_class)
    return;
  if(a == NULL){
    if(s->nadjacent >= P25_TRUNK_MAX_ADJACENT)
      return;
    a = &s->adjacent[s->nadjacent++];
  }
  a->sysid = sysid;
  a->rfss = rfss;
  a->site = site;
  a->ch = ch;
  a->svc_class = svc_class;
  site_event(t, P25_TRUNK_F_ADJACENT, chan_hz(t, ch), s->wacn, sysid, rfss, site, svc_class, ch);
}

// Secondary control channel broadcast: where control may move
static void note_secondary(struct p25_trunk *t, struct p25_chan ch){
  struct p25_trunk_site *s = &t->site;
  if(ch.num == 0 || s->nsecondary >= P25_TRUNK_MAX_SECONDARY)
    return;
  uint64_t const hz = chan_hz(t, ch);
  if(hz == 0)
    return;
  for(int i = 0; i < s->nsecondary; i++)
    if(s->secondary_hz[i] == hz)
      return;
  s->secondary_hz[s->nsecondary++] = hz;
  control_event(t, P25_TRUNK_F_SECONDARY, hz, "secondary");
}

// A control channel broadcast arrived: control is up, and stale patches are dropped. A patch
// neither granted nor added for P25_TRUNK_PATCH_FRESH is over.
static void control_activity(struct p25_trunk *t){
  struct p25_trunk_site *s = &t->site;
  if(!s->control_up){
    s->control_up = true;
    bool const was_lost = s->control_lost;
    s->control_lost = false;
    control_event(t, 0, t->control_freq, was_lost ? "regained" : "acquired");
  }
  s->last_control = t->now;
  for(int i = 0; i < P25_TRUNK_MAX_PATCHES; i++){
    struct patch *p = &t->patches[i];
    if(p->used && p->has_update && t->now - p->last_update > P25_TRUNK_PATCH_FRESH){
      patch_forget(t, p);
      p->harris = false;
      p->has_update = false;
      patch_gc(p);
    }
  }
  patch_prune(t);
}

static void control_loss(struct p25_trunk *t){
  struct p25_trunk_site *s = &t->site;
  if(!s->control_up || s->control_lost)
    return;
  if(t->now - s->last_control <= P25_TRUNK_CONTROL_LOSS)
    return;
  s->control_lost = true;
  s->control_up = false;
  control_event(t, P25_TRUNK_F_LOST, t->control_freq, "lost");
}

// Voice on the control channel's frequency: control has moved elsewhere. Not on the other
// timeslot of a TDMA control carrier whose LCCH is on one slot only: that slot carries traffic
static void voice_on_control(struct p25_trunk *t, uint64_t freq, int slot){
  if(!t->site.control_up || t->control_freq == 0 || freq != t->control_freq)
    return;
  if(t->control_slot >= 0 && slot >= 0 && slot != t->control_slot)
    return;
  t->site.control_up = false;
  control_event(t, P25_TRUNK_F_LOST, freq, "lost: voice on the control channel");
}

static void emergency_alarm(struct p25_trunk *t, uint16_t tg, uint32_t src){
  struct p25_trunk_event e;
  memset(&e, 0, sizeof e);
  e.type = P25_TRUNK_OTHER;
  e.slot = -1;
  e.talkgroup = tg;
  e.source = src;
  e.flags = P25_TRUNK_F_EMERGENCY;
  snprintf(e.text, sizeof e.text, "emergency alarm tg=%u src=%u", tg, src);
  emit(t, &e);
}

// ---- Entry points ----

struct p25_trunk *create_p25_trunk(struct p25_trunk_config const *config, p25_trunk_handler handler, void *arg){
  struct p25_trunk *t = calloc(1, sizeof *t);
  if(t == NULL)
    return NULL;
  if(config != NULL)
    t->config = *config;
  uint16_t *allow = NULL, *deny = NULL;
  if(t->config.nallow > 0 && t->config.allow != NULL){
    allow = malloc(t->config.nallow * sizeof *allow);
    if(allow != NULL)
      memcpy(allow, t->config.allow, t->config.nallow * sizeof *allow);
  }
  if(t->config.ndeny > 0 && t->config.deny != NULL){
    deny = malloc(t->config.ndeny * sizeof *deny);
    if(deny != NULL)
      memcpy(deny, t->config.deny, t->config.ndeny * sizeof *deny);
  }
  t->config.allow = allow;
  t->config.nallow = allow ? t->config.nallow : 0;
  t->config.deny = deny;
  t->config.ndeny = deny ? t->config.ndeny : 0;
  t->handler = handler;
  t->arg = arg;
  t->control_slot = -1;
  p25_bandplan_init(&t->plan);
  return t;
}

void delete_p25_trunk(struct p25_trunk *t){
  if(t == NULL)
    return;
  free((void *)t->config.allow);
  free((void *)t->config.deny);
  free(t);
}

// Each entry point sets the time, ends the calls that have timed out (begin()), takes its message
// in, then reports (finish()). A voice data unit comes in two calls (p25trunk.h): its word, kept
// as pending, then the unit, which takes the word only if it came from the same channel and is
// the kind that unit carries (an LDU1's or TDULC's link control, an LDU2's sync, a header's);
// any other pending word is dropped, so a word is never taken for another unit's
static void begin(struct p25_trunk *t, int64_t now){
  t->now = now;
  t->site.last_message = now;
  expire(t);
}

// Where a trunking message was heard: the control channel (Phase 1 blocks), an LCCH, or a voice
// channel. One heard on a voice channel is acted on like any other (a grant is a grant), but
// it says nothing about whether the control channel is being received
enum heard { ON_CONTROL, ON_LCCH, ON_VOICE };

static void control_msg(struct p25_trunk *t, struct p25_msg const *m, uint16_t nac, int64_t now, enum heard heard){
  if(t == NULL || m == NULL)
    return;
  if(t->config.nac != 0 && nac != t->config.nac)
    return; // another system's
  begin(t, now);
  if(heard == ON_LCCH)
    control_activity(t); // an LCCH is only ever sent on a control channel
  else if(heard == ON_CONTROL && m->family == P25_FAMILY_TSBK){
    // These opcodes are sent only by a control channel (any manufacturer's table): standard 0x00
    // and 0x02 group voice grant and grant update, 0x38 system service, 0x3a to 0x3d RFSS,
    // network and adjacent status and identifier update; Motorola 0x00 and 0x02 are regroup add
    // and supergroup grant
    switch(m->opcode){
    case 0x00: case 0x02: case 0x38: case 0x3a: case 0x3b: case 0x3c: case 0x3d:
      control_activity(t);
      break;
    default:
      break;
    }
  }
  bool const mbt = m->family == P25_FAMILY_MBT;
  // MAC structures: the same messages, read as the TSBKs they abbreviate (those that carry service
  // options do so in both forms). A voice channel's are not control channel activity; an LCCH's are
  // A protected block is encrypted past its opcode and manufacturer: it shows the control channel
  // is there (above) and its fields mean nothing
  if((m->family != P25_FAMILY_TSBK && !mbt && m->family != P25_FAMILY_MAC) || m->protect){
    finish(t);
    return;
  }
  switch(m->kind){
  case P25_GRP_V_CH_GRANT:
    follow_grant(t, mbt ? KIND_MBT : KIND_GROUP, m->ch, m->group, m->src, m->svc, P25_TRUNK_GROUP, 0, 0);
    break;
  case P25_GRP_V_CH_GRANT_UPDT:
    follow_update(t, m->ch, m->group);
    if(m->has_b)
      follow_update(t, m->ch_b, m->group_b);
    break;
  case P25_GRP_V_CH_GRANT_UPDT_EXP:
    follow_grant(t, KIND_GROUP_UPDATE_EXP, m->ch, m->group, 0, m->svc, P25_TRUNK_GROUP, 0, 0);
    break;
  case P25_UU_V_CH_GRANT:
  case P25_UU_V_CH_GRANT_UPDT:
    // The multi-block forms carry service options; the single-block ones do not
    follow_grant(t, mbt ? KIND_MBT : m->kind == P25_UU_V_CH_GRANT ? KIND_UU : KIND_UU_UPDATE, m->ch, 0, m->src,
                 mbt ? m->svc : 0, P25_TRUNK_UNIT, m->tgt, 0);
    break;
  case P25_TELE_INT_CH_GRANT:
  case P25_TELE_INT_CH_GRANT_UPDT:
    follow_grant(t, mbt ? KIND_MBT : m->kind == P25_TELE_INT_CH_GRANT ? KIND_TEL : KIND_TEL_UPDATE, m->ch, 0, 0,
                 m->svc, P25_TRUNK_TELEPHONE, m->tgt, 0);
    break;
  case P25_IND_DATA_CH_GRANT:
  case P25_GRP_DATA_CH_GRANT:
  case P25_SN_DATA_CH_GRANT:
    data_grant(t, m);
    break;
  case P25_MOT_GRG_CH_GRANT:
    {
      // A supergroup's call. The grant does not list members (only regroup adds do)
      struct patch *p = patch_ensure(t, m->group);
      if(p != NULL){
        p->has_update = true;
        p->last_update = now;
      }
      follow_grant(t, mbt ? KIND_MBT : KIND_PATCH, m->ch, m->group, m->src, m->svc, P25_TRUNK_GROUP, 0, m->group);
    }
    break;
  case P25_MOT_GRG_CH_UPDT:
    {
      struct patch *p = patch_ensure(t, m->group);
      if(p != NULL){
        p->has_update = true;
        p->last_update = now;
      }
      follow_grant(t, KIND_PATCH_UPDATE, m->ch, m->group, 0, 0, P25_TRUNK_GROUP, 0, m->group);
      if(m->has_b)
        follow_grant(t, KIND_PATCH_UPDATE, m->ch_b, m->group_b, 0, 0, P25_TRUNK_GROUP, 0, m->group_b);
    }
    break;
  case P25_MOT_GRG_ADD:
    patch_add(t, m->group, m->members, m->nmembers);
    break;
  case P25_MOT_GRG_DEL:
    patch_delete(t, m->group, m->members, m->nmembers);
    break;
  case P25_HARRIS_GRG_EXENC_CMD:
    harris_regroup(t, m);
    break;
  case P25_MOT_EMERG_ALRM:
    emergency_alarm(t, m->group, m->src);
    break;
  case P25_SCCB:
    note_secondary(t, m->ch);
    note_secondary(t, m->ch_b);
    break;
  case P25_SCCB_EXP:
    note_secondary(t, m->ch);
    break;
  case P25_RFSS_STS_BCST:
    note_rfss(t, m->sysid, m->rfss, m->site, m->ch, m->svc_class);
    break;
  case P25_NET_STS_BCST:
  case P25_NET_STS_BCST_EXP:   // multi-block and MAC explicit forms: the same fields
    note_network(t, m->wacn, m->sysid);
    break;
  case P25_ADJ_STS_BCST:
    note_adjacent(t, m->sysid, m->rfss, m->site, m->ch, m->svc_class);
    break;
  case P25_RFSS_STS_BCST_EXP:  // a form without a system ID is this site's
    note_rfss(t, m->sysid ? m->sysid : t->site.sysid, m->rfss, m->site, m->ch, m->svc_class);
    break;
  case P25_ADJ_STS_BCST_EXP:
    note_adjacent(t, m->sysid ? m->sysid : t->site.sysid, m->rfss, m->site, m->ch, m->svc_class);
    break;
  case P25_IDEN_UP:
  case P25_IDEN_UP_TDMA:
  case P25_IDEN_UP_VU:
    p25_bandplan_update(&t->plan, m);
    break;
  default:
    break;
  }
  finish(t);
}

void p25_trunk_control(struct p25_trunk *t, struct p25_msg const *m, uint16_t nac, int64_t now){
  control_msg(t, m, nac, now, ON_CONTROL);
}

void p25_trunk_lcch(struct p25_trunk *t, struct p25_msg const *m, uint16_t nac, int64_t now){
  control_msg(t, m, nac, now, ON_LCCH);
}

// Link control (LDU1 or TDULC) about the site, patches and alarms, not this channel's call
static void site_lc(struct p25_trunk *t, struct p25_msg const *m){
  if(m->protect)
    return; // encrypted: only the opcode is readable
  switch(m->kind){
  case P25_MOT_EMERG_ALRM:
    emergency_alarm(t, m->group, m->src);
    break;
  case P25_RFSS_STS_BCST:
  case P25_RFSS_STS_BCST_EXP: // the explicit form has no system ID: this site's
    note_rfss(t, m->sysid ? m->sysid : t->site.sysid, m->rfss, m->site, m->ch, m->svc_class);
    break;
  case P25_ADJ_STS_BCST:
  case P25_ADJ_STS_BCST_EXP:
    note_adjacent(t, m->sysid ? m->sysid : t->site.sysid, m->rfss, m->site, m->ch, m->svc_class);
    break;
  case P25_NET_STS_BCST:
  case P25_NET_STS_BCST_EXP:
    note_network(t, m->wacn, m->sysid);
    break;
  case P25_SCCB:
    note_secondary(t, m->ch);
    if(m->ch_b.num != m->ch.num)
      note_secondary(t, m->ch_b);
    break;
  case P25_SCCB_EXP:
    note_secondary(t, m->ch);
    break;
  case P25_MOT_GRG_ADD:
    patch_add(t, m->group, m->members, 2);
    break;
  case P25_MOT_GRG_DEL:
    patch_delete(t, m->group, m->members, 2);
    break;
  default:
    break;
  }
}

static bool channel_user(struct p25_msg const *m){
  if(m == NULL || m->protect)
    return false;
  switch(m->kind){
  case P25_LC_GRP_V_CH_USR:
  case P25_LC_MOT_GRG_V_CH_USR:
  case P25_LC_UU_V_CH_USR:
  case P25_LC_TEL_INT_V_CH_USR:
    return true;
  default:
    return false;
  }
}

static struct lcd lcd_from_msg(struct p25_msg const *m){
  struct lcd l;
  memset(&l, 0, sizeof l);
  l.type = m->kind == P25_LC_UU_V_CH_USR ? P25_TRUNK_UNIT : m->kind == P25_LC_TEL_INT_V_CH_USR ? P25_TRUNK_TELEPHONE
    : P25_TRUNK_GROUP;
  l.dest = l.type == P25_TRUNK_GROUP ? m->group : 0;
  l.unit = l.type == P25_TRUNK_GROUP ? 0 : m->tgt;
  l.src = m->src;
  l.svc = m->svc;
  l.enc = (m->svc & P25_SVC_ENCRYPTED) != 0;
  l.emerg = (m->svc & P25_SVC_EMERGENCY) != 0;
  l.prio = m->svc & P25_SVC_PRIORITY;
  return l;
}
static struct lcd lcd_from_call(struct call const *c){
  struct lcd l;
  memset(&l, 0, sizeof l);
  l.type = c->type;
  l.dest = c->tg;
  l.unit = c->unit;
  l.src = c->src;
  l.enc = c->encrypted;
  l.emerg = c->emergency;
  l.prio = c->prio;
  l.svc = c->svc;
  return l;
}

// CONTINUE: one voice unit for the call. Labels its frames as the call stands and decides
// whether they wait for the encryption to be known (at most P25_TRUNK_HOLD_UNITS units, then
// encrypted if the service options say so or a header or sync said so and nothing since said clear)
static int voice_frames(struct p25_trunk *t, struct call *c, struct lcd const *l, bool carried){
  struct p25_trunk_label *lb = &t->label;
  memset(lb, 0, sizeof *lb);
  if(c == NULL)
    return P25_TRUNK_VOICE_NONE;
  lb->call_id = c->id;
  lb->call_type = l->type;
  uint32_t const unit = l->type == P25_TRUNK_GROUP ? 0 : l->unit;
  lb->destination = unit ? unit : l->dest;
  lb->source = l->src ? l->src : c->src;
  lb->svc = l->svc;
  lb->emergency = l->emerg;
  lb->encrypted = l->enc || confirmed_encrypted(c);
  lb->carried = carried;
  lb->algid = c->alg;
  lb->keyid = c->kid;
  if(l->type == P25_TRUNK_GROUP){
    patch_files(t, c);
    if(c->patched){
      lb->nmembers = c->nfiles;
      memcpy(lb->members, c->files, sizeof c->files);
    }
  }
  bool const hold = !c->confirmed && (l->enc || c->encrypted || !c->svc_known || c->protected_seen
                                      || c->contradicted);
  if(l->src)
    set_source(t, c, l->src, false);
  c->encrypted = l->enc;
  c->prio = l->prio;
  c->emergency = l->emerg;
  c->svc = l->svc;
  c->last_activity = t->now;
  c->had_voice = true;
  if(!hold){
    lb->decision = lb->encrypted ? P25_TRUNK_VOICE_ENCRYPTED : P25_TRUNK_VOICE_CLEAR;
    return lb->decision;
  }
  lb->decision = P25_TRUNK_VOICE_HOLD;
  c->held++;
  if(c->held >= P25_TRUNK_HOLD_UNITS){
    // No header or usable encryption sync in time: go by what the service options say
    c->protected_seen = false; // a protected link control delays a call's frames once
    release(t, c, true);
  }
  return lb->decision;
}

// An LDU1: its link control (NULL if it did not decode) names who holds the channel
static int voice_ldu1(struct p25_trunk *t, struct key const *k, struct p25_msg const *m, int erasures){
  voice_on_control(t, k->freq, k->slot);
  if(m != NULL)
    site_lc(t, m);
  bool const user = channel_user(m);
  // A link control decoded without erasures may override the call on the channel
  bool const clean = user && erasures == 0;
  struct call *c = find_call(t, k->freq, k->slot);
  struct lcd l;
  if(user)
    l = lcd_from_msg(m);
  else if(c != NULL)
    l = lcd_from_call(c); // nothing here names the call: keep its identity
  else
    return voice_frames(t, NULL, NULL, false);
  if(l.dest >= 0xff00)
    patch_ensure(t, l.dest);
  uint16_t resolved = resolve_tg(t, l.dest);
  bool const special = special_tg(l.dest);
  bool had = c != NULL;
  uint16_t granted = had ? c->tg : 0;
  // A clean link control naming another call: the call on the channel ended unseen (a missed
  // terminator) or was wrongly assigned; this voice starts a new call
  if(had && clean && !(l.type == P25_TRUNK_GROUP && special) && !same_call(t, c, l.dest, l.type, l.unit)){
    end_call(t, c, P25_TRUNK_END_LC);
    c = NULL;
    had = false;
    granted = 0;
  }
  if(had && clean && l.src != 0 && c->src != 0 && l.src != c->src)
    new_talker(t, c, l.src);
  if(had && l.src == 0)
    l.src = c->src;
  if(had && user && (c->type != l.type || (l.type != P25_TRUNK_GROUP && c->unit != l.unit))){
    // A unit-to-unit or telephone link control on a group call, or the reverse: the grant wins
    l.type = c->type;
    l.unit = c->unit;
    l.dest = granted;
    resolved = resolve_tg(t, granted);
  } else if(had && !special && granted != l.dest){
    // The grant is the better word: a link control can keep bit errors past its decode
    l.dest = granted;
    resolved = resolve_tg(t, granted);
  }
  if(c == NULL){
    // Late entry: the link control labels the call
    c = begin_call(t, k, l.type, resolved, l.src, l.unit, l.svc, ALG_CLEAR, ORIGIN_LATE, KIND_LATE);
    if(c == NULL)
      return voice_frames(t, NULL, NULL, false);
  }
  if(l.src != 0)
    set_source(t, c, l.src, false);
  c->encrypted = l.enc;
  c->prio = l.prio;
  c->emergency = l.emerg;
  c->svc = l.svc;
  c->last_activity = t->now;
  if(user)
    note_svc(t, c, m->svc);
  else if(m != NULL && m->protect && !c->confirmed)
    // A protected link control hides its fields; whether the call is encrypted is what its
    // header, encryption sync and service options say. It only holds the frames of a call whose
    // encryption is not confirmed yet (a garbled link control passing its check reads as
    // protected too)
    c->protected_seen = true;
  if(user && m->kind == P25_LC_MOT_GRG_V_CH_USR)
    mark_patched(t, c, supergroup_for(t, m->group));
  bool const carried = m == NULL || m->protect;
  c->carried = carried;
  return voice_frames(t, c, &l, carried);
}

// Encryption sync from an LDU2 that decoded. It confirms the call's encryption when it agrees
// with the service options (a clear ALGID and no encryption bit, or the reverse) or repeats the
// previous one; one that contradicts them waits as pending, so a single miscorrected sync cannot
// mark a clear call encrypted. A confirmation the service options contradicted waits as pending
// too (note_svc), so the next sync names the encryption: the same ALGID and key confirm it again,
// a clear one that agrees with the service options makes the call clear. True if it confirmed
// (or reconfirmed) the encryption.
static bool accept_sync(struct call *c, uint8_t alg, uint16_t kid){
  if(c->confirmed && alg == c->alg && kid == c->kid){
    c->pending_ess = false;
    return true;
  }
  bool const svc_enc = (c->svc & P25_SVC_ENCRYPTED) != 0;
  bool const agrees = !c->svc_known || (alg != ALG_CLEAR) == svc_enc;
  bool const repeated = c->pending_ess && c->pend_alg == alg && c->pend_kid == kid;
  if((agrees && !c->confirmed) || repeated){
    c->alg = alg;
    c->kid = kid;
    c->confirmed = true;
    c->pending_ess = false;
    c->contradicted = false;
    return true;
  }
  c->pending_ess = true;
  c->pend_alg = alg;
  c->pend_kid = kid;
  return false;
}

static int voice_ldu2(struct p25_trunk *t, struct key const *k, struct p25_msg const *m){
  struct call *c = find_call(t, k->freq, k->slot);
  if(c == NULL)
    return voice_frames(t, NULL, NULL, false);
  if(m != NULL && accept_sync(c, m->algid, m->kid))
    release(t, c, false);
  struct lcd const l = lcd_from_call(c);
  return voice_frames(t, c, &l, c->carried);
}

// The header (Golay and Reed-Solomon protected) opens a transmission and names its talkgroup
// (for an encrypted call perhaps the only clear one): it starts the call, or a new one if the
// channel holds another, and confirms the encryption
static void voice_hdu(struct p25_trunk *t, struct key const *k, struct p25_msg const *m){
  if(m == NULL || m->mfid != P25_MFID_STD)
    return; // another manufacturer's layout
  struct call *c = find_call(t, k->freq, k->slot);
  if(m->group != 0 && (c == NULL || !same_call(t, c, m->group, P25_TRUNK_GROUP, 0)))
    c = begin_call(t, k, P25_TRUNK_GROUP, m->group, 0, 0, 0, m->algid, ORIGIN_HDU, KIND_LATE);
  if(c == NULL)
    return;
  c->alg = m->algid;
  c->kid = m->kid;
  c->confirmed = true;
  c->pending_ess = false;
  c->contradicted = false;
  release(t, c, false);
}

// A terminator ends the call, but not one that has had no voice: the channel may still be in
// the previous transmission's hang time when the grant comes
static void voice_end(struct p25_trunk *t, struct key const *k){
  struct call *c = find_call(t, k->freq, k->slot);
  if(c != NULL && c->had_voice)
    end_call(t, c, P25_TRUNK_END_TERMINATOR);
}

void p25_trunk_voice_word(struct p25_trunk *t, uint64_t freq_hz, int slot, struct p25_msg const *msg, int erasures,
                          int64_t now){
  if(t == NULL || msg == NULL)
    return;
  if(msg->family == P25_FAMILY_TSBK || msg->family == P25_FAMILY_MBT){
    control_msg(t, msg, t->config.nac, now, ON_VOICE); // the caller has checked the unit's NAC
    return;
  }
  t->pending = true;
  t->pend_freq = freq_hz;
  t->pend_slot = slot;
  t->pend_erasures = erasures;
  t->pend_msg = *msg;
}

void p25_trunk_voice_msg(struct p25_trunk *t, uint64_t freq_hz, int slot, struct p25_msg const *msg, int64_t now){
  p25_trunk_voice_word(t, freq_hz, slot, msg, 0, now);
}

int p25_trunk_voice_unit(struct p25_trunk *t, uint64_t freq_hz, int slot, int duid, int64_t now){
  if(t == NULL)
    return P25_TRUNK_VOICE_NONE;
  // The word passed just before, if it was this unit's
  struct p25_msg const *m = NULL;
  int erasures = 0;
  if(t->pending && t->pend_freq == freq_hz && t->pend_slot == slot){
    enum p25_family const f = t->pend_msg.family;
    if((duid == P25_DUID_LDU1 && f == P25_FAMILY_LC) || (duid == P25_DUID_TDULC && f == P25_FAMILY_TDULC)
       || (duid == P25_DUID_LDU2 && f == P25_FAMILY_ES) || (duid == P25_DUID_HDU && f == P25_FAMILY_HDU)){
      m = &t->pend_msg;
      erasures = t->pend_erasures;
    }
  }
  t->pending = false;
  begin(t, now);
  memset(&t->label, 0, sizeof t->label);
  struct key k = { .freq = freq_hz, .slot = slot };
  int r = P25_TRUNK_VOICE_NONE;
  switch(duid){
  case P25_DUID_LDU1:
    r = voice_ldu1(t, &k, m, erasures);
    break;
  case P25_DUID_LDU2:
    r = voice_ldu2(t, &k, m);
    break;
  case P25_DUID_HDU:
    voice_hdu(t, &k, m);
    break;
  case P25_DUID_TDU:
    voice_end(t, &k);
    break;
  case P25_DUID_TDULC:
    voice_on_control(t, freq_hz, slot);
    if(m != NULL)
      site_lc(t, m);
    voice_end(t, &k);
    break;
  case P25_DUID_PDU:
    {
      // Packet data keeps a data call going
      struct call *c = find_call(t, freq_hz, slot);
      if(c != NULL && c->type == P25_TRUNK_DATA){
        c->last_activity = now;
        c->had_voice = true;
      }
    }
    break;
  default:
    break;
  }
  finish(t);
  return r;
}

struct p25_trunk_label const *p25_trunk_label(struct p25_trunk const *t){
  return t ? &t->label : NULL;
}

void p25_trunk_tick(struct p25_trunk *t, int64_t now){
  if(t == NULL)
    return;
  t->now = now;
  expire(t);
  control_loss(t);
  finish(t);
}

void p25_trunk_set_control(struct p25_trunk *t, uint64_t freq_hz, float offset_hz){
  if(t == NULL)
    return;
  t->control_freq = freq_hz;
  t->control_offset = offset_hz;
}

void p25_trunk_set_heard(struct p25_trunk *t, uint32_t call_id, bool heard){
  for(int i = 0; t != NULL && i < P25_TRUNK_MAX_CALLS; i++)
    if(t->calls[i].used && t->calls[i].id == call_id)
      t->calls[i].unheard = !heard;
}

void p25_trunk_set_control_slot(struct p25_trunk *t, int slot){
  if(t == NULL)
    return;
  t->control_slot = slot == 0 || slot == 1 ? slot : -1;
}

struct p25_bandplan const *p25_trunk_bandplan(struct p25_trunk const *t){
  return t ? &t->plan : NULL;
}

struct p25_trunk_site const *p25_trunk_site(struct p25_trunk const *t){
  return t ? &t->site : NULL;
}

int p25_trunk_calls(struct p25_trunk const *t, struct p25_trunk_call *out, int max){
  if(t == NULL)
    return 0;
  int n = 0;
  for(int i = 0; i < P25_TRUNK_MAX_CALLS; i++){
    struct call const *c = &t->calls[i];
    if(!c->used)
      continue;
    if(out != NULL && n < max){
      struct p25_trunk_call *o = &out[n];
      memset(o, 0, sizeof *o);
      o->id = c->id;
      o->type = c->type;
      o->freq_hz = c->freq;
      o->slot = c->slot;
      o->ch = c->ch;
      o->talkgroup = c->tg;
      o->source = c->src;
      o->target = c->unit;
      o->svc = c->svc;
      o->svc_known = c->svc_known;
      o->emergency = c->emergency;
      o->encrypted = c->encrypted;
      o->enc_confirmed = c->confirmed;
      o->algid = c->alg;
      o->keyid = c->kid;
      o->pending_ess = c->pending_ess;
      o->had_voice = c->had_voice;
      o->held_units = c->held;
      o->followed = c->followed;
      o->why_not = c->why;
      o->patched = c->patched;
      o->supergroup = c->patch_sg;
      o->nmembers = c->nfiles;
      memcpy(o->members, c->files, sizeof c->files);
      o->started = c->started;
      o->last_activity = c->last_activity;
      o->last_grant = c->last_grant;
    }
    n++;
  }
  return n;
}

int p25_trunk_patches(struct p25_trunk const *t, struct p25_trunk_patch *out, int max){
  if(t == NULL)
    return 0;
  int n = 0;
  for(int i = 0; i < P25_TRUNK_MAX_PATCHES; i++){
    struct patch const *p = &t->patches[i];
    if(!p->used || !p->in_map)
      continue;
    if(out != NULL && n < max){
      struct p25_trunk_patch *o = &out[n];
      memset(o, 0, sizeof *o);
      o->supergroup = p->sg;
      o->nmembers = p->nmembers;
      memcpy(o->members, p->members, sizeof p->members);
      o->harris = p->harris;
      o->simulselect = p->simulselect;
      o->sequence = p->seq;
    }
    n++;
  }
  return n;
}

char const *p25_trunk_event_name(enum p25_trunk_event_type type){
  switch(type){
  case P25_TRUNK_CALL_START: return "CALL_START";
  case P25_TRUNK_CALL_UPDATE: return "CALL_UPDATE";
  case P25_TRUNK_CALL_TALKER: return "CALL_TALKER";
  case P25_TRUNK_CALL_END: return "CALL_END";
  case P25_TRUNK_SITE: return "SITE";
  case P25_TRUNK_CONTROL: return "CONTROL";
  case P25_TRUNK_PATCH: return "PATCH";
  case P25_TRUNK_OTHER: return "OTHER";
  }
  return "?";
}

char const *p25_trunk_end_name(enum p25_trunk_end reason){
  switch(reason){
  case P25_TRUNK_END_NONE: return "none";
  case P25_TRUNK_END_TERMINATOR: return "terminator";
  case P25_TRUNK_END_TIMEOUT: return "timeout";
  case P25_TRUNK_END_NEW_CALL: return "new_call";
  case P25_TRUNK_END_LC: return "link_control";
  }
  return "?";
}

char const *p25_trunk_why_name(enum p25_trunk_why why){
  switch(why){
  case P25_TRUNK_FOLLOWED: return "followed";
  case P25_TRUNK_NOT_ENCRYPTED: return "encrypted";
  case P25_TRUNK_NOT_TALKGROUP: return "talkgroup";
  case P25_TRUNK_NOT_PRIVATE: return "private";
  case P25_TRUNK_NOT_MAX_CALLS: return "max_calls";
  case P25_TRUNK_NOT_TABLE_FULL: return "table_full";
  }
  return "?";
}
