// Interactive program to tune radiod in ka9q-radio
// Copyright 2023 Phil Karn, KA9Q

#define _GNU_SOURCE 1
#include <stdio.h>
#include <assert.h>

#include <stdint.h>
#include <getopt.h>
#include <unistd.h>
#if defined(linux)
#include <bsd/stdlib.h>
#include <bsd/string.h>
#else
#include <stdlib.h>
#endif
#include <string.h>
#include <sys/socket.h>
#include <locale.h>
#include <errno.h>
#include <sysexits.h>
#include <poll.h>

#include "misc.h"
#include "multicast.h"
#include "status.h"

int Mcast_ttl = 1;
int IP_tos = 0;
const char *App_path;
int Verbose;
char const *Radio = NULL;
char const *Locale = "en_US.UTF-8";
char const *Iface;
char const *Mode;
uint32_t Ssrc;
float Gain = INFINITY;
double Frequency = INFINITY;
int Agc = -1;

struct sockaddr_storage Control_address;
int Status_sock = -1;
int Control_sock = -1;

char Optstring[] = "f:g:hi:vl:r:s:V";
struct option Options[] = {
  {"agc", no_argument, NULL, 'a'},
  {"frequency", required_argument, NULL, 'f'},
  {"gain", required_argument, NULL, 'g'},
  {"help", no_argument, NULL, 'h'},
  {"iface", required_argument, NULL, 'i'},
  {"mode", required_argument, NULL, 'm'},
  {"ssrc", required_argument, NULL, 's'},
  {"radio", required_argument, NULL, 'r'},
  {"locale", required_argument, NULL, 'l'},
  {"verbose", no_argument, NULL, 'v'},
  {"version", no_argument, NULL, 'V'},
  {NULL, 0, NULL, 0},
};

void usage(void);

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
      case 'f':
	Frequency = parse_frequency(optarg,true);
	break;
      case 'g':
	Gain = strtod(optarg,NULL);
	break;
      case 'i':
	Iface = optarg;
	break;
      case 'm':
	Mode = optarg;
	break;
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
      case 'a':
	Agc = 1;
	break;
      case 'V':
	VERSION();
	exit(EX_OK);
      default:
	fprintf(stdout,"Invalid command line option -%c\n",c);
      case 'h':
	usage();
	exit(EX_USAGE);
      }
    }
    // -r option not specified, see if it was given as an additional arg
    if(Radio == NULL && argc >= optind)
	Radio = argv[optind];
  }
  setlocale(LC_ALL,Locale);

  if(Radio == NULL)
    Radio = getenv("RADIO");

  if(Radio == NULL){
    fprintf(stdout,"--radio not specified and $RADIO not set\n");
    usage();
    exit(EX_USAGE);
  }
  if(Ssrc == 0){
    fprintf(stdout,"--ssrc not specified\n");
    usage();
    exit(EX_USAGE);
  }
  {
    if(Verbose)
      fprintf(stdout,"Resolving %s\n",Radio);
    char iface[1024];
    resolve_mcast(Radio,&Control_address,DEFAULT_STAT_PORT,iface,sizeof(iface));
    char const *ifc = (Iface != NULL) ? Iface : iface;
    

    if(Verbose)
      fprintf(stdout,"Listening\n");
    Status_sock = listen_mcast(&Control_address,ifc);

    if(Status_sock == -1){
      fprintf(stdout,"Can't open Status_sock socket to radio control channel %s: %s\n",Radio,strerror(errno));
      exit(EX_IOERR);
    }
    if(Verbose)
      fprintf(stdout,"Connecting\n");
    Control_sock = connect_mcast(&Control_address,ifc,Mcast_ttl,IP_tos);
    if(Control_sock == -1){
      fprintf(stdout,"Can't open cmd socket to radio control channel %s: %s\n",Radio,strerror(errno));
      exit(EX_IOERR);
    }
  }
  // Begin polling SSRC to ensure the multicast group is up and radiod is listening
  long long last_command_time = 0;


  uint32_t received_tag = 0;
  double received_freq = INFINITY;
  uint32_t received_ssrc = 0;
  int received_agc_enable = -1;
  float received_gain = INFINITY;
  char preset[256];
  float noise_density = INFINITY;
  float baseband_level = INFINITY;
  float low_edge = INFINITY;
  float high_edge = INFINITY;
  memset(preset,0,sizeof(preset));


  uint32_t sent_tag = 0;
  while(1){
    // (re)send command until we get a response;
    if(gps_time_ns() >= last_command_time + BILLION/10){ // Rate limit command packets to 10 Hz
      uint8_t cmd_buffer[9000];
      uint8_t *bp = cmd_buffer;
      *bp++ = 1; // Generate command packet
      sent_tag = arc4random();
      encode_int(&bp,COMMAND_TAG,sent_tag);
      encode_int(&bp,OUTPUT_SSRC,Ssrc);
      if(Mode != NULL)
	encode_string(&bp,PRESET,Mode,strlen(Mode));
      
      if(Frequency != INFINITY)
	encode_double(&bp,RADIO_FREQUENCY,Frequency); // Hz
      if(Gain != INFINITY){
	encode_float(&bp,GAIN,Gain);
	encode_int(&bp,AGC_ENABLE,false); // Turn off AGC for manual gain
      } else if(Agc != -1)
	encode_int(&bp,AGC_ENABLE,Agc);
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
    
    // Incoming packet should be ready
    uint8_t response_buffer[9000];
    uint8_t const * cp = response_buffer; // make response read-only
    int length = recvfrom(Status_sock,response_buffer,sizeof(response_buffer),0,NULL,NULL);
    
    if(length <= 0){
      fprintf(stdout,"recvfrom status socket error: %s\n",strerror(errno));
      exit(1);
    }
    if(Verbose)
      fprintf(stdout,"Message received, %d bytes, type %d\n",length,*cp);
    
    if(*cp++ != 0)
      continue; // ignore non-response; go back and receive again
    
    // Process response
    while(cp - response_buffer < length){
      enum status_type type = *cp++;
      if(type == EOL)
	break;
      unsigned int optlen = *cp++;
      if(cp - response_buffer + optlen > length)
	break; // Invalid length
      switch(type){
      default:
	break;
      case COMMAND_TAG:
	received_tag = (uint32_t)decode_int(cp,optlen);
	break;
      case RADIO_FREQUENCY:
	received_freq = decode_double(cp,optlen);
	break;
      case OUTPUT_SSRC:
	received_ssrc = decode_int(cp,optlen);
	break;
      case AGC_ENABLE:
	received_agc_enable = decode_int(cp,optlen);
	break;
      case GAIN:
	received_gain = decode_float(cp,optlen);
	  break;
      case PRESET:
	decode_string(cp,optlen,preset,sizeof(preset));
	break;
      case LOW_EDGE:
	low_edge = decode_float(cp,optlen);
	break;
      case HIGH_EDGE:
	high_edge = decode_float(cp,optlen);
	break;
      case NOISE_DENSITY:
	noise_density = decode_float(cp,optlen);
	break;
      case BASEBAND_POWER:
	baseband_level = decode_float(cp,optlen);
	break;
      }
      cp += optlen;
    }
    if(received_ssrc == Ssrc && received_tag == sent_tag)
      break; // For us; we're done
    if(Verbose)
      fprintf(stdout,"Not for us: ssrc %'u, tag %'u\n",(int)received_ssrc,(int)received_tag);
  }

  // Show responses
  printf("SSRC %'u\n",Ssrc);
  if(strlen(preset) > 0)
    printf("Preset %s\n",preset);
  if(received_freq != INFINITY)
    printf("Frequency %'.3lf Hz\n",received_freq);
  if(received_agc_enable != -1)
    printf("AGC %s\n",received_agc_enable ? "on" : "off");

  if(received_gain != INFINITY)
    printf("Gain %.1f dB\n",received_gain);

  if(baseband_level != INFINITY)
    printf("Baseband power %.1f dB\n",baseband_level);

  if(low_edge != INFINITY && high_edge != INFINITY)
    printf("Passband %.1f Hz to %.1f Hz (%.1f dB-Hz)\n",low_edge,high_edge,10*log10(fabsf(high_edge - low_edge)));

  if(noise_density != INFINITY)
    printf("N0 %.1f dB/Hz\n",noise_density);
  
  if(baseband_level != INFINITY && 
     low_edge != INFINITY &&
     high_edge != INFINITY &&
     noise_density != INFINITY){

    float noise_power = dB2power(noise_density) * fabsf(high_edge - low_edge);
    float signal_plus_noise_power = dB2power(baseband_level);

    printf("SNR %.1f dB\n",power2dB(signal_plus_noise_power / noise_power - 1));
  }
  exit(EX_OK); // We're done
}

void usage(void){
  fprintf(stdout,"Usage: %s [-h|--help] [-v|--verbose] -r/--radio RADIO -s/--ssrc SSRC [-i|--iface <iface>] [-l|--locale LOCALE]  \
[-f|- FREQUENCY]frequency] [[-a|--agc] | [-g|--Gain <gain dB>]] [-m|--mode <mode>]\n" ,App_path);
}
