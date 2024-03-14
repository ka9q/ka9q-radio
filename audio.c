// Audio multicast routines for ka9q-radio
// Handles linear 16-bit PCM, mono and stereo
// Copyright 2017-2023 Phil Karn, KA9Q

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

#define SAMPLES_PER_PKT 480        // 16-bit word count; must fit in Ethernet MTU

bool GetSockOptFailed = false;     // Have we issued this log message yet?
bool TempSendFailure = false;

// Send PCM output on stream; # of channels implicit in chan->output.channels
int send_output(struct channel * restrict const chan,float const * restrict buffer,int frames,bool const mute){
  assert(chan != NULL);
  assert(chan->output.data_fd >= 0);
  if(frames <= 0 || chan->output.channels == 0 || chan->output.samprate == 0)
    return 0;

  int const type = pt_from_info(chan->output.samprate,chan->output.channels);  // It might have changed
  if(type < 0)
    return 0; // Can't allocate a payload type!

  chan->output.rtp.type = type;

  if(mute){
    // Increment timestamp
    chan->output.rtp.timestamp += frames; // Increase by frame count
    chan->output.silent = true;
    return 0;
  }
  if(chan->output.data_fd < 0){
    fprintf(stdout,"ssrc %d: invalid output descriptor %d!\n",chan->output.rtp.ssrc,chan->output.data_fd);
    return 0;
  }
  int frames_per_pkt = 0;
#ifdef IP_MTU
  {
    // We can get the MTU of the outbound interface, use it to calculate maximum packet size
    int mtu;
    socklen_t intsize = sizeof(mtu);
    int r = getsockopt(chan->output.data_fd,IPPROTO_IP,IP_MTU,&mtu,&intsize);
    if(r != 0){
      if(!GetSockOptFailed){
	frames_per_pkt = SAMPLES_PER_PKT / chan->output.channels; // Default frames per packet for non-linux systems
	fprintf(stdout,"Can't get socket MTU (%s), using default %d samples\n",strerror(errno),frames_per_packet);
	GetSockOptFailed = true;
      }

    } else {
      frames_per_pkt = (mtu - 100) / (chan->output.channels * sizeof(int16_t)); // allow 100 bytes for headers
    }
  }
# else
  frames_per_pkt = SAMPLES_PER_PKT / chan->output.channels; // Default frames per packet for non-linux systems
#endif
  struct rtp_header rtp;
  memset(&rtp,0,sizeof(rtp));
  rtp.version = RTP_VERS;
  rtp.type = chan->output.rtp.type;
  rtp.ssrc = chan->output.rtp.ssrc;
  rtp.marker = chan->output.silent;
  chan->output.silent = false;
  useconds_t pacing = 0;
  if(chan->output.pacing)
    pacing = 1000 * Blocktime * frames_per_pkt / frames; // for optional pacing, in microseconds

  while(frames > 0){
    int chunk = min(frames_per_pkt,frames);
    rtp.timestamp = chan->output.rtp.timestamp;
    chan->output.rtp.timestamp += chunk; // Increase by frame count
    chan->output.rtp.bytes += sizeof(int16_t) * chunk * chan->output.channels;
    chan->output.rtp.packets++;
    rtp.seq = chan->output.rtp.seq++;
    uint8_t packet[PKTSIZE];
    int16_t *pcm_buf = (int16_t *)hton_rtp(packet,&rtp);
    for(int i=0; i < chunk * chan->output.channels; i++)
      *pcm_buf++ = htons(scaleclip(*buffer++));

    uint8_t const *dp = (uint8_t *)pcm_buf;
    int r = send(chan->output.data_fd,&packet,dp - packet,0);
    chan->output.samples += chunk * chan->output.channels; // Count frames
    if(r <= 0){
      if(errno == EAGAIN && !TempSendFailure){
	fprintf(stdout,"Temporary send failure, suggest increased buffering (see sysctl net.core.wmem_max, net.core.wmem_default\n");
	fprintf(stdout,"Additional messages suppressed\n");
	TempSendFailure = true;
	return 0; // Treat as soft failure
      }
      fprintf(stdout,"audio send failure: %s\n",strerror(errno));
      abort(); // Probably more serious, like the loss of an interface or route
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

  if(chan->output.data_fd > 0){
    close(chan->output.data_fd);
    chan->output.data_fd = -1;
  }
}
#endif
