// $Id: pcmspawn.c,v 1.9 2022/08/05 06:35:10 karn Exp $
// Receive and demux RTP PCM streams into a command pipeline
#define _GNU_SOURCE 1
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <limits.h>
#include <string.h>
#include <bsd/string.h>
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

struct session {
  struct session *prev;       // Linked list pointers
  struct session *next; 
  int type;                 // input RTP type (10,11)
  
  struct sockaddr sender;
  char addr[NI_MAXHOST];    // RTP Sender IP address
  char port[NI_MAXSERV];    // RTP Sender source port

  FILE *pipe;
  long long last_active;
 
  struct rtp_state rtp_state; // RTP input state

  unsigned long dropped_samples;  // Dropped samples (stereo samples) replaced with silence
  unsigned long resets; // rtp resets due to too many dropped samples at once
  long long packets;
};


// Command line params
const char *App_path;
int Verbose;                  // Verbosity flag (currently unused)

// Global variables
pthread_t Status_thread;
pthread_mutex_t Input_ready_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t Input_ready_cond = PTHREAD_COND_INITIALIZER;

int Status_fd = -1;           // Reading from radio status
int Status_out_fd = -1;       // Writing to radio status
int Input_fd = -1;            // Multicast receive socket
struct session *Sessions;
pthread_mutex_t Session_protect = PTHREAD_MUTEX_INITIALIZER;
char *Command;
char *Input;
char *Status;

void closedown(int);
struct session *lookup_session(const struct sockaddr *,uint32_t,int);
struct session *create_session(void);
int close_session(struct session *);
int send_samples(struct session *sp);
void *status(void *);

struct option Options[] =
  {
   {"iface", required_argument, NULL, 'A'},
   {"pcm-in", required_argument, NULL, 'I'},
   {"name", required_argument, NULL, 'N'},
   {"status-in", required_argument, NULL, 'S'},
   {"verbose", no_argument, NULL, 'v'},
   {NULL, 0, NULL, 0},

  };
   
char Optstring[] = "A:I:N:S:v";

struct sockaddr_storage Status_dest_address;
struct sockaddr_storage Status_input_source_address;
struct sockaddr_storage Local_status_source_address;
struct sockaddr_storage PCM_dest_address;
struct sockaddr_storage PCM_source_address;

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
      Command = optarg;
      break;
    case 'S':
      Status = optarg;
      break;
    case 'v':
      Verbose++;
      break;
    default:
      fprintf(stderr,"Usage: \n");
      exit(1);
    }
  }
  if(optind >= argc){
    fprintf(stderr,"Missing command\n");
    exit(1);
  }
  // This needs to be a proper macro expansion
  Command = argv[optind];
  
  char iface[1024];
  if(Input){
    resolve_mcast(Input,&PCM_dest_address,DEFAULT_RTP_PORT,iface,sizeof(iface));
    Input_fd = listen_mcast(&PCM_dest_address,NULL); // Port address already in place

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
    struct session *sp = lookup_session((const struct sockaddr *)&sender,pkt->rtp.ssrc,pkt->rtp.type);
    if(!sp){
      // Not found; create new session
      sp = create_session();
      assert(sp != NULL);
      // Initialize
      getnameinfo((struct sockaddr *)&sender,sizeof(sender),sp->addr,sizeof(sp->addr),
		    sp->port,sizeof(sp->port),NI_NOFQDN|NI_DGRAM);
      memcpy(&sp->sender,&sender,sizeof(struct sockaddr));
      sp->rtp_state.ssrc = pkt->rtp.ssrc;
      sp->rtp_state.seq = pkt->rtp.seq; // Can cause a spurious drop indication if # pcm pkts != # opus pkts
      sp->rtp_state.timestamp = pkt->rtp.timestamp;
      sp->type = pkt->rtp.type;

      // Spawn per-SSRC command
      // Command needs to be a macro-substituted string with params:
      // Channels
      // sample rate
      // sending IP address & port
      
      char command_line[2048];
      int const samprate = samprate_from_pt(sp->type);
      int const channels = channels_from_pt(sp->type);

      snprintf(command_line,sizeof(command_line),"%s %s:%s %d %d %d %d",
	       Command,sp->addr,sp->port,sp->rtp_state.ssrc,sp->type,samprate,channels);
      fprintf(stderr,"New session, %s\n",command_line);

      if((sp->pipe = popen(command_line,"w")) == NULL){
	fprintf(stderr,"popen(%s) failed: %s\n",command_line,strerror(errno));
	close_session(sp);
	continue;
      }
    }
    sp->packets++; // Count all packets, regardless of type
    sp->last_active = gps_time_ns(); // for reaping long-idle sessions

    int const channels = channels_from_pt(sp->type);
    int const frame_size = pkt->len / (sizeof(int16_t) * channels); // PCM sample times
    if(frame_size <= 0)
      goto endloop; // garbled packet?

    int const samples_skipped = rtp_process(&sp->rtp_state,&pkt->rtp,frame_size);
    if(samples_skipped < 0)
      goto endloop; // Old dupe

    if(samples_skipped){
      if(samples_skipped < 4 * 48000){ // 4 sec @ 48kHz is arbitrary
	sp->dropped_samples += samples_skipped;
	int const padding = 2 * channels * samples_skipped;
	
	for(int i=0; i < padding; i++)
	  fputc(0,sp->pipe);
      } else {
	sp->resets++;
      }
    }
    // raw copy, probably in network byte order
    if(fwrite(pkt->data,1,pkt->len,sp->pipe) != pkt->len){
      // Error to pipe
      close_session(sp);
      sp = NULL;
    }
  endloop:;
    free(pkt);
    pkt = NULL;
  }
}


// Monitor and report to radio status channel (only if specified)
void * status(void *p){
  pthread_detach(pthread_self());
  pthread_setname("opstat");

  char iface[1024];
  resolve_mcast(Status,&Status_dest_address,DEFAULT_STAT_PORT,iface,sizeof(iface));
  Status_fd = listen_mcast(&Status_dest_address,iface);
  if(Status_fd == -1){
    fprintf(stderr,"Can't set up input on %s: %s\n",Status,strerror(errno));
    return NULL;
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
	send_poll(Status_out_fd,0); // Timeout; send poll for all SSRCs
      continue;
    }

    // We MUST ignore our own packets, or we'll loop!
    if(address_match(&Status_input_source_address, &Local_status_source_address)
       && getportnumber(&Status_input_source_address) == getportnumber(&Local_status_source_address))
      continue;

    // Announce ourselves in response to commands
    if(buffer[0] == 1){
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




struct session *lookup_session(const struct sockaddr * const sender,const uint32_t ssrc,int type){
  struct session *sp;
  pthread_mutex_lock(&Session_protect);
  for(sp = Sessions; sp != NULL; sp = sp->next){
    if(sp->rtp_state.ssrc == ssrc && address_match(&sp->sender,sender) && sp->type == type){
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

int close_session(struct session * const sp){
  assert(sp != NULL);
  
  // Remove from linked list of sessions
  pthread_mutex_lock(&Session_protect);
  if(sp->next != NULL)
    sp->next->prev = sp->prev;
  if(sp->prev != NULL)
    sp->prev->next = sp->next;
  else
    Sessions = sp->next;
  pthread_mutex_unlock(&Session_protect);
  pclose(sp->pipe);
  free(sp);
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
