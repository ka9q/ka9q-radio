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


pthread_t Input_thread;

const char *App_path;
int Verbose;
bool Newline;
float Interval = 0.1;
int Control_sock;
int Status_sock;
int Count;
char *Radio;
uint32_t Ssrc;
int IP_tos;
int Status_packets;

long long Last_status_time;

char Locale[256] = "en_US.UTF-8";
int Mcast_ttl = 5;
char Optstring[] = "s:c:i:vnr:l:V";
struct option Options[] = {
  {"ssrc", required_argument, NULL, 's'},
  {"count", required_argument, NULL, 'c'},
  {"interval", required_argument, NULL, 'i'},
  {"verbose", no_argument, NULL, 'v'},  
  {"newline", no_argument, NULL, 'n'},
  {"radio", required_argument, NULL, 'r'},
  {"locale", required_argument, NULL, 'l'},
  {"version", no_argument, NULL, 'V'},
  {NULL, 0, NULL, 0},
};

void usage(void);
void *input_thread(void *);


int main(int argc,char *argv[]){
  App_path = argv[0];
  int c;

  while((c = getopt_long(argc,argv,Optstring,Options,NULL)) != -1){
    switch(c){
    case 'V':
      VERSION();
      exit(EX_OK);
    case 's':
      Ssrc = strtol(optarg,NULL,0);
      break;
    case 'c':
      Count = strtol(optarg,NULL,0);
      break;
    case 'i':
      Interval = strtod(optarg,NULL);
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
     default:
      usage();
      break;
    }
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
  resolve_mcast(Radio,&sock,DEFAULT_STAT_PORT,iface,sizeof(iface));
  if(Verbose){
    char result[1024];
    fprintf(stdout,"Listening on %s\n",formataddr(result,sizeof(result),&sock));
  }

  Status_sock = listen_mcast(&sock,iface);
  if(Status_sock < 0){
    fprintf(stdout,"Can't set up multicast input\n");
    exit(EX_IOERR);
  }
  if(Count != 0 || Interval != 0){
    if(Verbose)
      fprintf(stdout,"Connecting\n");
    Control_sock = connect_mcast(&sock,iface,Mcast_ttl,IP_tos);
    if(Control_sock == -1){
      fprintf(stdout,"Can't open cmd socket to radio control channel %s: %s\n",Radio,strerror(errno));
      exit(EX_IOERR);
    }
  }

  if(Verbose){
    fprintf(stdout,"Polling interval %'llu nanoseconds\n",(long long)(Interval * BILLION));
  }

  pthread_create(&Input_thread,NULL,input_thread,NULL);

  if(Interval == 0 || Ssrc == 0)
    while(1)
      sleep(1000);

  // Begin active polling SSRC to ensure the multicast group is up and radiod is listening
  // Time for a rate-limited poll?

  long long last_command_time = 0;
  while(1){
    long long now = gps_time_ns();
    long long latest = max(Last_status_time,last_command_time); // Time of latest event
    if(now >= latest + (long long)(Interval* BILLION)){
      // Haven't gotten a status nor sent a poll in Interval seconds
      uint8_t cmd_buffer[9000];
      uint8_t *bp = cmd_buffer;
      *bp++ = 1; // Generate command packet
      uint32_t sent_tag = arc4random();
      encode_int(&bp,COMMAND_TAG,sent_tag);
      encode_int(&bp,OUTPUT_SSRC,Ssrc);
      encode_eol(&bp);
      int cmd_len = bp - cmd_buffer;
      if(Verbose)
	fprintf(stdout,"Command sent\n");

      if(send(Control_sock, cmd_buffer, cmd_len, 0) != cmd_len){
	perror("command send");
	exit(1);
      }      
      latest = last_command_time = now;
    }
    // Sleep for Interval seconds past after the later of the last command and last status messages
    useconds_t sleep_time = (latest + (long long)(Interval * BILLION) - now)/ 1000;
    usleep(sleep_time);
  }
  exit(EX_OK); // can't reach
}


void usage(void){
  fprintf(stdout,"%s [-s|--ssrc <ssrc>] [-c|--count n] [-i|--interval f] [-v|--verbose] [-n|--newline] [-l|--locale] [ -r|--radio] control-channel\n",App_path);
}

// Process incoming packets
void *input_thread(void *p){
  while(Count == 0 || Status_packets < Count){
    uint8_t buffer[9000];
    struct sockaddr_storage source;
    socklen_t len = sizeof(source);
    int length = recvfrom(Status_sock,buffer,sizeof(buffer),0,(struct sockaddr *)&source,&len);
    if(length <= 0){
      fprintf(stderr,"Recvfrom error %s\n",strerror(errno));
      sleep(1);
      continue;
    }
    int64_t now = gps_time_ns();
    
    enum pkt_type const cr = buffer[0]; // Command/response byte
    char temp[1024];
    fprintf(stdout,"%s %s", format_gpstime(temp,sizeof(temp),now), formatsock(&source));
    fprintf(stdout," %s", cr == STATUS ? "STAT" : cr == CMD ? "CMD" : cr == SSRC_LIST ? "SSRC_LIST" : "unknown");
    dump_metadata(buffer+1,length-1,Newline);
    fflush(stdout);
    if(cr == STATUS){
      Status_packets++; // Don't count our own responses
      Last_status_time = gps_time_ns(); // Reset poll timeout
    }
  }
  exit(EX_OK);
}
