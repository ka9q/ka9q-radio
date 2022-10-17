// $Id: pcmcat.c,v 1.21 2022/05/10 04:01:32 karn Exp $
// Receive and stream PCM RTP data to stdout
// Should emit .wav format by default to encode sample rate & parameters for subsequent encoding

#define _GNU_SOURCE 1
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <locale.h>
#include <errno.h>
#include <ctype.h>
#include <sys/socket.h>
#include <netdb.h>
#include <time.h>

#include "multicast.h"

struct pcmstream {
  struct pcmstream *prev;       // Linked list pointers
  struct pcmstream *next; 
  uint32_t ssrc;            // RTP Sending Source ID
  int type;                 // RTP type (10,11,20)
  
  struct sockaddr sender;
  char addr[NI_MAXHOST];    // RTP Sender IP address
  char port[NI_MAXSERV];    // RTP Sender source port

  struct rtp_state rtp_state;
};

// Config constants
int const Bufsize = 2048;
float const Samprate = 48000;

// Command line params
char *Mcast_address_text;
int Quiet;
int Stereo;   // Force stereo output; otherwise output mono, downmixing if necessary
const char *App_path;
int Verbose;

int Input_fd = -1;
struct pcmstream *Pcmstream;
int Sessions; // Session count - limit to 1 for now
uint32_t Ssrc; // Requested SSRC

struct pcmstream *lookup_session(const struct sockaddr *sender,const uint32_t ssrc);
struct pcmstream *make_session(struct sockaddr const *sender,uint32_t ssrc,uint16_t seq,uint32_t timestamp);
int close_session(struct pcmstream *sp);

int main(int argc,char *argv[]){
  App_path = argv[0];
  setlocale(LC_ALL,getenv("LANG"));

  int c;
  while((c = getopt(argc,argv,"qhs:2")) != EOF){
    switch(c){
    case '2': // Force stereo
      Stereo++;
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
    exit(1);
  }


  // audio input thread
  // Receive audio multicasts, multiplex into sessions, send to output
  // What do we do if we get different streams?? think about this
  while(1){
    struct sockaddr sender;
    socklen_t socksize = sizeof(sender);
    unsigned char buffer[Bufsize];
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
    unsigned char const *dp = ntoh_rtp(&rtp,buffer);
    size -= dp - buffer;
    if(rtp.pad){
      // Remove padding
      size -= dp[size-1];
      rtp.pad = 0;
    }
    if(size <= 0)
      continue;

    struct pcmstream *sp = lookup_session(&sender,rtp.ssrc);
    if(sp == NULL){
      // Not found
      if(Sessions || (Ssrc !=0 && rtp.ssrc != Ssrc)){
	// Only take specified SSRC or first SSRC for now
	continue;
      }
      if((sp = make_session(&sender,rtp.ssrc,rtp.seq,rtp.timestamp)) == NULL){
	fprintf(stderr,"No room for new session!!\n");
	continue;
      }
      getnameinfo((struct sockaddr *)&sender,sizeof(sender),sp->addr,sizeof(sp->addr),
		  //		    sp->port,sizeof(sp->port),NI_NOFQDN|NI_DGRAM|NI_NUMERICHOST);
		    sp->port,sizeof(sp->port),NI_NOFQDN|NI_DGRAM);

      if(!Quiet){
	fprintf(stderr,"New session from %u@%s:%s, type %d",sp->ssrc,sp->addr,sp->port,rtp.type);

	switch(rtp.type){
	case PCM_STEREO_PT:
	case PCM_STEREO_24_PT:
	case PCM_STEREO_16_PT:
	case PCM_STEREO_12_PT:
	case PCM_STEREO_8_PT:
	  fprintf(stderr,", pcm stereo");
	  if(!Stereo)
	    fprintf(stderr,", downmixing to mono");
	  break;
	case PCM_MONO_PT:
	case PCM_MONO_24_PT:
	case PCM_MONO_16_PT:
	case PCM_MONO_12_PT:
	case PCM_MONO_8_PT:
	  fprintf(stderr,", pcm mono");
	  if(Stereo)
	    fprintf(stderr,", expanding to pseudo-stereo");	    
	  break;
	}
	fprintf(stderr,"\n");
      }

      Sessions++;
    }
    int samples_skipped = rtp_process(&sp->rtp_state,&rtp,0); // get rid of last arg
    if(samples_skipped < 0)
      continue; // old dupe? What if it's simply out of sequence?

    sp->type = rtp.type;
    int samples = 0;

    signed short *sdp = (signed short *)dp;
    switch(rtp.type){
    case PCM_STEREO_PT:
    case PCM_STEREO_24_PT:
    case PCM_STEREO_16_PT:
    case PCM_STEREO_12_PT:
    case PCM_STEREO_8_PT:
      
      samples = size / 4;
      while(samples-- > 0){
	// Swap sample to host order, cat to stdout
	signed short left = ntohs(*sdp++);
	signed short right = ntohs(*sdp++);
	if(Stereo){
	  fwrite(&left,sizeof(left),1,stdout);
	  fwrite(&right,sizeof(right),1,stdout);
	} else {
	  // Downmix to mono
	  signed short samp = (left + right) / 2;
	  fwrite(&samp,sizeof(samp),1,stdout);
	}
      }
      break;
    case PCM_MONO_PT: // Mono
    case PCM_MONO_24_PT:
    case PCM_MONO_16_PT:
    case PCM_MONO_12_PT:
    case PCM_MONO_8_PT:
      samples = size / 2;
      while(samples-- > 0){
	// Swap sample to host order, cat to stdout
	signed short d = ntohs(*sdp++);
	fwrite(&d,sizeof(d),1,stdout);
	if(Stereo)
	  fwrite(&d,sizeof(d),1,stdout);   // Force to pseudo-stereo
      }
      break;
    default:
      samples = 0;
      break; // ignore
    }
    fflush(stdout);
  }
  exit(0);
}



struct pcmstream *lookup_session(const struct sockaddr *sender,const uint32_t ssrc){
  struct pcmstream *sp;
  for(sp = Pcmstream; sp != NULL; sp = sp->next){
    if(sp->ssrc == ssrc && address_match(&sp->sender,sender)){
      // Found it
      if(sp->prev != NULL){
	// Not at top of bucket chain; move it there
	if(sp->next != NULL)
	  sp->next->prev = sp->prev;

	sp->prev->next = sp->next;
	sp->prev = NULL;
	sp->next = Pcmstream;
	Pcmstream = sp;
      }
      return sp;
    }
  }
  return NULL;
}
// Create a new session, partly initialize
struct pcmstream *make_session(struct sockaddr const *sender,uint32_t ssrc,uint16_t seq,uint32_t timestamp){
  struct pcmstream *sp;

  if((sp = calloc(1,sizeof(*sp))) == NULL)
    return NULL; // Shouldn't happen on modern machines!
  
  // Initialize entry
  memcpy(&sp->sender,sender,sizeof(struct sockaddr));
  sp->ssrc = ssrc;

  // Put at head of bucket chain
  sp->next = Pcmstream;
  if(sp->next != NULL)
    sp->next->prev = sp;
  Pcmstream = sp;
  return sp;
}

int close_session(struct pcmstream *sp){
  if(sp == NULL)
    return -1;
  
  // Remove from linked list
  if(sp->next != NULL)
    sp->next->prev = sp->prev;
  if(sp->prev != NULL)
    sp->prev->next = sp->next;
  else
    Pcmstream = sp->next;
  free(sp);
  return 0;
}



