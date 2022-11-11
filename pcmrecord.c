// $Id: pcmrecord.c,v 1.19 2022/08/05 06:35:10 karn Exp $ 
// Read and record PCM audio streams
// Adapted from iqrecord.c which is out of date
// Copyright 2021 Phil Karn, KA9Q
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
#include <time.h>


#include "misc.h"
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

// Simplified .wav file header
// http://soundfile.sapp.org/doc/WaveFormat/
struct wav {
  char ChunkID[4];
  int32_t ChunkSize;
  char Format[4];

  char Subchunk1ID[4];
  int32_t Subchunk1Size;
  int16_t AudioFormat;
  int16_t NumChannels;
  int32_t SampleRate;
  int32_t ByteRate;
  int16_t BlockAlign;
  int16_t BitsPerSample;

  char SubChunk2ID[4];
  int32_t Subchunk2Size;
};

// One for each session being recorded
struct session {
  struct session *prev;
  struct session *next;
  struct sockaddr iq_sender;   // Sender's IP address and source port

  char filename[PATH_MAX];
  struct wav header;

  uint32_t ssrc;               // RTP stream source ID
  struct rtp_state rtp_state;
  
  int type;                    // RTP payload type (with marker stripped)
  int channels;                // 1 (PCM_MONO) or 2 (PCM_STEREO)
  unsigned int samprate;       // implicitly 48 kHz in PCM

  FILE *fp;                    // File being recorded
  void *iobuffer;              // Big buffer to reduce write rate
  long long last_active;

  int SubstantialFile;        // At least one substantial segment has been seen
  int64_t CurrentSegmentSamples; // total samples in this segment without skips in timestamp
  int64_t SamplesWritten;
  int64_t TotalFileSamples;
};


float SubstantialFileTime = 0.2;  // Don't record bursts < 250 ms unless they're between two substantial segments

const char *App_path;
int Verbose;
char PCM_mcast_address_text[256];
char const *Recordings = ".";
int Subdirs; // Place recordings in subdirectories by SSID

struct sockaddr Sender;
struct sockaddr Input_mcast_sockaddr;
int Input_fd;
struct session *Sessions;
long long Timeout = 20; // 20 seconds max idle time before file close

void closedown(int a);
void input_loop(void);
void cleanup(void);
struct session *create_session(struct rtp_header *);


int main(int argc,char *argv[]){
  App_path = argv[0];
#if 0 // Better done manually or in systemd?
  // if we have root, up our priority and drop privileges
  int prio = getpriority(PRIO_PROCESS,0);
  prio = setpriority(PRIO_PROCESS,0,prio - 10);

  // Quickly drop root if we have it
  // The sooner we do this, the fewer options there are for abuse
  if(seteuid(getuid()) != 0)
    perror("seteuid");
#endif
  char const *locale = getenv("LANG");
  setlocale(LC_ALL,locale);

  // Defaults
  int c;
  while((c = getopt(argc,argv,"d:l:vt:m:s")) != EOF){
    switch(c){
    case 's':
      Subdirs = 1;
      break;
    case 'd':
      Recordings = optarg;
      break;
    case 'm':
      SubstantialFileTime = strtof(optarg,NULL);
      break;
    case 'l':
      locale = optarg;
      break;
    case 'v':
      Verbose = 1;
      break;
    case 't':
      {
	char *ptr;
	long long x = strtoll(optarg,&ptr,0); 
	if(ptr != optarg)
	  Timeout = x;
      }
      break;
    default:
      fprintf(stderr,"Usage: %s [-l locale] [-t timeout] [-v] [-m sec] PCM_multicast_address\n",argv[0]);
      exit(1);
      break;
    }
  }
  if(optind >= argc){
    fprintf(stderr,"Specify PCM_mcast_address_text_address\n");
    exit(1);
  }
  strlcpy(PCM_mcast_address_text,argv[optind],sizeof(PCM_mcast_address_text));
  setlocale(LC_ALL,locale);

  if(strlen(Recordings) > 0 && chdir(Recordings) != 0){
    fprintf(stderr,"Can't change to directory %s: %s, exiting\n",Recordings,strerror(errno));
    exit(1);
  }

  // Set up input socket for multicast data stream from front end
  {
    struct sockaddr_storage sock;
    char iface[1024];
    resolve_mcast(PCM_mcast_address_text,&sock,DEFAULT_RTP_PORT,iface,sizeof(iface));
    Input_fd = listen_mcast(&sock,iface);
  }
  if(Input_fd == -1){
    fprintf(stderr,"Can't set up PCM input, exiting\n");
    exit(1);
  }
  int n = 1 << 20; // 1 MB
  if(setsockopt(Input_fd,SOL_SOCKET,SO_RCVBUF,&n,sizeof(n)) == -1)
    perror("setsockopt");

  // Graceful signal catch
  signal(SIGPIPE,closedown);
  signal(SIGINT,closedown);
  signal(SIGKILL,closedown);
  signal(SIGQUIT,closedown);
  signal(SIGTERM,closedown);        
  signal(SIGPIPE,SIG_IGN);

  atexit(cleanup);

  input_loop(); // Doesn't return

  exit(0);
}

void closedown(int a){
  if(Verbose)
    fprintf(stderr,"iqrecord: caught signal %d: %s\n",a,strsignal(a));

  exit(1);  // Will call cleanup()
}

// Read from RTP network socket, assemble blocks of samples
void input_loop(){

  while(1){
    long long current_time = gps_time_ns();

    // Receive data
    fd_set fdset;
    FD_ZERO(&fdset);
    FD_SET(Input_fd,&fdset);
    struct timespec const polltime = {1, 0}; // force return after 1 second max
    int n = pselect(Input_fd + 1,&fdset,NULL,NULL,&polltime,NULL);
    if(n < 0)
      break; // error of some kind
    if(FD_ISSET(Input_fd,&fdset)){
      unsigned char buffer[MAXPKT];
      socklen_t socksize = sizeof(Sender);
      int size = recvfrom(Input_fd,buffer,sizeof(buffer),0,&Sender,&socksize);
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
      
      struct session *sp;
      for(sp = Sessions;sp != NULL;sp=sp->next){
	if(sp->ssrc == rtp.ssrc
	   && rtp.type  == sp->type
	   && address_match(&sp->iq_sender,&Sender))
	  break;
      }
      if(sp == NULL) // Not found; create new one
	sp = create_session(&rtp);
      if(sp == NULL || sp->fp == NULL)
	continue; // Couldn't create new session


      // A "sample" is a single audio sample, usually 16 bits.
      // A "frame" is the same as a sample for mono. It's two audio samples for stereo
      int samp_count = size / sizeof(*samples); // number of individual audio samples (not frames)
      int frame_count = samp_count / sp->channels; // 1 every sample period (e.g., 4 for stereo 16-bit)
      off_t offset = rtp_process(&sp->rtp_state,&rtp,frame_count); // rtp timestamps refer to frames
      
      // Flip endianness from network to host; wav wants little endian
      // So actually should convert from network order to little endian, not host
      for(int n = 0; n < samp_count; n++)
	samples[n] = ntohs(samples[n]);

      // The seek offset relative to the current position in the file is the signed (modular) difference between
      // the actual and expected RTP timestamps. This should automatically handle
      // 32-bit RTP timestamp wraps, which occur every ~1 days at 48 kHz and only 6 hr @ 192 kHz
      // Should I limit the range on this?
      if(offset){
	fseeko(sp->fp,offset * sizeof(*samples) * sp->channels,SEEK_CUR); // offset is in bytes
	if(offset > 0)
	  sp->CurrentSegmentSamples = 0;
      }
      sp->TotalFileSamples += samp_count + offset;
      sp->CurrentSegmentSamples += samp_count;
      sp->SamplesWritten += samp_count;
      if(sp->CurrentSegmentSamples >= SubstantialFileTime * sp->samprate)
	sp->SubstantialFile = 1;

      fwrite(samples,1,size,sp->fp);
      sp->last_active = gps_time_ns();
    } // end of packet processing

    // Walk through list, close idle sessions
    // should we do this on every packet? seems inefficient
    // Could be in a separate thread, but that creates synchronization issues
    struct session *next;
    for(struct session *sp = Sessions;sp != NULL; sp = next){
      next = sp->next;
      long long idle = current_time - sp->last_active;
      if(idle > Timeout * BILLION){
	// Close idle session
	if(!sp->SubstantialFile){
	  unlink(sp->filename);
	  if(Verbose)
	    printf("deleting %s %'.1f/%'.1f sec\n",sp->filename,
		   (float)sp->SamplesWritten / sp->samprate,
		   (float)sp->TotalFileSamples / sp->samprate);
	} else
	  if(Verbose)
	    printf("closing %s %'.1f/%'.1f sec\n",sp->filename,
		   (float)sp->SamplesWritten / sp->samprate,
		   (float)sp->TotalFileSamples / sp->samprate);
	
	if(sp->SubstantialFile){ // Don't bother for non-substantial files
	  // Get final file size, write .wav header with sizes
	  fflush(sp->fp);
	  struct stat statbuf;
	  fstat(fileno(sp->fp),&statbuf);
	  sp->header.ChunkSize = statbuf.st_size - 8;
	  sp->header.Subchunk2Size = statbuf.st_size - sizeof(sp->header);
	  rewind(sp->fp);
	  fwrite(&sp->header,sizeof(sp->header),1,sp->fp);
	  fflush(sp->fp);
	}
	fclose(sp->fp);
	sp->fp = NULL;
	free(sp->iobuffer);
	sp->iobuffer = NULL;
	if(sp->prev)
	  sp->prev->next = sp->next;
	else
	  Sessions = sp->next;
	if(sp->next)
	  sp->next->prev = sp->prev;
	free(sp);
	sp = NULL;
      }
    }
  }
}
 
void cleanup(void){
  while(Sessions){
    // Flush and close each write stream
    // Be anal-retentive about freeing and clearing stuff even though we're about to exit
    struct session *next_s = Sessions->next;
    fflush(Sessions->fp);
    fclose(Sessions->fp);
    Sessions->fp = NULL;
    free(Sessions->iobuffer);
    Sessions->iobuffer = NULL;
    free(Sessions);
    Sessions = next_s;
  }
}
struct session *create_session(struct rtp_header *rtp){

  struct session *sp = calloc(1,sizeof(*sp));
  if(sp == NULL)
    return NULL; // unlikely
  
  memcpy(&sp->iq_sender,&Sender,sizeof(sp->iq_sender));
  sp->type = rtp->type;
  sp->ssrc = rtp->ssrc;
  
  sp->channels = channels_from_pt(sp->type);
  sp->samprate = samprate_from_pt(sp->type);
  
  // Create file
  // Should we append to existing files instead? If we try this, watch out for timestamp wraparound
  struct timespec now;
  clock_gettime(CLOCK_REALTIME,&now);
  struct tm *tm = gmtime(&now.tv_sec);
  // yyyy-mm-dd-hh:mm:ss so it will sort properly
  
  sp->fp = NULL;
  if(Subdirs){
    char dir[PATH_MAX];
    snprintf(dir,sizeof(dir),"%u",sp->ssrc);
    if(mkdir(dir,0777) == -1 && errno != EEXIST)
      fprintf(stderr,"can't create directory %s: %s\n",dir,strerror(errno));
    // Try to create file in directory whether or not the mkdir succeeded
    snprintf(sp->filename,sizeof(sp->filename),"%u/%uk%4d-%02d-%02dT%02d:%02d:%02d.%dZ.wav",
	     sp->ssrc,
	     sp->ssrc,
	     tm->tm_year+1900,
	     tm->tm_mon+1,
	     tm->tm_mday,
	     tm->tm_hour,
	     tm->tm_min,
	     tm->tm_sec,
	     (int)(now.tv_nsec / 100000000));
    sp->fp = fopen(sp->filename,"w+");
    if(sp->fp == NULL)
      fprintf(stderr,"can't create/write file %s: %s\n",sp->filename,strerror(errno));
  }
  // (1) Subdirs not specified, or
  // (2) Subdirs specified but couldn't create directory or create file in directory; create in current dir
  if(!sp->fp){
    snprintf(sp->filename,sizeof(sp->filename),"%uk%4d-%02d-%02dT%02d:%02d:%02d.%dZ.wav",
	     sp->ssrc,
	     tm->tm_year+1900,
	     tm->tm_mon+1,
	     tm->tm_mday,
	     tm->tm_hour,
	     tm->tm_min,
	     tm->tm_sec,
	     (int)(now.tv_nsec / 100000000));
    sp->fp = fopen(sp->filename,"w+");
  }    
  if(sp->fp == NULL){
    fprintf(stderr,"can't create/write file %s: %s\n",sp->filename,strerror(errno));
    free(sp);
    sp = NULL;
    return NULL;
  }
  // file create succeded, now put us at top of list
  sp->prev = NULL;
  sp->next = Sessions;
  
  if(sp->next)
    sp->next->prev = sp;
  
  Sessions = sp;

  if(Verbose)
    fprintf(stderr,"creating %s\n",sp->filename);
  
  sp->iobuffer = malloc(BUFFERSIZE);
  setbuffer(sp->fp,sp->iobuffer,BUFFERSIZE);
  
  int const fd = fileno(sp->fp);
  fcntl(fd,F_SETFL,O_NONBLOCK); // Let's see if this keeps us from losing data
  
  attrprintf(fd,"samplerate","%lu",(unsigned long)sp->samprate);
  attrprintf(fd,"channels","%d",sp->channels);
  attrprintf(fd,"ssrc","%u",rtp->ssrc);
  attrprintf(fd,"sampleformat","s16le");
  
  // Write .wav header, skipping size fields
  memcpy(sp->header.ChunkID,"RIFF", 4);
  sp->header.ChunkSize = 0xffffffff; // Temporary
  memcpy(sp->header.Format,"WAVE",4);
  memcpy(sp->header.Subchunk1ID,"fmt ",4);
  sp->header.Subchunk1Size = 16;
  sp->header.AudioFormat = 1;
  sp->header.NumChannels = sp->channels;
  sp->header.SampleRate = sp->samprate;
  
  sp->header.ByteRate = sp->samprate * sp->channels * 16/8;
  sp->header.BlockAlign = sp->channels * 16/8;
  sp->header.BitsPerSample = 16;
  memcpy(sp->header.SubChunk2ID,"data",4);
  sp->header.Subchunk2Size = 0xffffffff; // Temporary
  fwrite(&sp->header,sizeof(sp->header),1,sp->fp);
  fflush(sp->fp); // get at least the header out there

  char sender_text[NI_MAXHOST];
  // Don't wait for an inverse resolve that might cause us to lose data
  getnameinfo((struct sockaddr *)&Sender,sizeof(Sender),sender_text,sizeof(sender_text),NULL,0,NI_NOFQDN|NI_DGRAM|NI_NUMERICHOST);
  attrprintf(fd,"source","%s",sender_text);
  attrprintf(fd,"multicast","%s",PCM_mcast_address_text);
  
  attrprintf(fd,"unixstarttime","%ld.%09ld",(long)now.tv_sec,(long)now.tv_nsec);
  return sp;
}
