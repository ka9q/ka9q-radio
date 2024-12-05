// Audio multicast routines for ka9q-radio
// Handles linear 16-bit PCM, mono and stereo
// Copyright 2017-2024 Phil Karn, KA9Q

#define _GNU_SOURCE 1
#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include <limits.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>

#include "misc.h"
#include "multicast.h"
#include "radio.h"

#define BYTES_PER_PKT 960        // byte count to fit in Ethernet MTU

bool GetSockOptFailed = false;     // Have we issued this log message yet?
bool TempSendFailure = false;

int Application = OPUS_APPLICATION_AUDIO; // Encoder optimization mode
bool Fec_enable = false;                  // Use forward error correction
int Opus_bitrate = 32000;        // Opus stream audio bandwidth; default 32 kb/s
bool Discontinuous = false;        // Off by default

// Send PCM output on stream; # of channels implicit in chan->output.channels
int send_output(struct channel * restrict const chan,float const * restrict buffer,int frames,bool const mute){
  assert(chan != NULL);
  if(frames <= 0 || chan->output.channels == 0 || chan->output.samprate == 0)
    return 0;

  if(mute){
    // Still increment timestamp
    if(chan->output.encoding == OPUS)
      chan->output.rtp.timestamp += frames * 48000 / chan->output.samprate; // Opus always at 48 kHz
    else
      chan->output.rtp.timestamp += frames;

    chan->output.silent = true;
    return 0;
  }
  int max_frames_per_pkt = 0;
  switch(chan->output.encoding){
  case S16BE:
  case S16LE:
    max_frames_per_pkt = BYTES_PER_PKT / (sizeof(int16_t) * chan->output.channels);
    break;
  case F32LE:
    max_frames_per_pkt = BYTES_PER_PKT / (sizeof(float) * chan->output.channels);
    break;
#ifdef FLOAT16
  case F16LE:
    max_frames_per_pkt = BYTES_PER_PKT / (sizeof(_Float16) * chan->output.channels);
    break;
#endif
  case OPUS:
    max_frames_per_pkt = INT_MAX; // No limit since they get compressed to a buffer limit
    break;
  default:
    return 0; // Don't send anything
    break;
  }

  struct rtp_header rtp;
  memset(&rtp,0,sizeof(rtp));
  rtp.version = RTP_VERS;
  rtp.type = chan->output.rtp.type;
  rtp.ssrc = chan->output.rtp.ssrc;
  // Send a marker to reset the receiver when the encoding changes or when we've been silent (e.g., FM squelch closed)
  rtp.marker = (chan->output.previous_encoding != chan->output.encoding) || chan->output.silent;
  chan->output.previous_encoding = chan->output.encoding;
  chan->output.silent = false;
  useconds_t pacing = 0;
  if(chan->output.pacing)
    pacing = 1000 * Blocktime * max_frames_per_pkt / frames; // for optional pacing, in microseconds

  while(frames > 0){
    int chunk = min(max_frames_per_pkt,frames);
    rtp.timestamp = chan->output.rtp.timestamp;
    rtp.seq = chan->output.rtp.seq;
    uint8_t packet[PKTSIZE];
    uint8_t * const dp = (uint8_t *)hton_rtp(packet,&rtp); // First byte after RTP header
    int bytes = 0;
    switch(chan->output.encoding){
    case S16BE:
      {
	int16_t *pcm_buf = (int16_t *)dp;
	for(int i=0; i < chunk * chan->output.channels; i++)
	  *pcm_buf++ = htons(scaleclip(*buffer++)); // Byte swap

	chan->output.rtp.timestamp += chunk;
	bytes = chunk * chan->output.channels * sizeof(*pcm_buf);
      }
      break;
    case S16LE:
      {
	int16_t *pcm_buf = (int16_t *)dp;
	for(int i=0; i < chunk * chan->output.channels; i++)
	  *pcm_buf++ = scaleclip(*buffer++); // No byte swap

	chan->output.rtp.timestamp += chunk;
	bytes = chunk * chan->output.channels * sizeof(*pcm_buf);
      }
      break;
    case F32LE:
      // Could use sendmsg() to avoid copy here since there's no conversion, but this doesn't use much
      memcpy(dp,buffer,chunk * chan->output.channels * sizeof(float));
      chan->output.rtp.timestamp += chunk;
      buffer += chunk * chan->output.channels;
      bytes = chunk * chan->output.channels * sizeof(float);
      break;
#ifdef FLOAT16
    case F16LE:
      {
	_Float16 *pcm_buf = (_Float16 *)dp;
	for(int i=0; i < chunk * chan->output.channels; i++)
	  *pcm_buf++ = *buffer++;

	chan->output.rtp.timestamp += chunk;
	bytes = chunk * chan->output.channels * sizeof(*pcm_buf);
      }
      break;
#endif
    case OPUS:
      if(chan->output.opus != NULL){
	// Encoder already created; see if the parameters have changed
	// There doesn't seem to be any way to read back the channel count, so we save that explicitly
	// If the sample rate changes we'll get restarted anyway, so this test isn't really needed. But do it anyway.
	int s;
	opus_encoder_ctl(chan->output.opus,OPUS_GET_SAMPLE_RATE(&s));
	if(s != chan->output.samprate || chan->output.opus_channels != chan->output.channels){
	  opus_encoder_destroy(chan->output.opus);
	  chan->output.opus = NULL;
	  chan->output.opus_channels = 0;
	}
      }
      if(chan->output.opus == NULL){
	int error = OPUS_OK;

	// Opus only supports a specific set of sample rates
	if(chan->output.samprate != 48000 && chan->output.samprate != 24000 && chan->output.samprate != 16000 && chan->output.samprate != 12000
	   && chan->output.samprate != 8000){
	  chan->output.silent = true;
	  break; // Simply drop until somebody fixes it
	}
	chan->output.opus = opus_encoder_create(chan->output.samprate,chan->output.channels,Application,&error);
	assert(error == OPUS_OK && chan->output.opus);
	chan->output.opus_channels = chan->output.channels; // In case it changes

	// A communications receiver is unlikely to have more than 96 dB of output range
	// In fact this could be made smaller as an experiment
	error = opus_encoder_ctl(chan->output.opus,OPUS_SET_LSB_DEPTH(16));
	assert(error == OPUS_OK);

	error = opus_encoder_ctl(chan->output.opus,OPUS_SET_DTX(Discontinuous)); // Create an option to set this
	assert(error == OPUS_OK);

	error = opus_encoder_ctl(chan->output.opus,OPUS_SET_BITRATE(chan->output.opus_bitrate));
	assert(error == OPUS_OK);

	if(Fec_enable){ // Create an option to set this, but understand it first
	  error = opus_encoder_ctl(chan->output.opus,OPUS_SET_INBAND_FEC(1));
	  assert(error == OPUS_OK);
	  error = opus_encoder_ctl(chan->output.opus,OPUS_SET_PACKET_LOSS_PERC(Fec_enable));
	  assert(error == OPUS_OK);
	}
      }
      bytes = opus_encode_float(chan->output.opus,buffer,chunk,dp,sizeof(packet) - (dp-packet)); // Max # bytes in compressed output buffer
      assert(bytes >= 0);
      if(Discontinuous && bytes < 3){
	chan->output.silent = true;
	bytes = 0;
      }
      buffer += chunk * chan->output.channels;
      chan->output.rtp.timestamp += chunk * 48000 / chan->output.samprate; // Always increases at 48 kHz
      break;
    default:
      chan->output.silent = true;
      break;
    }
    if(!chan->output.silent){
      int r = sendto(Output_fd,&packet,bytes + (dp - packet),0,(struct sockaddr *)&chan->output.dest_socket,sizeof(chan->output.dest_socket));
      chan->output.rtp.bytes += bytes;
      chan->output.rtp.packets++;
      chan->output.rtp.seq++;
      chan->output.samples += chunk * chan->output.channels; // Count stereo frames
      if(r <= 0){
	if(errno == EAGAIN){
	  if(!TempSendFailure){
	    fprintf(stdout,"Temporary send failure, suggest increased buffering (see sysctl net.core.wmem_max, net.core.wmem_default\n");
	    fprintf(stdout,"Additional messages suppressed\n");
	    TempSendFailure = true;
	  }
	} else {
	  fprintf(stdout,"audio send failure: %s\n",strerror(errno));
	  abort(); // Probably more serious, like the loss of an interface or route
	}
      }
    }
    frames -= chunk;
    if(chan->output.pacing && frames > 0)
      usleep(pacing);
  }
  return 0;
}

#if 0 // Not currently used
void output_cleanup(void *p){
  struct channel * const chan = p;
  if(chan == NULL)
    return;
}
#endif
