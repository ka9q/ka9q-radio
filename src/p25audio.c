// Voice output for the P25 decoder: IMBE and AMBE+2 vocoder (mbelib, optional), and per channel
// an RTP speech stream and an RTP vocoder frame stream, each with radiod-style status, so monitor
// plays the speech and pcmrecord records it, and a user's own vocoder can take the frames
// Copyright 2026, Matthew Kaufman, KA6SQG

#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#ifdef HAVE_MBELIB
#include <mbelib.h>
#endif

#include "misc.h"
#include "multicast.h"
#include "rtp.h"
#include "status.h"
#include "timing.h"
#include "avahi.h"
#include "p25msg.h"
#include "p25p1msg.h"
#include "p25p2mac.h"
#include "p25audio.h"

// Theory of operation
//
// What comes in. Phase 1 data units (p25_audio_unit()) and Phase 2 bursts of one timeslot
// (p25_audio_burst()), each with the time it ended on the IQ sender's clock and, where the
// channel has trunking logic, that logic's label for a voice unit. A voice unit is an LDU (nine
// IMBE frames, 180 ms) or a 4V or 2V burst (four or two AMBE+2 frames). What goes out, per
// channel: speech, 8 kHz PCM decoded here, and the vocoder frames as received, on two RTP
// streams with the same SSRC, timestamps and marker bits for the same frames.
//
// The rule. Nothing from an encrypted voice unit, or from one not yet known to be clear, is
// decoded to speech or sent as a clear frame (p25audio.h states it for callers). It is kept as
// a small state per channel, reset by every end of transmission:
//  - 'clear': a word that passed error correction said ALGID 0x80 in this transmission: a
//    header, an encryption sync, a MAC PTT or an ESS (got_crypto()). Only 0x80 counts; 0x00
//    is encrypted. p25_decode_es() reports an all-zero LDU2 encryption sync as 0x80.
//  - 'encrypted': a word said otherwise: such a word with another ALGID, link control marked
//    protected, or a voice channel user word (link control or MAC) with the service options'
//    encryption bit. It stays set for the rest of the transmission, whatever comes after.
//  - the held queue: voice units that may not go yet, in order of arrival, each with its own
//    time, position and encryption parameters, and whether the trunking logic has passed it
//    ('trunk_clear': labelled clear, or the channel has no such logic).
// A unit arriving goes out at once only when clear, not encrypted, passed by the trunking logic
// and nothing older is waiting; otherwise it is held. The queue is sent, front first, as long
// as its front unit is passed and the state is clear and not encrypted (flush_held()). Units
// sent late keep their original timestamps: the speech is the same, only later. The queue is
// bounded (P25_AUDIO_MAX_HELD units, P25_AUDIO_MAX_HELD_FRAMES frames); when full, the oldest
// is dropped.
//
// What un-confirms. Becoming encrypted: at the next voice unit what is held goes out as
// encrypted frames on a frame stream that asked for them, and is otherwise dropped, and no
// speech follows. Link control or a MAC voice channel user word naming another talkgroup, unit
// or talker than the transmission's means one ended and another began unseen: the transmission
// is ended there. Two exceptions on Phase 2. A MAC PTT for an individual call (unit to unit or
// telephone) carries the reserved group 0 and the talker as its source (TIA-102.BBAC 7.3.1;
// TIA-102.BBAD-A 2.3.3.1): it names no talkgroup, so it gives the source and the encryption only
// and the call's identity comes from the voice channel user that follows, which then names no
// other call. And the outbound SACCH of the last superframe of an ultraframe names the inbound
// talker, who may not be the source of the audio sent (TIA-102.BBAC-A 3.4): a voice channel user
// there, or in a SACCH whose place in the ultraframe is not known, gives no identity and no new
// talker; its encryption bit still counts.
//
// What is let through. A fade shorter than Hang does not end the transmission and does not
// withdraw 'clear'. If it hid the end of a clear transmission and the start of an encrypted one
// (terminator and header or PTT all lost, and no word naming another talker decoded yet), the
// new one is decoded until its first encryption sync or ESS says encrypted: at most one
// superframe, 18 frames (360 ms), more only while those words fail too. Likewise units held
// unconfirmed before a fade are sent if a word after it says clear. That burst at a weak signal
// is accepted, for simplicity. The encryption parameters carried by frames after a fade are the
// last received, flagged fresh only when they came just before the frames' superframe.
//
// What ends a transmission (end_transmission(): held units dropped, streams closed): Phase 1
// a header (it starts a new one), TDU or TDULC; Phase 2 a MAC PTT (likewise), END PTT, IDLE or
// HANGTIME, or a change of audio source (below); a new talker as above; a voice unit overdue by
// more than Hang (p25_audio_poll()); and the trunking logic ending the call. An END PTT whose
// color code is not the NAC the receiver descrambles with (struct p25p2_burst color_code) is
// another system's, sent unscrambled so that it can be told apart (TIA-102.BBAD-A 4.10;
// TIA-102.BBAC 7.3.3), and ends nothing; with no NAC known yet every END PTT counts.
//
// The trunking logic's labels act on the unit alone: NONE drops it, ENCRYPTED treats it and
// what is held as encrypted without setting 'encrypted' (the logic may change its mind), HOLD
// holds it until p25_audio_release() says which.
//
// Encryption parameters for the frame stream. ALGID, KID and MI received in one superframe
// (Phase 1: an LDU1 and LDU2 pair; Phase 2: a voice cycle of 18 frames) are the ones for the
// next, so each frame carries the parameters last received before its superframe began,
// flagged fresh if they came just before it (Phase 1: one voice unit before; Phase 2: in the
// previous cycle, or a PTT).
//
// The vocoder. IMBE frames are decoded with mbelib's parameter decoding and synthesis, but
// with the frame repeat and mute rules of TIA-102.BABA applied here from the error counts of
// the channel decoder (c0's, and the total over c0-c6), in place of mbelib's own. AMBE+2 frames
// go through mbelib's own rules. A lost frame is repeated, and after Max_repeats repeats in a
// row the output is muted.
//
// Phase 2 voice framing. A timeslot's voice bursts go 4V 4V 4V 4V 2V from the start of the talk
// spurt to its end, without regard to the SACCH (TIA-102.BBAC-A 6.1); where the 2V falls among
// the five voice positions places every frame in time and in its voice cycle, whose ESS gives
// the next cycle's encryption parameters. It is learned, in order of trust, from the offset
// field of a MAC PDU that passed its CRC (TIA-102.BBAD-A 4.2, p25p2_offset_2v(): every SACCH
// and FACCH PDU carries it, a PTT included), from the first voice burst after a PTT that gave
// none, or from the first 2V (a receiver may take the offset field or a 2V, whichever comes
// first: TIA-102.BBAC 7.2.4.2.2). Once known it stays for the transmission. A 2V seen elsewhere
// does not move it: it is a misread DUID or a change of source not seen, and moving would
// misplace every frame after a misread, while a change of source is signalled by a MAC PDU with
// the new offset at the change (TIA-102.BBAD-A 4.2) and repeated in every SACCH, so it is put
// right within a superframe; the encryption rule does not depend on the placing (a 2V's ESS
// counts at once). An offset field giving another framing than one an offset field gave is
// that change of source (a console interrupting, another unit repeated): the audio is another's
// and its encryption is not known, so the transmission is ended there as at a PTT. Against a
// framing learned from a burst it corrects the guess and places again what is held.
//
// RTP. A transmission's first timestamp is the sender's clock, at 8 kHz, at the first frame's
// start, so a recording lines up with the IQ; then 160 per frame, one packet per frame. The
// first packet carries the marker bit, and a status packet goes out before it and after every
// Status_interval data packets, as radiod sends them. Frames lost within a transmission are
// filled (repeated then muted in speech, flagged lost in frames) so the timestamps stay
// continuous; a gap longer than Hang, in frames or in sender time, starts a new transmission
// instead. Sequence numbers are kept per SSRC in the shared group, so a talkgroup's stream stays
// continuous when its calls move between channels.

// One vocoder frame is 20 ms; the RTP clock is the sender's clock at 8 kHz
#define NS_PER_SAMPLE (BILLION / P25_AUDIO_SAMPRATE)          // 125,000
#define FRAME_NS ((int64_t)P25_AUDIO_FRAME * NS_PER_SAMPLE)   // 20 ms
#define LDU_NS (P25_IMBE_FRAMES * FRAME_NS)                   // 180 ms
#define LDU_DIBITS (P25_SYMRATE * LDU_NS / BILLION)          // 864 dibits from one voice data unit to the next
#define MAX_UNIT_FRAMES P25_IMBE_FRAMES                       // a Phase 2 voice burst has at most 4

// Phase 2: a superframe is 12 bursts of 180 dibits, six per timeslot: five voice bursts (four 4V
// and one 2V, 18 frames) and a SACCH
#define P2_SLOT_DIBITS 180
#define P2_SUPERFRAME_DIBITS (P25P2_SUPERFRAME_SLOTS * P2_SLOT_DIBITS) // 2160, 360 ms
#define P2_CYCLE_FRAMES 18

// A gap in voice longer than this ends the transmission. Within it lost frames are filled: on
// Phase 1 up to two lost voice units (360 ms); a third makes 540 ms and starts a new transmission
static int64_t const Hang = BILLION / 2;
static int const Status_interval = 25;    // Data packets between status packets: radiod's default 'update' (defaults.h)
#ifdef HAVE_MBELIB
static int const Max_repeats = 3;         // Consecutive frame repeats before muting, as mbelib and jmbe do
static int const Uvquality = 3;           // mbelib unvoiced synthesis quality, as its own programs use
static float const Mbe_gain = 7;          // mbelib's float output to 16-bit PCM, as its mbe_floattoshort()
#endif

// ---------------------------------------------------------------------------
// Vocoder

struct p25_vocoder {
  enum p25_codec codec;
  float error_rate;        // IMBE: epsilon R of TIA-102.BABA
  int repeats;             // IMBE: consecutive frames repeated
#ifdef HAVE_MBELIB
  mbe_parms cur,prev,enhanced;
  char last[P25P2_AMBE_INFO_BITS]; // AMBE+2: the last frame, repeated for a lost one
#endif
};

char const *p25_vocoder_name(enum p25_codec codec){
#ifdef HAVE_MBELIB
  if(codec == P25_CODEC_IMBE || codec == P25_CODEC_AMBE2)
    return "mbelib " MBELIB_VERSION;
#endif
  (void)codec;
  return NULL;
}

struct p25_vocoder *p25_vocoder_create(enum p25_codec codec){
  if(p25_vocoder_name(codec) == NULL)
    return NULL;
  struct p25_vocoder *v = calloc(1,sizeof *v);
  if(v == NULL)
    return NULL;
  v->codec = codec;
  p25_vocoder_reset(v);
  return v;
}

void p25_vocoder_reset(struct p25_vocoder *v){
  if(v == NULL)
    return;
  v->error_rate = 0;
  v->repeats = 0;
#ifdef HAVE_MBELIB
  mbe_initMbeParms(&v->cur,&v->prev,&v->enhanced);
  memset(v->last,0,sizeof v->last);
#endif
}

void p25_vocoder_destroy(struct p25_vocoder *v){
  FREE(v);
}

#ifdef HAVE_MBELIB
static void to_pcm(int16_t pcm[P25_AUDIO_FRAME],float const f[P25_AUDIO_FRAME]){
  for(int i=0; i < P25_AUDIO_FRAME; i++){
    float const s = Mbe_gain * f[i];
    pcm[i] = (int16_t)(s > 32767 ? 32767 : s < -32767 ? -32767 : lrintf(s));
  }
}

// AMBE+2 through mbelib's own frame rules (repeat when errs2 > 3, mute after three repeats). A
// lost frame is the last one again with errs2 4, which mbelib repeats
static enum p25_voice ambe_decode(struct p25_vocoder *v,int16_t pcm[P25_AUDIO_FRAME],uint8_t const *bits,int errs,int errs2){
  if(bits != NULL){
    for(int i=0; i < P25P2_AMBE_INFO_BITS; i++)
      v->last[i] = (char)(bits[i] & 1);
  } else {
    errs = 4;
    errs2 = 4;
  }
  char err_str[64];
  if(errs2 > (int)sizeof err_str - 8)
    errs2 = sizeof err_str - 8; // mbelib writes one character per error
  float f[P25_AUDIO_FRAME];
  memset(f,0,sizeof f);
  mbe_processAmbe2450Dataf(f,&errs,&errs2,err_str,v->last,&v->cur,&v->prev,&v->enhanced,Uvquality);
  to_pcm(pcm,f);
  if(v->cur.repeat > Max_repeats)
    return P25_VOICE_MUTED;
  return v->cur.repeat > 0 ? P25_VOICE_REPEAT : P25_VOICE_SPEECH;
}
#endif

// IMBE, TIA-102.BABA: eR = 0.95 eR + 0.000365 eT; repeat the previous frame when e0 >= 2 and
// eT >= 10 + 40 eR (or when the fundamental frequency is invalid); mute while eR > 0.0875,
// and after more than three repeats in a row
// 0.000365 is 0.05 / 137, 137 being the bits under c0-c6's codes, so eR is a one-pole average
// over about 20 frames of the fraction of protected bits corrected: an estimate of the channel's
// bit error rate, muting above 8.75%. The repeat rule wants two or more errors in c0 (whose
// data also seeds the mask of c1-c6) and a total above a threshold that rises with the rate
enum p25_voice p25_vocoder_decode(struct p25_vocoder *v,int16_t pcm[P25_AUDIO_FRAME],uint8_t const *bits,int e0,int total){
  assert(v != NULL && pcm != NULL);
#ifdef HAVE_MBELIB
  if(v->codec == P25_CODEC_AMBE2)
    return ambe_decode(v,pcm,bits,e0,total);
#endif
  bool repeat = true; // a lost frame is repeated
  if(bits != NULL){
    v->error_rate = 0.95f * v->error_rate + 0.000365f * total;
    repeat = e0 >= 2 && total >= 10 + 40 * v->error_rate;
  }
  bool const mute = v->error_rate > 0.0875f;
#ifdef HAVE_MBELIB
  if(!repeat && !mute){
    char d[P25_IMBE_INFO_BITS]; // u0 .. u7, each msb first, which is mbelib's imbe_d[] order
    for(int i=0; i < P25_IMBE_INFO_BITS; i++)
      d[i] = (char)(bits[i] & 1);
    if(mbe_decodeImbe4400Parms(d,&v->cur,&v->prev) != 0)
      repeat = true; // invalid fundamental or harmonic count
  }
  if(repeat){
    mbe_useLastMbeParms(&v->cur,&v->prev);
    v->repeats++;
  } else
    v->repeats = 0;
  if(mute || v->repeats > Max_repeats){
    memset(pcm,0,P25_AUDIO_FRAME * sizeof pcm[0]);
    mbe_initMbeParms(&v->cur,&v->prev,&v->enhanced);
    return P25_VOICE_MUTED;
  }
  // The rest of mbe_processImbe4400Dataf(), whose own repeat rule is 'more than 5 bits corrected'
  float f[P25_AUDIO_FRAME];
  mbe_moveMbeParms(&v->cur,&v->prev);
  mbe_spectralAmpEnhance(&v->cur);
  mbe_synthesizeSpeechf(f,&v->cur,&v->enhanced,Uvquality);
  mbe_moveMbeParms(&v->cur,&v->enhanced);
  to_pcm(pcm,f);
  return repeat ? P25_VOICE_REPEAT : P25_VOICE_SPEECH;
#else
  (void)repeat; (void)mute;
  memset(pcm,0,P25_AUDIO_FRAME * sizeof pcm[0]);
  return P25_VOICE_MUTED;
#endif
}

// ---------------------------------------------------------------------------
// Output group, shared by every channel

struct p25_audio_group {
  int fd;                               // unconnected: data and status leave from the same port, as from radiod
  struct sockaddr_storage data;         // group:5004
  struct sockaddr_storage status;       // group:5006
  int ttl;
  bool frames;                          // vocoder frames, else speech
  bool encrypted;                       // frames: also those of encrypted voice units
  uint8_t pt;                           // RTP payload type
  atomic_flag send_error_logged;
  // RTP sequence numbers by SSRC. A trunked talkgroup's stream moves between channels (each its
  // own struct p25_audio), so its numbering is kept here to stay continuous for the listener
  pthread_mutex_t lock;
  struct {
    uint32_t ssrc;
    uint16_t seq;
    bool used;
  } seqs[64];
  int next_seq;                         // slot reused next when the table is full
};

static socklen_t socklen(struct sockaddr_storage const *s){
  return s->ss_family == AF_INET6 ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in);
}

static struct p25_audio_group *open_group(char const *target,char const *iface,int ttl,int tos,char const *service_name){
  if(target == NULL || strlen(target) == 0)
    return NULL;
  struct p25_audio_group *g = calloc(1,sizeof *g);
  if(g == NULL)
    return NULL;
  atomic_flag_clear(&g->send_error_logged);
  pthread_mutex_init(&g->lock,NULL);
  // Split "name[:port][,iface]" as resolve_mcast() does
  char host[256];
  strlcpy(host,target,sizeof host);
  char ifname[256] = {0};
  char *cp = strrchr(host,',');
  if(cp != NULL){
    *cp++ = '\0';
    strlcpy(ifname,cp,sizeof ifname);
  }
  int port = DEFAULT_RTP_PORT;
  struct in6_addr v6;
  if(inet_pton(AF_INET6,host,&v6) == 1){
    struct sockaddr_in6 * const sin6 = (struct sockaddr_in6 *)&g->data;
    sin6->sin6_family = AF_INET6;
    sin6->sin6_addr = v6;
#if defined(__APPLE__) || defined(__FreeBSD__)
    sin6->sin6_len = sizeof *sin6; // BSD sockets: without it, joining the group fails with EINVAL
#endif
  } else {
    if((cp = strrchr(host,':')) != NULL){
      *cp++ = '\0';
      port = atoi(cp);
    }
    struct sockaddr_in * const sin = (struct sockaddr_in *)&g->data;
    sin->sin_family = AF_INET;
#if defined(__APPLE__) || defined(__FreeBSD__)
    sin->sin_len = sizeof *sin;
#endif
    if(inet_pton(AF_INET,host,&sin->sin_addr) != 1){
      // A name: hash it into 239.0.0.0/8 and advertise it, as radiod does for its output streams
      uint32_t const addr = make_maddr(host);
      sin->sin_addr.s_addr = htonl(addr);
      if(service_name != NULL){
	char ttlmsg[128];
	snprintf(ttlmsg,sizeof ttlmsg,"TTL=%d",ttl);
	avahi_start(service_name,"_rtp._udp",DEFAULT_RTP_PORT,host,addr,ttlmsg);
      }
    }
  }
  setport(&g->data,port);
  g->status = g->data;
  setport(&g->status,DEFAULT_STAT_PORT);
  g->ttl = ttl;
  if(strlen(ifname) == 0 && iface != NULL)
    strlcpy(ifname,iface,sizeof ifname);
  g->fd = output_mcast(&g->data,strlen(ifname) > 0 ? ifname : NULL,ttl,tos);
  if(g->fd < 0){
    fprintf(stderr,"p25 audio: can't set up output to %s: %s\n",target,strerror(errno));
    pthread_mutex_destroy(&g->lock);
    FREE(g);
    return NULL;
  }
  return g;
}

struct p25_audio_group *p25_audio_open(char const *target,char const *iface,int ttl,int tos,char const *service_name){
  struct p25_audio_group * const g = open_group(target,iface,ttl,tos,service_name);
  if(g == NULL)
    return NULL;
  int const pt = pt_from_info(P25_AUDIO_SAMPRATE,1,S16BE); // 125, a static entry
  if(pt < 0){
    fprintf(stderr,"p25 audio: no RTP payload type for %d Hz mono s16be\n",P25_AUDIO_SAMPRATE);
    p25_audio_close(g);
    return NULL;
  }
  g->pt = (uint8_t)pt;
  return g;
}

struct p25_audio_group *p25_frames_open(char const *target,char const *iface,int ttl,int tos,char const *service_name,bool encrypted){
  struct p25_audio_group * const g = open_group(target,iface,ttl,tos,service_name);
  if(g == NULL)
    return NULL;
  g->frames = true;
  g->encrypted = encrypted;
  g->pt = P25_FRAME_PT;
  return g;
}

void p25_audio_close(struct p25_audio_group *g){
  if(g == NULL)
    return;
  close(g->fd);
  pthread_mutex_destroy(&g->lock);
  FREE(g);
}

// The next sequence number for a stream, and the one after its last packet when it stops
static uint16_t get_seq(struct p25_audio_group *g,uint32_t ssrc){
  uint16_t seq = (uint16_t)random();
  pthread_mutex_lock(&g->lock);
  for(int i=0; i < (int)(sizeof g->seqs / sizeof g->seqs[0]); i++){
    if(g->seqs[i].used && g->seqs[i].ssrc == ssrc){
      seq = g->seqs[i].seq;
      break;
    }
  }
  pthread_mutex_unlock(&g->lock);
  return seq;
}
static void put_seq(struct p25_audio_group *g,uint32_t ssrc,uint16_t seq){
  int const n = (int)(sizeof g->seqs / sizeof g->seqs[0]);
  pthread_mutex_lock(&g->lock);
  int slot = -1;
  for(int i=0; i < n; i++){
    if(g->seqs[i].used && g->seqs[i].ssrc == ssrc){
      slot = i;
      break;
    }
    if(slot < 0 && !g->seqs[i].used)
      slot = i;
  }
  if(slot < 0){
    slot = g->next_seq;
    g->next_seq = (g->next_seq + 1) % n;
  }
  g->seqs[slot].ssrc = ssrc;
  g->seqs[slot].seq = seq;
  g->seqs[slot].used = true;
  pthread_mutex_unlock(&g->lock);
}


// ---------------------------------------------------------------------------
// One channel's streams

// Encryption parameters, as received
struct crypto {
  uint8_t algid;           // 0: none received in this transmission
  uint16_t kid;
  uint8_t mi[9];
  bool fresh;              // received in the unit just before the superframe they are given for
};

// One vocoder frame as received
struct vframe {
  uint8_t bits[P25_IMBE_INFO_BITS]; // information bits, one per byte (AMBE+2: the first 49)
  uint8_t raw[P25_IMBE_BITS];       // the frame as received (AMBE+2: the first 72)
  uint8_t e0,e1,et;                 // the standard's counts: IMBE e0, e4, total; AMBE+2 e0, e1, sum
  uint8_t errs,errs2;               // mbelib's: IMBE e0, total; AMBE+2 data errors in u0, in u0 and u1
  uint8_t position;                 // in the superframe, 0-17
};

// A voice unit (Phase 1 LDU, Phase 2 voice burst) waiting until it may be sent
struct held_unit {
  enum p25_codec codec;
  bool trunk_clear;        // the trunking logic allows it (or the channel has none)
  int64_t when;            // sender's time at its end
  uint64_t pos;            // Phase 1: receiver's dibit count at its end; Phase 2: index of its first frame
  struct crypto crypto;    // the parameters that apply to its frames
  int64_t p2_sf;           // Phase 2: its superframe and voice burst (0-4), to place it again
  int p2_k;
  int nframes;
  struct vframe f[MAX_UNIT_FRAMES];
};

// One RTP stream: speech or frames
struct outstream {
  struct p25_audio_group *group; // NULL: this stream is off
  uint16_t seq;
  uint32_t timestamp;      // RTP timestamp of the next frame
  int64_t next_time;       // sender's time of that frame; timestamp == next_time / NS_PER_SAMPLE
  bool active;             // in a transmission
  bool marker;             // the next packet starts one
  int status_count;        // data packets until the next status
  enum p25_codec codec;    // of the units sent in this transmission
  int64_t last_sent;       // time at the end of the last unit sent, 0 if none in this transmission
  uint64_t last_pos;       // and its position: Phase 1 its last dibit, Phase 2 the index after its last frame
  uint64_t packets;
  uint64_t samples;
  double energy;           // speech: sum of squared samples (full scale 1) since the last status
  int64_t energy_samples;
};

struct p25_audio {
  struct outstream pcm;
  struct outstream frm;
  struct p25_vocoder *vocoder[2]; // by codec; NULL without speech output
  uint32_t channel_ssrc;   // the voice channel's
  uint32_t ssrc;           // the streams': the channel's, or what p25_audio_set_stream() chose

  // What the status carries
  double frequency;
  float bb_power,n0;       // radiod's figures for the channel (p25_audio_set_signal()); 0 until given
  float low,high;          // its filter edges, Hz
  uint16_t nac;
  bool have_call;
  bool unit_call;          // unit to unit: 'target' is a unit, else a talkgroup
  bool telephone;          // a unit call by telephone interconnect: 'target' is the radio in it
  uint32_t target;
  uint32_t source;
  char stream_description[128]; // from p25_audio_set_stream(), "" if none
  char description[128];

  // This transmission's encryption, as this module sees it
  bool clear;              // a header, encryption sync, PTT or ESS said clear (ALGID 0x80)
  bool encrypted;          // something said encrypted: nothing more of the transmission is clear
  struct crypto last;      // the parameters received last
  bool pending_fresh;      // ... for the superframe that follows
  int64_t pending_at;      // where they came: Phase 1 the unit's last dibit, Phase 2 its voice cycle
  struct crypto current;   // those that apply to the superframe in progress
  bool in_superframe;      // Phase 1: an LDU1 began the superframe in progress
  uint64_t ldu1_dibit;     // and where it ended
  int64_t cycle;           // Phase 2: voice cycle of the last burst
  int64_t last_unit;       // time at the end of the last voice unit seen
  struct held_unit held[P25_AUDIO_MAX_HELD]; // oldest first
  int nheld;
  int held_frames;

  // Phase 2 framing: where superframes start, and which of a timeslot's five voice bursts is the
  // 2V. The transmitter's voice cycle (four 4V, then the 2V) is not tied to the superframe (see
  // the top of the file)
  bool p2_ref_set;
  uint64_t p2_ref;
  int p2_k2;
  bool p2_k2_known;        // p2_k2 is this transmission's: fixed until it ends
  bool p2_k2_signalled;    // ... and an offset field gave it
  bool p2_after_ptt;       // a PTT with no framing: the next voice burst begins a cycle

  void (*tap)(void *,int64_t,uint64_t);
  void *tap_arg;

  struct p25_audio_stats stats;
};

struct p25_audio *p25_audio_create(struct p25_audio_group *pcm,struct p25_audio_group *frames,uint32_t ssrc){
  if(pcm != NULL && p25_vocoder_name(P25_CODEC_IMBE) == NULL)
    pcm = NULL; // built without a vocoder
  if(pcm == NULL && frames == NULL)
    return NULL;
  struct p25_audio *a = calloc(1,sizeof *a);
  if(a == NULL)
    return NULL;
  if(pcm != NULL){
    a->vocoder[P25_CODEC_IMBE] = p25_vocoder_create(P25_CODEC_IMBE);
    a->vocoder[P25_CODEC_AMBE2] = p25_vocoder_create(P25_CODEC_AMBE2);
    if(a->vocoder[P25_CODEC_IMBE] == NULL || a->vocoder[P25_CODEC_AMBE2] == NULL){
      p25_vocoder_destroy(a->vocoder[P25_CODEC_IMBE]);
      p25_vocoder_destroy(a->vocoder[P25_CODEC_AMBE2]);
      FREE(a);
      return NULL;
    }
  }
  a->pcm.group = pcm;
  a->frm.group = frames;
  a->channel_ssrc = a->ssrc = ssrc;
  // Where the 2V falls among a timeslot's five voice bursts, as p25p2 numbers the superframe's
  // slots: learned from the first 2V; this is where the real recording has it
  a->p2_k2 = 2;
  a->cycle = INT64_MIN;
  return a;
}

static void end_transmission(struct p25_audio *a);

void p25_audio_destroy(struct p25_audio *a){
  if(a == NULL)
    return;
  end_transmission(a);
  p25_vocoder_destroy(a->vocoder[P25_CODEC_IMBE]);
  p25_vocoder_destroy(a->vocoder[P25_CODEC_AMBE2]);
  FREE(a);
}

void p25_audio_get_stats(struct p25_audio const *a,struct p25_audio_stats *stats){
  if(a != NULL && stats != NULL)
    *stats = a->stats;
}

void p25_audio_set_unit_tap(struct p25_audio *a,void (*tap)(void *arg,int64_t when,uint64_t dibit),void *arg){
  if(a == NULL)
    return;
  a->tap = tap;
  a->tap_arg = arg;
}

static void send_error(struct p25_audio *a,struct p25_audio_group *g,char const *what){
  a->stats.errors++;
  if(!atomic_flag_test_and_set(&g->send_error_logged))
    fprintf(stderr,"p25 audio ssrc %u: %s send failed: %s (further messages suppressed)\n",a->ssrc,what,strerror(errno));
}

static void make_description(struct p25_audio *a){
  char *cp = a->description;
  size_t room = sizeof a->description;
  if(strlen(a->stream_description) > 0){
    strlcpy(cp,a->stream_description,room);
    return;
  }
  int n = 0;
  if(a->have_call){
    if(a->unit_call)
      n = snprintf(cp,room,"P25 %s %u",a->telephone ? "telephone" : "unit",a->target);
    else
      n = snprintf(cp,room,"P25 TG %u",a->target);
    if(n > 0 && (size_t)n < room && a->source != 0)
      n += snprintf(cp + n,room - n," src %u",a->source);
    if(n > 0 && (size_t)n < room)
      n += snprintf(cp + n,room - n," NAC %03x",a->nac);
  } else
    n = snprintf(cp,room,"P25 NAC %03x",a->nac);
  if(n > 0 && (size_t)n < room && a->encrypted)
    snprintf(cp + n,room - n," encrypted");
}

// Channel status in radiod's format (radio_status.c encode_radio_status()), with the fields
// monitor and pcmrecord use. Speech: 'closing' sends output level -infinity, as radiod sends when
// its squelch closes. Frames: the level is always -infinity (monitor keeps its squelch shut), the
// encoding is one no tool here records or plays, and there is no RTP_PT: pcmrecord makes a session
// from status and opens a file at the first data packet of that payload type whatever the
// encoding, so without it the frames match no session of pcmrecord's and are dropped
static void send_status(struct p25_audio *a,struct outstream *s,bool closing){
  struct p25_audio_group * const g = s->group;
  uint8_t packet[2048];
  uint8_t *bp = packet;
  *bp++ = STATUS;
  encode_int32(&bp,OUTPUT_SSRC,a->ssrc);
  make_description(a);
  encode_string(&bp,DESCRIPTION,a->description,strlen(a->description));
  // The clock reference: RTP timestamp 'timestamp' is sender time 'next_time'
  int64_t const now = s->next_time != 0 ? s->next_time : gps_time_ns();
  encode_int64(&bp,GPS_TIME,now);
  encode_int32(&bp,RTP_TIMESNAP,s->timestamp);
  encode_double(&bp,RADIO_FREQUENCY,a->frequency);
  // The I/Q channel's signal and noise, as radiod reported them: monitor works its SNR out from
  // these four (signal power less the noise in the filter, over that noise)
  if(isfinite(a->bb_power) && isfinite(a->n0) && a->bb_power > 0 && a->n0 > 0){
    encode_float(&bp,BASEBAND_POWER,power2dB(a->bb_power));
    encode_float(&bp,NOISE_DENSITY,power2dB(a->n0));
    encode_float(&bp,LOW_EDGE,a->low);
    encode_float(&bp,HIGH_EDGE,a->high);
  }
  encode_int32(&bp,OUTPUT_SAMPRATE,P25_AUDIO_SAMPRATE);
  encode_int32(&bp,OUTPUT_CHANNELS,1);
  float level = -INFINITY;
  if(g->frames){
    encode_int(&bp,OUTPUT_ENCODING,P25_FRAME_ENCODING);
  } else {
    encode_int(&bp,OUTPUT_ENCODING,S16BE);
    encode_byte(&bp,RTP_PT,g->pt);
    if(!closing && s->energy_samples > 0 && s->energy > 0)
      level = (float)(10 * log10(s->energy / s->energy_samples));
    encode_float(&bp,GAIN,0); // dB: the vocoder's output as it comes
  }
  encode_float(&bp,OUTPUT_LEVEL,level);
  s->energy = 0;
  s->energy_samples = 0;
  encode_int64(&bp,OUTPUT_DATA_PACKETS,s->packets);
  encode_int64(&bp,OUTPUT_SAMPLES,s->samples);
  encode_socket(&bp,OUTPUT_DATA_DEST_SOCKET,&g->data);
  encode_int32(&bp,OUTPUT_TTL,g->ttl);
  encode_int32(&bp,STATUS_INTERVAL,Status_interval);
  encode_int64(&bp,OUTPUT_METADATA_PACKETS,a->stats.status + 1);
  encode_int64(&bp,OUTPUT_ERRORS,a->stats.errors);
  encode_eol(&bp);
  assert(bp <= packet + sizeof packet);
  if(sendto(g->fd,packet,bp - packet,0,(struct sockaddr const *)&g->status,socklen(&g->status)) < 0)
    send_error(a,g,"status");
  a->stats.status++;
  s->status_count = Status_interval;
}

// Start a transmission on one stream whose first frame is at sender time 'start'
static void start_spurt(struct p25_audio *a,struct outstream *s,int64_t start,enum p25_codec codec){
  s->next_time = start - start % NS_PER_SAMPLE;
  s->timestamp = (uint32_t)(s->next_time / NS_PER_SAMPLE);
  s->seq = get_seq(s->group,a->ssrc);
  s->active = true;
  s->marker = true;
  s->status_count = 0;     // status goes out ahead of the first data: pcmrecord creates its session only from status
  s->codec = codec;
  s->last_sent = 0;
  if(s == &a->pcm){
    a->stats.spurts++;
    p25_vocoder_reset(a->vocoder[codec]);
  }
}

static void end_stream(struct p25_audio *a,struct outstream *s){
  if(s->group == NULL || !s->active)
    return;
  send_status(a,s,true);
  s->active = false;
  s->last_sent = 0;
  put_seq(s->group,a->ssrc,s->seq);
}

void p25_audio_end(struct p25_audio *a){
  if(a == NULL)
    return;
  end_stream(a,&a->pcm);
  end_stream(a,&a->frm);
}

// One packet of 'len' octets at the stream's next timestamp
static void send_packet(struct p25_audio *a,struct outstream *s,uint8_t const *payload,int len){
  struct p25_audio_group * const g = s->group;
  struct rtp_header rtp = {
    .version = RTP_VERS,
    .type = g->pt,
    .seq = s->seq,
    .timestamp = s->timestamp,
    .ssrc = a->ssrc,
    .marker = s->marker,
  };
  uint8_t packet[RTP_MIN_SIZE + 2 * P25_AUDIO_FRAME];
  assert(len <= 2 * P25_AUDIO_FRAME);
  uint8_t *dp = hton_rtp(packet,&rtp);
  memcpy(dp,payload,len);
  dp += len;
  if(s->status_count <= 0)
    send_status(a,s,false); // at the start of a transmission and every Status_interval packets, as radiod
  s->status_count--;
  if(sendto(g->fd,packet,dp - packet,0,(struct sockaddr const *)&g->data,socklen(&g->data)) < 0)
    send_error(a,g,"data");
  s->marker = false;
  s->seq++;
  s->timestamp += P25_AUDIO_FRAME;
  s->next_time += FRAME_NS;
  s->samples += P25_AUDIO_FRAME;
  s->packets++;
}

// One packet per 20 ms frame, as radiod sends 8 kHz PCM
static void send_pcm(struct p25_audio *a,int16_t const pcm[P25_AUDIO_FRAME]){
  uint8_t payload[2 * P25_AUDIO_FRAME];
  uint8_t *dp = payload;
  for(int i=0; i < P25_AUDIO_FRAME; i++){
    dp = put16(dp,(uint16_t)pcm[i]);
    float const s = pcm[i] * (1.0f / 32768);
    a->pcm.energy += s * s;
  }
  a->pcm.energy_samples += P25_AUDIO_FRAME;
  send_packet(a,&a->pcm,payload,sizeof payload);
  a->stats.packets++;
}

void p25_audio_pcm(struct p25_audio *a,int16_t const *pcm,int count,int64_t when){
  if(a == NULL || pcm == NULL || a->pcm.group == NULL)
    return;
  if(!a->pcm.active)
    start_spurt(a,&a->pcm,when != 0 ? when : gps_time_ns(),P25_CODEC_IMBE);
  for(int i=0; i + P25_AUDIO_FRAME <= count; i += P25_AUDIO_FRAME)
    send_pcm(a,pcm + i);
}

// Bits, one per byte, packed msb first; the last octet zero filled
static uint8_t *pack(uint8_t *dp,uint8_t const *bits,int nbits){
  for(int i=0; i < nbits; i += 8){
    uint8_t o = 0;
    for(int j=0; j < 8; j++)
      o = (uint8_t)(o << 1 | (i + j < nbits ? (bits[i + j] & 1) : 0));
    *dp++ = o;
  }
  return dp;
}

// One vocoder frame packet (f == NULL: lost). The layout is in docs/p25d.md
static void send_vframe(struct p25_audio *a,enum p25_codec codec,struct vframe const *f,struct crypto const *c,bool encrypted){
  uint8_t payload[P25_FRAME_IMBE_SIZE];
  memset(payload,0,sizeof payload);
  bool const imbe = codec == P25_CODEC_IMBE;
  int const size = imbe ? P25_FRAME_IMBE_SIZE : P25_FRAME_AMBE2_SIZE;
  payload[0] = P25_FRAME_VERSION;
  payload[1] = imbe ? P25_FRAME_CODEC_IMBE : P25_FRAME_CODEC_AMBE2;
  payload[2] = (uint8_t)((f == NULL ? P25_FRAME_LOST : 0) | (encrypted ? P25_FRAME_ENCRYPTED : 0)
			 | (f != NULL && c->fresh ? P25_FRAME_CRYPTO_FRESH : 0));
  payload[3] = f != NULL ? f->position : 0xff;
  if(f != NULL){
    payload[4] = f->e0;
    payload[5] = f->e1;
    payload[6] = f->et;
    payload[7] = f->errs;
    payload[8] = f->errs2;
    payload[9] = c->algid;
    payload[10] = (uint8_t)(c->kid >> 8);
    payload[11] = (uint8_t)c->kid;
    memcpy(payload + 12,c->mi,9);
    uint8_t *dp = pack(payload + P25_FRAME_HEADER,f->bits,imbe ? P25_IMBE_INFO_BITS : P25P2_AMBE_INFO_BITS);
    pack(dp,f->raw,imbe ? P25_IMBE_BITS : P25P2_AMBE_BITS);
  }
  send_packet(a,&a->frm,payload,size);
  a->stats.frame_packets++;
  if(encrypted)
    a->stats.frame_encrypted++;
}

// One frame (or a lost one) on one stream: decoded to speech, or as it is
static void send_frame(struct p25_audio *a,struct outstream *s,enum p25_codec codec,struct vframe const *f,
		       struct crypto const *c,bool encrypted){
  if(s == &a->frm){
    send_vframe(a,codec,f,c,encrypted);
    return;
  }
  int16_t pcm[P25_AUDIO_FRAME];
  enum p25_voice const r = p25_vocoder_decode(a->vocoder[codec],pcm,f != NULL ? f->bits : NULL,
					      f != NULL ? f->errs : 0,f != NULL ? f->errs2 : 0);
  a->stats.frames++;
  if(f == NULL)
    a->stats.lost++;
  if(r == P25_VOICE_REPEAT)
    a->stats.repeats++;
  else if(r == P25_VOICE_MUTED)
    a->stats.muted++;
  send_pcm(a,pcm);
}

// Frames lost on the air between the last unit sent on a stream and this one, or -1 when the gap
// is too long (or the stream has not started) and a new transmission begins.
// Phase 1 counts lost voice data units on the receiver's dibit clock, which the sender's clock
// references (GPS_TIME, RTP_TIMESNAP pairs, refreshed with each status) can't jitter. The
// sender's clock only shows a gap the receiver didn't see (p25d resynchronizes on a long
// timestamp jump instead of filling it): more time passed than dibits by over Hang.
// Phase 2 counts frames by their place in the superframe
static int64_t gap_frames(struct outstream const *s,struct held_unit const *h){
  if(!s->active || s->last_sent == 0 || s->codec != h->codec)
    return -1;
  int64_t frames,expected;
  if(h->codec == P25_CODEC_IMBE){
    int64_t const dibits = (int64_t)(h->pos - s->last_pos);
    int64_t const units = (dibits + LDU_DIBITS / 2) / LDU_DIBITS - 1;
    frames = units > 0 ? units * P25_IMBE_FRAMES : 0;
    expected = dibits * BILLION / P25_SYMRATE;
  } else {
    frames = (int64_t)(h->pos - s->last_pos);
    if(frames < 0)
      return -1;
    expected = (frames + h->nframes) * FRAME_NS;
  }
  int64_t const unseen = (h->when - s->last_sent) - expected;
  if(unseen > Hang || frames * FRAME_NS > Hang)
    return -1;
  return frames;
}

// Send one unit on one stream, filling what was lost before it
static void stream_unit(struct p25_audio *a,struct outstream *s,struct held_unit const *h,bool encrypted){
  if(s->group == NULL)
    return;
  int64_t const lost = gap_frames(s,h);
  if(lost < 0){
    end_stream(a,s);
    start_spurt(a,s,h->when - h->nframes * FRAME_NS,h->codec); // sender time of the unit's first frame
  } else {
    // Lost frames: repeated, then muted, in speech; marked lost in frames
    for(int64_t i=0; i < lost; i++)
      send_frame(a,s,h->codec,NULL,&h->crypto,encrypted);
  }
  for(int i=0; i < h->nframes; i++)
    send_frame(a,s,h->codec,&h->f[i],&h->crypto,encrypted);
  s->last_sent = h->when;
  s->last_pos = h->codec == P25_CODEC_IMBE ? h->pos : h->pos + (uint64_t)h->nframes;
}

// A unit that may be heard: speech and frames
static void send_clear(struct p25_audio *a,struct held_unit const *h){
  if(a->tap != NULL)
    (*a->tap)(a->tap_arg,h->when,h->pos);
  stream_unit(a,&a->pcm,h,false);
  stream_unit(a,&a->frm,h,false);
  a->stats.units++;
}

// A unit known to be encrypted: never speech; frames only if the frame group asked for them
static void send_encrypted(struct p25_audio *a,struct held_unit const *h){
  end_stream(a,&a->pcm);
  if(a->frm.group != NULL && a->frm.group->encrypted)
    stream_unit(a,&a->frm,h,true);
  else
    end_stream(a,&a->frm);
}

// Drop held units from the front
static void drop_held(struct p25_audio *a,int n){
  if(n > a->nheld)
    n = a->nheld;
  if(n <= 0)
    return;
  for(int i=0; i < n; i++)
    a->held_frames -= a->held[i].nframes;
  memmove(&a->held[0],&a->held[n],(a->nheld - n) * sizeof a->held[0]);
  a->nheld -= n;
}

// Send, in order, the held units that may now be sent as clear
static void flush_held(struct p25_audio *a){
  int n = 0;
  while(n < a->nheld && a->held[n].trunk_clear && a->clear && !a->encrypted)
    send_clear(a,&a->held[n++]);
  drop_held(a,n);
}

// Units still held are never sent
static void discard_held(struct p25_audio *a){
  a->stats.discarded += a->nheld;
  a->nheld = 0;
  a->held_frames = 0;
}

// The call turned out encrypted: what is held goes out as encrypted frames, or nowhere
static void encrypted_held(struct p25_audio *a){
  for(int i=0; i < a->nheld; i++)
    send_encrypted(a,&a->held[i]);
  discard_held(a);
  end_stream(a,&a->pcm);
}

// The transmission is over, or what it was turned out otherwise: forget it
static void end_transmission(struct p25_audio *a){
  discard_held(a);
  p25_audio_end(a);
  a->have_call = false;
  a->clear = false;
  a->encrypted = false;
  memset(&a->last,0,sizeof a->last);
  memset(&a->current,0,sizeof a->current);
  a->pending_fresh = false;
  a->in_superframe = false;
  a->cycle = INT64_MIN;
  a->p2_after_ptt = false;
  a->p2_k2_known = false; // p2_k2 stays as a guess for the next
  a->p2_k2_signalled = false;
  a->last_unit = 0;
}

void p25_audio_set_signal(struct p25_audio *a,float bb_power,float n0,float low,float high){
  if(a == NULL)
    return;
  a->bb_power = bb_power;
  a->n0 = n0;
  a->low = low;
  a->high = high;
}

void p25_audio_poll(struct p25_audio *a,double frequency,int64_t when){
  if(a == NULL)
    return;
  a->frequency = frequency;
  int64_t const t = when != 0 ? when : gps_time_ns();
  if(a->last_unit != 0 && t - a->last_unit > LDU_NS + Hang){
    // The next voice unit is overdue by more than Hang: the call is over even without a terminator
    end_transmission(a);
  }
}

// Encryption parameters received (header, encryption sync, PTT, ESS) at 'at': they apply to the
// next superframe
static void got_crypto(struct p25_audio *a,uint8_t algid,uint16_t kid,uint8_t const mi[9],int64_t at){
  a->last.algid = algid;
  a->last.kid = kid;
  memcpy(a->last.mi,mi,9);
  a->pending_fresh = true;
  a->pending_at = at;
  if(algid == 0x80) // unencrypted
    a->clear = true;
  else
    a->encrypted = true;
}

// A superframe begins: the parameters received last are the ones for it, fresh if they came just
// before it ('adjacent': the caller checks where they came against where it begins)
static void new_superframe(struct p25_audio *a,bool adjacent){
  a->current = a->last;
  a->current.fresh = a->pending_fresh && adjacent;
  a->pending_fresh = false;
}

// Phase 1: two units one voice data unit apart (864 dibits, give or take half)
static bool next_unit(uint64_t later,int64_t earlier){
  int64_t const d = (int64_t)later - earlier - LDU_DIBITS;
  return d >= -LDU_DIBITS / 2 && d <= LDU_DIBITS / 2;
}

// A voice channel user word (Phase 1 link control, Phase 2 MAC structure). The telephone
// interconnect form names one address, the radio in the call: it is taken as a unit call to that
// radio with no source (the decoders leave src zero), described as "P25 telephone", as a trunked
// telephone call's stream is (p25d.c)
static void voice_user(struct p25_audio *a,struct p25_msg const *msg){
  switch(msg->kind){
  case P25_LC_GRP_V_CH_USR:
  case P25_LC_MOT_GRG_V_CH_USR:
    a->have_call = true;
    a->unit_call = false;
    a->telephone = false;
    a->target = msg->group;
    a->source = msg->src;
    a->encrypted |= (msg->svc & P25_SVC_ENCRYPTED) != 0;
    break;
  case P25_LC_UU_V_CH_USR:
  case P25_LC_TEL_INT_V_CH_USR:
    a->have_call = true;
    a->unit_call = true;
    a->telephone = msg->kind == P25_LC_TEL_INT_V_CH_USR;
    a->target = msg->tgt;
    a->source = msg->src;
    a->encrypted |= (msg->svc & P25_SVC_ENCRYPTED) != 0;
    break;
  default:
    break;
  }
}

// Call details and encryption from the header, link control and encryption sync words
static void call_info(struct p25_audio *a,struct p25_frame const *frame){
  uint8_t octets[15];
  struct p25_msg msg;
  switch(frame->duid){
  case P25_DUID_HDU:
    if(frame->word == NULL || !frame->word->ok)
      return;
    p25_pack_bits(octets,frame->word->bits,120);
    if(p25_decode_hdu(&msg,octets) != 0)
      return;
    got_crypto(a,msg.algid,msg.kid,msg.mi,(int64_t)frame->dibit); // for the first superframe
    a->have_call = true;
    a->unit_call = false;
    a->telephone = false;
    a->target = msg.group;
    a->source = 0;
    break;
  case P25_DUID_LDU1:
    if(frame->ldu == NULL || !frame->ldu->word.ok)
      return;
    p25_pack_bits(octets,frame->ldu->word.bits,72);
    if(p25_decode_lc(&msg,octets,false) != 0)
      return;
    if(msg.protect){
      a->encrypted = true;
      return;
    }
    voice_user(a,&msg);
    break;
  case P25_DUID_LDU2:
    if(frame->ldu == NULL || !frame->ldu->word.ok)
      return;
    p25_pack_bits(octets,frame->ldu->word.bits,96);
    if(p25_decode_es(&msg,octets) != 0)
      return;
    got_crypto(a,msg.algid,msg.kid,msg.mi,(int64_t)frame->dibit); // for the next superframe
    break;
  default:
    break;
  }
}

// Link control naming another talkgroup (or unit) or another talker than the transmission's:
// a new transmission whose terminator and header were both missed. What was decided about the
// last one's encryption must not carry over to it
static bool new_talker(struct p25_audio const *a,struct p25_frame const *frame){
  if(!a->have_call || frame->ldu == NULL || !frame->ldu->word.ok)
    return false;
  uint8_t octets[9];
  struct p25_msg msg;
  p25_pack_bits(octets,frame->ldu->word.bits,72);
  if(p25_decode_lc(&msg,octets,false) != 0 || msg.protect)
    return false;
  bool unit_call;
  uint32_t target;
  switch(msg.kind){
  case P25_LC_GRP_V_CH_USR:
  case P25_LC_MOT_GRG_V_CH_USR:
    unit_call = false;
    target = msg.group;
    break;
  case P25_LC_UU_V_CH_USR:
  case P25_LC_TEL_INT_V_CH_USR:
    unit_call = true;
    target = msg.tgt;
    break;
  default:
    return false;
  }
  return unit_call != a->unit_call || target != a->target || (a->source != 0 && msg.src != 0 && msg.src != a->source);
}

// A voice unit arrives with the trunking logic's label: send it, hold it, or not
static void voice_arrival(struct p25_audio *a,struct held_unit const *u,enum p25_audio_label label){
  if(label == P25_AUDIO_NONE){
    a->stats.discarded++;
    return;
  }
  if(a->encrypted || label == P25_AUDIO_ENCRYPTED){
    // Not clear, nor anything held. What this module saw itself holds for the rest of the
    // transmission; the trunking logic's label is its word on this unit (it may revise it)
    encrypted_held(a);
    send_encrypted(a,u);
    a->stats.encrypted++;
    return;
  }
  bool const trunk_clear = label == P25_AUDIO_CLEAR || label == P25_AUDIO_UNLABELLED;
  if(!(trunk_clear && a->clear && a->nheld == 0))
    a->stats.held++; // it has to wait
  while(a->nheld > 0 && (a->nheld == P25_AUDIO_MAX_HELD || a->held_frames + u->nframes > P25_AUDIO_MAX_HELD_FRAMES)){
    // Waited too long without being confirmed clear: the oldest is never sent
    drop_held(a,1);
    a->stats.discarded++;
  }
  struct held_unit * const h = &a->held[a->nheld++];
  *h = *u;
  h->trunk_clear = trunk_clear;
  a->held_frames += h->nframes;
  flush_held(a);
}

void p25_audio_unit(struct p25_audio *a,struct p25_frame const *frame,int64_t when,enum p25_audio_label label){
  if(a == NULL || frame == NULL)
    return;
  int64_t const t = when != 0 ? when : gps_time_ns();
  a->nac = frame->nac;
  switch(frame->duid){
  case P25_DUID_HDU:
    end_transmission(a); // a header starts a new call
    call_info(a,frame);
    break;
  case P25_DUID_LDU1:
  case P25_DUID_LDU2:
    {
      struct p25_ldu const * const ldu = frame->ldu;
      if(ldu == NULL)
	break;
      // The parameters for this unit's frames: an LDU1 begins a superframe, and so does an LDU2
      // whose LDU1 was missed. This LDU2's own sync is for the next superframe
      bool const ldu1 = frame->duid == P25_DUID_LDU1;
      if(ldu1 && new_talker(a,frame))
	end_transmission(a);
      a->last_unit = t;
      if(ldu1 || !a->in_superframe || !next_unit(frame->dibit,(int64_t)a->ldu1_dibit))
	new_superframe(a,next_unit(frame->dibit,a->pending_at));
      a->in_superframe = ldu1;
      a->ldu1_dibit = frame->dibit;
      struct held_unit local; // on the stack: a few kilobytes, no allocation
      struct held_unit * const up = &local;
      up->codec = P25_CODEC_IMBE;
      up->when = t;
      up->pos = frame->dibit;
      up->crypto = a->current;
      up->nframes = P25_IMBE_FRAMES;
      for(int i=0; i < P25_IMBE_FRAMES; i++){
	struct p25_imbe const * const v = &ldu->voice[i];
	struct vframe * const f = &up->f[i];
	memcpy(f->bits,v->u,P25_IMBE_INFO_BITS);
	memcpy(f->raw,v->raw,P25_IMBE_BITS);
	f->e0 = (uint8_t)v->e0;
	f->e1 = (uint8_t)v->e4;
	f->et = (uint8_t)v->total;
	f->errs = (uint8_t)v->e0;
	f->errs2 = (uint8_t)v->total;
	f->position = (uint8_t)(i + (ldu1 ? 0 : P25_IMBE_FRAMES));
      }
      call_info(a,frame); // before deciding: this unit's own word counts
      voice_arrival(a,up,label);
    }
    break;
  case P25_DUID_TDU:
  case P25_DUID_TDULC:
    end_transmission(a);
    break;
  default:
    break;
  }
}

void p25_audio_frame(struct p25_audio *a,struct p25_frame const *frame,int64_t when){
  p25_audio_unit(a,frame,when,P25_AUDIO_UNLABELLED);
}

static int64_t floor_div(int64_t a,int64_t b){
  int64_t q = a / b;
  if((a % b != 0) && ((a < 0) != (b < 0)))
    q--;
  return q;
}

// A MAC voice channel user structure naming another talkgroup (or unit) or another talker than
// the transmission's: a new transmission whose END PTT and PTT were both missed, as new_talker()
// is for Phase 1 link control
static bool new_mac_talker(struct p25_audio const *a,struct p25_msg const *msg){
  if(!a->have_call)
    return false;
  bool unit_call;
  uint32_t target;
  switch(msg->kind){
  case P25_LC_GRP_V_CH_USR:
  case P25_LC_MOT_GRG_V_CH_USR:
    unit_call = false;
    target = msg->group;
    break;
  case P25_LC_UU_V_CH_USR:
  case P25_LC_TEL_INT_V_CH_USR:
    unit_call = true;
    target = msg->tgt;
    break;
  default:
    return false;
  }
  return unit_call != a->unit_call || target != a->target || (a->source != 0 && msg->src != 0 && msg->src != a->source);
}

static void p2_place(struct p25_audio const *a,struct held_unit *h);

// Where the 2V falls (see the top of the file): what is held is placed again if it moves
static void p2_framing(struct p25_audio *a,int k2,bool signalled){
  bool const moved = k2 != a->p2_k2;
  a->p2_k2 = k2;
  a->p2_k2_known = true;
  a->p2_k2_signalled |= signalled;
  a->p2_after_ptt = false;
  for(int i=0; moved && i < a->nheld; i++)
    if(a->held[i].codec == P25_CODEC_AMBE2)
      p2_place(a,&a->held[i]);
}

// Phase 2 call details and encryption from a signalling burst's MAC PDU (its CRC passed)
static void mac_info(struct p25_audio *a,struct p25p2_burst const *b){
  struct p25p2_mac const * const m = b->mac;
  struct p25_mac_pdu pdu;
  if(p25_mac_decode(&pdu,(enum p25_mac_chan)m->chan,m->octets,m->nbits) < 0)
    return;
  int const k2 = p25p2_offset_2v(b->superframe_slot,pdu.offset_4v); // -1: no framing given
  switch(pdu.type){
  case P25_MAC_PTT:
    end_transmission(a); // a PTT starts a new transmission
    got_crypto(a,pdu.algid,pdu.kid,pdu.mi,INT64_MIN); // for the first voice cycle, whichever it is
    if(k2 >= 0)
      p2_framing(a,k2,true);
    else
      a->p2_after_ptt = true;
    a->source = pdu.src;
    if(pdu.group != 0){
      a->have_call = true;
      a->unit_call = false;
      a->telephone = false;
      a->target = pdu.group;
    }
    // else an individual call's PTT (TIA-102.BBAC 7.3.1): group 0 is reserved and names no
    // talkgroup. The call stays unnamed until its unit to unit voice channel user, which then
    // is not a new talker (new_mac_talker() needs a call named)
    break;
  case P25_MAC_END_PTT:
    if(b->color_code >= 0 && pdu.nac != b->color_code)
      break; // another system's (TIA-102.BBAD-A 4.10): ends nothing here
    end_transmission(a);
    break;
  case P25_MAC_IDLE:
  case P25_MAC_HANGTIME:
    // No voice on this timeslot now: whatever comes next is another transmission. The voice
    // channel user structures of an idle or hang time PDU name the call, not a transmission
    end_transmission(a);
    break;
  default:
    if(k2 >= 0 && k2 != a->p2_k2 && a->p2_k2_signalled)
      end_transmission(a); // a change of audio source (TIA-102.BBAD-A 4.2): as at a PTT
    if(k2 >= 0)
      p2_framing(a,k2,true);
    {
      // The SACCH of an ultraframe's last superframe names the inbound talker, not the audio's
      // source (TIA-102.BBAC-A 3.4); one whose place is not known may be that one
      bool const identity = m->chan != P25_MAC_SACCH || (b->ultraframe >= 0 && b->ultraframe < 3);
      for(int i=0; i < pdu.nstruct; i++){
	struct p25_msg const * const msg = &pdu.s[i].msg;
	if(pdu.s[i].mac_kind != P25_MAC_MSG)
	  continue;
	if(!identity){
	  if(msg->kind == P25_LC_GRP_V_CH_USR || msg->kind == P25_LC_MOT_GRG_V_CH_USR || msg->kind == P25_LC_UU_V_CH_USR
	     || msg->kind == P25_LC_TEL_INT_V_CH_USR)
	    a->encrypted |= (msg->svc & P25_SVC_ENCRYPTED) != 0;
	  continue;
	}
	if(new_mac_talker(a,msg))
	  end_transmission(a);
	voice_user(a,msg);
      }
    }
    break;
  }
}

// Phase 2 voice cycles: the voice cycle of the burst at voice burst k of superframe sf, and its
// place in it (0-3 a 4V, 4 the 2V). A cycle is the four 4V bursts after a 2V and that 2V, 18
// frames: the ESS collected over it applies to the next
static void p2_cycle(struct p25_audio const *a,int64_t sf,int k,int64_t *cycle,int *c){
  int64_t const air = sf * 5 + k - (a->p2_k2 + 1);
  *cycle = floor_div(air,5);
  *c = (int)(air - *cycle * 5);
}
// A unit's first frame index and its frames' positions
static void p2_place(struct p25_audio const *a,struct held_unit *h){
  int64_t cycle;
  int c;
  p2_cycle(a,h->p2_sf,h->p2_k,&cycle,&c);
  h->pos = (uint64_t)(cycle * P2_CYCLE_FRAMES + 4 * c);
  for(int i=0; i < h->nframes; i++)
    h->f[i].position = (uint8_t)(4 * c + i);
}

void p25_audio_burst(struct p25_audio *a,struct p25p2_burst const *b,int64_t when,enum p25_audio_label label){
  if(a == NULL || b == NULL)
    return;
  int64_t const t = when != 0 ? when : gps_time_ns();
  // The burst's place: superframe from the dibit clock, position k (0-4 voice, 5 SACCH) from its slot
  int const k = b->superframe_slot / 2;
  uint64_t const sf_start = b->dibit - (uint64_t)b->superframe_slot * P2_SLOT_DIBITS;
  if(!a->p2_ref_set){
    a->p2_ref = sf_start;
    a->p2_ref_set = true;
  }
  int64_t const sf = floor_div((int64_t)(sf_start - a->p2_ref) + P2_SUPERFRAME_DIBITS / 2,P2_SUPERFRAME_DIBITS);
  bool const mac = b->mac != NULL;
  if(mac ? !b->mac->crc_ok : (b->voice == NULL || b->nframes <= 0 || b->nframes > MAX_UNIT_FRAMES || k > 4))
    return; // nothing decoded in this place
  if(mac){
    mac_info(a,b);
    return;
  }
  a->last_unit = t;
  if(a->p2_after_ptt)
    p2_framing(a,(k + 4) % 5,false); // this burst begins a voice cycle
  else if(b->duid == P25P2_DUID_2V && !a->p2_k2_known)
    p2_framing(a,k,false); // late entry: placed by a guess so far
  // (a 2V elsewhere once the framing is known moves nothing: see the top of the file)
  int64_t cycle;
  int c;
  p2_cycle(a,sf,k,&cycle,&c);
  if(cycle != a->cycle)
    new_superframe(a,a->pending_at == INT64_MIN || cycle == a->pending_at + 1);
  a->cycle = cycle;
  struct held_unit u;
  u.codec = P25_CODEC_AMBE2;
  u.when = t;
  u.crypto = a->current;
  u.p2_sf = sf;
  u.p2_k = k;
  u.nframes = b->nframes;
  for(int i=0; i < b->nframes; i++){
    struct p25p2_ambe const * const v = &b->voice[i];
    struct vframe * const f = &u.f[i];
    memset(f,0,sizeof *f);
    memcpy(f->bits,v->bits,P25P2_AMBE_INFO_BITS);
    memcpy(f->raw,v->raw,P25P2_AMBE_BITS);
    f->e0 = (uint8_t)v->errors[0];
    f->e1 = (uint8_t)v->errors[1];
    f->et = (uint8_t)(v->errors[0] + v->errors[1]);
    f->errs = (uint8_t)v->data_errors[0];
    f->errs2 = (uint8_t)(v->data_errors[0] + v->data_errors[1]);
  }
  p2_place(a,&u);
  // This burst's own encryption sync counts at once for the rule (an ALGID other than 0x80 makes
  // the whole transmission not clear, this burst included); its parameters are for the next cycle
  if(b->duid == P25P2_DUID_2V && b->ess != NULL && b->ess->ok)
    got_crypto(a,b->ess->algid,b->ess->kid,b->ess->mi,cycle);
  voice_arrival(a,&u,label);
}

void p25_audio_release(struct p25_audio *a,bool clear){
  if(a == NULL)
    return;
  if(!clear){
    // Treated as encrypted: nothing held is clear
    encrypted_held(a);
    return;
  }
  for(int i=0; i < a->nheld; i++)
    a->held[i].trunk_clear = true;
  flush_held(a);
}

void p25_audio_call_end(struct p25_audio *a){
  if(a == NULL)
    return;
  end_transmission(a);
  a->p2_ref_set = false; // Phase 2: superframes are counted afresh (the receiver may be a new one)
}

void p25_audio_set_stream(struct p25_audio *a,uint32_t ssrc,char const *description){
  if(a == NULL)
    return;
  if(ssrc == 0)
    ssrc = a->channel_ssrc;
  if(ssrc != a->ssrc){
    p25_audio_end(a);
    a->ssrc = ssrc;
  }
  strlcpy(a->stream_description,description != NULL ? description : "",sizeof a->stream_description);
}
