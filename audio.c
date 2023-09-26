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

#include "misc.h"
#include "multicast.h"
#include "radio.h"

#define PCM_BUFSIZE 480        // 16-bit word count; must fit in Ethernet MTU
#define PACKETSIZE 65536        // Somewhat larger than Ethernet MTU

// Send 'size' stereo samples, each in a pair of floats
int send_stereo_output(struct channel * restrict const chan,float const * restrict buffer,int size,bool const mute){
  if(mute){
    // Increment timestamp
    chan->output.rtp.timestamp += size; // Increase by sample count
    chan->output.silent = true;
    return 0;
  }

  int pcm_bufsize = PCM_BUFSIZE; // Default for non-linux systems
#ifdef IP_MTU
  {
    int mtu;
    socklen_t size = sizeof(mtu);
    getsockopt(chan->output.data_fd,IPPROTO_IP,IP_MTU,&mtu,&size);
    pcm_bufsize = (mtu - 100) / 2; // allow 100 bytes for headers
  }
#endif

  struct rtp_header rtp;
  memset(&rtp,0,sizeof(rtp));
  rtp.type = pt_from_info(chan->output.samprate,2);
  rtp.version = RTP_VERS;
  rtp.ssrc = chan->output.rtp.ssrc;

  while(size > 0){
    int chunk = min(pcm_bufsize,2*size);
    // If packet is all zeroes, don't send it but still increase the timestamp
    rtp.timestamp = chan->output.rtp.timestamp;
    chan->output.rtp.timestamp += chunk/2; // Increase by sample count
    chan->output.rtp.bytes += sizeof(int16_t) * chunk;
    chan->output.rtp.packets++;
    rtp.marker = chan->output.silent;
    chan->output.silent = false;
    rtp.seq = chan->output.rtp.seq++;
    uint8_t packet[PACKETSIZE];
    int16_t *pcm_buf = (int16_t *)hton_rtp(packet,&rtp);
    for(int i=0; i < chunk; i ++)
      *pcm_buf++ = htons(scaleclip(*buffer++));

    uint8_t *dp = (uint8_t *)pcm_buf;
    int r = send(chan->output.data_fd,&packet,dp - packet,0);
    chan->output.samples += chunk/2; // Count stereo samples
    if(r <= 0){
      perror("pcm send");
      return -1;
    }
    size -= chunk/2;
  }
  return 0;
}

// Send 'size' mono samples, each in a float
int send_mono_output(struct channel * restrict const chan,float const * restrict buffer,int size,bool const mute){
  if(mute){
    // Increment timestamp
    chan->output.rtp.timestamp += size; // Increase by sample count
    chan->output.silent = true;
    return 0;
  }
  int pcm_bufsize = PCM_BUFSIZE; // Default for non-linux systems
#ifdef IP_MTU
  {
    int mtu;
    socklen_t size = sizeof(mtu);
    getsockopt(chan->output.data_fd,IPPROTO_IP,IP_MTU,&mtu,&size);
    pcm_bufsize = (mtu - 100) / 2; // allow 100 bytes for headers
  }
#endif

  struct rtp_header rtp;
  memset(&rtp,0,sizeof(rtp));
  rtp.version = RTP_VERS;
  rtp.type = pt_from_info(chan->output.samprate,1);
  rtp.ssrc = chan->output.rtp.ssrc;

  while(size > 0){
    int chunk = min(pcm_bufsize,size); // # of mono samples (frames)

    // If packet is muted, don't send it but still increase the timestamp
    rtp.timestamp = chan->output.rtp.timestamp;
    chan->output.rtp.timestamp += chunk; // Increase by sample count
    chan->output.rtp.packets++;
    chan->output.rtp.bytes += sizeof(int16_t) * chunk;
    // Transition from silence emits a mark bit
    rtp.marker = chan->output.silent;
    chan->output.silent = false;
    rtp.seq = chan->output.rtp.seq++;
    uint8_t packet[PACKETSIZE];
    int16_t *pcm_buf = (int16_t *)hton_rtp(packet,&rtp);
    for(int i=0; i < chunk; i++)
      *pcm_buf++ = htons(scaleclip(*buffer++));

    uint8_t *dp = (uint8_t *)pcm_buf;
    int r = send(chan->output.data_fd,&packet,dp - packet,0);
    chan->output.samples += chunk;
    if(r <= 0){
      perror("pcm send");
      return -1;
    }
    size -= chunk;
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
