// Real Time Protocol support routines and tables

#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <netinet/in.h>
#include <opus/opus.h>
#include <math.h>
#include "rtp.h"

struct pt_table PT_table[128] = {
{ 8000, 1, MULAW }, // 0
{ 0, 0, 0 }, // 1
{ 0, 0, 0 }, // 2
{ 0, 0, 0 }, // 3
{ 0, 0, 0 }, // 4
{ 0, 0, 0 }, // 5
{ 0, 0, 0 }, // 6
{ 0, 0, 0 }, // 7
{ 8000, 1, ALAW }, // 8
{ 0, 0, 0 }, // 9
{ 44100, 2, S16BE }, // 10
{ 44100, 1, S16BE }, // 11
{ 0, 0, 0 }, // 12
{ 0, 0, 0 }, // 13
{ 0, 0, 0 }, // 14
{ 0, 0, 0 }, // 15
{ 0, 0, 0 }, // 16
{ 0, 0, 0 }, // 17
{ 0, 0, 0 }, // 18
{ 0, 0, 0 }, // 19
{ 0, 0, 0 }, // 20
{ 0, 0, 0 }, // 21
{ 0, 0, 0 }, // 22
{ 0, 0, 0 }, // 23
{ 0, 0, 0 }, // 24
{ 0, 0, 0 }, // 25
{ 0, 0, 0 }, // 26
{ 0, 0, 0 }, // 27
{ 0, 0, 0 }, // 28
{ 0, 0, 0 }, // 29
{ 0, 0, 0 }, // 30
{ 0, 0, 0 }, // 31
{ 0, 0, 0 }, // 32
{ 0, 0, 0 }, // 33
{ 0, 0, 0 }, // 34
{ 0, 0, 0 }, // 35
{ 0, 0, 0 }, // 36
{ 0, 0, 0 }, // 37
{ 0, 0, 0 }, // 38
{ 0, 0, 0 }, // 39
{ 0, 0, 0 }, // 40
{ 0, 0, 0 }, // 41
{ 0, 0, 0 }, // 42
{ 0, 0, 0 }, // 43
{ 0, 0, 0 }, // 44
{ 0, 0, 0 }, // 45
{ 0, 0, 0 }, // 46
{ 0, 0, 0 }, // 47
{ 0, 0, 0 }, // 48
{ 0, 0, 0 }, // 49
{ 0, 0, 0 }, // 50
{ 0, 0, 0 }, // 51
{ 0, 0, 0 }, // 52
{ 0, 0, 0 }, // 53
{ 0, 0, 0 }, // 54
{ 0, 0, 0 }, // 55
{ 0, 0, 0 }, // 56
{ 0, 0, 0 }, // 57
{ 0, 0, 0 }, // 58
{ 0, 0, 0 }, // 59
{ 0, 0, 0 }, // 60
{ 0, 0, 0 }, // 61
{ 0, 0, 0 }, // 62
{ 0, 0, 0 }, // 63
{ 0, 0, 0 }, // 64
{ 0, 0, 0 }, // 65
{ 0, 0, 0 }, // 66
{ 0, 0, 0 }, // 67
{ 0, 0, 0 }, // 68
{ 0, 0, 0 }, // 69
{ 0, 0, 0 }, // 70
{ 0, 0, 0 }, // 71
{ 0, 0, 0 }, // 72
{ 0, 0, 0 }, // 73
{ 0, 0, 0 }, // 74
{ 0, 0, 0 }, // 75
{ 0, 0, 0 }, // 76
{ 0, 0, 0 }, // 77
{ 0, 0, 0 }, // 78
{ 0, 0, 0 }, // 79
{ 0, 0, 0 }, // 80
{ 0, 0, 0 }, // 81
{ 0, 0, 0 }, // 82
{ 0, 0, 0 }, // 83
{ 0, 0, 0 }, // 84
{ 0, 0, 0 }, // 85
{ 0, 0, 0 }, // 86
{ 0, 0, 0 }, // 87
{ 0, 0, 0 }, // 88
{ 0, 0, 0 }, // 89
{ 0, 0, 0 }, // 90
{ 0, 0, 0 }, // 91
{ 0, 0, 0 }, // 92
{ 0, 0, 0 }, // 93
{ 0, 0, 0 }, // 94
{ 0, 0, 0 }, // 95
{ 0, 0, 0 }, // 96
{ 0, 0, 0 }, // 97
{ 0, 0, 0 }, // 98
{ 0, 0, 0 }, // 99
{ 8000, 0, 0 }, // 100  defacto owned by RTP Event
{ 0, 0, 0 }, // 101
{ 0, 0, 0 }, // 102
{ 0, 0, 0 }, // 103
{ 0, 0, 0 }, // 104
{ 0, 0, 0 }, // 105
{ 0, 0, 0 }, // 106
{ 0, 0, 0 }, // 107
{ 0, 0, 0 }, // 108
{ 0, 0, 0 }, // 109
{ 0, 0, 0 }, // 110
{ 48000, 2, OPUS }, // 111  Opus always uses a 48K virtual sample rate
{ 48000, 1, S16BE }, // 112
{ 48000, 2, S16BE }, // 113
{ 0, 0, 0 }, // 114
{ 0, 0, 0 }, // 115
{ 24000, 1, S16BE }, // 116
{ 24000, 2, S16BE }, // 117
{ 0, 0, 0 }, // 118
{ 16000, 1, S16BE }, // 119
{ 16000, 2, S16BE }, // 120
{ 0, 0, 0 }, // 121
{ 12000, 1, S16BE }, // 122
{ 12000, 2, S16BE }, // 123
{ 0, 0, 0 }, // 124
{ 8000, 1, S16BE }, // 125
{ 8000, 2, S16BE }, // 126
{ 0, 0, 0 }, // 127
};

#define AX25_PT (96)  // NON-standard payload type for my raw AX.25 frames - clean this up and remove
#define OPUS_PT (111) // Hard-coded NON-standard payload type for OPUS (should be dynamic with sdp)


int const Opus_pt = OPUS_PT;
int const AX25_pt = AX25_PT;

// Add an encoding to the RTP payload type table
// The mappings are typically extracted from a radiod status channel and kept in a table so they can
// be changed midstream without losing anything
int add_pt(int type, int samprate, int channels, enum encoding encoding){
  if(encoding == NO_ENCODING)
    return -1;

  if(encoding == OPUS){
    // Force Opus to fixed values
    samprate = OPUS_SAMPRATE;
    channels = 2;
  }
  if(type >= 0 && type < 128){
    PT_table[type].channels = channels;
    PT_table[type].samprate = samprate;
    PT_table[type].encoding = encoding;
    return 0;
  } else
    return -1;
}
// Convert RTP header from network (wire) big-endian format to internal host structure
// Written to be insensitive to host byte order and C structure layout and padding
// Use of unsigned formats is important to avoid unwanted sign extension
void const *ntoh_rtp(struct rtp_header * const rtp,void const * const data){
  uint32_t const *dp = data;

  uint32_t const w = ntohl(*dp++);
  rtp->version = w >> 30;
  rtp->pad = (w >> 29) & 1;
  rtp->extension = (w >> 28) & 1;
  rtp->cc = (w >> 24) & 0xf;
  rtp->marker = (w >> 23) & 1;
  rtp->type = (w >> 16) & 0x7f;
  rtp->seq = w & 0xffff;

  rtp->timestamp = ntohl(*dp++);
  rtp->ssrc = ntohl(*dp++);

  for(int i=0; i<rtp->cc; i++)
    rtp->csrc[i] = ntohl(*dp++);

  if(rtp->extension){
    int ext_len = ntohl(*dp++) & 0xffff;    // Ignore any extension, but skip over it
    dp += ext_len;
  }
  return dp;
}

// Convert RTP header from internal host structure to network (wire) big-endian format
// Written to be insensitive to host byte order and C structure layout and padding
void *hton_rtp(void * const data, struct rtp_header const * const rtp){
  uint32_t *dp = data;
  int cc = rtp->cc & 0xf; // Ensure in range, <= 15
  *dp++ = htonl(RTP_VERS << 30 | rtp->pad << 29 | rtp->extension << 28 | cc << 24 | rtp->marker << 23
		| (rtp->type & 0x7f) << 16 | rtp->seq);
  *dp++ = htonl(rtp->timestamp);
  *dp++ = htonl(rtp->ssrc);
  for(int i=0; i < cc ; i++)
    *dp++ = htonl(rtp->csrc[i]);

  return dp;
}


// Process sequence number and timestamp in incoming RTP header:
// count dropped and duplicated packets, but it gets confused
// Determine timestamp jump from the next expected one
int rtp_process(struct rtp_state * const state,struct rtp_header const * const rtp,size_t const sampcnt){
  if(rtp->ssrc != state->ssrc){
    // Normally this will happen only on the first packet in a session since
    // the caller demuxes the SSRC to multiple instances.
    // But a single-instance, interactive application like 'radio' lets the SSRC
    // change so it doesn't have to restart when the stream sender does.
    state->init = false;
    state->ssrc = rtp->ssrc; // Must be filtered elsewhere if you want it
  }
  if(!state->init){
    state->packets = 0;
    state->seq = rtp->seq;
    state->timestamp = rtp->timestamp;
    state->dupes = 0;
    state->drops = 0;
    state->init = true;
  }
  state->packets++;
  // Sequence number check
  int const seq_step = (int16_t)(rtp->seq - state->seq);
  if(seq_step != 0){
    if(seq_step < 0)
      state->dupes++;
    else
      state->drops += seq_step;
  }
  state->seq = rtp->seq + 1;

  int const time_step = (int)(rtp->timestamp - state->timestamp);
  state->timestamp = (uint32_t)(rtp->timestamp + sampcnt);
  return time_step;
}
int samprate_from_pt(int const type){
  if(type < 0 || type > 127)
    return 0;
  return PT_table[type].samprate;
}

int channels_from_pt(int const type){
  if(type < 0 || type > 127)
    return 0;
  return PT_table[type].channels;
}

enum encoding encoding_from_pt(int const type){
  if(type < 0 || type > 127)
    return NO_ENCODING;
  return PT_table[type].encoding;
}
// Dynamically create a new one if not found
// Should lock the table when it's modified
// Use for sending only! Receivers need to build a table for each sender
int pt_from_info(int samprate,int channels,enum encoding encoding){
  if(samprate <= 0 || channels <= 0 || channels > 2 || encoding == NO_ENCODING || encoding >= UNUSED_ENCODING)
    return -1;

  if(encoding == OPUS){
    // Force Opus to fixed values; merges all variations to single PT
    channels = 2;
    samprate = OPUS_SAMPRATE;
  }

  // Search table for existing entry, otherwise create new entry
  for(int type=0; type < 128; type++){
    if(PT_table[type].samprate == samprate && PT_table[type].channels == channels && PT_table[type].encoding == encoding)
      return type;
  }
  // The dynamic pool starts at 96 but sometimes I need more than are available during testing with lots of encoder/
  // sample rate/channel combinations. 77-95 is unassigned, so I'm squatting. I do avoid 100 since it's de-facto RTP Event
  for(int type=77; type < 128; type++){ // Allocate a new type in the dynamic range
    if(type == 100)
      continue; // avoid this one, de-facto RTP Event
    if(PT_table[type].samprate == 0){
      // allocate it
      if(add_pt(type,samprate,channels,encoding) == -1)
	return -1;
      return type;
    }
  }
  return -1;
}
char const *encoding_string(enum encoding e){
  switch(e){
  default:
    return "none";
  case S16LE:
    return "s16le";
  case S16BE:
    return "s16be";
  case OPUS:
    return "opus";
  case OPUS_VOIP:
    return "opus-voip";
  case F32LE:
    return "f32le";
  case F32BE:
    return "f32be";
  case AX25:
    return "ax.25";
  case F16LE:
    return "f16le";
  case F16BE:
    return "f16be";
  case MULAW:
    return "ulaw";
  case ALAW:
    return "alaw";
  }
}
enum encoding parse_encoding(char const *str){
  if(strcasecmp(str,"s16be") == 0 || strcasecmp(str,"s16") == 0 || strcasecmp(str,"int") == 0)
    return S16BE;
  else if(strcasecmp(str,"s16le") == 0)
    return S16LE;
  else if(strcasecmp(str,"f32") == 0 || strcasecmp(str,"float") == 0 || strcasecmp(str,"f32le") == 0)
    return F32LE;
  else if(strcasecmp(str,"f32be") == 0)
    return F32BE;
  else if(strcasecmp(str,"f16") == 0 || strcasecmp(str,"f16le") == 0)
    return F16LE;
  else if(strcasecmp(str,"f16be") == 0)
    return F16BE;
  else if(strcasecmp(str,"opus-voip") == 0)
    return OPUS_VOIP;
  else if(strcasecmp(str,"opus") == 0)
    return OPUS;
  else if(strcasecmp(str,"ax25") == 0 || strcasecmp(str,"ax.25") == 0)
    return AX25;
  else if (strcasecmp(str, "ulaw") == 0 || strcasecmp(str, "mulaw") == 0 || strcasecmp(str,"μlaw") == 0 || strcasecmp(str,"pcmu") == 0)
    return MULAW;
  else if (strcasecmp(str, "alaw") == 0)
    return ALAW;
  else
    return NO_ENCODING;
}

// _CELT and _SILK seem to have been added with verson 1.6
// Don't break the compilation if we have an older version
struct string_table Opus_application[] = {
  {"voip", OPUS_APPLICATION_VOIP},
  {"audio", OPUS_APPLICATION_AUDIO},
  {"lowdelay",OPUS_APPLICATION_RESTRICTED_LOWDELAY},
#ifdef OPUS_APPLICATION_RESTRICTED_CELT
  {"celt", OPUS_APPLICATION_RESTRICTED_CELT},
#endif
#ifdef OPUS_APPLICATION_SILK
  {"silk", OPUS_APPLICATION_SILK},
#endif
  { NULL, -1 },
};

char const *opus_application_string(int x){
  for(int i=0; Opus_application[i].value != -1; i++){
    if(Opus_application[i].value == x)
      return Opus_application[i].str;
  }
  return NULL;
}

// Interpret an Opus bandwidth constant
int opus_bandwidth(char const **str,int code){
  int bw = 0;
  char const *s = NULL;
  switch(code){
  case OPUS_BANDWIDTH_NARROWBAND:
    bw = 4000;
    s = "narrowband";
    break;
  case OPUS_BANDWIDTH_MEDIUMBAND:
    bw = 6000;
    s = "mediumband";
    break;
  case OPUS_BANDWIDTH_WIDEBAND:
    bw = 8000;
    s = "wideband";
    break;
  case OPUS_BANDWIDTH_SUPERWIDEBAND:
    bw = 12000;
    s = "superwideband";
    break;
  case OPUS_BANDWIDTH_FULLBAND:
    bw = 20000;
    s = "fullband";
    break;
  default:
    bw = 0;
    s = "invalid";
    break;
  }
  if(str != NULL)
    *str = s;
  return bw;
}
// Allowable Opus block durations, millisec * 10
int Opus_blocksizes[] = {
  25, 50, 100, 200, 400, 600, 800, 1000, 1200, -1,
};
int Opus_samprates[] = {
  8000, 12000, 16000, 24000, 48000, -1,
};

// Return slowest bandwidth code that can support the specified bandwidth
int opus_bandwidth_to_code(int bw){
  if(bw <= 4000)
    return OPUS_BANDWIDTH_NARROWBAND;
  else if(bw <= 6000)
    return OPUS_BANDWIDTH_MEDIUMBAND;
  else if(bw <= 8000)
    return OPUS_BANDWIDTH_WIDEBAND;
  else if(bw <= 12000)
    return OPUS_BANDWIDTH_SUPERWIDEBAND;
  else
    return OPUS_BANDWIDTH_FULLBAND;
}
struct string_table Opus_signal[] = {
  {"auto", OPUS_AUTO},
  {"music", OPUS_SIGNAL_MUSIC},
  {"voice", OPUS_SIGNAL_VOICE},
  {NULL,  -1},
};

bool legal_opus_size(int n){
  // 2.5, 5, 10, 20, 40, 60, 80, 100, 120
  if(n == 120 || n == 240 || n == 480 || n == 960 || n == 1920 || n == 2880 || n == 3840 || n == 4800 || n == 5760)
    return true;
  return false;
}

bool legal_opus_samprate(int n){
  for(int i=0;Opus_samprates[i] != -1; i++){
    if(n == Opus_samprates[i])
      return true;
  }
  return false;
}

// μ-law from ChatGPT

#define G711_BIAS 0x84   // 132
#define G711_CLIP 32635
_Static_assert(sizeof(unsigned int) == 4, "need 32-bit unsigned int for __builtin_clz");
uint8_t float_to_mulaw(float fsample){
  if (fsample > 1)
    fsample = 1;
  else if (fsample < -1)
    fsample = -1;
  int32_t sample = lrintf(ldexpf(fsample,15));
  int sign = (sample < 0);
  int32_t pcm  = sign ? -sample : sample;
  if (pcm > G711_CLIP)
    pcm = G711_CLIP;

  pcm += G711_BIAS;

  // Find segment (exponent)
  int exponent = (31 - __builtin_clz((uint32_t)pcm)) - 7;
  if (exponent < 0)
    exponent = 0;
  else if (exponent > 7)
    exponent = 7;

  // Mantissa is next 4 bits after exponent bit
  int mantissa = (pcm >> (exponent + 3)) & 0x0F;

  return (uint8_t)~((uint8_t)((exponent << 4) | mantissa) | (sign << 7));
}

float mulaw_to_float(uint8_t ulaw){
  ulaw = (uint8_t)~ulaw;

  int sign     = ulaw & 0x80;
  int exponent = (ulaw >> 4) & 0x07;
  int mantissa = ulaw & 0x0F;

  int32_t pcm = ((mantissa << 3) + G711_BIAS) << exponent;
  pcm -= G711_BIAS;

  return ldexpf((float)(sign ? -pcm : pcm),-15);
}

#define G711_ALAW_CLIP 32635

uint8_t float_to_alaw(float fsample){
  // Clamp input
  if(fsample > 1.0f)
    fsample =  1.0f;
  else if(fsample < -1.0f)
    fsample = -1.0f;

  int32_t sample = (int32_t)lrintf(ldexpf(fsample, 15));
  if(sample > 32767)
    sample =  32767;
  else if(sample < -32768)
    sample = -32768;

  int sign = (sample < 0);
  int32_t pcm = sign ? -sample : sample;

  if(pcm > G711_ALAW_CLIP)
    pcm = G711_ALAW_CLIP;

  int exponent = 0;
  if(pcm >= 256)
    exponent = (31 - __builtin_clz((uint32_t)pcm)) - 7;

  if(exponent < 0)
    exponent = 0;
  else if(exponent > 7)
    exponent = 7;

  int mantissa;
  if(exponent == 0)
    mantissa = (pcm >> 4) & 0x0F;
  else
    mantissa = (pcm >> (exponent + 3)) & 0x0F;

  uint8_t a = (uint8_t)((exponent << 4) | mantissa);
  a ^= (sign ? 0xD5 : 0x55);   // A-law XOR mask

  return a;
}

float alaw_to_float(uint8_t alaw){
  alaw ^= 0x55;

  int sign     = alaw & 0x80;
  int exponent = (alaw >> 4) & 0x07;
  int mantissa = alaw & 0x0F;

  int32_t pcm;
  if (exponent == 0)
    pcm = (mantissa << 4) + 8;
  else
    pcm = ((mantissa << 4) + 0x108) << (exponent - 1);

  return ldexpf((float)(sign ? -pcm : pcm), -15);
}
