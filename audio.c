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

// Send PCM output on stream; # of channels implicit in chan->output.channels
int send_output(struct channel * restrict const chan,float const * restrict buffer,int frames,bool const mute){
  assert(chan != NULL);
  if(frames <= 0 || chan->output.channels == 0 || chan->output.samprate == 0)
    return 0;

  if(mute){
    // Still increment timestamp
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
  case F32:
    max_frames_per_pkt = BYTES_PER_PKT / (sizeof(float) * chan->output.channels);
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
  rtp.marker = chan->output.silent;
  chan->output.silent = false;
  useconds_t pacing = 0;
  if(chan->output.pacing)
    pacing = 1000 * Blocktime * max_frames_per_pkt / frames; // for optional pacing, in microseconds

  while(frames > 0){
    int chunk = min(max_frames_per_pkt,frames);
    rtp.timestamp = chan->output.rtp.timestamp;
    chan->output.rtp.timestamp += chunk; // Increase by frame count
    chan->output.rtp.packets++;
    rtp.seq = chan->output.rtp.seq++;
    uint8_t packet[PKTSIZE];
    uint8_t *dp = (uint8_t *)hton_rtp(packet,&rtp); // First byte after RTP header
    int bytes = 0;
    switch(chan->output.encoding){
    case S16BE:
      {
	int16_t *pcm_buf = (int16_t *)dp;
	for(int i=0; i < chunk * chan->output.channels; i++)
	  *pcm_buf++ = htons(scaleclip(*buffer++)); // Byte swap

	bytes = chunk * chan->output.channels * sizeof(int16_t);
      }
      break;
    case S16LE:
      {
	int16_t *pcm_buf = (int16_t *)dp;
	for(int i=0; i < chunk * chan->output.channels; i++)
	  *pcm_buf++ = scaleclip(*buffer++); // No byte swap

	bytes = chunk * chan->output.channels * sizeof(int16_t);
      }
      break;
    case F32:
      {
	// Could use sendmsg() to avoid copy here since there's no conversion, but this doesn't use much
	memcpy(dp,buffer,chunk * chan->output.channels * sizeof(float));
	bytes = chunk * chan->output.channels * sizeof(float);
      }
      break;
    default:
      break;
    }
    int r = sendto(Output_fd,&packet,bytes + (dp - packet),0,(struct sockaddr *)&chan->output.dest_socket,sizeof(chan->output.dest_socket));
    chan->output.rtp.bytes += bytes;
    chan->output.samples += chunk * chan->output.channels; // Count frames
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
