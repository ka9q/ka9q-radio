// $Id: packetd.c,v 1.6 2022/08/05 06:35:10 karn Exp $
// AFSK/FM packet demodulator
// Reads RTP PCM audio stream, emits decoded frames in multicast RTP
// Copyright 2018, Phil Karn, KA9Q

#define _GNU_SOURCE 1
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <locale.h>
#include <netdb.h>
#include <getopt.h>

#include "osc.h"
#include "filter.h"
#include "misc.h"
#include "multicast.h"
#include "ax25.h"
#include "status.h"

struct hdlc {
  unsigned char frame[16384];
  int frame_bits;
  int flag_seen;
  int last_bits;
};
static int hdlc_process(struct hdlc *hp,int bit);

// Needs to be redone with common RTP receiver module
struct session {
  struct session *next; 
  
  struct rtp_state rtp_state_in;
  struct rtp_state rtp_state_out;
  int samprate;
  int write_fd,read_fd;

  pthread_t decode_thread;
  unsigned int decoded_packets;
  struct hdlc hdlc;
};

// Config constants
#define MAX_MCAST 20          // Maximum number of multicast addresses
static float const SCALE = 1./32768;
static int const AL = 960; // 20 ms @ 48 kHz = 1x 20 ms blocks = 24 bit times @ 1200 bps
static int const AM = 961;
static float Bitrate = 1200;

// Command line params
const char *App_path;
int Verbose;
int IP_tos = 0;
int Mcast_ttl = 10;           // Very low intensity output

// Global variables
static int Nfds;          // Number of PCM streams
static fd_set Fdset_template; // Mask for select()
static int Max_fd = 2;        // Highest number fd for select()
static int Input_fd[MAX_MCAST];    // Multicast receive sockets
static pthread_t Input_thread;

static int Output_fd = -1;
static int Status_fd = -1;
#if 0
static int Status_out_fd = -1; // Not used yet
#endif
static struct session *Session;
static pthread_mutex_t Output_mutex = PTHREAD_MUTEX_INITIALIZER;
struct sockaddr_storage Status_dest_address;
struct sockaddr_storage Status_input_source_address;
struct sockaddr_storage Local_status_source_address;
struct sockaddr_storage PCM_dest_address; // From incoming status messages (max 1)

static struct session *lookup_session(const uint32_t ssrc);
static struct session *create_session(uint32_t ssrc);
#if 0
static int close_session(struct session *sp);
#endif
static void *input(void *arg);
static void *decode_task(void *arg);
static void printtime(FILE *fp);

static struct option Options[] =
  {
   {"iface", required_argument, NULL, 'A'},
   {"pcm-in", required_argument, NULL, 'I'},
   {"ax25-out", required_argument, NULL, 'R'},
   {"name", required_argument, NULL, 'N'},
   {"status-in", required_argument, NULL, 'S'},
   {"ttl", required_argument, NULL, 'T'},
   {"verbose", no_argument, NULL, 'v'},
#if 0
   {"samprate",required_argument,NULL,'r'},
   {"samplerate",required_argument,NULL,'r'},
#endif
   {"tos", required_argument, NULL, 'p'},
   {"iptos", required_argument, NULL, 'p'},
   {"ip-tos", required_argument, NULL, 'p'},    
   {NULL, 0, NULL, 0},
  };

static char const Optstring[] = "A:I:N:R:S:T:vp:";
char const *Name;
char const *Output;
char const *Input[MAX_MCAST];

int main(int argc,char *argv[]){
  App_path = argv[0];
  // Drop root if we have it
  if(seteuid(getuid()) != 0)
    fprintf(stdout,"seteuid: %s\n",strerror(errno));

  setlocale(LC_ALL,getenv("LANG"));

  FD_ZERO(&Fdset_template);
  // Unlike aprs and aprsfeed, stdout is not line buffered because each packet
  // generates a multi-line dump. So we have to be sure to fflush(stdout) after each
  // packet in case we're redirected into a file

  int c;
  while((c = getopt_long(argc,argv,Optstring,Options,NULL)) != EOF){
    switch(c){
    case 'N':
      Name = optarg;
      break;
    case 'A':
      Default_mcast_iface = optarg;
      break;
    case 'I':
      if(Nfds == MAX_MCAST){
	fprintf(stdout,"Too many multicast addresses; max %d\n",MAX_MCAST);
	break;
      }
      Input[Nfds] = optarg;
      Input_fd[Nfds] = setup_mcast_in(optarg,NULL,0);
      if(Input_fd[Nfds] == -1){
	fprintf(stdout,"Can't set up input %s\n",optarg);
	break;
      }
      Max_fd = max(Max_fd,Input_fd[Nfds]);
      FD_SET(Input_fd[Nfds],&Fdset_template);
      Nfds++;
      if(Status_fd != -1)
	fprintf(stdout,"warning: --status-in ignored when --pcm-in specified\n");
      break;
    case 'R':
      Output = optarg;
      break;
    case 'S':
      if(Nfds != 0){
	fprintf(stdout,"--status-in ignored when --pcm-in specified\n");
	break;
      }
      if(Status_fd != -1){
	fprintf(stdout,"Warning: only last --status-in is used\n");
	close(Status_fd);
	Status_fd = -1;
      }
      Status_fd = setup_mcast_in(optarg,(struct sockaddr *)&Status_dest_address,2);
      if(Status_fd == -1){
	fprintf(stdout,"Can't set up status input on %s: %s\n",optarg,strerror(errno));
	exit(1);
      }
#if 0 // Later use?
      Status_out_fd = setup_mcast(NULL,(struct sockaddr *)&Status_dest_address,1,Mcast_ttl,IP_tos,2);
      {
	socklen_t len;
	len = sizeof(Local_status_source_address);
	getsockname(Status_out_fd,(struct sockaddr *)&Local_status_source_address,&len);
      }
#endif
      break;
    case 'p':
      IP_tos = strtol(optarg,NULL,0);
      break;
    case 'T':
      Mcast_ttl = strtol(optarg,NULL,0);
      break;
    case 'v':
      Verbose++;
      break;
      break;
    default:
      fprintf(stdout,"Usage: %s [--verbose|-v] [--ttl|-T mcast_ttl] [--pcm-in|-I input_mcast_address [--pcm-in|-I address2]] [--ax25-out|-R output_mcast_address] [input_address ...]\n",argv[0]);
      exit(1);
    }
  }
  if(Name == NULL)
    Name = argv[0]; // Give it a default


  // Also accept groups without -I option
  for(int i=optind; i < argc; i++){
    if(Nfds == MAX_MCAST){
      fprintf(stdout,"Too many multicast addresses; max %d\n",MAX_MCAST);
      break;
    }
    Input[Nfds] = argv[i];
    Input_fd[Nfds] = setup_mcast_in(Input[Nfds],NULL,0);
    if(Input_fd[Nfds] == -1){
      fprintf(stdout,"Can't set up input %s\n",Input[Nfds]);
      continue;
    }
    Max_fd = max(Max_fd,Input_fd[Nfds]);
    FD_SET(Input_fd[Nfds],&Fdset_template);
    Nfds++;
    if(Status_fd != -1)
      fprintf(stdout,"warning: --status-in ignored when --pcm-in specified\n");
  }

  if(Nfds == 0  && Status_fd == -1){
    fprintf(stdout,"Must specify either --status-in or --pcm-in\n");
    exit(1);
  }
  {
    char description[1024];
    memset(description,0,sizeof(description));
    int p = snprintf(description,sizeof(description),"pcm-source=");
    for(int i=0; i < Nfds;i++){
      if(sizeof(description) <= p)
	break; // Too long!
      p += snprintf(&description[p],sizeof(description)-p,"%s%s",i > 0 ? "," : "" ,Input[i]);
    }
    avahi_start(Name,"_ax25._udp",DEFAULT_RTP_PORT,Output,ElfHashString(Output),description);
  }
  Output_fd = setup_mcast(Output,NULL,1,Mcast_ttl,IP_tos,0);
  if(Output_fd == -1){
    fprintf(stdout,"Must specify --ax25-out\n");
    exit(1);
  }

  if(Nfds > 0)
    pthread_create(&Input_thread,NULL,input,NULL);

  while(Status_fd == -1)
    sleep(10000); // Status channel not specified; sleep indefinitely

  // Process status messages that may tell us the PCM input
  while(1){
    socklen_t socklen = sizeof(Status_input_source_address);
    unsigned char buffer[16384];
    int length = recvfrom(Status_fd,buffer,sizeof(buffer),0,(struct sockaddr *)&Status_input_source_address,&socklen);

    // We MUST ignore our own status packets, or we'll loop!
    if(address_match(&Status_input_source_address, &Local_status_source_address)
       && getportnumber(&Status_input_source_address) == getportnumber(&Local_status_source_address))
      continue;

    if(length <= 0){
      usleep(10000);
      continue;
    }
    // Parse entries
    {
      int const cr = buffer[0];
      if(cr == 1)
	continue; // Ignore commands
      unsigned char const *cp = buffer+1;

      while(cp - buffer < length){
	enum status_type const type = *cp++;
	
	if(type == EOL)
	  break;
	
	unsigned int const optlen = *cp++;
	if(cp - buffer + optlen > length)
	  break;
	
	switch(type){
	case EOL:
	  goto done;
	case OUTPUT_DATA_DEST_SOCKET:
	  decode_socket(&PCM_dest_address,cp,optlen);
	  if(Nfds == 0){
	    // For now, process at most one source in status messages only if not explicitly given with --pcm-in
	    if(Verbose){
	      printtime(stdout);
	      fprintf(stdout,"joining pcm input channel %s\n",formatsock(&PCM_dest_address));
	    }

	    Input_fd[Nfds] = setup_mcast_in(NULL,(struct sockaddr *)&PCM_dest_address,0);
	    if(Input_fd[Nfds] != -1){
	      Max_fd = max(Max_fd,Input_fd[Nfds]);
	      FD_SET(Input_fd[Nfds],&Fdset_template);
	      Nfds++;
	      pthread_create(&Input_thread,NULL,input,NULL);
	    }
	  }
	  break;
	default:  // Ignore all others for now
	  break;
	}
	cp += optlen;
      }
    done:;
    }
  }
}

// Process input PCM
// audio input thread
// Receive audio multicasts, multiplex into sessions
static void *input(void *arg){

  while(1){
    struct rtp_header rtp_hdr;
    struct sockaddr sender;

    // Wait for traffic to arrive
    fd_set fdset = Fdset_template;
    int const s = select(Max_fd+1,&fdset,NULL,NULL,NULL);
    if(s < 0 && errno != EAGAIN && errno != EINTR)
      break;
    if(s == 0)
      continue; // Nothing arrived; probably just an ignored signal

    for(int fd_index = 0;fd_index < Nfds;fd_index++){
      if(Input_fd[fd_index] == -1 || !FD_ISSET(Input_fd[fd_index],&fdset))
	continue;

      unsigned char buffer[PKTSIZE];
      socklen_t socksize = sizeof(sender);
      int size = recvfrom(Input_fd[fd_index],buffer,sizeof(buffer),0,&sender,&socksize);
      if(size == -1){
	if(errno != EINTR){ // Happens routinely
	  perror("recvfrom");
	  usleep(1000); // avoid tight loop
	}
	continue;
      }
      if(size < RTP_MIN_SIZE)
	continue; // Too small to be valid RTP

      // Extract RTP header
      unsigned char const *dp = buffer;
      dp = ntoh_rtp(&rtp_hdr,dp);
      size -= dp - buffer;
      
      if(rtp_hdr.pad){
	// Remove padding
	size -= dp[size-1];
	rtp_hdr.pad = 0;
      }
      if(size < 0)
	continue; // garbled RTP header?
      
      // Should distinguish between these with different filter balances
      if(channels_from_pt(rtp_hdr.type) != 1)
	continue; // Only mono PCM for now
      
      struct session *sp = lookup_session(rtp_hdr.ssrc);
      if(sp == NULL){
	// Not found
	if((sp = create_session(rtp_hdr.ssrc)) == NULL){
	  printtime(stdout);
	  fprintf(stdout," No room for new session ssrc %u\n",rtp_hdr.ssrc);
	  fflush(stdout);
	  continue;
	}
	sp->rtp_state_out.ssrc = sp->rtp_state_in.ssrc = rtp_hdr.ssrc;
	// Extract sample rate (what it if later changes??)
	sp->samprate = samprate_from_pt(rtp_hdr.type);
	int fildes[2];
	if(pipe(fildes) != 0){
	  if(Verbose)
	    perror("pipe() failed");
	  continue;
	}
	sp->read_fd = fildes[0]; sp->write_fd = fildes[1];

	pthread_create(&sp->decode_thread,NULL,decode_task,sp); // One decode thread per stream
	if(Verbose){
	  printtime(stdout);
	  fprintf(stdout," New session from %s, ssrc %u\n",formatsock(&sender),sp->rtp_state_in.ssrc);
	  fflush(stdout);
	}
      }
      int const sample_count = size / sizeof(signed short); // 16-bit sample count
      int skipped_samples = rtp_process(&sp->rtp_state_in,&rtp_hdr,sample_count);
      if(rtp_hdr.marker)
	skipped_samples = 0; // Ignore samples skipped before mark

      if(Verbose && skipped_samples != 0){
	printtime(stdout);
	fprintf(stdout," skipped samples %d\n",skipped_samples); fflush(stdout);
      }
      if(skipped_samples < 0)
	continue;	// Drop probable duplicate(s)

      if(skipped_samples > 0){
	// Don't worry too much about skipped samples right now
	// There's no FEC, and enough are probably dropped that sync wouldn't be maintained anyway
	int max_skip = min(skipped_samples,1920); // Pad only a short interruption, max
	int16_t zeroes[max_skip];
	memset(zeroes,0,sizeof(zeroes));
	if(write(sp->write_fd,zeroes,sizeof(zeroes)) != sizeof(zeroes))
	  perror("write zeroes");
      }
      if(write(sp->write_fd,dp,sample_count * sizeof(int16_t)) != sample_count * sizeof(int16_t))
	perror("write samples");
    }
  }
  return NULL; // Never gets here
}



// Find existing session in table, if it exists
static struct session *lookup_session(const uint32_t ssrc){
  struct session *sp;
  for(sp = Session; sp != NULL; sp = sp->next){
    if(sp->rtp_state_in.ssrc == ssrc)
      // Found it
      return sp;
  }
  return NULL;
}

// Create a new session, partly initialize
static struct session *create_session(uint32_t ssrc){
  struct session *sp;

  if((sp = calloc(1,sizeof(*sp))) == NULL)
    return NULL; // Shouldn't happen on modern machines!
  
  sp->rtp_state_in.ssrc = ssrc;

  // Put at head of bucket chain
  sp->next = Session;
  Session = sp;
  return sp;
}

#if 0
// Remove a session entry. Not yet used, so sessions keep growing
static int close_session(struct session *sp){
  if(sp == NULL)
    return -1;
  
  // Remove from linked list
  struct session *se,*se_prev = NULL;
  for(se = Session; se && se != sp; se_prev = se,se = se->next)
    ;
  if(!se)
    return -1;
  
  if(se == sp){
    if(se_prev)
      se_prev->next = sp->next;
    else
      Session = se_prev;
  }
  return 0;
}
#endif

const float mark_tone = 1200;
const float space_tone = 2200;

// AFSK demod
static void *decode_task(void *arg){
  float const twist = mark_tone/space_tone; // Scale back upper tone from FM demod

  pthread_setname("afsk");
  struct session *sp = (struct session *)arg;
  assert(sp != NULL);

  struct filter_in *filter_in = create_filter_input(AL,AM,REAL);
  struct filter_out *filter_out = create_filter_output(filter_in,NULL,AL,COMPLEX);
  const float filter_low = min(mark_tone,space_tone) - Bitrate/4;
  const float filter_high = max(mark_tone,space_tone) + Bitrate/4;
  set_filter(filter_out,filter_low/sp->samprate,filter_high/sp->samprate,3.0); // Creates analytic, band-limited signal

  // Tone replica generators (-1200 and -2200 Hz)
  struct osc mark;
  memset(&mark,0,sizeof(mark));
  set_osc(&mark,-mark_tone/sp->samprate, 0.0);
  
  struct osc space;
  memset(&space,0,sizeof(space));
  set_osc(&space,-space_tone/sp->samprate, 0.0);  
    
  int samppbit = sp->samprate / Bitrate;

  // Tone integrators
  int symphase = 0;
  float complex mark_accum = 0; // On-time
  float complex space_accum = 0;
  float complex mark_offset_accum = 0; // Straddles previous zero crossing
  float complex space_offset_accum = 0;
  float last_val = 0;  // Last on-time symbol
  float mid_val = 0;   // Last zero crossing symbol

  FILE *fp = fdopen(sp->read_fd,"r");
  if(fp == NULL){
    perror("fdopen");
    return NULL;
  }

  int pad = 0;

  while(1){
    signed short samples[AL];

    if(pad > 0){
      pad--;
      memset(samples,0,sizeof(samples));
    } else {
      if(fread(samples,sizeof(samples[0]),AL,fp) != AL){
	fprintf(stderr,"pipe read error, exiting thread\n");
	fclose(fp);
 	break;
      }
      // Look for 100 zeroes at end of frame to indicate squelch closing
      int nonzero = 0;
      for(int i=AL-100; i < AL; i++)
	nonzero |= samples[i];
      if(!nonzero)
	pad = 5; // flush filters with 5 blocks of padding
    }

    assert(filter_in->ilen == AL);
    assert(filter_out->olen == AL);
    for(int n=0; n < AL; n++){
      if(put_rfilter(filter_in,ntohs(samples[n]) * SCALE) == 0)
	continue;
      execute_filter_output(filter_out,0);    // Shouldn't block
      for(int n=0; n<filter_out->olen; n++){
	// Spin down by mark and space frequencies, accumulate each in boxcar (comb) filters
	// Mark and space each have in-phase and offset integrators for timing recovery
	float complex s;
	s = filter_out->output.c[n] * step_osc(&mark);
	mark_accum += s;
	mark_offset_accum += s;
	
	s = filter_out->output.c[n] * step_osc(&space);
	space_accum += s;
	space_offset_accum += s;
	
	if(++symphase == samppbit/2){
	  // Finish offset integrator and reset
	  mid_val = cnrmf(mark_offset_accum) - twist * cnrmf(space_offset_accum);
	  mark_offset_accum = space_offset_accum = 0;
	}
	if(symphase < samppbit)
	  continue;
	
	// Finished whole bit
	float const cur_val = cnrmf(mark_accum) - twist * cnrmf(space_accum);
	mark_accum = space_accum = 0;
	
	if(cur_val * last_val >= 0){ // cur_val and last_val have same sign; no transition
	  // No transition == NRZI one
	  symphase = 0;
	  hdlc_process(&sp->hdlc,1); // Frame can't end with 1-bit, so don't check return
	} else {	// transition occurred --> NRZI zero
	  symphase = ((cur_val - last_val) * mid_val) > 0 ? +1 : -1;	// Gardner-style clock adjust
	  int const bytes = hdlc_process(&sp->hdlc,0);
	  if(Verbose && bytes < 0){
	    // Lock output to prevent intermingled output
	    pthread_mutex_lock(&Output_mutex);
	    printtime(stdout);
	    fprintf(stdout," ssrc %u CRC fail\n",sp->rtp_state_in.ssrc);
	    fflush(stdout);
	    pthread_mutex_unlock(&Output_mutex);
	  } else if(bytes > 0){ // Valid frame
	    if(Verbose){
	      pthread_mutex_lock(&Output_mutex);
	      printtime(stdout);
	      fprintf(stdout," ssrc %u packet %d len %d:\n",sp->rtp_state_in.ssrc,sp->decoded_packets++,bytes);
	      dump_frame(stdout,sp->hdlc.frame,bytes);
	      fflush(stdout);
	      pthread_mutex_unlock(&Output_mutex);
	    } // Verbose
	    struct rtp_header rtp_hdr;
	    memset(&rtp_hdr,0,sizeof(rtp_hdr));
	    rtp_hdr.version = 2;
	    rtp_hdr.type = AX25_PT;
	    rtp_hdr.seq = sp->rtp_state_out.seq++;
	    // RTP timestamp??
	    rtp_hdr.timestamp = sp->rtp_state_out.timestamp;
	    sp->rtp_state_out.timestamp += bytes;
	    rtp_hdr.ssrc = sp->rtp_state_out.ssrc;
	    
	    int const plen = bytes + 76 + 10; // Max RTP header is 76 bytes; allow a little slack
	    unsigned char packet[plen],*dp;
	    dp = packet;
	    dp = hton_rtp(dp,&rtp_hdr);
	    memcpy(dp,sp->hdlc.frame,bytes);
	    sp->hdlc.frame_bits = 0;
	    dp += bytes;
	    send(Output_fd,packet,dp - packet,0); // Check return code?
	    sp->rtp_state_out.packets++;
	    sp->rtp_state_out.bytes += bytes;
	  } // if(bytes > 0
	}
	last_val = cur_val;
      }
    }
  }
  return NULL;
}

// Process incoming HDLC bit
// Return nonzero byte count if there's a complete valid frame
// Caller recovers frame (including 2-byte CRC) in hp->frame, must set hp->frame_bits = 0 when done
static int hdlc_process(struct hdlc *hp,int bit){
  bit &= 1;

  hp->last_bits <<= 1; // Note last_bits is big-endian, HDLC bytes are actually little-endian
  hp->last_bits |= bit;
  
  if((hp->last_bits & 0xff) == 0x7e){
    // 01111110 - Flag
    int const bytes = (hp->frame_bits - 7) >> 3; // Don't count leading 7 bits of flag
    if(hp->flag_seen && bytes > 2){
      hp->frame_bits = 0;
      if(crc_good(hp->frame,bytes)){
	return bytes; // Caller must set frame_bits to 0 when done
      } else
	return -1;
    }
    hp->frame_bits = 0;
    hp->flag_seen = 1;
    return 0;
  }
  if(!hp->flag_seen)
    return 0; // Nothing more to do until there's a flag
 
  if((hp->last_bits & 0x7f) == 0x7f){
    // .1111111 - 7 consecutive 1's - abort
    hp->frame_bits = 0;
    hp->flag_seen = 0; // Do nothing else until we see a flag again
    return 0;
  } else if((hp->last_bits & 0x3f) == 0x3e){
    // ..111110 - drop stuffed zero
    return 0;
  }
  // Add bit to frame
  if(hp->frame_bits > sizeof(hp->frame) << 3){
    // Too long; abort
    hp->frame_bits = 0;
    hp->flag_seen = 0;
    return 0;
  }
  // Clear each new byte
  if((hp->frame_bits & 7) == 0)
    hp->frame[hp->frame_bits >> 3] = 0;

  // Write bit in little-endian order
  hp->frame[hp->frame_bits >> 3] |= bit << (hp->frame_bits & 7);
  hp->frame_bits++;

  return 0;
}
void printtime(FILE *fp){
  char result[1024];
  format_gpstime(result,sizeof(result),gps_time_ns());
  fputs(result,fp);
}



