// $Id: audio.c,v 1.105 2022/06/23 22:04:20 karn Exp $
// Audio multicast routines for KA9Q SDR receiver
// Handles linear 16-bit PCM, mono and stereo
// Copyright 2017 Phil Karn, KA9Q

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

#include "misc.h"
#include "multicast.h"
#include "radio.h"

#define PCM_BUFSIZE 480        // 16-bit word count; must fit in Ethernet MTU
#define PACKETSIZE 2048        // Somewhat larger than Ethernet MTU

// Send 'size' stereo samples, each in a pair of floats
int send_stereo_output(struct demod * restrict const demod,float const * restrict buffer,int size,bool const mute){

  if(mute){
    // Increment timestamp
    demod->output.rtp.timestamp += size; // Increase by sample count
    demod->output.silent = true;
    return 0;
  }

  struct rtp_header rtp;
  memset(&rtp,0,sizeof(rtp));
  rtp.type = pt_from_info(demod->output.samprate,2);
  rtp.version = RTP_VERS;
  rtp.ssrc = demod->output.rtp.ssrc;

  while(size > 0){
    int chunk = min(PCM_BUFSIZE,2*size);
    // If packet is all zeroes, don't send it but still increase the timestamp
    rtp.timestamp = demod->output.rtp.timestamp;
    demod->output.rtp.timestamp += chunk/2; // Increase by sample count
    demod->output.rtp.bytes += sizeof(signed short) * chunk;
    demod->output.rtp.packets++;
    rtp.marker = demod->output.silent;
    demod->output.silent = false;
    rtp.seq = demod->output.rtp.seq++;
    unsigned char packet[PACKETSIZE];
    int16_t *pcm_buf = (int16_t *)hton_rtp(packet,&rtp);
    for(int i=0; i < chunk; i ++)
      *pcm_buf++ = htons(scaleclip(*buffer++));

    unsigned char *dp = (unsigned char *)pcm_buf;
    int r = send(demod->output.data_fd,&packet,dp - packet,0);
    demod->output.samples += chunk/2; // Count stereo samples
    if(r <= 0){
      perror("pcm send");
      return -1;
    }
    size -= chunk/2;
  }
  return 0;
}

// Send 'size' mono samples, each in a float
int send_mono_output(struct demod * restrict const demod,float const * restrict buffer,int size,bool const mute){
  if(mute){
    // Increment timestamp
    demod->output.rtp.timestamp += size; // Increase by sample count
    demod->output.silent = true;
    return 0;
  }
  struct rtp_header rtp;
  memset(&rtp,0,sizeof(rtp));
  rtp.version = RTP_VERS;
  rtp.type = pt_from_info(demod->output.samprate,1);
  rtp.ssrc = demod->output.rtp.ssrc;

  while(size > 0){
    int chunk = min(PCM_BUFSIZE,size); // # of mono samples (frames)

    // If packet is muted, don't send it but still increase the timestamp
    rtp.timestamp = demod->output.rtp.timestamp;
    demod->output.rtp.timestamp += chunk; // Increase by sample count
    demod->output.rtp.packets++;
    demod->output.rtp.bytes += sizeof(signed short) * chunk;
    // Transition from silence emits a mark bit
    rtp.marker = demod->output.silent;
    demod->output.silent = false;
    rtp.seq = demod->output.rtp.seq++;
    unsigned char packet[PACKETSIZE];
    int16_t *pcm_buf = (int16_t *)hton_rtp(packet,&rtp);
    for(int i=0; i < chunk; i++)
      *pcm_buf++ = htons(scaleclip(*buffer++));

    unsigned char *dp = (unsigned char *)pcm_buf;
    int r = send(demod->output.data_fd,&packet,dp - packet,0);
    demod->output.samples += chunk;
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
  struct demod * const demod = p;
  if(demod == NULL)
    return;

  if(demod->output.data_fd > 0){
    close(demod->output.data_fd);
    demod->output.data_fd = -1;
  }
}
#endif
