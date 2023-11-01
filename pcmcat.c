// Receive and stream PCM RTP data to stdout
// Should emit .wav format by default to encode sample rate & parameters for subsequent encoding
// Revised Aug 2023 to more cleanly handle sender restarts
// Copyright 2023 Phil Karn, KA9Q

#define _GNU_SOURCE 1
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <locale.h>
#include <errno.h>
#include <ctype.h>
#include <sys/socket.h>
#include <netdb.h>
#include <time.h>
#include <sysexits.h>

#include "misc.h"
#include "multicast.h"

struct pcmstream {
  uint32_t ssrc;            // RTP Sending Source ID
  int type;                 // RTP type (10,11,20)
  
  struct sockaddr sender;
  char addr[NI_MAXHOST];    // RTP Sender IP address
  char port[NI_MAXSERV];    // RTP Sender source port

  struct rtp_state rtp_state;
  int channels;
  
};

// Command line params
static char const *Mcast_address_text;
static int Quiet;
static int Channels = 1;   // Output channels
const char *App_path;
int Verbose;

static int Input_fd = -1;
static struct pcmstream Pcmstream;
static uint32_t Ssrc; // Requested SSRC

static int init(struct pcmstream *pc,struct rtp_header const *rtp,struct sockaddr const *sender);

int main(int argc,char *argv[]){
  App_path = argv[0];
  setlocale(LC_ALL,getenv("LANG"));

  int c;
  while((c = getopt(argc,argv,"qhs:2V")) != EOF){
    switch(c){
    case 'V':
      VERSION();
      exit(EX_OK);
    case '2': // Force stereo
      Channels = 2;
      break;
    case 'v':
      Verbose++;
      break;
    case 'q':
      Quiet++;
      break;
    case 's':
      Ssrc = strtol(optarg,NULL,0);
      break;
    case 'h':
    default:
      fprintf(stderr,"Usage: %s [-v] [-s ssrc] mcast_address\n",argv[0]);
      fprintf(stderr,"       hex ssrc requires 0x prefix\n");
      exit(1);
    }
  }
  if(optind != argc-1){
    fprintf(stderr,"mcast_address not specified\n");
      exit(1);
  }
  Mcast_address_text = argv[optind];

  // Set up multicast input
  Input_fd = setup_mcast_in(Mcast_address_text,NULL,0);
  if(Input_fd == -1){
    fprintf(stderr,"Can't set up input from %s\n",
	    Mcast_address_text);
    exit(EX_USAGE);
  }


  // audio input thread
  // Receive audio multicasts, multiplex into sessions, send to output
  // What do we do if we get different streams?? think about this
  while(true){
    struct sockaddr sender;
    socklen_t socksize = sizeof(sender);
    uint8_t buffer[PKTSIZE];
    // Gets all packets to multicast destination address, regardless of sender IP, sender port, dest port, ssrc
    int size = recvfrom(Input_fd,buffer,sizeof(buffer),0,&sender,&socksize);
    if(size == -1){
      if(errno != EINTR){ // Happens routinely
	perror("recvmsg");
	usleep(1000);
      }
      continue;
    }
    if(size < RTP_MIN_SIZE)
      continue; // Too small to be valid RTP

    struct rtp_header rtp;
    uint8_t const *dp = ntoh_rtp(&rtp,buffer);

    size -= dp - buffer;
    if(rtp.pad){
      // Remove padding
      size -= dp[size-1];
      rtp.pad = 0;
    }
    if(size <= 0)
      continue;

    if(rtp.ssrc == 0 || (Ssrc != 0 && rtp.ssrc != Ssrc))
       continue; // Ignore unwanted or invalid SSRCs

    if(Pcmstream.ssrc == 0){
      // First packet on stream, initialize
      init(&Pcmstream,&rtp,&sender);
      
      if(!Quiet){
	fprintf(stderr,"New session from %u@%s:%s, type %d, channels %d\n",
		Pcmstream.ssrc,
		Pcmstream.addr,
		Pcmstream.port,rtp.type,Pcmstream.channels);
      }
    } else if(rtp.ssrc != Pcmstream.ssrc)
      continue; // unwanted SSRC, ignore

    if(!address_match(&sender,&Pcmstream.sender) || getportnumber(&Pcmstream.sender) != getportnumber(&sender)){
      // Source changed, the sender restarted
      init(&Pcmstream,&rtp,&sender);
      if(!Quiet){
	fprintf(stderr,"Session restart from %u@%s:%s\n",
		Pcmstream.ssrc,
		Pcmstream.addr,
		Pcmstream.port);
      }
    }
    if(rtp.marker)
      Pcmstream.rtp_state.timestamp = rtp.timestamp;      // Resynch

    if(Pcmstream.channels != channels_from_pt(rtp.type)){
      if(!Quiet)
	fprintf(stderr,"Channel count changed from %d to %d\n",Pcmstream.channels,channels_from_pt(rtp.type));
      Pcmstream.channels = channels_from_pt(rtp.type); 
    }
    if(Pcmstream.channels != 1 && Pcmstream.channels != 2)
      continue; // Invalid

    int const time_step = (int32_t)(rtp.timestamp - Pcmstream.rtp_state.timestamp);
    if(time_step < 0){
      // Old dupe
      Pcmstream.rtp_state.dupes++;
      continue;
    } else if(time_step > 0){
      Pcmstream.rtp_state.drops++;
      fprintf(stderr,"Drops %llu\n",(long long unsigned)Pcmstream.rtp_state.drops);
      if(time_step < 48000){	// Arbitrary threshold - clean this up!
	int16_t zeroes[time_step];
	memset(zeroes,0,sizeof(zeroes));
	fwrite(zeroes,sizeof(*zeroes),time_step,stdout);
	if(Channels == 2)
	  fwrite(zeroes,sizeof(*zeroes),time_step,stdout); // Write it twice
      }
      // Resync
      Pcmstream.rtp_state.timestamp = rtp.timestamp; // Bring up to date?
    }
    Pcmstream.rtp_state.bytes += size;
    
    int const sampcount = size / sizeof(int16_t); // # of 16-bit samples, regardless of mono or stereo
    int const framecount = sampcount / Pcmstream.channels; // == sampcount for mono, sampcount/2 for stereo
    int16_t * const sdp = (int16_t *)dp;

    // Byte swap incoming buffer, regardless of channels
    for(int i=0; i < sampcount; i++)
      sdp[i] = ntohs(sdp[i]);
    
    if(Channels == Pcmstream.channels) {
      fwrite(sdp,sizeof(*sdp),sampcount,stdout); // Both mono or stereo, no expansion/mixing needed
    } else if(Channels == 1 && Pcmstream.channels == 2) {
      for(int i=0; i < framecount; i++) // Downmix to mono
	sdp[i] = (sdp[2*i] + sdp[2*i + 1]) / 2;

      fwrite(sdp,sizeof(*sdp),framecount,stdout);
    } else {
      // Expand to pseudo-stereo
      int16_t output[2*sampcount];
      for(int i=0; i < sampcount; i++)
	output[2*i] = output[2*i+1] = sdp[i];

      fwrite(output,sizeof(*output),sampcount*2,stdout);
    }
    fflush(stdout);
    Pcmstream.rtp_state.timestamp += framecount;
    Pcmstream.rtp_state.seq = rtp.seq + 1;
  }
  exit(0); // Not reached
}
static int init(struct pcmstream *pc,struct rtp_header const *rtp,struct sockaddr const *sender){
  // First packet on stream, initialize
  pc->ssrc = rtp->ssrc;
  pc->type = rtp->type;
  pc->channels = channels_from_pt(rtp->type);
  
  memcpy(&pc->sender,sender,sizeof(pc->sender)); // Remember sender
  getnameinfo((struct sockaddr *)&pc->sender,sizeof(pc->sender),
	      pc->addr,sizeof(pc->addr),
	      pc->port,sizeof(pc->port),NI_NOFQDN|NI_DGRAM);
  pc->rtp_state.timestamp = rtp->timestamp;
  pc->rtp_state.seq = rtp->seq;
  pc->rtp_state.packets = 0;
  pc->rtp_state.bytes = 0;
  pc->rtp_state.drops = 0;
  pc->rtp_state.dupes = 0;
  return 0;
}

