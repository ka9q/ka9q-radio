// $Id: rdsd.c,v 1.4 2022/08/05 06:35:10 karn Exp $
// FM RDS demodulator/decoder
#define _GNU_SOURCE 1
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <limits.h>
#include <string.h>
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
#include "filter.h"
#include "iir.h"

#define BUFFERSIZE 16384  // Tune this

struct session {
  struct session *prev;       // Linked list pointers
  struct session *next; 
  
  struct sockaddr sender;
  char addr[NI_MAXHOST];    // RTP Sender IP address
  char port[NI_MAXSERV];    // RTP Sender source port

  pthread_t thread;
  pthread_mutex_t qmutex;
  pthread_cond_t qcond;
  struct packet *queue;
 
  struct rtp_state rtp_state_in; // RTP input state
  struct rtp_state rtp_state_out; // RTP output state

  long long packets;
};


// Global config variables
int const Bufsize = 1540;     // Maximum samples/words per RTP packet - must be smaller than Ethernet MTU
// Each block of stereo output @ 48kHz must fit in an ethernet packet
// 5 ms * 48000 = 240 stereo frames; 240 * 2 * 2 = 960 bytes
float Blocktime = 5; // milliseconds
int In_samprate = 384000;         // Composite input rate
int Out_samprate = 48000;         // stereo output rate
float Kaiser_beta = 3.5 * M_PI;
float const SCALE = 1./SHRT_MAX;

// Command line params
const char *App_path;
int Verbose;                  // Verbosity flag (currently unused)
int Mcast_ttl = 10;           // our multicast output is frequently routed
int IP_tos = 48; // AF12 << 2

// Global variables
int Status_fd = -1;           // Reading from radio status
int Status_out_fd = -1;       // Writing to radio status
int Input_fd = -1;            // Multicast receive socket
int Output_fd = -1;           // Multicast send socket
char *Input;
char *Output;
char *Status;
char *Name = "rds";
struct session *Audio;
pthread_mutex_t Audio_protect = PTHREAD_MUTEX_INITIALIZER;
uint64_t Output_packets;

void closedown(int);
struct session *lookup_session(const struct sockaddr *,uint32_t);
struct session *create_session(void);
int close_session(struct session *);
int send_samples(struct session *sp);
void *input(void *arg);
void *decode(void *arg);

struct option Options[] =
  {
   {"iface", required_argument, NULL, 'A'},
   {"pcm-in", required_argument, NULL, 'I'},
   {"status-in", required_argument, NULL, 'S'},
   {"ttl", required_argument, NULL, 'T'},
   {"verbose", no_argument, NULL, 'v'},
   {"tos", required_argument, NULL, 'p'},
   {"iptos", required_argument, NULL, 'p'},
   {"ip-tos", required_argument, NULL, 'p'},    
   {NULL, 0, NULL, 0},
  };
   
char Optstring[] = "A:I:N:S:T:vp:";

struct sockaddr_storage Status_dest_address;
struct sockaddr_storage Status_input_source_address;
struct sockaddr_storage Local_status_source_address;
struct sockaddr_storage PCM_dest_address;
struct sockaddr_storage Stereo_source_address;
struct sockaddr_storage Stereo_dest_address;

int main(int argc,char * const argv[]){
  App_path = argv[0];

  setlocale(LC_ALL,getenv("LANG"));

  int c;
  while((c = getopt_long(argc,argv,Optstring,Options,NULL)) != -1){
    switch(c){
    case 'A':
      Default_mcast_iface = optarg;
      break;
    case 'I':
      Input = optarg;
      break;
    case 'N':
      Name = optarg;
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
    case 'v':
      Verbose++;
      break;
    default:
      fprintf(stderr,"Usage: %s [-v] [-T mcast_ttl] -I input_mcast_address -R output_mcast_address\n",argv[0]);
      pthread_mutex_destroy(&Audio_protect);
      exit(1);
    }
  }

  if(Input){
    char iface[1024];
    resolve_mcast(Input,&PCM_dest_address,DEFAULT_RTP_PORT,iface,sizeof(iface));
    Input_fd = listen_mcast(&PCM_dest_address,iface);
    if(Input_fd == -1)
      fprintf(stderr,"Can't set up input on %s: %sn",optarg,strerror(errno));
    
  }
  if(Status){
    char iface[1024];
    resolve_mcast(Status,&Status_dest_address,DEFAULT_STAT_PORT,iface,sizeof(iface));
    Status_fd = listen_mcast(&Status_dest_address,iface);
    if(Status_fd == -1){
      fprintf(stderr,"Can't set up input on %s: %s\n",optarg,strerror(errno));
      pthread_mutex_destroy(&Audio_protect);
      exit(1);
    }
    Status_out_fd = connect_mcast(&Status_dest_address,iface,Mcast_ttl,IP_tos);
    {
      socklen_t len;
      len = sizeof(Local_status_source_address);
      getsockname(Status_out_fd,(struct sockaddr *)&Local_status_source_address,&len);
    }
  }

  {
    char output[1024];
    snprintf(output,sizeof(output),"%s-pcm.local",Name);
    char service_name[2000];
    snprintf(service_name,sizeof(service_name),"%s (%s)",Name,output);
    char description[1024];
    snprintf(description,sizeof(description),"pcm-source=%s",Input);
    avahi_start(service_name,"_rtp._udp",DEFAULT_RTP_PORT,output,ElfHashString(output),description);

    resolve_mcast(output,&Stereo_dest_address,DEFAULT_RTP_PORT,NULL,0);
    Output_fd = connect_mcast(&Stereo_dest_address,NULL,Mcast_ttl,IP_tos);
    if(Output_fd == -1)
      fprintf(stderr,"Can't set up output on %s: %s\n",output,strerror(errno));
    socklen_t len = sizeof(Stereo_source_address);
    getsockname(Output_fd,(struct sockaddr *)&Stereo_source_address,&len);
  }

  fftwf_init_threads();
  fftwf_make_planner_thread_safe();
  fftwf_plan_with_nthreads(1);

  // Set up multicast
  if(Input_fd == -1 && Status_fd == -1){
    fprintf(stderr,"Must specify either --status-in or --pcm-in\n");
    pthread_mutex_destroy(&Audio_protect);
    exit(1);
  }
  if(Output_fd == -1){
    fprintf(stderr,"Must specify --opus-out\n");
    pthread_mutex_destroy(&Audio_protect);
    exit(1);
  }

  // Set up to receive PCM in RTP/UDP/IP
  pthread_t input_thread;
  if(Input_fd != -1)
    pthread_create(&input_thread,NULL,input,NULL);

  // Graceful signal catch
  signal(SIGPIPE,closedown);
  signal(SIGINT,closedown);
  signal(SIGKILL,closedown);
  signal(SIGQUIT,closedown);
  signal(SIGTERM,closedown);
  signal(SIGPIPE,SIG_IGN);

  // Radio status channel reception and transmission

  while(Status_fd == -1)
    sleep(1); // Status channel not specified

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
      int cr = buffer[0];
      if(cr == 1)
	continue; // Ignore commands
      unsigned char *cp = buffer+1;

      while(cp - buffer < length){
	enum status_type type = *cp++;
	
	if(type == EOL)
	  break;
	
	unsigned int optlen = *cp++;
	if(cp - buffer + optlen > length)
	  break;
	
	// Should probably extract sample rate too, instead of assuming 48 kHz
	switch(type){
	case EOL:
	  goto done;
	case OUTPUT_DATA_DEST_SOCKET:
	  decode_socket(&PCM_dest_address,cp,optlen);
	  if(Input_fd == -1){
	    if(Verbose)
	      fprintf(stderr,"Listening for PCM on %s\n",formatsock(&PCM_dest_address));

	    Input_fd = listen_mcast(&PCM_dest_address,NULL);
	    if(Input_fd != -1)
	      pthread_create(&input_thread,NULL,input,cp);
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


// There's one of these threads per input multicast group, possibly with many SSRCs
// Process incoming RTP packets, demux to per-SSRC thread
// Warning: the input() thread allocates memory for packet buffers and passes them to the decode() thread
// The decode thread must free these buffers to avoid a memory leak
void *input(void *arg){
  char *mcast_address_text = (char *)arg;
  
  {
    char pname[16];
    snprintf(pname,sizeof(pname),"opin %s",mcast_address_text);
    pthread_setname(pname);
  }

  // Main loop begins here
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
      sp = create_session();
      assert(sp != NULL);
      // Initialize
      getnameinfo((struct sockaddr *)&sender,sizeof(sender),sp->addr,sizeof(sp->addr),
		    sp->port,sizeof(sp->port),NI_NOFQDN|NI_DGRAM);
      memcpy(&sp->sender,&sender,sizeof(struct sockaddr));
      sp->rtp_state_out.ssrc = sp->rtp_state_in.ssrc = pkt->rtp.ssrc;
      sp->rtp_state_in.seq = pkt->rtp.seq;
      sp->rtp_state_in.timestamp = pkt->rtp.timestamp;

      // Span per-SSRC thread
      if(pthread_create(&sp->thread,NULL,decode,sp) == -1){
	perror("pthread_create");
	close_session(sp);
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


// Per-SSRC thread - does actual decoding
// Warning! do not use "continue" within the loop as this will cause a memory leak.
// Jump to "endloop" instead
void *decode(void *arg){
  struct session * const sp = (struct session *)arg;
  assert(sp != NULL);
  {
    char threadname[16];
    snprintf(threadname,sizeof(threadname),"stereo %u",sp->rtp_state_out.ssrc);
    pthread_setname(threadname);
  }
  // We will exit after 10 sec of idleness so detach ourselves to ensure resource recovery
  // Not doing this caused a nasty memory leak
  pthread_detach(pthread_self());

  // Set up audio filters: mono, pilot & stereo difference
  // These blocksizes depend on front end sample rate and blocksize
  // At Blocktime = 5ms and 384 kHz, L = 1920, M = 1921, N = 3840
  int const L = roundf(In_samprate * Blocktime * .001); // Number of input samples in Blocktime
  int const M = L + 1;
  int const N = L + M - 1;

  // 'audio_L' stereo samples must fit in an output packet
  // At Blocktime = 5 ms, audio_N = 240
  int const audio_L = (L * Out_samprate) / In_samprate;

  // Baseband signal 50 Hz - 15 kHz contains mono (L+R) signal
  struct filter_in * const baseband = create_filter_input(L,M,REAL);
  if(baseband == NULL)
    return NULL;

  // Baseband filters, decimate from 384 Khz to 48 KHz

  // Narrow filter at 19 kHz for stereo pilot
  struct filter_out * const pilot = create_filter_output(baseband,NULL,audio_L, COMPLEX);
  if(pilot == NULL)
    return NULL;
  set_filter(pilot,-100./Out_samprate, 100./Out_samprate, Kaiser_beta);

  // RDS info at 57 kHz = 19 kHz * 3
  struct filter_out * const rds = create_filter_output(baseband,NULL,audio_L, COMPLEX);
  if(rds == NULL)
    return NULL;
  set_filter(rds,-2000./Out_samprate, 2000./Out_samprate, Kaiser_beta);

  // Assume the remainder is zero, as it is for clean sample rates @ 200 Hz multiples
  // If not, then a mop-up oscillator has to be provided
  double const hzperbin = In_samprate / N;              // 100 hertz per FFT bin @ 384 kHz and 5 ms
  int const quantum = N / (M - 1);       // rotate by multiples of (2) bins due to overlap-save (100 * 2 = 200 Hz)
  int const pilot_rotate = quantum * round(19000./(hzperbin * quantum));
  int const subc_rotate = quantum * round(57000./(hzperbin * quantum));

  while(1){
    struct packet *pkt = NULL;

    {
      struct timespec waittime;
      clock_gettime(CLOCK_REALTIME,&waittime);
      // wait 10 seconds for a new packet
      waittime.tv_sec += 10; // 10 seconds in the future
      { // Mutex-protected segment
	pthread_mutex_lock(&sp->qmutex);
	while(!sp->queue){      // Wait for packet to appear on queue
	  int ret = pthread_cond_timedwait(&sp->qcond,&sp->qmutex,&waittime);
	  assert(ret != EINVAL);
	  if(ret == ETIMEDOUT){
	    // Idle timeout after 10 sec; close session and terminate thread
	    pthread_mutex_unlock(&sp->qmutex);
	    close_session(sp); 
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
      
    int frame_size = 0;
    switch(pkt->rtp.type){
    case PCM_MONO_PT:
      frame_size = pkt->len / sizeof(short);
      break;
    default:
      goto endloop; // Discard all but mono PCM to avoid polluting session table
    }

    int samples_skipped = rtp_process(&sp->rtp_state_in,&pkt->rtp,frame_size);
    if(samples_skipped < 0)
      goto endloop; // Old dupe
    
    signed short const * const samples = (signed short *)pkt->data;
    
    for(int i=0; i<frame_size; i++){
      float const s = SCALE * (signed short)ntohs(samples[i]);
      if(put_rfilter(baseband,s) == 0)
	continue;
      // Filter input buffer full
      // Decimate to audio sample rate, do stereo processing

      // ensure output pkt big enough for output filter buffer size
      unsigned char packet[PKTSIZE],*dp;
      dp = packet;
      struct rtp_header out_rtp;
      out_rtp.type = PCM_STEREO_PT; // 48 kHz stereo PCM
      out_rtp.version = RTP_VERS;
      out_rtp.ssrc = sp->rtp_state_in.ssrc;
      out_rtp.timestamp = sp->rtp_state_out.timestamp;
      out_rtp.marker = 0;
      out_rtp.seq = sp->rtp_state_out.seq++;
      dp = hton_rtp(dp,&out_rtp);

      sp->rtp_state_out.timestamp += audio_L;
      sp->rtp_state_out.bytes += 2 * sizeof(signed short) * audio_L;
      sp->rtp_state_out.packets++;

      execute_filter_output(pilot,pilot_rotate); // pilot spun down to 0 Hz, 48 kHz rate
      execute_filter_output(rds,subc_rotate); // L-R baseband spun down to 0 Hz, 48 kHz rate

      int16_t *wp = (int16_t *)dp;
      for(int n= 0; n < audio_L; n++){
	//	complex float subc_phasor = pilot->output.c[n]; // 19 kHz pilot
	//	subc_phasor *= subc_phasor * subc_phasor;       // triple to 57 kHz
	//	subc_phasor /= approx_magf(subc_phasor);  // and normalize
	//	float complex subc_info = conjf(subc_phasor) * rds->output.c[n];
	float complex subc_info = rds->output.c[n];
	// Need to add de-emphasis!
	*wp++ = htons(scaleclip(__real__ subc_info));
	*wp++ = htons(scaleclip(__imag__ subc_info));
      }
      dp = (unsigned char *)wp;
      int const r = send(Output_fd,&packet,dp - packet,0);
      if(r <= 0){
	perror("pcm send");
	abort();
      }
    }
  endloop:;
    free(pkt);
    pkt = NULL;
  }
}

struct session *lookup_session(const struct sockaddr * const sender,const uint32_t ssrc){
  struct session *sp;
  pthread_mutex_lock(&Audio_protect);
  for(sp = Audio; sp != NULL; sp = sp->next){
    if(sp->rtp_state_in.ssrc == ssrc && address_match(&sp->sender,sender)){
      // Found it
      if(sp->prev != NULL){
	// Not at top of list; move it there
	if(sp->next != NULL)
	  sp->next->prev = sp->prev;

	sp->prev->next = sp->next;
	sp->prev = NULL;
	sp->next = Audio;
	Audio->prev = sp;
	Audio = sp;
      }
      break;
    }
  }
  pthread_mutex_unlock(&Audio_protect);
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
  pthread_mutex_lock(&Audio_protect);
  sp->prev = NULL;
  sp->next = Audio;
  if(sp->next != NULL)
    sp->next->prev = sp;
  Audio = sp;
  pthread_mutex_unlock(&Audio_protect);
  return sp;
}

int close_session(struct session * const sp){
  assert(sp != NULL);
  
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
  pthread_mutex_lock(&Audio_protect);
  if(sp->next != NULL)
    sp->next->prev = sp->prev;
  if(sp->prev != NULL)
    sp->prev->next = sp->next;
  else
    Audio = sp->next;
  pthread_mutex_unlock(&Audio_protect);
  free(sp);
  return 0;
}
void closedown(int s){
#if 0
  // Causes deadlock when we get called from a section where Audio_protect is already locked
  // Which is the usual case
  // Not really necessary anyway, since we're exiting
  pthread_mutex_lock(&Audio_protect);
  while(Audio != NULL)
    close_session(Audio);
  pthread_mutex_unlock(&Audio_protect);
#endif

  pthread_mutex_destroy(&Audio_protect);
  exit(0);
}

