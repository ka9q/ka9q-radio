// $Id: wspr-decoded.c,v 1.3 2022/08/05 06:35:10 karn Exp $ 
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
#include <sys/types.h>
#include <sys/wait.h>
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
#define BUFFERSIZE (1<<16)

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
  struct sockaddr sender;   // Sender's IP address and source port

  char filename[PATH_MAX];
  struct wav header;

  uint32_t ssrc;               // RTP stream source ID
  struct rtp_state rtp_state;
  
  int type;                    // RTP payload type (with marker stripped)
  int channels;                // 1 (PCM_MONO) or 2 (PCM_STEREO)
  unsigned int samprate;

  FILE *fp;                    // File being recorded
  void *iobuffer;              // Big buffer to reduce write rate

  int64_t SamplesWritten;
  int64_t TotalFileSamples;
};


const char *App_path;
int Verbose;
int Keep_wav;
char PCM_mcast_address_text[256];
char *Recordings = ".";
char *Wsprd_command = "wsprd -a %s/%u -o 2 -f %.6lf -w -d %s";

struct sockaddr Sender;
struct sockaddr Input_mcast_sockaddr;
int Input_fd;
struct session *Sessions;

void closedown(int a);
void input_loop(void);
void cleanup(void);
struct session *create_session(struct rtp_header *);
void close_session(struct session **p);

int main(int argc,char *argv[]){
  App_path = argv[0];
  char const * locale = getenv("LANG");
  setlocale(LC_ALL,locale);

  // Defaults
  int c;
  while((c = getopt(argc,argv,"c:d:l:vk")) != EOF){
    switch(c){
    case 'c':
      Wsprd_command = optarg;
      break;
    case 'd':
      Recordings = optarg;
      break;
    case 'l':
      locale = optarg;
      break;
    case 'v':
      Verbose = 1;
      break;
    case 'k':
      Keep_wav = 1;
      break;
    default:
      fprintf(stderr,"Usage: %s [-l locale] [-v] [-k] [-d recdir] PCM_multicast_address\n",argv[0]);
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
    char iface[1024];
    struct sockaddr sock;
    resolve_mcast(PCM_mcast_address_text,&sock,DEFAULT_RTP_PORT,iface,sizeof(iface));
    Input_fd = listen_mcast(&sock,iface);
  }

  if(Input_fd == -1){
    fprintf(stderr,"Can't set up PCM input from %s, exiting\n",PCM_mcast_address_text);
    exit(1);
  }
  int const n = 1 << 20; // 1 MB
  if(setsockopt(Input_fd,SOL_SOCKET,SO_RCVBUF,&n,sizeof(n)) == -1)
    perror("setsockopt");

  // Graceful signal catch
#if 1 // Ignoring child death signals causes system() inside fork() to return errno 10
  signal(SIGCHLD,SIG_IGN); // Don't let children become zombies
#endif
  
  signal(SIGPIPE,closedown);
  signal(SIGINT,closedown);
  signal(SIGKILL,closedown);
  signal(SIGQUIT,closedown);
  signal(SIGTERM,closedown);        
  signal(SIGPIPE,SIG_IGN);

  atexit(cleanup);

  input_loop();

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
    // Receive network data
    fd_set fdset;
    FD_ZERO(&fdset);
    FD_SET(Input_fd,&fdset);
    {
      struct timespec const polltime = {1, 0}; // return after 1 sec
      int n = pselect(Input_fd + 1,&fdset,NULL,NULL,&polltime,NULL);
      if(n < 0)
	break; // error of some kind
    }

    int const sec = utc_time_sec() % 120; // UTC second within 0-120 period

    if(sec >= 114){
      // End of 2-minute frame; process everything
      for(struct session *sp = Sessions;sp != NULL;){

	// Finished current period, decode
	char cmd[16384];
	snprintf(cmd,sizeof(cmd),Wsprd_command,
		 Recordings,sp->ssrc,(double)sp->ssrc * 1e-6,sp->filename);

	if(Verbose)
	  fprintf(stderr,"%s\n",cmd);
	
	if(fork() == 0){
	  // If running at high priority, drop back to normal
	  if(getpriority(PRIO_PROCESS,0) < 0)
	    setpriority(PRIO_PROCESS,0,0);
	  int const r = system(cmd);
	  if(Verbose && r != 0)
	    fprintf(stderr,"system() returned %d errno %d (%s)\n",r,errno,strerror(errno));
	  if(!Keep_wav)
	    unlink(sp->filename);
	  exit(0);
	}
	struct session * const next = sp->next;
	close_session(&sp);
	sp = next;
      }
    }
    if(FD_ISSET(Input_fd,&fdset)){
      unsigned char buffer[MAXPKT];
      socklen_t socksize = sizeof(Sender);
      int size = recvfrom(Input_fd,buffer,sizeof(buffer),0,&Sender,&socksize);

      if(sec >= 114)
	continue; // Discard all data until the next cycle

      if(size <= 0){    // ??
	perror("recvfrom");
	usleep(50000);
	continue;
      }
      if(size < RTP_MIN_SIZE)
	continue; // Too small for RTP, ignore
      
      unsigned char const * dp = buffer;
      struct rtp_header rtp;
      dp = ntoh_rtp(&rtp,dp);
      if(rtp.pad){
	// Remove padding
	size -= dp[size-1];
	rtp.pad = 0;
      }
      if(size <= 0)
	continue; // Bogus RTP header
      
      int16_t const * const samples = (int16_t *)dp;
      size -= (dp - buffer);

      struct session *sp;
      for(sp = Sessions;sp != NULL;sp=sp->next){
	if(sp->ssrc == rtp.ssrc
	   && rtp.type == sp->type
	   && address_match(&sp->sender,&Sender))
	  break;
      }
      if(sp == NULL)
	sp = create_session(&rtp);	// create new session only if we're not in the dead time

      if(!sp)
	continue;

      // A "sample" is a single audio sample, usually 16 bits.
      // A "frame" is the same as a sample for mono. It's two audio samples for stereo
      int const samp_count = size / sizeof(*samples); // number of individual audio samples (not frames)
      int const frame_count = samp_count / sp->channels; // 1 every sample period (e.g., 4 for stereo 16-bit)
      off_t const offset = rtp_process(&sp->rtp_state,&rtp,frame_count); // rtp timestamps refer to frames
      
      // The seek offset relative to the current position in the file is the signed (modular) difference between
      // the actual and expected RTP timestamps. This should automatically handle
      // 32-bit RTP timestamp wraps, which occur every ~1 days at 48 kHz and only 6 hr @ 192 kHz
      // Should I limit the range on this?
      if(offset)
	fseeko(sp->fp,offset * sizeof(*samples) * sp->channels,SEEK_CUR); // offset is in bytes

      sp->TotalFileSamples += samp_count + offset;
      sp->SamplesWritten += samp_count;
      
      // Packet samples are in big-endian order; write to .wav file in little-endian order
      for(int n = 0; n < samp_count; n++){
	fputc(samples[n] >> 8,sp->fp);
	fputc(samples[n],sp->fp);
      }
    } // end of packet processing
  }      
}
void cleanup(void){
  while(Sessions){
    // Flush and close each write stream
    // Be anal-retentive about freeing and clearing stuff even though we're about to exit
    struct session * const next_s = Sessions->next;
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

  struct session * const sp = calloc(1,sizeof(*sp));
  if(sp == NULL)
    return NULL; // unlikely
  
  memcpy(&sp->sender,&Sender,sizeof(sp->sender));
  sp->type = rtp->type;
  sp->ssrc = rtp->ssrc;
  
  sp->channels = channels_from_pt(sp->type);
  sp->samprate = samprate_from_pt(sp->type);
  
  long long now = utc_time_ns();
  // Microsecond within 2-minute (120 sec) period
  long long const start_offset_nsec = now % (120 * BILLION);
  
  // Use the previous 120-second point as the start of this file
  long long start_time = now - start_offset_nsec;
  time_t start_time_sec = start_time / BILLION;
  
  struct tm const * const tm = gmtime(&start_time_sec);
  
  int fd = -1;
  char dir[PATH_MAX];
  snprintf(dir,sizeof(dir),"%u",sp->ssrc);
  if(mkdir(dir,0777) == -1 && errno != EEXIST)
    fprintf(stderr,"can't create directory %s: %s\n",dir,strerror(errno));
  // Try to create file in directory whether or not the mkdir succeeded
  snprintf(sp->filename,sizeof(sp->filename),"%s/%u/%02d%02d%02d_%02d%02d.wav",
	   Recordings,
	   sp->ssrc,
	   (tm->tm_year+1900) % 100,
	   tm->tm_mon+1,
	   tm->tm_mday,
	   tm->tm_hour,
	   tm->tm_min);
  fd = open(sp->filename,O_RDWR|O_CREAT,0777);
  if(fd == -1){
    // couldn't create directory or create file in directory; create in current dir
    fprintf(stderr,"can't create/write file %s: %s\n",sp->filename,strerror(errno));
    snprintf(sp->filename,sizeof(sp->filename),"%02d%02d%02d_%02d%02d.wav",
	     (tm->tm_year+1900) % 100,
	     tm->tm_mon+1,
	     tm->tm_mday,
	     tm->tm_hour,
	     tm->tm_min);
    fd = open(sp->filename,O_RDWR|O_CREAT,0777);
  }    
  if(fd == -1){
    fprintf(stderr,"can't create/write file %s: %s\n",sp->filename,strerror(errno));
    free(sp);
    return NULL;
  }
  // Use fdopen on a file descriptor instead of fopen(,"w+") to avoid the implicit truncation
  // This allows testing where we're killed and rapidly restarted in the same cycle
  sp->fp = fdopen(fd,"w+");
  if(Verbose)
    fprintf(stderr,"creating %s\n",sp->filename);

  // file create succeded, now put us at top of list
  sp->prev = NULL;
  sp->next = Sessions;
  
  if(sp->next)
    sp->next->prev = sp;
  
  Sessions = sp;
  
  sp->iobuffer = malloc(BUFFERSIZE);
  setbuffer(sp->fp,sp->iobuffer,BUFFERSIZE);
  
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
  attrprintf(fd,"unixstarttime","%.9lf",(double)start_time / 1.e9);

  // Seek into the file for the first write
  // The parentheses are carefully drawn to ensure the result is on a block boundary despite truncations
  fseeko(sp->fp,(off_t)((start_offset_nsec * sp->samprate)/ BILLION) * sp->header.BlockAlign,SEEK_CUR); // offset is in bytes
  return sp;
}

void close_session(struct session **p){
  if(p == NULL)
    return;
  struct session * const sp = *p;
  if(sp == NULL)
    return;

  if(Verbose)
    printf("closing %s %'.1f/%'.1f sec\n",sp->filename,
	   (float)sp->SamplesWritten / sp->samprate,
	   (float)sp->TotalFileSamples / sp->samprate);
  
  // Get final file size, write .wav header with sizes
  fflush(sp->fp);
  struct stat statbuf;
  fstat(fileno(sp->fp),&statbuf);
  sp->header.ChunkSize = statbuf.st_size - 8;
  sp->header.Subchunk2Size = statbuf.st_size - sizeof(sp->header);
  rewind(sp->fp);
  fwrite(&sp->header,sizeof(sp->header),1,sp->fp);
  fflush(sp->fp);
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
  *p = NULL;
}
