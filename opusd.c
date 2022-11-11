// $Id: opusd.c,v 1.5 2022/08/05 06:35:10 karn Exp $
// Opus transcoder
// Read PCM audio from one or more multicast groups, compress with Opus and retransmit on another with same SSRC
// Currently subject to memory leaks as old group states aren't yet aged out
// Copyright Jan 2018 Phil Karn, KA9Q
// Major rewrite Nov 2020 for multithreaded encoding with one Opus encoder per thread
// Makes better use of multicore CPUs under heavy load (like encoding the entire 2m band at once)
#define _GNU_SOURCE 1
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <limits.h>
#include <string.h>
#if defined(linux)
#include <bsd/string.h>
#endif
#include <opus/opus.h>
#include <netdb.h>
#include <locale.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <signal.h>
#include <getopt.h>
#include <pthread.h>

#include "misc.h"
#include "multicast.h"
#include "status.h"
#include "iir.h"

#define BUFFERSIZE 16384  // Big enough for 120 ms @ 48 kHz stereo (11,520 16-bit samples)

struct session {
  struct session *prev;       // Linked list pointers
  struct session *next; 
  int type;                 // input RTP type (10,11)
  
  struct sockaddr sender;
  char addr[NI_MAXHOST];    // RTP Sender IP address
  char port[NI_MAXSERV];    // RTP Sender source port

  pthread_t thread;
  pthread_mutex_t qmutex;
  pthread_cond_t qcond;
  struct packet *queue;
 
  struct rtp_state rtp_state_in; // RTP input state
  int samprate; // PCM sample rate Hz
  int channels;
  OpusEncoder *opus;        // Opus encoder handle
  int silence;              // Currently suppressing silence

  float audio_buffer[BUFFERSIZE];      // Buffer to accumulate PCM until enough for Opus frame
  int audio_write_index;          // Index of next sample to write into audio_buffer

  struct rtp_state rtp_state_out; // RTP output state

  unsigned long underruns;  // Callback count of underruns (stereo samples) replaced with silence
  float deemph_rate;
  float deemph_gain;
  float deemph_state_left;
  float deemph_state_right;  
  long long packets;
};


// Global config variables
int const Bufsize = 1540;     // Maximum samples/words per RTP packet - must be smaller than Ethernet MTU

float const SCALE = 1./SHRT_MAX;

// Command line params
int Mcast_ttl = 1;
int IP_tos = 48;              // AF12 << 2
const char *App_path;
int Verbose;                  // Verbosity flag (currently unused)
int Opus_bitrate = 32;        // Opus stream audio bandwidth; default 32 kb/s
int Discontinuous = 0;        // Off by default
int Opus_blocktime = 20;      // Minimum frame size 20 ms, a reasonable default
int Fec_enable = 0;                  // Use forward error correction
int Application = OPUS_APPLICATION_AUDIO; // Encoder optimization mode
const float Corner_freq = 300; // Hz - corner frequency in de-emphasis integrator
const float LF_gain = 4;       // == 12 dB; empirical to make equal subjective voice loudness with flat FM
                               // Will make PL tone louder by this amount until we implement a filter
const float Latency = 0.02;    // chunk size for audio output callback

// Global variables
pthread_t Status_thread;
pthread_mutex_t Input_ready_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t Input_ready_cond = PTHREAD_COND_INITIALIZER;

int Status_fd = -1;           // Reading from radio status
int Status_out_fd = -1;       // Writing to radio status
int Input_fd = -1;            // Multicast receive socket
int Output_fd = -1;           // Multicast receive socket
struct session *Sessions;
pthread_mutex_t Session_protect = PTHREAD_MUTEX_INITIALIZER;
uint64_t Output_packets;
char *Name;
char *Output;
char *Input;
char *Status;

void closedown(int);
struct session *lookup_session(const struct sockaddr *,uint32_t);
struct session *create_session(void);
int close_session(struct session **);
int send_samples(struct session *sp);
void *input(void *arg);
void *encode(void *arg);
void *status(void *);

struct option Options[] =
  {
   {"iface", required_argument, NULL, 'A'},
   {"blocktime", required_argument, NULL, 'B'},
   {"block-time", required_argument, NULL, 'B'},
   {"pcm-in", required_argument, NULL, 'I'},
   {"name", required_argument, NULL, 'N'},
   {"status-in", required_argument, NULL, 'S'},
   {"opus-out", required_argument, NULL, 'R'},
   {"ttl", required_argument, NULL, 'T'},
   {"fec", no_argument, NULL, 'f'},
   {"bitrate", required_argument, NULL, 'o'},
   {"bit-rate", required_argument, NULL, 'o'},
   {"verbose", no_argument, NULL, 'v'},
   {"discontinuous", no_argument, NULL, 'x'},
   {"lowdelay",no_argument, NULL, 'l'},
   {"low-delay",no_argument, NULL, 'l'},
   {"voice", no_argument, NULL, 'V'},
   {"tos", required_argument, NULL, 'p'},
   {"iptos", required_argument, NULL, 'p'},
   {"ip-tos", required_argument, NULL, 'p'},    
   {NULL, 0, NULL, 0},

  };
   
char Optstring[] = "A:B:I:N:R:S:T:fo:vxp:";

struct sockaddr_storage Status_dest_address;
struct sockaddr_storage Status_input_source_address;
struct sockaddr_storage Local_status_source_address;
struct sockaddr_storage PCM_dest_address;
struct sockaddr_storage PCM_source_address;
struct sockaddr_storage Opus_dest_address;
struct sockaddr_storage Opus_source_address;

int main(int argc,char * const argv[]){
  App_path = argv[0];

  setlocale(LC_ALL,getenv("LANG"));

  int c;
  while((c = getopt_long(argc,argv,Optstring,Options,NULL)) != -1){
    switch(c){
    case 'A':
      Default_mcast_iface = optarg;
      break;
    case 'B':
      Opus_blocktime = strtol(optarg,NULL,0);
      break;
    case 'I':
      Input = optarg;
      break;
    case 'N':
      Name = optarg;
      break;
    case 'R':
      Output = optarg;
      break;
    case 'S':
      Status = optarg;
      break;
    case 'p':
      IP_tos = strtol(optarg,NULL,0);
      break;
    case 'T':
      Mcast_ttl = strtol(optarg,NULL,0);
      break;
    case 'f':
      Fec_enable = 1;
      break;
    case 'o':
      Opus_bitrate = strtol(optarg,NULL,0);
      break;
    case 'v':
      Verbose++;
      break;
    case 'x':
      Discontinuous = 1;
      break;
    case 'l':
      Application = OPUS_APPLICATION_RESTRICTED_LOWDELAY;
      break;
    case 'V':
      Application = OPUS_APPLICATION_VOIP;
      break;
    default:
      fprintf(stderr,"Usage: %s [-l|-V] [-x] [-v] [-f] [-p tos] [-o bitrate] [-B blocktime] [-N name] [-T ttl] [-A iface] [-I input_mcast_address | -S input_status_address] -R output_mcast_address\n",argv[0]);
      exit(1);
    }
  }
  if(Opus_blocktime != 2.5 && Opus_blocktime != 5
     && Opus_blocktime != 10 && Opus_blocktime != 20
     && Opus_blocktime != 40 && Opus_blocktime != 60
     && Opus_blocktime != 80 && Opus_blocktime != 100
     && Opus_blocktime != 120){
    fprintf(stderr,"opus block time must be 2.5/5/10/20/40/60/80/100/120 ms\n");
    fprintf(stderr,"80/100/120 supported only on opus 1.2 and later\n");
    exit(1);
  }
  if(Opus_bitrate < 500)
    Opus_bitrate *= 1000; // Assume it was given in kb/s

  if(!Output){
    fprintf(stderr,"Must specify --opus-out\n");
    exit(1);
  }
  
  char iface[1024];
  if(Input){
    resolve_mcast(Input,&PCM_dest_address,DEFAULT_RTP_PORT,iface,sizeof(iface));
    if(strlen(iface) == 0 && Default_mcast_iface != NULL)
      strlcpy(iface,Default_mcast_iface,sizeof(iface));
    Input_fd = listen_mcast(&PCM_dest_address,iface); // Port address already in place

    if(Input_fd == -1){
      fprintf(stderr,"Can't resolve input PCM group %s\n",Input);
      Input = NULL; // but maybe the status will work, if specified
    }
  }

  if(Status){
    pthread_create(&Status_thread,NULL,status,NULL);

    // Wait until the status thread discovers the input PCM stream
    pthread_mutex_lock(&Input_ready_mutex);
    while(Input_fd == -1)
      pthread_cond_wait(&Input_ready_cond,&Input_ready_mutex);
    pthread_mutex_unlock(&Input_ready_mutex);
  } else if(Input == NULL){
    fprintf(stderr,"Must specify either --status-in or --pcm-in\n");
    exit(1);
  }

  assert(Input_fd != -1);

  char description[1024];
  snprintf(description,sizeof(description),"pcm-source=%s",Input); // what if it changes?
  avahi_start(Name,"_opus._udp",5004,Output,ElfHashString(Output),description);

  // Can't resolve this until the avahi service is started
  resolve_mcast(Output,&Opus_dest_address,DEFAULT_RTP_PORT,iface,sizeof(iface));
  if(strlen(iface) == 0 && Default_mcast_iface != NULL)
    strlcpy(iface,Default_mcast_iface,sizeof(iface));
  Output_fd = connect_mcast(&Opus_dest_address,iface,Mcast_ttl,IP_tos);

  if(Output_fd == -1){
    fprintf(stderr,"Can't set up output on %s: %s\n",Output,strerror(errno));
    exit(1);
  }
  {
    socklen_t len = sizeof(Opus_source_address);
    getsockname(Output_fd,(struct sockaddr *)&Opus_source_address,&len);
  }

  // Graceful signal catch
  signal(SIGPIPE,closedown);
  signal(SIGINT,closedown);
  signal(SIGKILL,closedown);
  signal(SIGQUIT,closedown);
  signal(SIGTERM,closedown);
  signal(SIGPIPE,SIG_IGN);

  // Loop forever processing and dispatching incoming PCM packets
  // Process incoming RTP packets, demux to per-SSRC thread
  struct packet *pkt = NULL;
  while(1){
    // Need a new packet buffer?
    if(!pkt)
      pkt = malloc(sizeof(*pkt));
    // Zero these out to catch any uninitialized derefs
    pkt->next = NULL;
    pkt->data = NULL;
    pkt->len = 0;
    
    struct sockaddr_storage sender;
    socklen_t socksize = sizeof(sender);
    int size = recvfrom(Input_fd,&pkt->content,sizeof(pkt->content),0,(struct sockaddr *)&sender,&socksize);
    
    if(size == -1){
      if(errno != EINTR){ // Happens routinely, e.g., when window resized
	perror("recvfrom");
	usleep(1000);
      }
      continue;  // Reuse current buffer
    }
    if(size <= RTP_MIN_SIZE)
      continue; // Must be big enough for RTP header and at least some data
    
    // Extract and convert RTP header to host format
    unsigned char const *dp = ntoh_rtp(&pkt->rtp,pkt->content);
    pkt->data = dp;
    pkt->len = size - (dp - pkt->content);
    if(pkt->rtp.pad){
      pkt->len -= dp[pkt->len-1];
      pkt->rtp.pad = 0;
    }
    if(pkt->len <= 0)
      continue; // Used to be an assert, but would be triggered by bogus packets
    
    // Find appropriate session; create new one if necessary
    struct session *sp = lookup_session((const struct sockaddr *)&sender,pkt->rtp.ssrc);
    if(!sp){
      // Not found
      int const samprate = samprate_from_pt(pkt->rtp.type);
      if(samprate == 0)
	continue; // Unknown sample rate
      int const channels = channels_from_pt(pkt->rtp.type);
      if(channels == 0)
	continue; // Unknown channels

      sp = create_session();
      assert(sp != NULL);
      // Initialize
      getnameinfo((struct sockaddr *)&sender,sizeof(sender),sp->addr,sizeof(sp->addr),
		    sp->port,sizeof(sp->port),NI_NOFQDN|NI_DGRAM);
      memcpy(&sp->sender,&sender,sizeof(struct sockaddr));
      sp->rtp_state_out.ssrc = sp->rtp_state_in.ssrc = pkt->rtp.ssrc;
      sp->rtp_state_in.seq = pkt->rtp.seq; // Can cause a spurious drop indication if # pcm pkts != # opus pkts
      sp->rtp_state_in.timestamp = pkt->rtp.timestamp;
      sp->samprate = samprate;
      sp->channels = channels;

      // Span per-SSRC thread, each with its own instance of opus encoder
      if(pthread_create(&sp->thread,NULL,encode,sp) == -1){
	perror("pthread_create");
	close_session(&sp);
	continue;
      }
    }
    
    // Insert onto queue sorted by sequence number, wake up thread
    struct packet *q_prev = NULL;
    struct packet *qe = NULL;
    { // Mutex-protected segment
      pthread_mutex_lock(&sp->qmutex);
      for(qe = sp->queue; qe && pkt->rtp.seq >= qe->rtp.seq; q_prev = qe,qe = qe->next)
	;
      
      pkt->next = qe;
      if(q_prev)
	q_prev->next = pkt;
      else
	sp->queue = pkt; // Front of list
      pkt = NULL;        // force new packet to be allocated
      // wake up decoder thread
      pthread_cond_signal(&sp->qcond);
      pthread_mutex_unlock(&sp->qmutex);
    }
  }      
}


// Monitor and report to radio status channel (only if specified)
void * status(void *p){
  pthread_detach(pthread_self());
  pthread_setname("opstat");

  char iface[1024];
  resolve_mcast(Status,&Status_dest_address,DEFAULT_STAT_PORT,iface,sizeof(iface));
  if(strlen(iface) == 0 && Default_mcast_iface != NULL)
    strlcpy(iface,Default_mcast_iface,sizeof(iface));
  Status_fd = listen_mcast(&Status_dest_address,iface);
  if(Status_fd == -1){
    fprintf(stderr,"Can't set up input on %s: %s\n",Status,strerror(errno));
    return NULL;
  }
  // We will also emit our status to the radio status group
  Status_out_fd = connect_mcast(&Status_dest_address,iface,Mcast_ttl,IP_tos);
  {
    socklen_t len;
    len = sizeof(Local_status_source_address);
    getsockname(Status_out_fd,(struct sockaddr *)&Local_status_source_address,&len);
  }
  if(Input_fd == -1){
    // Timeout reads so we'll poll until we get a radio status message wth the PCM stream socket
    struct timeval timeout;
    timeout.tv_sec = 1;
    timeout.tv_usec = 0;
    setsockopt(Status_fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
  }

  while(1){
    socklen_t socklen = sizeof(Status_input_source_address);
    unsigned char buffer[16384];
    int const length = recvfrom(Status_fd,buffer,sizeof(buffer),0,(struct sockaddr *)&Status_input_source_address,&socklen);
    if(length <= 0){
      if(errno == EAGAIN || errno == ETIMEDOUT)
	send_poll(Status_out_fd,0); // Timeout; send poll
      continue;
    }

    // We MUST ignore our own packets, or we'll loop!
    if(address_match(&Status_input_source_address,&Local_status_source_address)
       && getportnumber(&Status_input_source_address) == getportnumber(&Local_status_source_address))
      continue;

    // Announce ourselves in response to commands
    if(buffer[0] == 1){
      unsigned char packet[2048];
      unsigned char *bp = packet;
      *bp++ = 0; // Response (not a command)
      encode_socket(&bp,OPUS_SOURCE_SOCKET,&Opus_source_address);
      encode_socket(&bp,OPUS_DEST_SOCKET,&Opus_dest_address);
      encode_int(&bp,OPUS_BITRATE,Opus_bitrate);
      encode_int(&bp,OPUS_PACKETS,Output_packets);
      encode_int(&bp,OPUS_TTL,Mcast_ttl);
      // Add more later
      encode_eol(&bp);
      int const len = bp - packet;
      if(len > 2)
	send(Status_out_fd,packet,len,0);
    } else {
      // Parse radio status for PCM output socket
      unsigned char const *cp = buffer+1;

      while(cp - buffer < length){
	enum status_type const type = *cp++;
	
	if(type == EOL)
	  break;
	
	unsigned int const optlen = *cp++;
	if(cp - buffer + optlen > length)
	  break;

	// Should probably extract sample rate too, though we get it from the RTP payload type
	switch(type){
	case EOL:
	  goto done;
	case OUTPUT_DATA_DEST_SOCKET:
	  {
	    struct sockaddr_storage dest_temp;
	    memset(&dest_temp,0,sizeof(dest_temp));
	    decode_socket(&dest_temp,cp,optlen);
	    if(address_match(&dest_temp,&PCM_dest_address)
	       && getportnumber(&dest_temp) == getportnumber(&PCM_dest_address))
	      break; // nothing changed

	    // new or changed PCM multicast group
	    if(Verbose)
	      fprintf(stderr,"Listening for PCM on %s\n",formatsock(&dest_temp));

	    int const fd = listen_mcast(&dest_temp,NULL); // Port address already in place
	    if(fd == -1){
	      if(Verbose){
		fprintf(stderr,"Multicast listen on %s failed\n",formatsock(&dest_temp));
	      }
	      break;
	    }
	    pthread_mutex_lock(&Input_ready_mutex);
	    if(Input_fd != -1)
	      close(Input_fd);
	    Input_fd = fd;
	    memcpy(&PCM_dest_address,&dest_temp,sizeof(dest_temp));
	    pthread_cond_broadcast(&Input_ready_cond);
	    pthread_mutex_unlock(&Input_ready_mutex);
	    
	    // Cancel timeouts and polls
	    struct timeval timeout;
	    timeout.tv_sec = 0;
	    timeout.tv_usec = 0;
	    setsockopt(Status_fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
	  }
	  break;
	default:  // Ignore all others for now
	  break;
	}
	cp += optlen;
      }
    }
    done:;
  }
  return NULL;
}


// Per-SSRC thread - does actual Opus encoding
// Warning! do not use "continue" within the loop as this will cause a memory leak.
// Jump to "endloop" instead
void *encode(void *arg){
  struct session *sp = (struct session *)arg;
  assert(sp != NULL);
  {
    char threadname[16];
    snprintf(threadname,sizeof(threadname),"op enc %u",sp->rtp_state_out.ssrc);
    pthread_setname(threadname);
  }
  // We will exit after 10 sec of idleness so detach ourselves to ensure resource recovery
  // Not doing this caused a nasty memory leak
  pthread_detach(pthread_self());

  int error = 0;
  sp->opus = opus_encoder_create(sp->samprate,sp->channels,Application,&error);
  assert(error == OPUS_OK && sp);
  
  error = opus_encoder_ctl(sp->opus,OPUS_SET_DTX(Discontinuous));
  assert(error == OPUS_OK);
  
  error = opus_encoder_ctl(sp->opus,OPUS_SET_BITRATE(Opus_bitrate));
  assert(error == OPUS_OK);
  
  if(Fec_enable){
    error = opus_encoder_ctl(sp->opus,OPUS_SET_INBAND_FEC(1));
    assert(error == OPUS_OK);
    error = opus_encoder_ctl(sp->opus,OPUS_SET_PACKET_LOSS_PERC(Fec_enable));
    assert(error == OPUS_OK);
  }
  
#if 0 // Is this even necessary?
      // Always seems to return error -5 even when OK??
  error = opus_encoder_ctl(sp->opus,OPUS_FRAMESIZE_ARG,Opus_blocktime);
  assert(1 || error == OPUS_OK);
#endif

  while(1){
    struct packet *pkt = NULL;
    {
      struct timespec waittime;
      clock_gettime(CLOCK_REALTIME,&waittime);
      waittime.tv_sec += 10;      // wait 10 seconds for a new packet
      { // Mutex-protected segment
	pthread_mutex_lock(&sp->qmutex);
	while(!sp->queue){      // Wait for packet to appear on queue
	  int ret = pthread_cond_timedwait(&sp->qcond,&sp->qmutex,&waittime);
	  assert(ret != EINVAL);
	  if(ret == ETIMEDOUT){
	    // Idle timeout after 10 sec; close session and terminate thread
	    pthread_mutex_unlock(&sp->qmutex);
	    close_session(&sp); 
	    return NULL; // exit thread
	  }
	}
	pkt = sp->queue;
	sp->queue = pkt->next;
	pkt->next = NULL;
	pthread_mutex_unlock(&sp->qmutex);
      } // End of mutex protected segment
    }

    sp->packets++; // Count all packets, regardless of type
    int const frame_size = pkt->len / (sizeof(int16_t) * sp->channels); // PCM sample times
    if(frame_size <= 0)
      goto endloop; // garbled packet?

    int const samples_skipped = rtp_process(&sp->rtp_state_in,&pkt->rtp,frame_size);
    if(samples_skipped < 0)
      goto endloop; // Old dupe
    
    if(sp->type != pkt->rtp.type){ // Handle transitions both ways
      sp->type = pkt->rtp.type;
    }
    if(sp->channels != channels_from_pt(pkt->rtp.type) || sp->samprate != samprate_from_pt(pkt->rtp.type)){
      // channels or sample rate changed; Re-initialize encoder
      sp->channels = channels_from_pt(pkt->rtp.type);
      sp->samprate = samprate_from_pt(pkt->rtp.type);
      opus_encoder_init(sp->opus,sp->samprate,sp->channels,Application);
    }

    if(pkt->rtp.marker || samples_skipped > 4 * 48000 * Opus_blocktime){ // Opus works on 48 kHz virtual samples
      // reset encoder state after 4 seconds of skip or a RTP marker bit
      opus_encoder_ctl(sp->opus,OPUS_RESET_STATE);
      sp->silence = 1;
    }
    signed short const *samples = (signed short *)pkt->data;
    
    for(int i=0; i < frame_size;i++){
      float left = SCALE * (signed short)ntohs(*samples++);
      sp->audio_buffer[sp->audio_write_index++] = left;
      if(sp->channels == 2){
	float right = SCALE * (signed short)ntohs(*samples++);
	sp->audio_buffer[sp->audio_write_index++] = right;
      }
    }
  endloop:;
    free(pkt);
    pkt = NULL;

    // send however many opus frames we can
    send_samples(sp);
  }
}

struct session *lookup_session(const struct sockaddr * const sender,const uint32_t ssrc){
  struct session *sp;
  pthread_mutex_lock(&Session_protect);
  for(sp = Sessions; sp != NULL; sp = sp->next){
    if(sp->rtp_state_in.ssrc == ssrc
       && address_match(&sp->sender,sender)){
      // Found it
      if(sp->prev != NULL){
	// Not at top of list; move it there
	if(sp->next != NULL)
	  sp->next->prev = sp->prev;

	sp->prev->next = sp->next;
	sp->prev = NULL;
	sp->next = Sessions;
	Sessions->prev = sp;
	Sessions = sp;
      }
      break;
    }
  }
  pthread_mutex_unlock(&Session_protect);
  return sp;
}
// Create a new session, partly initialize
struct session *create_session(void){

  struct session * const sp = calloc(1,sizeof(*sp));
  assert(sp != NULL); // Shouldn't happen on modern machines!
  
  // Initialize entry
  pthread_mutex_init(&sp->qmutex,NULL);
  pthread_cond_init(&sp->qcond,NULL);

  // Put at head of list
  pthread_mutex_lock(&Session_protect);
  sp->prev = NULL;
  sp->next = Sessions;
  if(sp->next != NULL)
    sp->next->prev = sp;
  Sessions = sp;
  pthread_mutex_unlock(&Session_protect);
  return sp;
}

int close_session(struct session ** p){
  if(p == NULL)
    return -1;
  struct session *sp = *p;
  if(sp == NULL)
    return -1;
  
  if(sp->opus != NULL){
    opus_encoder_destroy(sp->opus);
    sp->opus = NULL;
  }

  // packet queue should be empty, but just in case
  pthread_mutex_lock(&sp->qmutex);
  while(sp->queue){
    struct packet *pkt = sp->queue->next;
    free(sp->queue);
    sp->queue = pkt;
  }
  pthread_mutex_unlock(&sp->qmutex);
  pthread_mutex_destroy(&sp->qmutex);

  // Remove from linked list of sessions
  pthread_mutex_lock(&Session_protect);
  if(sp->next != NULL)
    sp->next->prev = sp->prev;
  if(sp->prev != NULL)
    sp->prev->next = sp->next;
  else
    Sessions = sp->next;
  pthread_mutex_unlock(&Session_protect);
  free(sp);
  *p = NULL;
  return 0;
}
void closedown(int s){
#if 0
  // Causes deadlock when we get called from a section where Session_protect is already locked
  // Which is the usual case
  // Not really necessary anyway, since we're exiting
  pthread_mutex_lock(&Session_protect);
  while(Sessions != NULL)
    close_session(Sessions);
  pthread_mutex_unlock(&Session_protect);
#endif

  pthread_mutex_destroy(&Session_protect);
  exit(0);
}
// Encode and send one or more Opus frames when we have enough
int send_samples(struct session * const sp){
  assert(sp != NULL);

  int pcm_samples_written = 0;
  while(1){
    float const ms_in_buffer = 1000.0 * sp->audio_write_index / (sp->channels * sp->samprate);
    if(ms_in_buffer < Opus_blocktime)
      break; // Less than minimum allowable Opus block size; wait

    // Choose largest Opus frame size <= time in buffer
    int frame_size; // Opus block size in (mono or stereo) samples
    if(ms_in_buffer >= 120)
      frame_size = 120 * sp->samprate / 1000;
    else if(ms_in_buffer >= 100)
      frame_size = 100 * sp->samprate / 1000;
    else if(ms_in_buffer >= 80)
      frame_size = 80 * sp->samprate / 1000;
    else if(ms_in_buffer >= 60)
      frame_size = 60 * sp->samprate / 1000;      
    else if(ms_in_buffer >= 40)
      frame_size = 40 * sp->samprate / 1000;      
    else if(ms_in_buffer >= 20)
      frame_size = 20 * sp->samprate / 1000;      
    else if(ms_in_buffer >= 10)
      frame_size = 10 * sp->samprate / 1000;
    else if(ms_in_buffer >= 5)
      frame_size = 5 * sp->samprate / 1000;      
    else if(ms_in_buffer >= 2.5)
      frame_size = 2.5 * sp->samprate / 1000;
    else
      break; // Shouldn't be reached with reasonable Opus_blocktime

    // Set up to transmit Opus RTP/UDP/IP
    struct rtp_header rtp;
    memset(&rtp,0,sizeof(rtp));
    rtp.version = RTP_VERS;
    rtp.type = OPUS_PT; // Opus
    rtp.seq = sp->rtp_state_out.seq;
    rtp.timestamp = sp->rtp_state_out.timestamp;
    rtp.ssrc = sp->rtp_state_out.ssrc;
    
    if(sp->silence){
      // Beginning of talk spurt after silence, set marker bit
      rtp.marker = 1;
      sp->silence = 0;
    } else
      rtp.marker = 0;
    
    unsigned char output_buffer[Bufsize]; // to hold RTP header + Opus-encoded frame
    unsigned char * const opus_write_pointer = hton_rtp(output_buffer,&rtp);
    int packet_bytes_written = opus_write_pointer - output_buffer;

    int const opus_output_bytes = opus_encode_float(sp->opus,
						    sp->audio_buffer,
						    frame_size,  // Number of uncompressed *stereo* samples per frame
						    opus_write_pointer,
						    Bufsize - packet_bytes_written); // Max # bytes in compressed output buffer
    packet_bytes_written += opus_output_bytes;
    
    if(!Discontinuous || opus_output_bytes > 2){
      // ship it
      if(send(Output_fd,output_buffer,packet_bytes_written,0) < 0)
	return -1;
      Output_packets++; // all sessions
      sp->rtp_state_out.seq++; // Increment only if packet is sent
      sp->rtp_state_out.bytes += opus_output_bytes;
      sp->rtp_state_out.packets++;
    } else
      sp->silence = 1;
    
    sp->rtp_state_out.timestamp += frame_size * 48000 / sp->samprate; // Always increase timestamp by virtual 48k sample rate
    const int remaining_bytes = sizeof(sp->audio_buffer[0]) * (sp->audio_write_index - sp->channels * frame_size);
    assert(remaining_bytes >= 0);
    memmove(sp->audio_buffer,&sp->audio_buffer[sp->channels * frame_size],remaining_bytes);
    sp->audio_write_index -= frame_size * sp->channels;
    pcm_samples_written += frame_size * sp->channels;
  }
  return pcm_samples_written;
}
