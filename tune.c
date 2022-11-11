// $Id: tune.c,v 1.6 2022/08/01 23:33:57 karn Exp $
// Interactive program to tune radio

#define _GNU_SOURCE 1
#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <getopt.h>
#include <unistd.h>
#if defined(linux)
#include <bsd/string.h>
#endif
#include <string.h>
#include <sys/socket.h>
#include <locale.h>
#include <errno.h>

#include "misc.h"
#include "multicast.h"
#include "status.h"

int Mcast_ttl = 5;
int IP_tos = 0;
const char *App_path;
int Verbose;
char *Radio = NULL;
char *Locale = "en_US.UTF-8";
uint32_t Ssrc;

struct sockaddr_storage Control_address;
int Status_sock = -1;
int Control_sock = -1;

char Optstring[] = "hvl:r:s:";
struct option Options[] = {
  {"help", no_argument, NULL, 'h'},
  {"ssrc", required_argument, NULL, 's'},
  {"radio", required_argument, NULL, 'r'},
  {"locale", required_argument, NULL, 'l'},
  {"verbose", no_argument, NULL, 'v'},
  {NULL, 0, NULL, 0},
};

int main(int argc,char *argv[]){
  App_path = argv[0];
  {
    char * const cp = getenv("LANG");
    if(cp != NULL)
      Locale = cp;
  }
  {
    int c;
    while((c = getopt_long(argc,argv,Optstring,Options,NULL)) != -1){
      switch(c){
      case 's':
	Ssrc = strtol(optarg,NULL,0);
	break;
      case 'v':
	Verbose++;
	break;
      case 'l':
	Locale = optarg;
	break;
      case 'r':
	Radio = optarg;
	break;
      default:
    fprintf(stderr,"Invalid command line option -%c\n",c);
      case 'h':
    fprintf(stderr,"Usage: %s [-h] [-l LOCALE] -r/--radio RADIO -s/--ssrc SSRC [-v] FREQUENCY",argv[0]);
    exit(-1);
      }
    }
  }
  setlocale(LC_ALL,Locale);

  if(Radio == NULL)
    Radio = getenv("RADIO");

  if(Radio == NULL){
    fprintf(stderr,"--radio not specified and $RADIO not set\n");
    exit(1);
  }
  if(Ssrc == 0){
    fprintf(stderr,"--ssrc not specified\n");
    exit(1);
  }

  {
    char iface[1024];
    resolve_mcast(Radio,&Control_address,DEFAULT_STAT_PORT,iface,sizeof(iface));
    Status_sock = listen_mcast(&Control_address,iface);

    if(Status_sock == -1){
      fprintf(stderr,"Can't open Status_sock socket to radio control channel %s: %s\n",Radio,strerror(errno));
      exit(1);
    }
    Control_sock = connect_mcast(&Control_address,iface,Mcast_ttl,IP_tos);
    if(Control_sock == -1){
      fprintf(stderr,"Can't open cmd socket to radio control channel %s: %s\n",Radio,strerror(errno));
      exit(1);
    }
  }
    
  double sent_freq = 0;
  uint32_t sent_tag = 0; // Used only if sent_freq != 0
  if(argc <= optind){
    fprintf(stderr,"freq not specified\n");
    exit(1);
  }
  // Frequency specified; generate a command

  double f = parse_frequency(argv[optind]);
  f = fabs(f);
  // If frequency would be out of range, guess kHz or MHz
  if(f >= 0.1 && f < 100)
    f = f*1e6; // 0.1 - 99.999 Only MHz can be valid
  else if(f < 500)         // 100-499.999 could be kHz or MHz, assume MHz
    f = f*1e6;
  else if(f < 2000)        // 500-1999.999 could be kHz or MHz, assume kHz
    f = f*1e3;
  else if(f < 100000)      // 2000-99999.999 can only be kHz
    f = f*1e3;
  
  unsigned char buffer[8192];
  unsigned char *bp = buffer;
  
  *bp++ = 1; // Generate command packet
  sent_tag = random();
  encode_int(&bp,COMMAND_TAG,sent_tag);
  encode_int(&bp,OUTPUT_SSRC,Ssrc);
  sent_freq = f;
  encode_double(&bp,RADIO_FREQUENCY,sent_freq); // Hz
  encode_eol(&bp);
  int cmd_len = bp - buffer;
  if(send(Control_sock, buffer, cmd_len, 0) != cmd_len)
    perror("command send");


  // Read and process status
  for(;;){
    unsigned char buffer[8192];
    int length = recvfrom(Status_sock,buffer,sizeof(buffer),0,NULL,NULL);
    if(length <= 0){
      fprintf(stderr,"recvfrom status socket error: %s\n",strerror(errno));
      sleep(1);
      continue;
    }
    // We could check the source address here, but we have no way of verifying it.
    // But there should only be one host sending status to this group anyway
    unsigned char const * cp = buffer;
    if(*cp++ != 0)
      continue; // Look only at status packets

    uint32_t received_tag = 0;
    double received_freq = 0;
    int freq_seen = 0;
    uint32_t received_ssrc = 0;

    while(cp - buffer < length){
      enum status_type type = *cp++;
      if(type == EOL)
	break;
      unsigned int optlen = *cp++;
      if(cp - buffer + optlen > length)
	break; // Invalid length
      switch(type){
      default:
	break;
      case COMMAND_TAG:
	received_tag = (uint32_t)decode_int(cp,optlen);
	break;
      case RADIO_FREQUENCY:
	received_freq = decode_double(cp,optlen);
	freq_seen = 1;
	break;
      case OUTPUT_SSRC:
	received_ssrc = decode_int(cp,optlen);
	break;
      }
      cp += optlen;
    }
    // Ignore compressed status packets omitting frequency
    // We may see these if we didn't send a command
    if(!freq_seen)
      continue;

    // If we sent a command, wait for its acknowledgement
    // Otherwise, just display the current frequency
    if(received_tag != sent_tag || received_ssrc != Ssrc)
      continue;
    printf("Frequency %'.3lf Hz\n",received_freq);
    break;
  }
  exit(0);
}

