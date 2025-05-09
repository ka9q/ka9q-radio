// Mostly obsolete now that radiod can generate opus directly, but probably should be updated
// Note! Doesn't really work right with consumer progs that get metadata from status beacons on data stream
// Should modify, not just copy, metadata to indicate transcoding to Opus

// Opus transcoder
// Read PCM audio from one or more multicast groups, compress with Opus and retransmit on another with same SSRC
// Currently subject to memory leaks as old group states aren't yet aged out

// Major rewrite Nov 2020 for multithreaded encoding with one Opus encoder per thread
// Makes better use of multicore CPUs under heavy load (like encoding the entire 2m band at once)
// Copyright Jan 2018-2023 Phil Karn, KA9Q

#define _GNU_SOURCE 1
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
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
#include <sched.h>
#include <sysexits.h>
#include <fcntl.h>

#include "misc.h"
#include "multicast.h"
#include "rtp.h"
#include "status.h"
#include "iir.h"
#include "avahi.h"

#define BUFFERSIZE 16384  // Big enough for 120 ms @ 48 kHz stereo (11,520 16-bit samples)

struct session {
  struct session *prev;       // Linked list pointers
  struct session *next;
  int type;                 // input RTP type (10,11)

  struct sockaddr sender;
  char const *source;

  pthread_t thread;
  pthread_mutex_t qmutex;
  pthread_cond_t qcond;
  struct packet *queue;

  struct rtp_state rtp_state_in; // RTP input state
  int samprate; // PCM sample rate Hz
  int channels;
  OpusEncoder *opus;        // Opus encoder handle
  bool silence;              // Currently suppressing silence

  float audio_buffer[BUFFERSIZE];      // Buffer to accumulate PCM until enough for Opus frame
  int audio_write_index;          // Index of next sample to write into audio_buffer

  struct rtp_state rtp_state_out; // RTP output state

  unsigned long underruns;  // Callback count of underruns (stereo samples) replaced with silence
  uint64_t packets;
};


float const SCALE = 1./INT16_MAX;

// Command line params
int Mcast_ttl = 1;
int Mcast_tos = 48;              // AF12 << 2
const char *App_path;
int Verbose;                  // Verbosity flag (currently unused)
int Opus_bitrate = 32000;        // Opus stream audio bandwidth; default 32 kb/s
bool Discontinuous = false;        // Off by default
int Opus_blocktime = 20;      // Minimum frame size 20 ms, a reasonable default
bool Fec_enable = false;                  // Use forward error correction
int Application = OPUS_APPLICATION_AUDIO; // Encoder optimization mode
const float Latency = 0.02;    // chunk size for audio output callback

// Global variables
int Status_fd = -1;           // Reading from radio status
int Input_fd = -1;            // Multicast receive socket
int Output_fd = -1;           // Multicast receive socket
struct session *Sessions;
pthread_mutex_t Session_protect = PTHREAD_MUTEX_INITIALIZER;
uint64_t Output_packets;
char const *Name;
char const *Output;
char const *Input;

void closedown(int);
struct session *lookup_session(const struct sockaddr *,uint32_t);
struct session *create_session(void);
int close_session(struct session **);
int send_samples(struct session *sp);
void *input(void *arg);
void *encode(void *arg);

struct option Options[] =
  {
   {"iface", required_argument, NULL, 'A'},
   {"blocktime", required_argument, NULL, 'B'},
   {"block-time", required_argument, NULL, 'B'},
   {"pcm-in", required_argument, NULL, 'I'},
   {"name", required_argument, NULL, 'N'},
   {"opus-out", required_argument, NULL, 'R'},
   {"ttl", required_argument, NULL, 'T'},
   {"fec", no_argument, NULL, 'f'},
   {"bitrate", required_argument, NULL, 'o'},
   {"bit-rate", required_argument, NULL, 'o'},
   {"verbose", no_argument, NULL, 'v'},
   {"discontinuous", no_argument, NULL, 'x'},
   {"lowdelay",no_argument, NULL, 'l'},
   {"low-delay",no_argument, NULL, 'l'},
   {"voice", no_argument, NULL, 's'},
   {"speech", no_argument, NULL, 's'},
   {"tos", required_argument, NULL, 'p'},
   {"iptos", required_argument, NULL, 'p'},
   {"ip-tos", required_argument, NULL, 'p'},
   {"version", no_argument, NULL, 'V'},
   {NULL, 0, NULL, 0},

  };

char const Optstring[] = "A:B:I:N:R:T:fo:vxp:V";

struct sockaddr PCM_in_socket;
struct sockaddr Metadata_in_socket;
struct sockaddr Opus_out_socket;
struct sockaddr Metadata_out_socket;

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
    case 'p':
      Mcast_tos = strtol(optarg,NULL,0);
      break;
    case 'T':
      Mcast_ttl = strtol(optarg,NULL,0);
      break;
    case 'f':
      Fec_enable = true;
      break;
    case 'o':
      Opus_bitrate = strtol(optarg,NULL,0);
      break;
    case 'v':
      Verbose++;
      break;
    case 'x':
      Discontinuous = true;
      break;
    case 'l':
      Application = OPUS_APPLICATION_RESTRICTED_LOWDELAY;
      break;
    case 's':
      Application = OPUS_APPLICATION_VOIP;
      break;
    case 'V':
      VERSION();
      exit(EX_OK);
    default:
      fprintf(stderr,"Usage: %s [-V|--version] [-l |--lowdelay|--low-delay |-s | --speech | --voice] \
[-x|--discontinuous] [-v|--verbose] [-f|--fec] [-p|--iptos|--tos|--ip-tos tos|] \
[-o|--bitrate|--bit-rate bitrate] [-B|--blocktime|--block-time --blocktime] [-N|--name name] \
[-T|--ttl ttl] [-A|--iface iface] [-I|--pcm-in input_mcast_address ] \
-R|--opus-out output_mcast_address\n",argv[0]);
      exit(EX_USAGE);
    }
  }
  if(Opus_blocktime != 2.5 && Opus_blocktime != 5
     && Opus_blocktime != 10 && Opus_blocktime != 20
     && Opus_blocktime != 40 && Opus_blocktime != 60
     && Opus_blocktime != 80 && Opus_blocktime != 100
     && Opus_blocktime != 120){
    fprintf(stderr,"opus block time must be 2.5/5/10/20/40/60/80/100/120 ms\n");
    fprintf(stderr,"80/100/120 supported only on opus 1.2 and later\n");
    exit(EX_USAGE);
  }
  if(Opus_bitrate < 500)
    Opus_bitrate *= 1000; // Assume it was given in kb/s

  if(!Output){
    fprintf(stderr,"Must specify --opus-out\n");
    exit(EX_USAGE);
  }
  if(Input == NULL){
    fprintf(stderr,"Must specify --pcm-in\n");
    exit(EX_USAGE);
  }
  char iface[1024];
  if(Input){
    resolve_mcast(Input,&PCM_in_socket,DEFAULT_RTP_PORT,iface,sizeof(iface),0);
    if(strlen(iface) == 0 && Default_mcast_iface != NULL)
      strlcpy(iface,Default_mcast_iface,sizeof(iface));
    Input_fd = listen_mcast(NULL,&PCM_in_socket,iface); // Port address already in place

    if(Input_fd == -1){
      fprintf(stderr,"Can't resolve input PCM group %s\n",Input);
      Input = NULL; // but maybe the status will work, if specified - need to rewrite this
    }
    {
      // Same IP address, but status port number
      Metadata_in_socket = PCM_in_socket;
      struct sockaddr_in *sin = (struct sockaddr_in *)&Metadata_in_socket;
      sin->sin_port = htons(DEFAULT_STAT_PORT);
    }
    resolve_mcast(Input,&Metadata_in_socket,DEFAULT_STAT_PORT,iface,sizeof(iface),0);
    Status_fd = listen_mcast(NULL,&Metadata_in_socket,iface);
  }

  {
    char description[1024];
    snprintf(description,sizeof(description),"pcm-source=%s",Input); // what if it changes?
    size_t socksize = sizeof(Opus_out_socket);
    uint32_t addr = make_maddr(Output);
    avahi_start(Name,"_opus._udp",DEFAULT_RTP_PORT,Output,addr,description,&Opus_out_socket,&socksize);
    struct sockaddr_in *sin = (struct sockaddr_in *)&Metadata_out_socket;
    sin->sin_family = AF_INET;
    sin->sin_addr.s_addr = htonl(addr);
    sin->sin_port = htons(DEFAULT_STAT_PORT);
  }
  // Can't resolve this until the avahi service is started
  if(strlen(iface) == 0 && Default_mcast_iface != NULL)
    strlcpy(iface,Default_mcast_iface,sizeof(iface));

  Output_fd = output_mcast(&Opus_out_socket,iface,Mcast_ttl,Mcast_tos);
  if(Output_fd < 0){
    fprintf(stdout,"can't create output socket: %s\n",strerror(errno));
    exit(EX_OSERR); // let systemd restart us
  }

  // Graceful signal catch
  signal(SIGPIPE,closedown);
  signal(SIGINT,closedown);
  signal(SIGKILL,closedown);
  signal(SIGQUIT,closedown);
  signal(SIGTERM,closedown);
  signal(SIGPIPE,SIG_IGN);

  realtime(50);

  // Loop forever processing and dispatching incoming PCM and status packets

  struct packet *pkt = NULL;
  while(true){
    struct pollfd fds[2];
    fds[0].fd = Input_fd;
    fds[0].events = POLLIN;
    fds[0].revents = 0;
    fds[1].fd = Status_fd;
    fds[1].events = POLLIN;
    fds[1].revents = 0;
    int n = poll(fds,2,-1); // Wait indefinitely for either stat or pcm data
    if(n < 0)
      break; // Error of some kind
    if(n == 0)
      continue; // Possible with 0 timeout?

    if(fds[1].revents & POLLIN){
      // Simply copy status on output
      struct sockaddr sender;
      socklen_t socksize = sizeof(sender);
      uint8_t buffer[PKTSIZE];
      int size = recvfrom(Status_fd,buffer,sizeof buffer,0,&sender,&socksize);
      if(sendto(Output_fd,buffer,size,0,&Metadata_out_socket,sizeof Metadata_out_socket) < 0)
	perror("status sendto");

    }
    if(fds[0].revents & POLLIN){
      // Process incoming RTP packets, demux to per-SSRC thread
      // Need a new packet buffer?
      if(!pkt)
	pkt = malloc(sizeof(*pkt));
      assert(pkt != NULL);
      // Zero these out to catch any uninitialized derefs
      pkt->next = NULL;
      pkt->data = NULL;
      pkt->len = 0;

      struct sockaddr sender;
      socklen_t socksize = sizeof(sender);
      int size = recvfrom(Input_fd,&pkt->content,sizeof pkt->content,0,&sender,&socksize);

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
      uint8_t const *dp = ntoh_rtp(&pkt->rtp,pkt->content);
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
	sp->source = formatsock(&sender,false);
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
}

// Per-SSRC thread - does actual Opus encoding
// Warning! do not use "continue" within the loop as this will cause a memory leak.
// Jump to "endloop" instead
void *encode(void *arg){
  struct session * sp = (struct session *)arg;
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

  while(true){
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
      // channels or sample rate changed; Re-create encoder
      sp->channels = channels_from_pt(pkt->rtp.type);
      sp->samprate = samprate_from_pt(pkt->rtp.type);
      opus_encoder_destroy(sp->opus);
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
    }

    if(pkt->rtp.marker || samples_skipped > 4 * 48000 * Opus_blocktime){ // Opus works on 48 kHz virtual samples
      // reset encoder state after 4 seconds of skip or a RTP marker bit
      opus_encoder_ctl(sp->opus,OPUS_RESET_STATE);
      sp->silence = true;
    }
    int16_t const *samples = (int16_t *)pkt->data;

    for(int i=0; i < frame_size;i++){
      float left = SCALE * (int16_t)ntohs(*samples++);
      sp->audio_buffer[sp->audio_write_index++] = left;
      if(sp->channels == 2){
	float right = SCALE * (int16_t)ntohs(*samples++);
	sp->audio_buffer[sp->audio_write_index++] = right;
      }
    }
  endloop:;
    FREE(pkt);

    // send however many opus frames we can
    send_samples(sp);
  }
}

struct session *lookup_session(struct sockaddr const * const sender,const uint32_t ssrc){
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
    FREE(sp->queue);
    sp->queue = pkt;
  }
  pthread_mutex_unlock(&sp->qmutex);

  ASSERT_UNLOCKED(&sp->qmutex);
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
  FREE(sp);
  *p = NULL;
  return 0;
}
void closedown(int s){
  (void)s; // unused
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
  _exit(EX_OK);
}
// Encode and send one or more Opus frames when we have enough
int send_samples(struct session * const sp){
  assert(sp != NULL);

  int pcm_samples_written = 0;
  while(true){
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
    rtp.type = Opus_pt; // Opus
    rtp.seq = sp->rtp_state_out.seq;
    rtp.timestamp = sp->rtp_state_out.timestamp;
    rtp.ssrc = sp->rtp_state_out.ssrc;

    if(sp->silence){
      // Beginning of talk spurt after silence, set marker bit
      rtp.marker = true;
      sp->silence = false;
    } else
      rtp.marker = false;

    uint8_t output_buffer[PKTSIZE]; // to hold RTP header + Opus-encoded frame
    uint8_t * const opus_write_pointer = hton_rtp(output_buffer,&rtp);
    int packet_bytes_written = opus_write_pointer - output_buffer;

    int const opus_output_bytes = opus_encode_float(sp->opus,
						    sp->audio_buffer,
						    frame_size,  // Number of uncompressed *stereo* samples per frame
						    opus_write_pointer,
						    sizeof(output_buffer) - packet_bytes_written); // Max # bytes in compressed output buffer
    packet_bytes_written += opus_output_bytes;

    if(!Discontinuous || opus_output_bytes > 2){
      // ship it
      if(sendto(Output_fd,output_buffer,packet_bytes_written,0,&Opus_out_socket,sizeof Opus_out_socket) < 0)
	return -1;
      Output_packets++; // all sessions
      sp->rtp_state_out.seq++; // Increment only if packet is sent
      sp->rtp_state_out.bytes += opus_output_bytes;
      sp->rtp_state_out.packets++;
    } else
      sp->silence = true;

    sp->rtp_state_out.timestamp += frame_size * 48000 / sp->samprate; // Always increase timestamp by virtual 48k sample rate
    const int remaining_bytes = sizeof(sp->audio_buffer[0]) * (sp->audio_write_index - sp->channels * frame_size);
    assert(remaining_bytes >= 0);
    memmove(sp->audio_buffer,&sp->audio_buffer[sp->channels * frame_size],remaining_bytes);
    sp->audio_write_index -= frame_size * sp->channels;
    pcm_samples_written += frame_size * sp->channels;
  }
  return pcm_samples_written;
}
