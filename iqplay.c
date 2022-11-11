// $Id: iqplay.c,v 1.42 2022/08/05 06:35:10 karn Exp $
// Read from IQ recording, multicast in (hopefully) real time
// Copyright 2018 Phil Karn, KA9Q
#define _GNU_SOURCE 1 // allow bind/connect/recvfrom without casting sockaddr_in6
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <math.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <signal.h>
#include <locale.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <getopt.h>
#include <libgen.h>

#include "misc.h"
#include "radio.h"
#include "multicast.h"
#include "attr.h"
#include "status.h"


const char *App_path;
int Verbose;
int Mcast_ttl = 1; // Don't send fast IQ streams beyond the local network by default
int IP_tos = 48; // AF12 << 2
double Frequency = 0;
long Samprate = 48000;
const int Bufsize = 16384;
int Blocksize = 256;
char *Description;
char *Output;
char *Status;
struct sockaddr_storage Output_data_dest_address;
struct sockaddr_storage Output_data_source_address;
struct sockaddr_storage Output_metadata_dest_address;
struct sockaddr_storage Output_metadata_source_address;
uint64_t Commands;
uint64_t Output_metadata_packets;
float Power;
struct rtp_state Rtp_state;
int Status_sock = -1;
int Rtp_sock = -1; // Socket handle for sending real time stream
int Nctl_sock = -1;
int Channels;
int Bitspersample;
float Min_IF,Max_IF;


void send_iqplay_status(int full);
int playfile(int,int,int);
void *ncmd(void *);


struct option const Options[] =
  {
   {"iface", required_argument, NULL, 'A'},
   {"pcm-out", required_argument, NULL, 'D'},
   {"iq-out", required_argument, NULL, 'D'},
   {"status-out", required_argument, NULL, 'R'},
   {"ssrc", required_argument, NULL, 'S'},
   {"ttl", required_argument, NULL, 'T'},
   {"blocksize", required_argument, NULL, 'b'},
   {"frequency", required_argument, NULL, 'f'},
   {"tos", required_argument, NULL, 'p'},
   {"iptos", required_argument, NULL, 'p'},
   {"ip-tos", required_argument, NULL, 'p'},    
   {"samprate", required_argument, NULL, 'r'},
   {"verbose", no_argument, NULL, 'v'},
   {NULL, 0, NULL, 0},
  };
char const Optstring[] = "A:D:R:S:T:b:f:vr:";


int main(int argc,char *argv[]){
  App_path = argv[0];
#if 0 // Better done manually?
  // if we have root, up our priority and drop privileges
  int prio = getpriority(PRIO_PROCESS,0);
  prio = setpriority(PRIO_PROCESS,0,prio - 10);

  // Quickly drop root if we have it
  // The sooner we do this, the fewer options there are for abuse
  if(seteuid(getuid()) != 0)
    perror("seteuid");
#endif

  {
    char *locale = getenv("LANG");
    if(locale == NULL)
      locale = "en_US.UTF-8";
    
    setlocale(LC_ALL,locale);
  }
  Rtp_state.ssrc = gps_time_sec(); // Default, can be overridden

  int c;
  while((c = getopt_long(argc,argv,Optstring,Options,NULL)) != -1){
    switch(c){
    case 'A':
      Default_mcast_iface = optarg;
      break;
    case 'r':
      Samprate = strtol(optarg,NULL,0);
      break;
    case 'D':
      Output = optarg;
      break;
    case 'R':
      Status = optarg;
      break;
    case 'S':
      Rtp_state.ssrc = strtol(optarg,NULL,0);
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
    case 'b':
      Blocksize = strtol(optarg,NULL,0);
      break;
    case 'f': // Used only if there's no tag on a file, or for stdin
      Frequency = strtod(optarg,NULL);
      break;
    }
  }
  if(argc < optind){
    fprintf(stderr,"Usage: %s [options] [filename|-]\n",argv[0]);
    exit(1);
  }
  if(Output == NULL){
    fprintf(stderr,"Output (-D/--iq-out/--pcm-out) must be specified\n");
    exit(1);
  }
  if(Status == NULL){
    fprintf(stderr,"Status (-R/--status-out) must be specified\n");
    exit(1);
  }

  {
    // Start Avahi client that will maintain our mDNS registrations
    // Service name, if present, must be unique
    // Description, if present becomes TXT record if present
    char data_dns_name[1024];
    char metadata_dns_name[1024];


    // Append .local to incomplete domain names
    if(strchr(Status,'.') == NULL)
      snprintf(metadata_dns_name,sizeof(metadata_dns_name),"%s.local",Status);
    else
      snprintf(metadata_dns_name,sizeof(metadata_dns_name),"%s",Status);

    if(strchr(Output,'.') == NULL)
       snprintf(data_dns_name,sizeof(data_dns_name),"%s.local",Output);
    else
       snprintf(data_dns_name,sizeof(data_dns_name),"%s",Output);

    char service_name[1060];
    snprintf(service_name,sizeof(service_name),"iqplay(%s)",metadata_dns_name);
    avahi_start(service_name,"_ka9q-ctl._udp",DEFAULT_STAT_PORT,metadata_dns_name,ElfHashString(metadata_dns_name),NULL);

    snprintf(service_name,sizeof(service_name),"iqplay(%s)",data_dns_name);
    avahi_start(service_name,"_rtp._udp",DEFAULT_RTP_PORT,data_dns_name,ElfHashString(data_dns_name),NULL);

    // Now resolve them back
    resolve_mcast(metadata_dns_name,&Output_metadata_dest_address,DEFAULT_STAT_PORT,NULL,0);
    Status_sock = connect_mcast(&Output_metadata_dest_address,NULL,Mcast_ttl,IP_tos);
    if(Status_sock <= 0){
      fprintf(stderr,"Can't create multicast status socket to %s: %s\n",metadata_dns_name,strerror(errno));
      exit(1);
    }
    // Set up new control socket on port 5006
    Nctl_sock = listen_mcast(&Output_metadata_dest_address,NULL);
    if(Nctl_sock <= 0){
      fprintf(stderr,"Can't create multicast command socket from %s: %s\n",metadata_dns_name,strerror(errno));
      exit(1);
    }
    resolve_mcast(data_dns_name,&Output_data_dest_address,DEFAULT_RTP_PORT,NULL,0);
    Rtp_sock = connect_mcast(&Output_data_dest_address,NULL,Mcast_ttl,IP_tos);
    if(Rtp_sock == -1){
      fprintf(stderr,"Can't create multicast socket to %s: %s\n",data_dns_name,strerror(errno));
      exit(1);
    }
    socklen_t len = sizeof(Output_data_source_address);
    getsockname(Rtp_sock,(struct sockaddr *)&Output_data_source_address,&len);
  }

  signal(SIGPIPE,SIG_IGN);

  pthread_t status;
  pthread_create(&status,NULL,ncmd,NULL);

  if(optind == argc){
    // No file arguments, read from stdin
    if(Verbose)
      fprintf(stderr,"Transmitting from stdin");
    Description = "stdin";
    playfile(Rtp_sock,0,Blocksize);
  } else {
    for(int i=optind;i<argc;i++){
      int fd;
      if((fd = open(argv[i],O_RDONLY)) == -1){
	fprintf(stderr,"Can't read %s: %s\n",argv[i],strerror(errno));
	continue;
      }
      if(Verbose)
	fprintf(stderr,"Transmitting %s",argv[i]);
      Description = argv[i];
      playfile(Rtp_sock,fd,Blocksize);
      close(fd);
      fd = -1;
    }
  }
  close(Rtp_sock);
  Rtp_sock = -1;
  exit(0);
}

// Play I/Q file with descriptor 'fd' on network socket 'sock'
int playfile(int sock,int fd,int blocksize){
  attrscanf(fd,"samplerate","%ld",&Samprate);
  attrscanf(fd,"frequency","%lf",&Frequency);
  attrscanf(fd,"bitspersample","%d",&Bitspersample);
  attrscanf(fd,"channels","%d",&Channels);
  attrscanf(fd,"ssrc","%u",&Rtp_state.ssrc);
  attrscanf(fd,"min_IF","%f",&Min_IF);
  attrscanf(fd,"max_IF","%f",&Max_IF);

  if(Verbose)
    fprintf(stderr,": fd %d, %'ld samp/s, RF LO %'.1lf Hz\n",fd,Samprate,Frequency);


  struct rtp_header rtp_header;
  memset(&rtp_header,0,sizeof(rtp_header));
  rtp_header.version = RTP_VERS;
  switch(Bitspersample){
  case 8:
    rtp_header.type = Channels == 1 ? REAL_PT8 : IQ_PT8;
    break;
  case 12:
    rtp_header.type = (Channels == 1) ? REAL_PT12 : IQ_PT12;
    break;
  case 16:
    rtp_header.type = (Channels == 1) ? PCM_MONO_PT : PCM_STEREO_PT;
    break;
  default:
    fprintf(stderr,"unsupported bits per sample %d\n",Bitspersample);
    return -1;
  }
  long long start_time = gps_time_ns();

  rtp_header.ssrc = Rtp_state.ssrc;
  
  // nanosec between packets.
  long long dt_ns = (BILLION * blocksize) / Samprate;
  // Nanoseconds since start for next scheduled transmission; will transmit first immediately
  long long sked_time = 0;

  while(1){
    rtp_header.seq = Rtp_state.seq++;
    rtp_header.timestamp = Rtp_state.timestamp;
    Rtp_state.timestamp += blocksize;
    
    // Is it time yet?
    while(1){
      // Nanoseconds since start
      long long diff = gps_time_ns() - start_time;
      if(diff >= sked_time)
	break;
      if(sked_time > diff + 100000){ // 100 microsec
	struct timespec ts;
	ns2ts(&ts,diff);
	nanosleep(&ts,NULL);
      }
    }
    unsigned char output_buffer[4*blocksize + 256]; // will this allow for largest possible RTP header??
    unsigned char *dp = output_buffer;
    dp = hton_rtp(dp,&rtp_header);

    int r = pipefill(fd,dp,4*blocksize);
    if(r <= 0){
      if(Verbose)
	fprintf(stderr,"pipefill returns %d\n",r);
      break;
    }
    // This depends on the sample format
    signed short *sp = (signed short *)dp;
    float p = 0;
    for(int n=0; n < 2*blocksize; n ++){
      p += (float)(*sp) * (float)(*sp);
      *sp = htons(*sp);
      sp++;
    }
    Power = p / (32767. * 32767. * blocksize);

    dp = (unsigned char *)sp;

    int length = dp - output_buffer;
    if(send(sock,output_buffer,length,0) == -1)
      perror("send");
    
    Rtp_state.packets++;
    // Update time of next scheduled transmission
    sked_time += dt_ns;
  }
  return 0;
}


// Thread to send metadata and process commands
void *ncmd(void *arg){
  pthread_setname("iqsendcmd");
  
  // Set up status socket on port 5006
  struct timeval tv;
  tv.tv_sec = 0;
  tv.tv_usec = 100000; // 100 ms

  if(setsockopt(Nctl_sock,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv))){
    perror("ncmd setsockopt");
    return NULL;
  }
  int counter = 0;
  while(1){
    unsigned char buffer[Bufsize];
    memset(buffer,0,sizeof(buffer));
    int length = recv(Nctl_sock,buffer,sizeof(buffer),0); // Waits up to 100 ms for command
    if(length > 0){

      // Parse entries
      unsigned char *cp = buffer;

      int cr = *cp++; // Command/response
      if(cr == 0)
	continue; // Ignore our own status messages
      Commands++;
      //      decode_iqplay_commands(sdr,cp,length-1);      // Implement later
      counter = 0; // Respond with full status
    }
    Output_metadata_packets++;
    send_iqplay_status(counter == 0);
    if(counter-- <= 0)
      counter = 10;
  }
}


void send_iqplay_status(int full){
  unsigned char packet[2048],*bp;
  memset(packet,0,sizeof(packet));
  bp = packet;
  
  *bp++ = 0; // command/response = response
  //  encode_int32(&bp,COMMAND_TAG,...);
  encode_int64(&bp,CMD_CNT,Commands);
  encode_int64(&bp,GPS_TIME,gps_time_ns());

  if(Description)
    encode_string(&bp,DESCRIPTION,Description,strlen(Description));
  
  encode_socket(&bp,OUTPUT_DATA_SOURCE_SOCKET,&Output_data_source_address);
  encode_socket(&bp,OUTPUT_DATA_DEST_SOCKET,&Output_data_dest_address);
  encode_int32(&bp,OUTPUT_SSRC,Rtp_state.ssrc);
  encode_byte(&bp,OUTPUT_TTL,Mcast_ttl);
  encode_int32(&bp,OUTPUT_SAMPRATE,Samprate);
  encode_int64(&bp,OUTPUT_DATA_PACKETS,Rtp_state.packets);
  encode_int64(&bp,OUTPUT_METADATA_PACKETS,Output_metadata_packets);
  encode_float(&bp,LOW_EDGE,Min_IF);
  encode_float(&bp,HIGH_EDGE,Max_IF);
  
  // Front end
  encode_byte(&bp,DIRECT_CONVERSION,0);
  encode_float(&bp,GAIN,0.0); 
  
  // Tuning
  encode_double(&bp,RADIO_FREQUENCY,Frequency);
  
  // Filtering
  encode_float(&bp,OUTPUT_LEVEL,power2dB(Power));

  encode_byte(&bp,DEMOD_TYPE,0); // Actually LINEAR_MODE
  encode_int32(&bp,OUTPUT_CHANNELS,Channels);
  encode_int32(&bp,OUTPUT_BITS_PER_SAMPLE,Bitspersample);

  encode_eol(&bp);
  int len = bp - packet;
  assert(len < sizeof(packet));
  send(Status_sock,packet,len,0);
}
