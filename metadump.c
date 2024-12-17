// Utility to trace ka9q-radio multicast SDR metadata
// Copyright 2018-2023 Phil Karn, KA9Q

#define _GNU_SOURCE 1
#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include <stdint.h>
#include <limits.h>
#include <pthread.h>
#include <string.h>
#if defined(linux)
#include <bsd/string.h>
#include <bsd/stdlib.h>
#else
#include <stdlib.h>
#endif
#include <math.h>
#include <complex.h>
#undef I
#include <sys/time.h>
#include <ctype.h>
#include <sys/socket.h>
#include <netdb.h>
#include <locale.h>
#include <sysexits.h>
#include <errno.h>
#include <poll.h>
#include <getopt.h>

#include "misc.h"
#include "multicast.h"
#include "status.h"


static pthread_t Input_thread;
const char *App_path;
static bool Newline;
static bool All;
static int64_t Interval = 1 * BILLION; // Default 1 sec
static int Control_sock;
static int Status_sock;
static int Count = 2;
static char const *Radio;
static uint32_t Ssrc;
static int Status_packets;
static int64_t Last_status_time;
struct sockaddr_storage Metadata_dest_socket;

static char Locale[256] = "en_US.UTF-8";
int Verbose;
int IP_tos;
int Mcast_ttl = 5;
static char Optstring[] = "tas:c:i:vnr:l:Vx::";
static struct option Options[] = {
  {"stdin", no_argument, NULL, 't'},
  {"all", no_argument, NULL, 'a'},
  {"ssrc", required_argument, NULL, 's'},
  {"count", required_argument, NULL, 'c'},
  {"interval", required_argument, NULL, 'i'},
  {"verbose", no_argument, NULL, 'v'},
  {"newline", no_argument, NULL, 'n'},
  {"radio", required_argument, NULL, 'r'},
  {"locale", required_argument, NULL, 'l'},
  {"version", no_argument, NULL, 'V'},
  {"retries", required_argument, NULL, 'R'},
  {NULL, 0, NULL, 0},
};

void usage(void);
void *input_thread(void *);

void decode_stdin(){
  uint8_t buffer[PKTSIZE];
  int length=read(STDIN_FILENO,buffer,PKTSIZE);
  if (length>0){
    enum pkt_type const cr = buffer[0]; // Command/response byte
    fprintf(stdout," %s", cr == STATUS ? "STAT" : "CMD");
    dump_metadata(stdout,buffer+1,length-1,Newline);
    fflush(stdout);
  }
}

int main(int argc,char *argv[]){
  App_path = argv[0];
  int retries = 0;
  int c;

  while((c = getopt_long(argc,argv,Optstring,Options,NULL)) != -1){
    switch(c){
    case 'V':
      VERSION();
      exit(EX_OK);
      break;
    case 'a':
      All = true; // Dump every SSRC
      break;
    case 't':
      decode_stdin();
      exit(EX_OK);
    case 's':
      Ssrc = strtol(optarg,NULL,0);
      break;
    case 'c':
      Count = strtol(optarg,NULL,0);
      break;
    case 'i':
      Interval = fabs(strtod(optarg,NULL)) * BILLION; // ensure it's not negative
      break;
    case 'v':
      Verbose++;
      break;
    case 'n':
      Newline = true;
      break;
    case 'r':
      Radio = optarg;
      break;
    case 'l':
      strlcpy(Locale,optarg,sizeof(Locale));
      break;
    case 'R':
      retries = labs(strtol(optarg,NULL,0));
      break;
     default:
      usage();
      break;
    }
  }
  if(All){
    Ssrc = 0xffffffff; // All 1's means poll every channel
    Interval = min((int64_t)BILLION,Interval); // No more than 1/sec, since the responses will be rate limited
  }
  if(Radio == NULL){
    if(argc <= optind){
      usage();
      exit(1);
    }
    Radio = argv[optind];
  }
  {
    char const * const cp = getenv("LANG");
    if(cp != NULL){
      strlcpy(Locale,cp,sizeof(Locale));
    }
  }
  setlocale(LC_ALL,Locale); // Set either the hardwired default or the value of $LANG if it exists
  struct sockaddr_storage sock;
  char iface[1024];
  iface[0] = '\0';

  if(Verbose)
    fprintf(stdout,"Resolving %s\n",Radio);
  if(resolve_mcast(Radio,&sock,DEFAULT_STAT_PORT,iface,sizeof(iface),retries) == -1){
    fprintf(stdout,"Can't resolve %s\n",Radio);
    exit(EX_UNAVAILABLE);
  }    
  if(Verbose){
    char result[1024];
    fprintf(stdout,"Listening on %s\n",formataddr(result,sizeof(result),&sock));
  }

  Status_sock = listen_mcast(&sock,iface);
  if(Status_sock < 0){
    fprintf(stdout,"Can't set up multicast input\n");
    exit(EX_IOERR);
  }
  if(Verbose)
    fprintf(stdout,"Connecting\n");
  Control_sock = connect_mcast(&sock,iface,Mcast_ttl,IP_tos);
  if(Control_sock == -1){
    fprintf(stdout,"Can't open cmd socket to radio control channel %s: %s\n",Radio,strerror(errno));
    exit(EX_IOERR);
  }

  if(Verbose && Interval != 0)
    fprintf(stdout,"Polling %u interval %'.1f sec count %llu\n",(unsigned)Ssrc,(float)Interval * 1e-9,(long long)Count);

  pthread_create(&Input_thread,NULL,input_thread,NULL);
  if(Ssrc == 0){
    fprintf(stdout,"No ssrc specified, waiting passively for responses\n");
    while(true)
      sleep(1000); // passive mode indefinitely
  }
  int64_t last_command_time = 0;

  while(Interval != 0){
    // Send polls
    uint8_t cmd_buffer[PKTSIZE];
    uint8_t *bp = cmd_buffer;
    *bp++ = 1; // Generate command packet
    uint32_t sent_tag = arc4random();
    encode_int(&bp,COMMAND_TAG,sent_tag);
    encode_int(&bp,OUTPUT_SSRC,Ssrc);
    encode_eol(&bp);
    int cmd_len = bp - cmd_buffer;

    if(Verbose)
      fprintf(stdout,"Send poll\n");
    if(send(Control_sock, cmd_buffer, cmd_len, 0) != cmd_len){
      perror("command send");
      exit(1);
    }
    last_command_time = gps_time_ns();
    // usleep takes usleep_t but it's unsigned and the while loop will hang
    int32_t sleep_time = Interval / 1000; // nanosec -> microsec

    while(sleep_time > 0){
      usleep(sleep_time); // Sleeps at least this long
      // sleep Interval beyond latest event
      if(Last_status_time > last_command_time)
	sleep_time = (Last_status_time + Interval - gps_time_ns()) / 1000;
      else
	sleep_time = (last_command_time + Interval - gps_time_ns()) / 1000;
    }
  }
  exit(EX_OK); // can't reach
}


void usage(void){
  fprintf(stdout,"%s [-R|--retries count] [-s|--ssrc <ssrc>|-a|--all] [-c|--count n] [-i|--interval f] [-v|--verbose] [-n|--newline] [-l|--locale] [-t|--stdin] [-r|--radio] control-channel\n",App_path);
}

// Process incoming packets
void *input_thread(void *p){
  for(int i=0; i < Count;){
    uint8_t buffer[PKTSIZE];
    struct sockaddr_storage source;
    socklen_t len = sizeof(source);
    int length = recvfrom(Status_sock,buffer,sizeof(buffer),0,(struct sockaddr *)&source,&len);
    if(length <= 0){
      fprintf(stderr,"Recvfrom error %s\n",strerror(errno));
      sleep(1);
      continue;
    }
    if(Ssrc != 0){
      // ssrc specified, ignore others
      uint32_t ssrc = get_ssrc(buffer+1,length-1);
      if(ssrc != Ssrc)
	continue;
    }
    int64_t now = gps_time_ns();
    char temp[1024];
    fprintf(stdout,"%s %s", format_gpstime(temp,sizeof(temp),now), formatsock(&source));
    enum pkt_type const cr = buffer[0]; // Command/response byte
    fprintf(stdout," %s", cr == STATUS ? "STAT" : "CMD");
    if(cr == STATUS){
      Status_packets++; // Don't count our own responses
      Last_status_time = now; // Reset poll timeout
    }
    dump_metadata(stdout,buffer+1,length-1,Newline);
    fflush(stdout);
    i++;
  }
  exit(EX_OK);
}
