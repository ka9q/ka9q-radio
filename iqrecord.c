// $Id: iqrecord.c,v 1.37 2022/08/05 06:35:10 karn Exp $
// NOT CURRENTLY USABLE - needs to read status stream to get sample rate, etc
// Read and record complex I/Q stream or PCM baseband audio
// This version reverts to file I/O from an unsuccessful experiment to use mmap()
// Copyright 2018 Phil Karn, KA9Q
#define _GNU_SOURCE 1
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <string.h>
#if defined(linux)
#include <bsd/string.h>
#endif
#include <math.h>
#include <complex.h>
#undef I
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <locale.h>
#include <signal.h>
#include <netdb.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/stat.h>


#include "radio.h"
#include "attr.h"
#include "multicast.h"

// Largest Ethernet packet
// Normally this would be <1500,
// but what about Ethernet interfaces that can reassemble fragments?
// 65536 should be safe since that's the largest IPv4 datagram.
// But what about IPv6?
#define MAXPKT 65535

// size of stdio buffer for disk I/O
// This should be large to minimize write calls, but how big?
#define BUFFERSIZE (1<<20)

const char *App_path;
int Verbose;
int Quiet;
double Duration = INFINITY;

struct sockaddr Sender;
struct sockaddr Input_mcast_sockaddr;
char *Input;
char *Status;
char *Filedir;
struct frontend Frontend;

void cleanup(void);

int main(int argc,char *argv[]){
  App_path = argv[0];
  char *locale;
  locale = getenv("LANG");
  setlocale(LC_ALL,locale);

  // Defaults
  Quiet = 0;
  int c;
  while((c = getopt(argc,argv,"D:S:l:r:qd:v")) != EOF){
    switch(c){
    case 'D':
      Filedir = optarg;
      break;
    case 'S':
      Status = optarg;
      break;
    case 'l':
      locale = optarg;
      setlocale(LC_ALL,locale);
      break;
    case 'q':
      Quiet++; // Suppress display
      break;
    case 'r':
      Frontend.sdr.samprate = strtod(optarg,NULL);
      break;
    case 'd':
      Duration = strtod(optarg,NULL);
      break;
    case 'v':
      Verbose++;
      break;
    default:
      fprintf(stderr,"Usage: %s -I iq multicast address [-l locale] [-d duration][-q][-v]\n",argv[0]);
      exit(1);
      break;
    }
  }
  if(Status == NULL){
    fprintf(stderr,"Must specify status channel -S\n");
    exit(1);
  }


  char iface[1024];
  if(Verbose)
    fprintf(stderr,"Resolving status channel %s\n",Status);
  resolve_mcast(Status,&Frontend.input.metadata_dest_address,DEFAULT_STAT_PORT,iface,sizeof(iface));
  Frontend.input.status_fd = listen_mcast(&Frontend.input.metadata_dest_address,iface);
  if(Frontend.input.status_fd == -1){
    fprintf(stderr,"Can't set up status input from %s\n",Status);
    exit(1);
  }
  // Acquire data stream info from status stream
  pthread_create(&Frontend.status_thread,NULL,sdr_status,&Frontend);
  
  // We must acquire a status stream before we can proceed further
  // Segment copied from main.c
  if(Verbose)
    fprintf(stderr,"Waiting for front end data...\n");
  pthread_mutex_lock(&Frontend.sdr.status_mutex);
  while(Frontend.sdr.samprate == 0 || Frontend.input.data_dest_address.ss_family == 0)
    pthread_cond_wait(&Frontend.sdr.status_cond,&Frontend.sdr.status_mutex);
  pthread_mutex_unlock(&Frontend.sdr.status_mutex);
  fprintf(stderr,"Input sample rate %'d Hz, %s\n",
	  Frontend.sdr.samprate,Frontend.sdr.isreal ? "real" : "complex");
  
  // Input socket for I/Q data from SDR, set from OUTPUT_DEST_SOCKET in SDR metadata
  Frontend.input.data_fd = listen_mcast(&Frontend.input.data_dest_address,NULL);
  if(Frontend.input.data_fd < 3){
    fprintf(stderr,"Can't set up IF input from %s\n",formatsock(&Frontend.input.data_dest_address));
    exit(1);
  }

  // Create file with name iqrecord-frequency-ssrc or pcmrecord-ssrc
  int suffix;
  char filename[PATH_MAX];
  for(suffix=0;suffix<100;suffix++){
    struct stat statbuf;
    
    if(Filedir)
      snprintf(filename,sizeof(filename),"%s/iqrecord-%.1lfHz-%u-%d",Filedir,Frontend.sdr.frequency,Frontend.input.rtp.ssrc,suffix);
    else
      snprintf(filename,sizeof(filename),"iqrecord-%.1lfHz-%u-%d",Frontend.sdr.frequency,Frontend.input.rtp.ssrc,suffix);
    if(stat(filename,&statbuf) == -1 && errno == ENOENT)
      break;
  }
  if(suffix == 100){
    fprintf(stderr,"Can't generate filename %s to write\n",filename);
    // After this many tries, something is probably seriously wrong
    exit(1);
  }
  FILE *fp = fopen(filename,"w+");
  if(fp == NULL){
    fprintf(stderr,"can't write file %s\n",filename);
    perror("open");
    exit(1);
  }
  if(!Quiet)
    fprintf(stderr,"creating file %s\n",filename);
  
  void *iobuffer = malloc(BUFFERSIZE);
  setbuffer(fp,iobuffer,BUFFERSIZE);
  
  int const fd = fileno(fp);
  fcntl(fd,F_SETFL,O_NONBLOCK); // Let's see if this keeps us from losing data
  
  attrprintf(fd,"frequency","%lf",Frontend.sdr.frequency);
  attrprintf(fd,"samplerate","%lu",(unsigned long)Frontend.sdr.samprate);
  attrprintf(fd,"channels","%d",Frontend.sdr.isreal ? 1 : 2);
  attrprintf(fd,"ssrc","%u",Frontend.input.rtp.ssrc);
  attrprintf(fd,"min_IF","%f",Frontend.sdr.min_IF);
  attrprintf(fd,"max_IF","%f",Frontend.sdr.max_IF);
  attrprintf(fd,"bitspersample","%d",Frontend.sdr.bitspersample);

  char sender_text[NI_MAXHOST];
  // Don't wait for an inverse resolve that might cause us to lose data
  getnameinfo((struct sockaddr *)&Sender,sizeof(Sender),sender_text,sizeof(sender_text),NULL,0,NI_NOFQDN|NI_DGRAM|NI_NUMERICHOST);
  attrprintf(fd,"multicast","%s",formatsock(&Frontend.input.data_dest_address));
      
  struct timespec now;
  clock_gettime(CLOCK_REALTIME,&now);
  attrprintf(fd,"unixstarttime","%ld.%09ld",(long)now.tv_sec,(long)now.tv_nsec);

  // Graceful signal catch
  signal(SIGPIPE,SIG_IGN);
  atexit(cleanup);


  double t = 0;

  struct rtp_state rtp_state;

  while(!isfinite(Duration) || t < Duration){
    // Receive I/Q data from front end
    unsigned char buffer[MAXPKT];
    socklen_t socksize = sizeof(Sender);
    int size = recvfrom(Frontend.input.data_fd,buffer,sizeof(buffer),0,&Sender,&socksize);
    if(size <= 0){    // ??
      perror("recvfrom");
      usleep(50000);
      continue;
    }
    if(size < RTP_MIN_SIZE)
      continue; // Too small for RTP, ignore

    unsigned char const *dp = buffer;
    struct rtp_header rtp;
    dp = ntoh_rtp(&rtp,dp);
    if(rtp.pad){
      // Remove padding
      size -= dp[size-1];
      rtp.pad = 0;
    }
    if(size <= 0)
      continue; // Bogus RTP header
    
    signed short *samples = (signed short *)dp;
    size -= (dp - buffer);

    int sample_count = (size * 8) /
      (Frontend.sdr.isreal ? Frontend.sdr.bitspersample : 2 * Frontend.sdr.bitspersample);
    off_t offset = rtp_process(&rtp_state,&rtp,sample_count);

    // The seek offset relative to the current position in the file is the signed (modular) difference between
    // the actual and expected RTP timestamps. This should automatically handle
    // 32-bit RTP timestamp wraps, which occur every ~1 days at 48 kHz and only 6 hr @ 192 kHz

    // Should I limit the range on this?

    if(offset)
      fseeko(fp,offset,SEEK_CUR);
    fwrite(samples,1,size,fp);
    t += (double)sample_count / Frontend.sdr.samprate;
  }
}
 
void cleanup(void){
}

