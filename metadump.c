// $Id: metadump.c,v 1.19 2022/12/29 05:58:17 karn Exp $
// Utility to trace multicast SDR metadata
// Copyright 2018 Phil Karn, KA9Q

#define _GNU_SOURCE 1
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <limits.h>
#include <pthread.h>
#include <string.h>
#if defined(linux)
#include <bsd/string.h>
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



const char *App_path;
int Verbose,Dump;
bool Newline;
float Interval = 0.1;
int Control_sock;
int Status_sock;
int Count;
char *Radio;
uint32_t Ssrc;
int IP_tos;

char Locale[256] = "en_US.UTF-8";
int Mcast_ttl = 5;
char Optstring[] = "s:c:i:vnr:l:";
struct option Options[] = {
  {"ssrc", required_argument, NULL, 's'},
  {"count", required_argument, NULL, 'c'},
  {"interval", required_argument, NULL, 'i'},
  {"verbose", no_argument, NULL, 'v'},  
  {"newline", no_argument, NULL, 'n'},
  {"radio", required_argument, NULL, 'r'},
  {"locale", required_argument, NULL, 'l'},

  {NULL, 0, NULL, 0},
};

void usage(void);


int main(int argc,char *argv[]){
  App_path = argv[0];
  int c;

  while((c = getopt(argc,argv,"s:c:i:vnr:")) != -1){
    switch(c){
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
  int status_packets = 0;
  if(Verbose){
    fprintf(stdout,"Polling interval %'llu nanoseconds\n",(long long)(Interval * BILLION));
  }
  long long last_command_time = 0;
  while(Count == 0 || status_packets < Count){ // Count = 0 means infinite
    // Begin polling SSRC to ensure the multicast group is up and radiod is listening
    if(Ssrc != 0 && Control_sock > 2){
      // Time for a rate-limited poll?
      if(gps_time_ns() >= last_command_time + (long long)(Interval* BILLION)){
	uint8_t cmd_buffer[9000];
	uint8_t *bp = cmd_buffer;
	*bp++ = 1; // Generate command packet
	uint32_t sent_tag = arc4random();
	encode_int(&bp,COMMAND_TAG,sent_tag);
	encode_int(&bp,OUTPUT_SSRC,Ssrc);
	encode_eol(&bp);
	int cmd_len = bp - cmd_buffer;
	if(send(Control_sock, cmd_buffer, cmd_len, 0) != cmd_len)
	  perror("command send");
	
	last_command_time = gps_time_ns();
	if(Verbose)
	  fprintf(stdout,"Command sent\n");
      }
      // Look for response
      // Set 100 ms timeout for response
      struct pollfd fds[1];
      fds[0].fd = Status_sock;
      fds[0].events = POLLIN;
      int event = poll(fds,1,100);
      if(event == 0)
	continue; // Timeout; go back and resend
      
      if(event < 0){
	fprintf(stdout,"poll error: %s\n",strerror(errno));
	exit(1);
      }      
    }
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
      status_packets++; // Don't count our own responses
      last_command_time = gps_time_ns(); // Reset poll timeout
    }
  }
  exit(EX_OK); // can't reach
}


void usage(void){
  fprintf(stdout,"%s [-s|--ssrc <ssrc>] [-c|--count n] [-i|--interval f] [-v|--verbose] [-n|--newline] [-l|--locale] [ -r|--radio] control-channel\n",App_path);
}
