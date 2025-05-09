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
#include "rtp.h"
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
float Low = INFINITY;
float High = INFINITY;
int Samprate = 0;
bool Quiet = false;
enum encoding Encoding = NO_ENCODING;
float RFgain = INFINITY;
float RFatten = INFINITY;
int Agc_enable = -1;

struct sockaddr Control_address;
int Status_sock = -1;
int Control_sock = -1;
char const *Source;
struct sockaddr_in *Source_socket;

char Optstring[] = "aA:e:f:g:G:H:hi:L:l:m:qr:R:s:vVo:";
struct option Options[] = {
  {"agc", no_argument, NULL, 'a'},
  {"rfatten", required_argument, NULL, 'A'},
  {"featten", required_argument, NULL, 'A'},
  {"encoding", required_argument, NULL, 'e'},
  {"frequency", required_argument, NULL, 'f'},
  {"gain", required_argument, NULL, 'g'},
  {"rfgain", required_argument, NULL, 'G'},
  {"fegain", required_argument, NULL, 'G'},
  {"help", no_argument, NULL, 'h'},
  {"iface", required_argument, NULL, 'i'},
  {"locale", required_argument, NULL, 'l'},
  {"low", required_argument, NULL, 'L'},
  {"high", required_argument, NULL, 'H'},
  {"mode", required_argument, NULL, 'm'},
  {"quiet", no_argument, NULL, 'q'},
  {"radio", required_argument, NULL, 'r'},
  {"samprate", required_argument, NULL, 'R'},
  {"ssrc", required_argument, NULL, 's'},
  {"verbose", no_argument, NULL, 'v'},
  {"version", no_argument, NULL, 'V'},
  {"source", required_argument, NULL, 'o'},
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
      case 'a':
	Agc_enable = 1;
	break;
      case 'A':
	RFatten = strtod(optarg,NULL);
	break;
      case 'G':
	RFgain = strtod(optarg,NULL);
	break;
      case 'e':
	Encoding = parse_encoding(optarg);
	if(Encoding == NO_ENCODING){
	  fprintf(stdout,"Unknown encoding %s\n",optarg);
	  fprintf(stdout,"Encodings: S16BE S16LE F32 F16 OPUS\n");
	}
	break;
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
      case 'q':
	Quiet = true;
	break;
      case 'R':
	Samprate = parse_frequency(optarg,false);
	break;
      case 'L':
	Low = parse_frequency(optarg,false);
	break;
      case 'H':
	High = parse_frequency(optarg,false);
	break;
      case 'V':
	VERSION();
	exit(EX_OK);
      case 'o':
	Source = optarg;
	break;
      default: // including 'h'
	fprintf(stdout,"Invalid command line option -%c\n",c);
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
    if(Source != NULL){
      Source_socket = calloc(1,sizeof(struct sockaddr_storage));
      if(Verbose)
	fprintf(stdout,"Resolving source %s\n",Source);
      resolve_mcast(Source,Source_socket,0,NULL,0,0);
    }

    if(Verbose)
      fprintf(stdout,"Resolving %s\n",Radio);
    char iface[1024];
    resolve_mcast(Radio,&Control_address,DEFAULT_STAT_PORT,iface,sizeof(iface),0);
    char const *ifc = (Iface != NULL) ? Iface : iface;


    if(Verbose){
      if(Source)
	fprintf(stdout,"Listening to %s only from %s\n",Radio,Source);
      else
	fprintf(stdout,"Listening to %s\n",Radio);
    }
    Status_sock = listen_mcast(Source_socket,&Control_address,ifc);

    if(Status_sock == -1){
      fprintf(stdout,"Can't open Status_sock socket to radio control channel %s: %s\n",Radio,strerror(errno));
      exit(EX_IOERR);
    }
    if(Verbose)
      fprintf(stdout,"Connecting\n");

    Control_sock = output_mcast(&Control_address,ifc,Mcast_ttl,IP_tos);
    if(Control_sock == -1){
      fprintf(stdout,"Can't open cmd socket to radio control channel %s: %s\n",Radio,strerror(errno));
      exit(EX_IOERR);
    }
  }
  // Begin polling SSRC to ensure the multicast group is up and radiod is listening
  long long last_command_time = 0;

  if(Low > High){
    float temp = Low;
    Low = High;
    High = temp;
  }
  uint32_t received_tag = 0;
  double received_freq = INFINITY;
  uint32_t received_ssrc = 0;
  int received_agc_enable = -1;
  float received_gain = INFINITY;
  char *preset = NULL;
  float noise_density = INFINITY;
  float baseband_level = INFINITY;
  float low_edge = INFINITY;
  float high_edge = INFINITY;
  float received_rf_gain = INFINITY;
  float received_rf_atten = INFINITY;
  enum encoding received_encoding = NO_ENCODING;
  int received_rf_agc = -1;
  int samprate = 0;

  uint32_t sent_tag = 0;
  while(true){
    // (re)send command until we get a response;
    if(gps_time_ns() >= last_command_time + BILLION/10){ // Rate limit command packets to 10 Hz
      uint8_t cmd_buffer[PKTSIZE];
      uint8_t *bp = cmd_buffer;
      *bp++ = 1; // Generate command packet
      sent_tag = arc4random();
      encode_int(&bp,COMMAND_TAG,sent_tag);
      encode_int(&bp,OUTPUT_SSRC,Ssrc);

      if(Mode != NULL)
	encode_string(&bp,PRESET,Mode,strlen(Mode));

      if(Samprate != 0)
	encode_int(&bp,OUTPUT_SAMPRATE,Samprate);

      if(Low != INFINITY)
	encode_float(&bp,LOW_EDGE,Low);

      if(High != INFINITY)
	encode_float(&bp,HIGH_EDGE,High);

      if(Frequency != INFINITY)
	encode_double(&bp,RADIO_FREQUENCY,Frequency); // Hz
      if(Gain != INFINITY){
	encode_float(&bp,GAIN,Gain);
	encode_int(&bp,AGC_ENABLE,false); // Turn off AGC for manual gain
      } else if(Agc_enable == 1)
	encode_int(&bp,AGC_ENABLE,true);

      if(Encoding != NO_ENCODING)
	encode_int(&bp,OUTPUT_ENCODING,Encoding);

      if(RFgain != INFINITY)
	encode_float(&bp,RF_GAIN,RFgain);
      if(RFatten != INFINITY)
	encode_float(&bp,RF_ATTEN,RFatten);

      encode_eol(&bp);
      int cmd_len = bp - cmd_buffer;
      if(sendto(Control_sock, cmd_buffer, cmd_len, 0,&Control_address,sizeof Control_address) != cmd_len)
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
    uint8_t response_buffer[PKTSIZE];
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
    while(cp < response_buffer + length){
      enum status_type type = *cp++;
      if(type == EOL)
	break;
      unsigned int optlen = *cp++;
      if(optlen & 0x80){
         // length is >= 128 bytes; fetch actual length from next N bytes, where N is low 7 bits of optlen
         int length_of_length = optlen & 0x7f;
         optlen = 0;
         while(length_of_length > 0){
            optlen <<= 8;
            optlen |= *cp++;
            length_of_length--;
         }
      }

      if(cp + optlen > response_buffer + length)
	break; // Invalid length
      switch(type){
      default:
	break;
      case COMMAND_TAG:
	received_tag = decode_int32(cp,optlen);
	break;
      case RADIO_FREQUENCY:
	received_freq = decode_double(cp,optlen);
	break;
      case OUTPUT_SSRC:
	received_ssrc = decode_int32(cp,optlen);
	break;
      case AGC_ENABLE:
	received_agc_enable = decode_bool(cp,optlen);
	break;
      case GAIN:
	received_gain = decode_float(cp,optlen);
	break;
      case RF_GAIN:
	received_rf_gain = decode_float(cp,optlen);
	break;
      case RF_ATTEN:
	received_rf_atten = decode_float(cp,optlen);
	break;
      case RF_AGC:
	received_rf_agc = decode_int(cp,optlen);
	break;
      case PRESET:
	FREE(preset); // Unlikely, but just in case
	preset = decode_string(cp,optlen);
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
      case OUTPUT_SAMPRATE:
	samprate = decode_int(cp,optlen);
	break;
      case OUTPUT_ENCODING:
	received_encoding = decode_int(cp,optlen);
	break;
      }
      cp += optlen;
    }
    if(received_ssrc == Ssrc && received_tag == sent_tag)
      break; // For us; we're done
    if(Verbose)
      fprintf(stdout,"Not for us: ssrc %'u, tag %'u\n",(int)received_ssrc,(int)received_tag);
  }

  // Show responses unless quiet
  if(!Quiet){
    printf("SSRC %'u\n",Ssrc);
    if ((preset) && (strlen(preset)>0)){
      printf("Preset %s\n",preset);
      FREE(preset);
    }
    if(samprate != 0)
      printf("Sample rate %'d Hz\n",samprate);

    if(received_encoding != NO_ENCODING)
      printf("Encoding %s\n",encoding_string(received_encoding));

    if(received_freq != INFINITY)
      printf("Frequency %'.3lf Hz\n",received_freq);

    if(received_agc_enable != -1)
      printf("Channel AGC %s\n",received_agc_enable ? "on" : "off");

    if(received_gain != INFINITY)
      printf("Channel Gain %.1f dB\n",received_gain);

    if(received_rf_agc != -1)
      printf("RF AGC %s\n",received_rf_agc ? "on" : "off");

    if(received_rf_gain != INFINITY)
      printf("RF Gain %.1f dB\n",received_rf_gain);

    if(received_rf_atten != INFINITY)
      printf("RF Atten %.1f dB\n",received_rf_atten);

    if(baseband_level != INFINITY)
      printf("Baseband power %.1f dB\n",baseband_level);

    if(low_edge != INFINITY && high_edge != INFINITY)
      printf("Passband %'.1f Hz to %'.1f Hz (%.1f dB-Hz)\n",low_edge,high_edge,10*log10(fabsf(high_edge - low_edge)));

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
  }
  exit(EX_OK); // We're done
}

void usage(void){
  fprintf(stdout,"Usage: %s [-h|--help] [-v|--verbose] -r/--radio RADIO -s/--ssrc SSRC [-R|--samprate <sample_rate>] [-i|--iface <iface>] [-l|--locale LOCALE]  \
[-f|--frequency <frequency>] [-L|--low <low-edge>] [-H|--high <high-edge>] [[-a|--agc] [-g|--gain <gain dB>]] [-m|--mode <mode>] [--rfgain <gain dB>] [--rfatten <atten dB>] [-o|--source <source-name-or-address>\n" ,App_path);
}
