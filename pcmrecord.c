// Read and record PCM audio streams
// Copyright 2021 Phil Karn, KA9Q
#define _GNU_SOURCE 1
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <string.h>
#if defined(linux)
#include <bsd/string.h>
#include <byteswap.h>
#else // bsd
#define bswap_16(value) ((((value) & 0xff) << 8) | ((value) >> 8)) // hopefully gets optimized
#endif
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <locale.h>
#include <sys/stat.h>
#include <poll.h>
#include <sysexits.h>
#include <signal.h>
#include <getopt.h>

#include "misc.h"
#include "attr.h"
#include "multicast.h"

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

   // 'auxi' chunk to pass center frequency to SDR Console
   // http://www.moetronix.com/files/spectravue.pdf had some details on this chunk
   // and https://sdrplay.com/resources/IQ/ft4.zip
   // has some .wav files with a center frequency that SDR Console can use
   char AuxID[4];
   int32_t AuxSize;
   int16_t StartYear;
   int16_t StartMon;
   int16_t StartDOW;
   int16_t StartDay;
   int16_t StartHour;
   int16_t StartMinute;
   int16_t StartSecond;
   int16_t StartMillis;
   int16_t StopYear;
   int16_t StopMon;
   int16_t StopDOW;
   int16_t StopDay;
   int16_t StopHour;
   int16_t StopMinute;
   int16_t StopSecond;
   int16_t StopMillis;
   int32_t CenterFrequency;
   char AuxUknown[128];

  char SubChunk2ID[4];
  int32_t Subchunk2Size;
};

// One for each session being recorded
struct session {
  struct session *prev;
  struct session *next;
  struct sockaddr sender;      // Sender's IP address and source port

  char filename[PATH_MAX];
  struct wav header;

  uint32_t ssrc;               // RTP stream source ID
  struct rtp_state rtp_state;

  int type;                    // RTP payload type (with marker stripped)
  int channels;                // 1 (PCM_MONO) or 2 (PCM_STEREO)
  unsigned int samprate;       // implicitly 48 kHz in PCM

  FILE *fp;                    // File being recorded
  void *iobuffer;              // Big buffer to reduce write rate
  int64_t last_active;         // gps time of last activity

  bool substantial_file;       // At least one substantial segment has been seen
  int64_t current_segment_samples; // total samples in this segment without skips in timestamp
  int64_t samples_written;
  int64_t total_file_samples;
  int64_t samples_remaining;   // Samples remaining before file is closed; 0 means indefinite
};


static float SubstantialFileTime = 0.2;  // Don't record bursts < 250 ms unless they're between two substantial segments
static float FileLengthLimit = 0; // Length of file in seconds; 0 = unlimited
const char *App_path;
int Verbose;
static char PCM_mcast_address_text[256];
static char const *Recordings = ".";
static bool Subdirs; // Place recordings in subdirectories by SSID
static char const *Locale;
static int Samprate;
static int Channels = 1;

static uint32_t CenterFrequency=1115000;

static int Input_fd;
static struct session *Sessions;
static int64_t Timeout = 20; // 20 seconds max idle time before file close

static void closedown(int a);
static void input_loop(void);
static void cleanup(void);
static struct session *create_session(struct rtp_header const *, struct sockaddr const *sender);
static int close_file(struct session **spp);

static struct option Options[] = {
  {"channels", required_argument, NULL, 'c'},
  {"directory", required_argument, NULL, 'd'},
  {"locale", required_argument, NULL, 'l'},
  {"minfiletime", required_argument, NULL, 'm'},
  {"mintime", required_argument, NULL, 'm'},
  {"samprate", required_argument, NULL, 'r'},
  {"subdirectories", no_argument, NULL, 's'},
  {"subdirs", no_argument, NULL, 's'},
  {"timeout", required_argument, NULL, 't'},
  {"verbose", no_argument, NULL, 'v'},
  {"lengthlimit", required_argument, NULL, 'L'},
  {"limit", required_argument, NULL, 'L'},
  {"frequency", required_argument, NULL, 'f'},
  {"version", no_argument, NULL, 'V'},
  {NULL, no_argument, NULL, 0},
};
static char Optstring[] = "c:d:l:m:r:st:vL:f:V";

int main(int argc,char *argv[]){
  App_path = argv[0];

  // Defaults
  Locale = getenv("LANG");

  int c;
  char *ptr;
  while((c = getopt_long(argc,argv,Optstring,Options,NULL)) != EOF){
    switch(c){
    case 'c':
      Channels = strtol(optarg,&ptr,0);
      break;
    case 'd':
      Recordings = optarg;
      break;
    case 'l':
      Locale = optarg;
      break;
    case 'm':
      SubstantialFileTime = strtof(optarg,NULL);
      break;
    case 'r':
      Samprate = strtol(optarg,&ptr,0);
      break;
    case 's':
      Subdirs = true;
      break;
    case 't':
      {
	char *ptr;
	int64_t x = strtoll(optarg,&ptr,0);
	if(ptr != optarg)
	  Timeout = x;
      }
      break;
    case 'v':
      Verbose = 1;
      break;
    case 'L':
      FileLengthLimit = strtof(optarg,NULL);
      break;
    case 'f':
       CenterFrequency = strtoul(optarg,NULL,0);
      break;
    case 'V':
      VERSION();
      exit(EX_OK);
    default:
      fprintf(stderr,"Usage: %s [-c 1|2]] [-s] [-d directory] [-l locale] [-L maxtime] [-t timeout] [-v] [-m sec] [-f freq] PCM_multicast_address\n",argv[0]);
      exit(EX_USAGE);
      break;
    }
  }
  setlocale(LC_ALL,Locale);
  if(optind >= argc){
    fprintf(stderr,"Specify PCM_mcast_address_text_address\n");
    exit(EX_USAGE);
  }
  if(Channels != 1 && Channels != 2){
    fprintf(stderr,"Channels %d invalid\n",Channels);
    Channels = 0;
  }

  strlcpy(PCM_mcast_address_text,argv[optind],sizeof(PCM_mcast_address_text));
  setlocale(LC_ALL,Locale);
  setlinebuf(stdout); // In case we're redirected to a file

  // Set up input socket for multicast data stream from front end
  {
    struct sockaddr_storage sock;
    char iface[1024];
    resolve_mcast(PCM_mcast_address_text,&sock,DEFAULT_RTP_PORT,iface,sizeof(iface));
    Input_fd = listen_mcast(&sock,iface);
  }
  if(Input_fd == -1){
    fprintf(stderr,"Can't set up PCM input, exiting\n");
    exit(EX_IOERR);
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

  exit(EX_OK);
}

static void closedown(int a){
  if(Verbose)
    fprintf(stderr,"%s: caught signal %d: %s\n",App_path,a,strsignal(a));

  cleanup();
  exit(EX_SOFTWARE);  // Will call cleanup()
}

// Read from RTP network socket, assemble blocks of samples
static void input_loop(){

  struct sockaddr sender;
  while(true){
    int64_t current_time = gps_time_ns();

    // Receive data
    struct pollfd pfd[1];
    pfd[0].fd = Input_fd;
    pfd[0].events = POLLIN;
    pfd[0].revents = 0;
    int const n = poll(pfd,sizeof(pfd)/sizeof(pfd[0]),1000); // Wait 1 sec max so we can scan active session list
    if(n < 0)
      break; // error of some kind
    if(pfd[0].revents & (POLLIN|POLLPRI)){
      uint8_t buffer[PKTSIZE];
      socklen_t socksize = sizeof(sender);
      int size = recvfrom(Input_fd,buffer,sizeof(buffer),0,&sender,&socksize);
      if(size <= 0){    // ??
	perror("recvfrom");
	break; // Some sort of error, quit
      }
      if(size < RTP_MIN_SIZE)
	continue; // Too small for RTP, ignore

      struct rtp_header rtp;
      uint8_t const * const dp = ntoh_rtp(&rtp,buffer);
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
	   && address_match(&sp->sender,&sender)
	   && getportnumber(&sp->sender) == getportnumber(&sender))
	  break;
      }
      if(sp == NULL){ // Not found; create new one
	// Repeat this each time we create a session to ensure we're in the right directory.
	// This might have failed on earlier attempts should we start before the fs is successfully mounted
	if(strlen(Recordings) > 0 && chdir(Recordings) != 0){
	  fprintf(stderr,"Can't change to directory %s: %s, exiting\n",Recordings,strerror(errno));
	  exit(EX_CANTCREAT);
	}
	sp = create_session(&rtp,&sender);
      }
      if(sp == NULL || sp->fp == NULL)
#if 1
	// Let systemd restart us after a delay instead of rapidly filling the log with, e.g., disk full errors
	exit(EX_CANTCREAT);
#else
	continue; // Couldn't create new session
#endif

      // A "sample" is a single audio sample, usually 16 bits.
      // A "frame" is the same as a sample for mono. It's two audio samples for stereo
      int const samp_count = size / sizeof(*samples); // number of individual audio samples (not frames)
      int const frame_count = samp_count / sp->channels; // 1 every sample period (e.g., 4 for stereo 16-bit)
      off_t const offset = rtp_process(&sp->rtp_state,&rtp,frame_count); // rtp timestamps refer to frames

      // The seek offset relative to the current position in the file is the signed (modular) difference between
      // the actual and expected RTP timestamps. This should automatically handle
      // 32-bit RTP timestamp wraps, which occur every ~1 days at 48 kHz and only 6 hr @ 192 kHz
      // Should I limit the range on this?
      if(offset != 0){
	fseeko(sp->fp,offset * sizeof(*samples) * sp->channels,SEEK_CUR); // offset is in bytes
	if(offset > 0)
	  sp->current_segment_samples = 0;
      }
      sp->total_file_samples += samp_count + offset;
      sp->current_segment_samples += samp_count;
      sp->samples_written += samp_count;
      if(sp->current_segment_samples >= SubstantialFileTime * sp->samprate)
	sp->substantial_file = true;

      // Flip endianness from big-endian on network to little endian wanted by .wav
      // byteswap.h is linux-specific; need to find a portable way to get the machine instructions
      uint16_t wbuffer[samp_count];
      for(int n = 0; n < samp_count; n++)
	wbuffer[n] = bswap_16((uint16_t)samples[n]);
      fwrite(wbuffer,sizeof(*wbuffer),samp_count,sp->fp);
      sp->last_active = gps_time_ns();

      if(sp->samples_remaining > 0 && (sp->samples_remaining -= samp_count) <= 0){
	cleanup(); // Close all files
	exit(EX_OK);
      }
    } // end of packet processing

    // Walk through list, close idle sessions
    // should we do this on every packet? seems inefficient
    // Could be in a separate thread, but that creates synchronization issues
    struct session *next;
    for(struct session *sp = Sessions;sp != NULL; sp = next){
      next = sp->next; // save in case sp is closed
      int64_t idle = current_time - sp->last_active;
      if(idle > Timeout * BILLION){
	close_file(&sp); // sp will be NULL
	// Close idle session
      }
    }
  }
}

static void cleanup(void){
  while(Sessions){
    // Flush and close each write stream
    // Be anal-retentive about freeing and clearing stuff even though we're about to exit
    struct session *next_s = Sessions->next;
    close_file(&Sessions); // Sessions will be NULL
    Sessions = next_s;
  }
}
static struct session *create_session(struct rtp_header const *rtp,struct sockaddr const *sender){

  struct session *sp = calloc(1,sizeof(*sp));
  if(sp == NULL)
    return NULL; // unlikely

  memcpy(&sp->sender,sender,sizeof(sp->sender));
  sp->type = rtp->type;
  sp->ssrc = rtp->ssrc;

  sp->channels = Channels ? Channels : channels_from_pt(sp->type);
  sp->samprate = Samprate ? Samprate : samprate_from_pt(sp->type);
  if(sp->channels == 0 || sp->samprate == 0){
    fprintf(stderr,"Unknown payload type %d and channels/samprate not specified on command line\n",sp->type);
    return NULL;
  }
  sp->samples_remaining = sp->samprate * FileLengthLimit * Channels; // If file is being limited in length
  // Create file
  // Should we append to existing files instead? If we try this, watch out for timestamp wraparound
  struct timespec now;
  clock_gettime(CLOCK_REALTIME,&now);
  struct tm const * const tm = gmtime(&now.tv_sec);
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
	     (int)(now.tv_nsec / 100000000)); // 100 million, i.e., convert to tenths of a sec
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
    FREE(sp);
    return NULL;
  }
  // file create succeded, now put us at top of list
  sp->prev = NULL;
  sp->next = Sessions;

  if(sp->next)
    sp->next->prev = sp;

  Sessions = sp;

  if(Verbose)
    fprintf(stdout,"creating %s\n",sp->filename);

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

  // fill in the auxi chunk (start time, center frequency)
  memcpy(sp->header.AuxID, "auxi", 4);
  sp->header.AuxSize=164;
  sp->header.StartYear=tm->tm_year+1900;
  sp->header.StartMon=tm->tm_mon+1;
  sp->header.StartDOW=tm->tm_wday;
  sp->header.StartDay=tm->tm_mday;
  sp->header.StartHour=tm->tm_hour;
  sp->header.StartMinute=tm->tm_min;
  sp->header.StartSecond=tm->tm_sec;
  sp->header.StartMillis=(int16_t)(now.tv_nsec / 1000000);
  sp->header.CenterFrequency=CenterFrequency;
  memset(sp->header.AuxUknown, 0, 128);

  fwrite(&sp->header,sizeof(sp->header),1,sp->fp);
  fflush(sp->fp); // get at least the header out there

  char sender_text[NI_MAXHOST];
  // Don't wait for an inverse resolve that might cause us to lose data
  getnameinfo((struct sockaddr *)sender,sizeof(*sender),sender_text,sizeof(sender_text),NULL,0,NI_NOFQDN|NI_DGRAM|NI_NUMERICHOST);
  attrprintf(fd,"source","%s",sender_text);
  attrprintf(fd,"multicast","%s",PCM_mcast_address_text);

  attrprintf(fd,"unixstarttime","%ld.%09ld",(long)now.tv_sec,(long)now.tv_nsec);
  return sp;
}

// Close a session, update .wav header, remove from session table
// If the file is not "substantial", just delete it
static int close_file(struct session **spp){
  struct session *sp = *spp;

  if(sp->substantial_file){ // Don't bother for non-substantial files
    if(Verbose){
      fprintf(stdout,"closing %s %'.1f/%'.1f sec\n",sp->filename,
            (float)sp->samples_written / (sp->samprate * Channels),
            (float)sp->total_file_samples / (sp->samprate *Channels));
    }
    // Get final file size, write .wav header with sizes
    fflush(sp->fp);
    struct stat statbuf;
    if(fstat(fileno(sp->fp),&statbuf) != 0){
      printf("fstat(%d) [%s] failed! %s\n",fileno(sp->fp),sp->filename,strerror(errno));
      abort();
    }
    sp->header.ChunkSize = statbuf.st_size - 8;
    sp->header.Subchunk2Size = statbuf.st_size - sizeof(sp->header);

    // write end time into the auxi chunk
    struct timespec now;
    clock_gettime(CLOCK_REALTIME,&now);
    struct tm const * const tm = gmtime(&now.tv_sec);
    sp->header.StopYear=tm->tm_year+1900;
    sp->header.StopMon=tm->tm_mon+1;
    sp->header.StopDOW=tm->tm_wday;
    sp->header.StopDay=tm->tm_mday;
    sp->header.StopHour=tm->tm_hour;
    sp->header.StopMinute=tm->tm_min;
    sp->header.StopSecond=tm->tm_sec;
    sp->header.StopMillis=(int16_t)(now.tv_nsec / 1000000);

    rewind(sp->fp);
    fwrite(&sp->header,sizeof(sp->header),1,sp->fp);
    fflush(sp->fp);
    if(Verbose && (sp->rtp_state.dupes != 0 || sp->rtp_state.drops != 0))
      printf("file %s dupes %llu drops %llu\n",sp->filename,(long long unsigned)sp->rtp_state.dupes,(long long unsigned)sp->rtp_state.drops);
  } else {
    unlink(sp->filename);
    if(Verbose)
      printf("deleting %s %'.1f/%'.1f sec\n",sp->filename,
            (float)sp->samples_written / (sp->samprate * Channels),
            (float)sp->total_file_samples / (sp->samprate * Channels));
  }
  fclose(sp->fp);
  sp->fp = NULL;
  FREE(sp->iobuffer);
  if(sp->prev)
    sp->prev->next = sp->next;
  else
    Sessions = sp->next;
  if(sp->next)
    sp->next->prev = sp->prev;
  FREE(sp);
  
  return 0;
}
