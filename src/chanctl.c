// Ask radiod to create, keep alive and release receiver channels on behalf of a client program
// Commands go to radiod's command group; replies come back on the status stream the client already reads
// Copyright 2026, Matthew Kaufman, KA6SQG

#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>

#include "misc.h"
#include "multicast.h"
#include "rtp.h"
#include "status.h"
#include "radio.h"
#include "chanctl.h"

// Theory of operation
//
// radiod's terms. radiod has no separate create, keepalive or delete command. Any command packet
// names an SSRC; if no channel has that SSRC, radiod makes one from its [global] template and then
// applies the packet's fields to it, in packet order (radio_status.c). Every command packet,
// a bare poll (tag and SSRC only) included, reloads the channel's lifetime from its last LIFETIME
// setting, and the channel counts its lifetime down one per block, going at 0 (radio.c). A
// lifetime of 0 never runs down: that is a permanent channel, and it is also the template's
// default (defaults.h DEFAULT_LIFETIME). So here:
// - a request is one FULL command: every field of the format, the frequency and LIFETIME;
// - a keepalive is a command carrying only LIFETIME;
// - a release is LIFETIME 1, so the channel goes at its next block. None is sent for a channel
//   whose lifetime has run out since our last command: radiod has let that one go already.
// The lifetime is given in seconds and sent in blocks (frames()), using the block time learned
// from status. A channel asked for with a lifetime goes by itself if the client stops, so a
// client that dies leaves nothing behind for longer than that.
//
// Why a keepalive is never a bare poll. If radiod has lost the channel (it ran out, radiod
// restarted, someone else released it), a poll makes a new channel from the template, with the
// template's frequency, format and lifetime 0: a permanent channel nobody will ever release. A
// keepalive's LIFETIME means such a channel at worst lives one lifetime. Status then shows it in
// the wrong form, and the entry goes back to PENDING and asks for it in full (chanctl_observe()).
// Polls are sent only to someone else's channel (SHARED below), where setting the lifetime would
// override its owner's choice, and only while its status shows it surely still there.
//
// Pacing. radiod queues at most two commands per channel and executes one per block, dropping
// the rest, so commands for one SSRC go at most every Min_gap. Refreshes go every third of the
// lifetime (refresh_interval()), so one can be lost and the next still comes in time.
//
// What status tells us. A caller that decodes radiod's status passes each packet to
// chanctl_observe(). That gives the front end's tuning, so a frequency it does not cover can be
// refused (ERANGE) rather than asked for: a radiod whose front end is not locked would retune it
// for the channel, and every channel left outside the new coverage would go quiet. It gives
// radiod's block time. It confirms a request: the entry is UP once a
// status shows the SSRC on the frequency and in the form asked for (same_form()). And it keeps a
// table of every SSRC seen (struct seen), so a request for an SSRC that is someone else's channel
// is either refused (EEXIST: another frequency or form, which a request would change under its
// owner) or shared (the same frequency and form: use it as it is). Without status nothing is
// confirmed: a request is resent with backoff and taken as up after Max_retry.
//
// The kinds and states of an entry.
// - OWN: made by us. PENDING after a request, resent at First_retry doubling to Max_retry until
//   status confirms it; UP once confirmed, kept alive by keepalives; back to PENDING if status
//   shows it changed or recreated from the template; RELEASING after a release, the release
//   resent while status still shows it running, forgotten after Release_hold.
// - SHARED: someone else's channel with a lifetime, in our form. Polled to keep it, never
//   released; if it may have gone, it becomes OWN and is asked for in full.
// - STATIC: someone else's permanent channel in our form. Nothing is ever sent for it.
// An entry nobody has asked for or refreshed within its lifetime is forgotten (maintain()),
// since radiod has let the channel go by then.
//
// The template's payload type. radiod skips the payload type update on a command that also
// restarts the demodulator, as a new channel's first command does when its sample rate differs
// from the template's, so the stream keeps the template's payload type. A PENDING entry whose
// status shows the wrong type sends two encoding changes, which make radiod recompute it, at most
// twice (see chanctl_observe()).

// radiod's block time until its status tells us (defaults.h DEFAULT_BLOCKTIME)
static double const Default_blocktime = 0.020;
// At most one command per SSRC this often. radiod queues two commands per channel and executes one
// per block (radio.h CQLEN, radio.c handle_commands()), dropping the rest; tune.c sends at 10 Hz
static int64_t const Min_gap = 100000000LL;
// Unanswered requests are resent after this, doubling up to Max_retry
static int64_t const First_retry = 250000000LL;
static int64_t const Max_retry = 2000000000LL;
// Status from a channel we didn't create counts for this long
static int64_t const Seen_valid = 5000000000LL;
static int const Max_seen = 256;
// Released channels are forgotten after this
static int64_t const Release_hold = 2000000000LL;
// A release still not taken this long after it was sent is sent again, up to Release_tries in all
static int64_t const Release_retry = 300000000LL;
static int const Release_tries = 3;

enum kind {
  OWN,       // we created it, we refresh it with LIFETIME and release it
  SHARED,    // someone else's dynamic channel in our form: we keep it alive with polls and never kill it
  STATIC,    // someone else's permanent channel (lifetime 0) in our form: used as is, nothing sent
};

struct entry {
  struct entry *next;
  uint32_t ssrc;
  double frequency;
  struct chanctl_format format;
  double lifetime;         // s, 0 = none
  enum chanctl_state state;
  enum kind kind;
  uint32_t tag;            // of the last command sent
  int64_t first_request;   // when it went PENDING
  int64_t last_send;       // last command of any kind
  int64_t retry;           // current retry interval while PENDING
  int64_t last_wanted;     // last request() or refresh()
  int64_t released;
  int release_tries;
  int pt_fixes;
};

// Last status seen for each SSRC of our radiod
struct seen {
  struct seen *next;
  uint32_t ssrc;
  double frequency;
  struct chanctl_format format;
  bool have_format;
  double lifetime;         // s remaining, 0 = none
  int64_t when;
  // One of ours that we released. Its last status packets as it goes show a lifetime of 0, which
  // is also what a permanent channel shows; without this it would pass for someone else's
  // permanent channel for Seen_valid, and a request for its frequency then would be refused
  // (EEXIST) or take it for a static channel. Cleared by a status with a lifetime running
  bool gone;
};

struct chanctl {
  struct sockaddr_storage radio;
  int fd;
  bool observing;          // chanctl_observe() is being called, so requests can be confirmed
  // Front end, from status
  bool fe_known;
  double fe_frequency;     // first LO, Hz
  float fe_min_IF,fe_max_IF;
  double blocktime;        // s
  struct entry *entries;
  struct seen *seen;
  int nseen;
};

static int64_t now_ns(void);
static struct entry *find_entry(struct chanctl *ctl,uint32_t ssrc);
static struct seen *find_seen(struct chanctl *ctl,uint32_t ssrc,int64_t now);
static bool alive(struct seen const *s,int64_t now,double fraction);
static void remove_entry(struct chanctl *ctl,struct entry *e);
static void maintain(struct chanctl *ctl,int64_t now);
static bool same_form(double f1,struct chanctl_format const *a,double f2,struct chanctl_format const *b);
static bool covered(struct chanctl const *ctl,double frequency,struct chanctl_format const *format);
static int frames(struct chanctl const *ctl,double seconds);
static int64_t refresh_interval(struct chanctl const *ctl,struct entry const *e);
static int send_command(struct chanctl *ctl,struct entry *e,int what,int64_t now);
static int keep_alive(struct chanctl *ctl,struct entry *e,int64_t now);

enum { FULL, KEEPALIVE, POLL, RELEASE, OTHER_ENCODING, ENCODING };

int chanctl_format(struct chanctl_format *format,struct frontend const *frontend,chan_t const *chan){
  if(format == NULL || chan == NULL)
    return -1;
  (void)frontend; // nothing needed from it yet
  memset(format,0,sizeof *format);
  if(chan->output.samprate <= 0 || (chan->output.channels != 1 && chan->output.channels != 2)
     || (chan->output.dest_socket.ss_family != AF_INET && chan->output.dest_socket.ss_family != AF_INET6))
    return -1;

  // decode_status.c leaves gains as amplitude ratios and squelch levels as power ratios; commands take dB
  format->demod_type = chan->demod_type;
  format->samprate = chan->output.samprate;
  format->channels = chan->output.channels;
  format->encoding = chan->output.encoding;
  format->low = chan->filter.min_IF;
  format->high = chan->filter.max_IF;
  format->kaiser_beta = chan->filter.kaiser_beta;
  format->filter2 = chan->filter2.blocking;
  format->shift = chan->tune.shift;
  format->maxdelay = chan->output.maxdelay;
  format->status_interval = chan->status.output_interval;
  format->agc = chan->linear.agc;
  format->gain = voltage2dB(chan->output.gain);
  format->headroom = voltage2dB(chan->output.headroom);
  format->hangtime = chan->linear.hangtime;
  format->threshold = voltage2dB(chan->linear.threshold);
  format->recovery_rate = voltage2dB(chan->linear.recovery_rate);
  format->pll = chan->pll.enable;
  format->pll_square = chan->pll.square;
  format->pll_bw = chan->pll.loop_bw;
  format->envelope = chan->linear.env;
  format->isb = chan->filter2.out.isb;
  format->snr_squelch = chan->squelch.snr_enable;
  format->squelch_open = power2dB(chan->squelch.open);
  format->squelch_close = power2dB(chan->squelch.close);
  format->threshold_extend = chan->fm.threshold;
  format->data = chan->output.dest_socket;
  format->pt = chan->output.rtp.type;
  return 0;
}

struct chanctl *chanctl_create(struct sockaddr_storage const *radio,char const *iface,int ttl){
  if(radio == NULL || (radio->ss_family != AF_INET && radio->ss_family != AF_INET6)){
    errno = EINVAL;
    return NULL;
  }
  struct chanctl *ctl = calloc(1,sizeof *ctl);
  if(ctl == NULL)
    return NULL;
  ctl->radio = *radio;
#if defined(__APPLE__) || defined(__FreeBSD__)
  // decode_socket() doesn't fill in the BSD length field; joining the group fails without it
  if(ctl->radio.ss_family == AF_INET)
    ((struct sockaddr_in *)&ctl->radio)->sin_len = sizeof(struct sockaddr_in);
  else
    ((struct sockaddr_in6 *)&ctl->radio)->sin6_len = sizeof(struct sockaddr_in6);
#endif
  if(getportnumber(&ctl->radio) <= 0)
    setportnumber(&ctl->radio,DEFAULT_STAT_PORT); // radiod listens for commands on its status group (radio.c, Ctl_fd)
  ctl->blocktime = Default_blocktime;
  ctl->fd = output_mcast(&ctl->radio,iface,ttl,-1);
  if(ctl->fd == -1){
    int const e = errno;
    FREE(ctl);
    errno = e;
    return NULL;
  }
  return ctl;
}

void chanctl_destroy(struct chanctl *ctl){
  if(ctl == NULL)
    return;
  int64_t const now = now_ns();
  while(ctl->entries != NULL){
    struct entry * const e = ctl->entries;
    if(e->kind == OWN && e->state != CHANCTL_RELEASING)
      send_command(ctl,e,RELEASE,now);
    remove_entry(ctl,e);
  }
  while(ctl->seen != NULL){
    struct seen *s = ctl->seen;
    ctl->seen = s->next;
    FREE(s);
  }
  close(ctl->fd);
  FREE(ctl);
}

void chanctl_observe(struct chanctl *ctl,struct frontend const *frontend,chan_t const *chan){
  if(ctl == NULL || frontend == NULL || chan == NULL)
    return;
  ctl->observing = true;
  // Every status packet carries its radiod's command group (radio_status.c, STATUS_DEST_SOCKET);
  // ignore other radiods sharing the data group
  if(address_match(&frontend->metadata_dest_socket,&ctl->radio) != 1
     || getportnumber(&frontend->metadata_dest_socket) != getportnumber(&ctl->radio))
    return;
  int64_t const now = now_ns();
  if(frontend->samprate > 0 && frontend->L > 0)
    ctl->blocktime = (double)frontend->L / frontend->samprate; // as radio.c sets Blocktime
  if(frontend->max_IF > frontend->min_IF && frontend->frequency > 0){
    ctl->fe_known = true;
    ctl->fe_frequency = frontend->frequency;
    ctl->fe_min_IF = frontend->min_IF;
    ctl->fe_max_IF = frontend->max_IF;
  }
  uint32_t const ssrc = chan->output.rtp.ssrc;
  if(ssrc == 0 || ssrc == 0xffffffffu)
    return; // front end only

  struct seen *s = find_seen(ctl,ssrc,0);
  if(s == NULL){
    if(ctl->nseen >= Max_seen){
      // Recycle the stalest
      struct seen *oldest = ctl->seen;
      for(struct seen *p = ctl->seen; p != NULL; p = p->next)
	if(p->when < oldest->when)
	  oldest = p;
      s = oldest;
    } else {
      s = calloc(1,sizeof *s);
      if(s == NULL)
	return;
      s->next = ctl->seen;
      ctl->seen = s;
      ctl->nseen++;
    }
  }
  struct entry * const e = find_entry(ctl,ssrc);
  if(s->ssrc != ssrc)
    s->gone = false; // a new record (or a recycled one)
  s->ssrc = ssrc;
  s->frequency = chan->tune.freq;
  s->have_format = (chanctl_format(&s->format,frontend,chan) == 0);
  s->lifetime = chan->lifetime * ctl->blocktime;
  s->when = now;
  if(e != NULL && e->kind == OWN && e->state == CHANCTL_RELEASING)
    s->gone = true;
  else if(chan->lifetime > 1)
    s->gone = false; // running: ours again, or someone else's

  if(e == NULL)
    return;
  bool const same = s->have_format && same_form(e->frequency,&e->format,s->frequency,&s->format);
  switch(e->state){
  case CHANCTL_PENDING:
    if(same && e->kind == OWN && e->format.pt > 0 && chan->output.rtp.type != e->format.pt && e->pt_fixes < 2){
      // radiod skips the payload type update when a command also restarts the demodulator, as a new
      // channel's first command does when its sample rate differs from radiod's template
      // (radio_status.c decode_radio_commands() returns before it), so the stream keeps the template's type.
      // Two encoding changes, one per command, make it recompute it
      // Our last command has been executed if its tag is echoed, so the queue has room for two
      if((uint32_t)chan->status.tag == e->tag || now - e->last_send >= Min_gap){
	send_command(ctl,e,OTHER_ENCODING,now);
	send_command(ctl,e,ENCODING,now);
	e->pt_fixes++;
      }
      break;
    }
    if(same){
      e->state = CHANCTL_UP;
      e->first_request = 0;
      e->retry = First_retry;
    }
    break;
  case CHANCTL_UP:
    if(!same && e->kind == OWN){
      // Changed under us (or recreated from radiod's template); ask again
      e->state = CHANCTL_PENDING;
      e->first_request = now;
      e->retry = First_retry;
      e->last_send = 0;
    }
    break;
  case CHANCTL_RELEASING:
    // A channel that has taken its release shows a lifetime of a block or less as it goes
    if(e->kind == OWN && chan->lifetime > 1 && e->release_tries < Release_tries && now - e->last_send >= Release_retry){
      send_command(ctl,e,RELEASE,now);
      e->release_tries++;
    }
    break;
  default:
    break;
  }
}

int chanctl_request(struct chanctl *ctl,uint32_t ssrc,double frequency,struct chanctl_format const *format,double lifetime){
  if(ctl == NULL || format == NULL || ssrc == 0 || ssrc == 0xffffffffu
     || !isfinite(frequency) || frequency <= 0 || !isfinite(lifetime) || lifetime < 0
     || format->samprate <= 0 || (format->channels != 1 && format->channels != 2)
     || (format->data.ss_family != AF_INET && format->data.ss_family != AF_INET6)){
    errno = EINVAL;
    return -1;
  }
  int64_t const now = now_ns();
  maintain(ctl,now);
  if(!covered(ctl,frequency,format)){
    errno = ERANGE;
    return -1;
  }
  struct entry *e = find_entry(ctl,ssrc);
  if(e == NULL){
    // Is it someone else's?
    struct seen const *s = find_seen(ctl,ssrc,now);
    if(s != NULL && !alive(s,now,1))
      s = NULL; // its lifetime has run out since
    if(s != NULL && !(s->have_format && same_form(frequency,format,s->frequency,&s->format))){
      errno = EEXIST;
      return -1;
    }
    if(s != NULL && !alive(s,now,0.5))
      s = NULL; // may be about to go; make our own (the same command, so harmless if it's still there)
    e = calloc(1,sizeof *e);
    if(e == NULL)
      return -1;
    e->ssrc = ssrc;
    e->frequency = frequency;
    e->format = *format;
    e->lifetime = lifetime;
    e->retry = First_retry;
    e->next = ctl->entries;
    ctl->entries = e;
    if(s != NULL){
      // Already there in the form we want: use it rather than fight over it
      e->kind = (s->lifetime == 0) ? STATIC : SHARED;
      e->state = CHANCTL_UP;
      e->last_send = now; // nothing sent, but don't poll it right away
      e->last_wanted = now;
      return 0;
    }
    e->kind = OWN;
    e->pt_fixes = 0;
    e->state = CHANCTL_PENDING;
    e->first_request = now;
    e->last_wanted = now;
    return send_command(ctl,e,FULL,now);
  }
  e->last_wanted = now;
  if(e->kind != OWN){
    if(!same_form(frequency,format,e->frequency,&e->format)){
      // Wants something else now; it isn't ours to change
      errno = EEXIST;
      return -1;
    }
    return keep_alive(ctl,e,now);
  }
  bool const changed = !same_form(frequency,format,e->frequency,&e->format);
  e->lifetime = lifetime;
  if(changed || e->state == CHANCTL_RELEASING){
    e->frequency = frequency;
    e->format = *format;
    e->state = CHANCTL_PENDING;
    e->first_request = now;
    e->retry = First_retry;
    e->release_tries = 0;
    e->pt_fixes = 0;
    if(now - e->last_send < Min_gap){
      e->retry = Min_gap;
      return 0; // the next call sends it
    }
    return send_command(ctl,e,FULL,now);
  }
  return keep_alive(ctl,e,now);
}

int chanctl_refresh(struct chanctl *ctl,uint32_t ssrc){
  if(ctl == NULL){
    errno = EINVAL;
    return -1;
  }
  int64_t const now = now_ns();
  maintain(ctl,now);
  struct entry * const e = find_entry(ctl,ssrc);
  if(e == NULL || e->state == CHANCTL_RELEASING){
    errno = ENOENT;
    return -1;
  }
  e->last_wanted = now;
  return keep_alive(ctl,e,now);
}

int chanctl_release(struct chanctl *ctl,uint32_t ssrc){
  if(ctl == NULL){
    errno = EINVAL;
    return -1;
  }
  int64_t const now = now_ns();
  maintain(ctl,now);
  struct entry * const e = find_entry(ctl,ssrc);
  if(e == NULL){
    errno = ENOENT;
    return -1;
  }
  if(e->kind != OWN){
    remove_entry(ctl,e); // not ours to kill; it lives or expires as its owner decides
    return 0;
  }
  if(e->state == CHANCTL_RELEASING)
    return 0;
  if(e->lifetime > 0 && now - e->last_send > (int64_t)(e->lifetime * 1e9)){
    // radiod has let it go by now (as in keep_alive()): a release would only make radiod create
    // a channel from its template, to delete it again
    remove_entry(ctl,e);
    return 0;
  }
  struct seen * const s = find_seen(ctl,ssrc,0);
  if(s != NULL)
    s->gone = true;
  e->state = CHANCTL_RELEASING;
  e->released = now;
  e->release_tries = 1;
  // Sent even inside Min_gap: radiod queues two commands per channel
  return send_command(ctl,e,RELEASE,now);
}

enum chanctl_state chanctl_state(struct chanctl *ctl,uint32_t ssrc,uint32_t *tag,double *pending){
  if(tag != NULL)
    *tag = 0;
  if(pending != NULL)
    *pending = 0;
  if(ctl == NULL)
    return CHANCTL_NONE;
  int64_t const now = now_ns();
  maintain(ctl,now);
  struct entry const * const e = find_entry(ctl,ssrc);
  if(e == NULL)
    return CHANCTL_NONE;
  if(tag != NULL)
    *tag = e->tag;
  if(pending != NULL && e->state == CHANCTL_PENDING)
    *pending = (now - e->first_request) * 1e-9;
  return e->state;
}

uint32_t chanctl_ssrc(struct chanctl *ctl,double frequency,struct chanctl_format const *format){
  if(!isfinite(frequency) || frequency <= 0)
    return 0;
  // radio.c: ssrc = frequency in kHz, rounded, incremented past collisions up to 100 times
  uint32_t ssrc = (uint32_t)lrint(frequency / 1000.0);
  if(ctl == NULL)
    return ssrc;
  int64_t const now = now_ns();
  for(int i=0; i < 100; i++,ssrc++){
    if(ssrc == 0 || ssrc == 0xffffffffu)
      continue;
    struct entry const * const e = find_entry(ctl,ssrc);
    if(e != NULL){
      if(fabs(e->frequency - frequency) < 0.5)
	return ssrc; // ours already
      continue;
    }
    struct seen const * const s = find_seen(ctl,ssrc,now);
    if(s == NULL || !alive(s,now,1))
      return ssrc; // free
    if(fabs(s->frequency - frequency) < 0.5 && (format == NULL || (s->have_format && same_form(frequency,format,s->frequency,&s->format))))
      return ssrc; // already there as we want it
  }
  return 0;
}

static int64_t now_ns(void){
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC,&ts);
  return ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static struct entry *find_entry(struct chanctl *ctl,uint32_t ssrc){
  for(struct entry *e = ctl->entries; e != NULL; e = e->next)
    if(e->ssrc == ssrc)
      return e;
  return NULL;
}

// now == 0: regardless of age
static struct seen *find_seen(struct chanctl *ctl,uint32_t ssrc,int64_t now){
  for(struct seen *s = ctl->seen; s != NULL; s = s->next){
    if(s->ssrc == ssrc)
      return (now == 0 || now - s->when < Seen_valid) ? s : NULL;
  }
  return NULL;
}

// Seen recently enough, as a fraction of the lifetime it showed, to be still there
static bool alive(struct seen const *s,int64_t now,double fraction){
  if(s->gone)
    return false;
  if(s->lifetime <= 0)
    return now - s->when < Seen_valid;
  return now - s->when < (int64_t)(fraction * s->lifetime * 1e9);
}

static void remove_entry(struct chanctl *ctl,struct entry *e){
  for(struct entry **pp = &ctl->entries; *pp != NULL; pp = &(*pp)->next){
    if(*pp == e){
      *pp = e->next;
      FREE(e);
      return;
    }
  }
}

// Forget released channels, and ones nobody has asked for since radiod would have let them go
static void maintain(struct chanctl *ctl,int64_t now){
  struct entry *next;
  for(struct entry *e = ctl->entries; e != NULL; e = next){
    next = e->next;
    if(e->state == CHANCTL_RELEASING){
      if(now - e->released > Release_hold)
	remove_entry(ctl,e);
      continue;
    }
    if(e->kind == STATIC)
      continue;
    if(e->lifetime > 0 && now - e->last_wanted > (int64_t)(e->lifetime * 1e9) && now - e->last_send > (int64_t)(e->lifetime * 1e9))
      remove_entry(ctl,e);
  }
}

static bool same_form(double f1,struct chanctl_format const *a,double f2,struct chanctl_format const *b){
  if(fabs(f1 - f2) >= 0.5)
    return false;
  if(a->demod_type != b->demod_type || a->samprate != b->samprate || a->channels != b->channels || a->encoding != b->encoding)
    return false;
  if(fabsf(a->low - b->low) >= 1 || fabsf(a->high - b->high) >= 1)
    return false;
  if(address_match(&a->data,&b->data) != 1)
    return false;
  if(a->demod_type == LINEAR_DEMOD){
    if(a->agc != b->agc || a->pll != b->pll || a->envelope != b->envelope || a->isb != b->isb)
      return false;
    if(!a->agc && fabsf(a->gain - b->gain) >= 0.1f)
      return false;
  }
  return true;
}

// Would radiod have to retune the front end for this? (radio.c set_freq())
static bool covered(struct chanctl const *ctl,double frequency,struct chanctl_format const *format){
  if(!ctl->fe_known)
    return true;
  double const new_if = frequency - ctl->fe_frequency;
  return !(new_if > ctl->fe_max_IF - format->high || new_if < ctl->fe_min_IF - format->low);
}

// Seconds to radiod's LIFETIME, which counts blocks (radio.c downconvert())
static int frames(struct chanctl const *ctl,double seconds){
  if(seconds <= 0)
    return 0;
  long const n = lrint(seconds / ctl->blocktime);
  return n < 2 ? 2 : (n > INT32_MAX ? INT32_MAX : (int)n); // 1 would release it
}

static int64_t refresh_interval(struct chanctl const *ctl,struct entry const *e){
  (void)ctl;
  if(e->lifetime <= 0)
    return INT64_MAX; // never expires, never needs refreshing
  // A third of the lifetime, spread +/-10% by the last tag so clients don't fall into step (docs/ka9q-api.md)
  int64_t r = (int64_t)(e->lifetime * 1e9 / 3 * (0.9 + 0.2 * (e->tag & 0xffff) / 65535.0));
  if(r < 2 * Min_gap)
    r = 2 * Min_gap;
  return r;
}

// Rate-limited: resend an unanswered request, or refresh a live channel when due
static int keep_alive(struct chanctl *ctl,struct entry *e,int64_t now){
  if(e->kind == STATIC)
    return 0;
  if(now - e->last_send < Min_gap)
    return 0;
  if(e->state == CHANCTL_PENDING){
    if(now - e->last_send < e->retry)
      return 0;
    if(!ctl->observing && now - e->first_request >= Max_retry){
      // Nobody shows us status, so we can't confirm; assume it worked and just keep it alive
      e->state = CHANCTL_UP;
      e->first_request = 0;
      return keep_alive(ctl,e,now);
    }
    e->retry = (2 * e->retry > Max_retry) ? Max_retry : 2 * e->retry;
    return send_command(ctl,e,FULL,now);
  }
  if(e->lifetime > 0 && now - e->last_send > (int64_t)(e->lifetime * 1e9)){
    // radiod has let it go by now; a refresh would only create a channel from its template
    if(e->kind != OWN){
      remove_entry(ctl,e);
      errno = ENOENT;
      return -1;
    }
    e->state = CHANCTL_PENDING;
    e->first_request = now;
    e->retry = First_retry;
    return send_command(ctl,e,FULL,now);
  }
  if(now - e->last_send < refresh_interval(ctl,e))
    return 0;
  if(e->kind == SHARED){
    // Poll only while it's surely still there: a command to a missing SSRC creates a channel from radiod's
    // template (radio_status.c). If it may be gone, its owner has let it go; make it ours
    struct seen const * const s = find_seen(ctl,e->ssrc,now);
    if(s != NULL && alive(s,now,0.5))
      return send_command(ctl,e,POLL,now);
    e->kind = OWN;
    e->state = CHANCTL_PENDING;
    e->first_request = now;
    e->retry = First_retry;
    return send_command(ctl,e,FULL,now);
  }
  return send_command(ctl,e,KEEPALIVE,now);
}

// The order matters: radiod applies fields in packet order (radio_status.c decode_radio_commands())
static int send_command(struct chanctl *ctl,struct entry *e,int what,int64_t now){
  uint8_t buffer[PKTSIZE];
  uint8_t *bp = buffer;
  *bp++ = CMD;
  uint32_t tag;
  do {
    tag = arc4random();
  } while(tag == 0);
  encode_int32(&bp,COMMAND_TAG,tag);
  encode_int32(&bp,OUTPUT_SSRC,e->ssrc);
  struct chanctl_format const * const f = &e->format;
  switch(what){
  case FULL:
    encode_int(&bp,DEMOD_TYPE,f->demod_type);
    encode_int(&bp,OUTPUT_SAMPRATE,f->samprate); // before the edges, which are clipped to it
    encode_int(&bp,OUTPUT_CHANNELS,f->channels);
    encode_int(&bp,OUTPUT_ENCODING,f->encoding); // after the sample rate, which opus checks
    if(f->kaiser_beta > 0)
      encode_float(&bp,KAISER_BETA,f->kaiser_beta);
    // An edge beyond the other current edge is ignored, so low, high, low reaches any pair from any start
    encode_float(&bp,LOW_EDGE,f->low);
    encode_float(&bp,HIGH_EDGE,f->high);
    encode_float(&bp,LOW_EDGE,f->low);
    encode_int(&bp,FILTER2,f->filter2);
    encode_double(&bp,SHIFT_FREQUENCY,f->shift); // before the frequency: a shift change retunes relative to it
    encode_double(&bp,RADIO_FREQUENCY,e->frequency);
    if(f->demod_type == LINEAR_DEMOD){
      encode_bool(&bp,PLL_ENABLE,f->pll);
      if(f->pll){
	encode_bool(&bp,PLL_SQUARE,f->pll_square);
	encode_float(&bp,PLL_BW,f->pll_bw);
      }
      encode_bool(&bp,ENVELOPE,f->envelope);
      encode_bool(&bp,INDEPENDENT_SIDEBAND,f->isb);
      if(f->agc){
	encode_bool(&bp,AGC_ENABLE,true);
	encode_float(&bp,HEADROOM,f->headroom);
	encode_float(&bp,AGC_HANGTIME,f->hangtime);
	encode_float(&bp,AGC_THRESHOLD,f->threshold);
	encode_float(&bp,AGC_RECOVERY_RATE,f->recovery_rate);
      } else {
	encode_float(&bp,GAIN,f->gain); // also turns AGC off
	encode_bool(&bp,AGC_ENABLE,false);
      }
    } else if(f->demod_type == FM_DEMOD){
      encode_bool(&bp,THRESH_EXTEND,f->threshold_extend);
    }
    if(f->demod_type != SPECT_DEMOD && f->demod_type != SPECT2_DEMOD){
      encode_bool(&bp,SNR_SQUELCH,f->snr_squelch);
      if(isfinite(f->squelch_open) && isfinite(f->squelch_close)){
	encode_float(&bp,SQUELCH_OPEN,f->squelch_open);
	encode_float(&bp,SQUELCH_CLOSE,f->squelch_close);
      }
    }
    encode_int(&bp,MAXDELAY,f->maxdelay);
    encode_int(&bp,STATUS_INTERVAL,f->status_interval);
    encode_socket(&bp,OUTPUT_DATA_DEST_SOCKET,&f->data);
    encode_int(&bp,LIFETIME,frames(ctl,e->lifetime));
    break;
  case KEEPALIVE:
    // Any command reloads the lifetime; this one also sets it, so a channel that was lost and is recreated
    // from radiod's template by it doesn't live forever
    encode_int(&bp,LIFETIME,frames(ctl,e->lifetime));
    break;
  case POLL:
    break; // SSRC and tag only: reloads the owner's lifetime without changing it
  case RELEASE:
    encode_int(&bp,LIFETIME,1); // gone at the next block
    break;
  case OTHER_ENCODING:
    encode_int(&bp,OUTPUT_ENCODING,f->encoding == S16BE ? S16LE : S16BE);
    break;
  case ENCODING:
    encode_int(&bp,OUTPUT_ENCODING,f->encoding);
    break;
  }
  encode_eol(&bp);
  size_t const len = bp - buffer;
  socklen_t const slen = ctl->radio.ss_family == AF_INET ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6);
  e->last_send = now; // even if it fails, so a dead network isn't hammered
  e->tag = tag;
  if(sendto(ctl->fd,buffer,len,0,(struct sockaddr *)&ctl->radio,slen) != (ssize_t)len)
    return -1;
  return 1;
}
