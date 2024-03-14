// Hack of wspr-decoded for ft8 (15 second clips)
// Copyright 2023 Phil Karn, KA9Q
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
#include <stdint.h>
#include <libgen.h>
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
#include <sysexits.h>

#include "misc.h"
#include "attr.h"
#include "multicast.h"


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
  uint32_t next_timestamp;     // Next expected RTP timestamp
  
  int type;                    // RTP payload type (with marker stripped)
  int channels;                // 1 (PCM_MONO) or 2 (PCM_STEREO)
  unsigned int samprate;

  FILE *fp;                    // File being recorded
  void *iobuffer;              // Big buffer to reduce write rate

  int64_t SamplesWritten;
  int64_t TotalFileSamples;
};

char const *App_path;
int Verbose;
bool Keep_wav;
char PCM_mcast_address_text[256];
char const *Recordings = ".";

struct {
  double cycle_time;
  double transmission_time;
  char const *decode;
} Modetab[] = {
  { 120, 114, "wsprd"},
  { 15, 12.64, "decode_ft8"},
  { 7.5, 4.48, "decode_ft8"},  
  { 0, 0, NULL},
};
enum {
  WSPR,
  FT8,
  FT4,
} Mode;

struct sockaddr Sender;
struct sockaddr Input_mcast_sockaddr;
int Input_fd;
struct session *Sessions;

void input_loop(void);
void cleanup(void);
struct session *init_session(struct session *sp,struct rtp_header *rtp,int size);
void process_file(struct session *sp);
void create_new_file(struct session *sp,time_t);

void usage(){
  fprintf(stdout,"Usage: %s [-L locale] [-v] [-k] [-d recording_dir] [-4|-8|-w] PCM_multicast_address\n",App_path);
  exit(EX_USAGE);
}

int main(int argc,char *argv[]){
  App_path = argv[0];
  char const * locale = getenv("LANG");
  setlocale(LC_ALL,locale);
  setlinebuf(stdout);

  // Defaults
  int c;
  while((c = getopt(argc,argv,"w84d:L:vkV")) != EOF){
    switch(c){
    case 'w':
      Mode = WSPR;
      break;
    case '4':
      Mode = FT4;
      break;
    case '8':
      Mode = FT8;
      break;
    case 'd':
      Recordings = optarg;
      break;
    case 'L':
      locale = optarg;
      break;
    case 'v':
      Verbose++;
      break;
    case 'k':
      Keep_wav = true;
      break;
    case 'V':
      VERSION();
      exit(EX_OK);
    default:
      break;
    }
  }
  setlocale(LC_ALL,locale);
  // Stdout should already be in append mode, just make sure
  if(fcntl(1,F_SETFL,O_APPEND) == -1)
    fprintf(stdout,"fcntl of stdout to set O_APPEND failed: %s\n",strerror(errno));

  if(Verbose){
    for(int i=0; i < argc; i++)
      fprintf(stdout," [%d]%s",i,argv[i]);
    fprintf(stdout,"\n");
  }
  if(optind >= argc){
    fprintf(stdout,"Specify PCM Multicast IP address or domain name\n");
    usage();
  }

  char const * const target = argv[optind];
  strlcpy(PCM_mcast_address_text,target,sizeof(PCM_mcast_address_text));

  if(strlen(Recordings) > 0 && chdir(Recordings) != 0){
    fprintf(stdout,"Can't change to directory %s: %s, exiting\n",Recordings,strerror(errno));
    exit(EX_CANTCREAT);
  }

  // Set up input socket for multicast data stream from front end
  {
    char iface[1024];
    struct sockaddr sock;
    resolve_mcast(PCM_mcast_address_text,&sock,DEFAULT_RTP_PORT,iface,sizeof(iface));
    Input_fd = listen_mcast(&sock,iface);
  }

  if(Input_fd == -1){
    fprintf(stdout,"Can't set up PCM input from %s, exiting\n",PCM_mcast_address_text);
    exit(EX_IOERR);
  }
  int const n = 1 << 20; // 1 MB
  if(setsockopt(Input_fd,SOL_SOCKET,SO_RCVBUF,&n,sizeof(n)) == -1)
    perror("setsockopt");

  atexit(cleanup);

  input_loop();

  exit(EX_OK);
}

// Read from RTP network socket, assemble blocks of samples
// As currently written, requires data from a new frame to flush out and execute finished ones
// This doesn't seem like a big problem
void input_loop(){
  while(true){
    uint8_t buffer[PKTSIZE];
    socklen_t socksize = sizeof(Sender);
    int size = recvfrom(Input_fd,buffer,sizeof(buffer),0,&Sender,&socksize);
    // stash time now in case we are slowed by the code below
    int64_t const now = utc_time_ns();

    if(size <= 0){    // ??
      perror("recvfrom");
      usleep(50000);
      continue;
    }
    if(size < RTP_MIN_SIZE)
      continue; // Too small for RTP, ignore

    struct rtp_header rtp;
    uint8_t const *dp = ntoh_rtp(&rtp,buffer);
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
    if(sp == NULL){
      // New session; create empty descriptor
      sp = calloc(1,sizeof(*sp));
      if(!sp)
	// Let systemd restart us after a delay instead of rapidly filling the log with, e.g., disk full errors
	exit(EX_TEMPFAIL);

      sp->prev = NULL;
      sp->next = Sessions;
      if(sp->next)
	sp->next->prev = sp;
      Sessions = sp;
    }
    memcpy(&sp->sender,&Sender,sizeof(sp->sender));
    sp->type = rtp.type;
    sp->ssrc = rtp.ssrc;
  
    sp->channels = channels_from_pt(sp->type);
    sp->samprate = samprate_from_pt(sp->type);
    int64_t const modtime = now % (int64_t)(Modetab[Mode].cycle_time * BILLION); // where we are in the cycle

    if(sp->fp == NULL){
      if(modtime >= Modetab[Mode].transmission_time * BILLION){
	// In dead time and file already processed, drop data and wait for new data frame
	continue;
      }
      // Start of new data frame, create new file
      // Use UTC at start of this cycle as name for new file
      int64_t const start_time = now - modtime;
      time_t const start_time_sec = start_time / BILLION;
      create_new_file(sp,start_time_sec);
      assert(sp->fp != NULL);

      if(Verbose > 1)
	fprintf(stdout,"creating %s, cycle start offset %'.3f sec\n",
		sp->filename,(float)modtime/BILLION);
      
      // Remember the starting RTP timestamp
      sp->next_timestamp = rtp.timestamp;
      
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
    }
    assert(sp->fp != NULL);
    // File is open and ready for writing
    // Write data into current file
    // A "sample" is a single audio sample, usually 16 bits.
    // A "frame" is the same as a sample for mono. It's two audio samples for stereo
    int const samp_count = size / sizeof(*samples); // number of individual audio samples (not frames)

    // Seek to the proper place based on RTP timestamp. Normally offset = 0 when packets are in order
    off_t const offset = (int32_t)(rtp.timestamp - sp->next_timestamp) * sizeof(uint16_t) * sp->channels;
    fseeko(sp->fp,offset,SEEK_CUR);

    sp->TotalFileSamples += samp_count;
    sp->SamplesWritten += samp_count;

    // Packet samples are in big-endian order; write to .wav file in little-endian order
    for(int n = 0; n < samp_count; n++){
      fputc(samples[n] >> 8,sp->fp);
      fputc(samples[n],sp->fp);
    }
    sp->next_timestamp = rtp.timestamp + samp_count / sp->channels;
    if(modtime >= Modetab[Mode].transmission_time * BILLION){
      // We've reached the end of the current transmission.
      // Close current file, hand it to the decoder
      process_file(sp);
    }
    // Continuously reap children (decoders) at any time so they won't become zombies
    // They can take tens of seconds and finish in any order
    int status = 0;
    int pid;
    while((pid = waitpid(-1,&status,WNOHANG)) > 0)
      ;
    if(Verbose > 1 && pid > 0)
      fprintf(stdout,"child %d wait status %d\n",pid,status);
  }
}
// Set up new file on session with name derived from start_time_sec
void create_new_file(struct session *sp,time_t start_time_sec){
  struct tm const * const tm = gmtime(&start_time_sec);

  char dir[PATH_MAX];
  snprintf(dir,sizeof(dir),"%u",sp->ssrc);
  if(mkdir(dir,0777) == -1 && errno != EEXIST)
    fprintf(stdout,"can't create directory %s: %s\n",dir,strerror(errno));

  // Try to create file in directory whether or not the mkdir succeeded
  char filename[PATH_MAX];
  switch(Mode){
  case FT4:
  case FT8:
    snprintf(filename,sizeof(filename),"%s/%u/%02d%02d%02d_%02d%02d%02d.wav",
	     Recordings,
	     sp->ssrc,
	     (tm->tm_year+1900) % 100,
	     tm->tm_mon+1,
	     tm->tm_mday,
	     tm->tm_hour,
	     tm->tm_min,
	     tm->tm_sec);
    break;
  case WSPR:
    snprintf(filename,sizeof(filename),"%s/%u/%02d%02d%02d_%02d%02d.wav",
	     Recordings,
	     sp->ssrc,
	     (tm->tm_year+1900) % 100,
	     tm->tm_mon+1,
	     tm->tm_mday,
	     tm->tm_hour,
	     tm->tm_min);
    break;
  }    
  int fd = -1;
  if((fd = open(filename,O_RDWR|O_CREAT,0777)) != -1){
    strlcpy(sp->filename,filename,sizeof(sp->filename));
  } else {
    // couldn't create directory or create file in directory; create in current dir
    fprintf(stdout,"can't create/write file %s: %s\n",filename,strerror(errno));
    char const *bn = basename(filename);
    
    if((fd = open(bn,O_RDWR|O_CREAT,0777)) == -1){
      fprintf(stdout,"can't create/write file %s: %s, can't create session\n",bn,strerror(errno));
      exit(EX_CANTCREAT);
    }
    strlcpy(sp->filename,bn,sizeof(sp->filename));
  }
  // Use fdopen on a file descriptor instead of fopen(,"w+") to avoid the implicit truncation
  // This allows testing where we're killed and rapidly restarted in the same cycle
  assert(fd != -1);
  sp->fp = fdopen(fd,"w+");
  assert(sp->fp != NULL);
  sp->iobuffer = malloc(BUFFERSIZE);
  setbuffer(sp->fp,sp->iobuffer,BUFFERSIZE);
  fcntl(fd,F_SETFL,O_NONBLOCK); // Let's see if this keeps us from losing data
}
void cleanup(void){
  while(Sessions){
    // Flush and close each write stream
    // Be anal-retentive about freeing and clearing stuff even though we're about to exit
    struct session * const next_s = Sessions->next;
    if(Sessions->fp){
      fflush(Sessions->fp);
      fclose(Sessions->fp);
      Sessions->fp = NULL;
    }
    FREE(Sessions->iobuffer);
    FREE(Sessions);
    Sessions = next_s;
  }
}

// Close and process file
void process_file(struct session *sp){
  assert(sp != NULL && sp->fp != NULL);
  if(sp == NULL || sp->fp == NULL)
    return;

  if(Verbose)
    fprintf(stdout,"closing %s %'.1f/%'.1f sec\n",sp->filename,
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

  FREE(sp->iobuffer);
  sp->TotalFileSamples = 0;
  sp->SamplesWritten = 0;

  int child = fork();
  if(child == 0){
    // Double fork so we don't have to wait. Seems ugly, is there a better way??
    int grandchild = fork();
    if(grandchild == 0){
      {
	// set working directory to the one containing the file
	// dirname_r() is only available on MacOS, so we can't use it here
	char *fname_dup = strdup(sp->filename); // in case dirname modifies its arg
	int r = chdir(dirname(fname_dup));
	FREE(fname_dup);
	
	if(r != 0)
	  perror("chdir");
      }
      char freq[100];
      snprintf(freq,sizeof(freq),"%lf",(double)sp->ssrc * 1e-6);
      
      switch(Mode){
      case WSPR:
	if(Verbose)
	  fprintf(stdout,"%s %s %s %s %s\n",Modetab[Mode].decode,"-f",freq,"-w",sp->filename);
	
	execlp(Modetab[Mode].decode,Modetab[Mode].decode,"-f",freq,"-w",sp->filename,(char *)NULL);
	break;
      case FT8:
	// Note: requires my version of decode_ft8 that accepts -f basefreq
	if(Verbose)
	  fprintf(stdout,"%s -f %s %s\n",Modetab[Mode].decode,freq,sp->filename);
	
	execlp(Modetab[Mode].decode,Modetab[Mode].decode,"-f",freq,sp->filename,(char *)NULL);
	break;
      case FT4:
	// Note: requires my version of decode_ft8 that accepts -f basefreq
	if(Verbose)
	  fprintf(stdout,"%s -f %s -4 %s\n",Modetab[Mode].decode,freq,sp->filename);
	
	execlp(Modetab[Mode].decode,Modetab[Mode].decode,"-f",freq,"-4",sp->filename,(char *)NULL);
	break;
      }
      // Gets here only if exec fails
      fprintf(stdout,"execlp(%s) returned errno %d (%s)\n",Modetab[Mode].decode,errno,strerror(errno));
      exit(EX_SOFTWARE); // Exit from grandchild context
    }
    // In child context
    if(Verbose > 1)
      fprintf(stdout,"forked grandchild %d\n",grandchild);
    // Wait for decoder to finish, then remove its input file
    int status = 0;
    if(waitpid(-1,&status,0) == -1){
      fprintf(stdout,"error waiting for grandchild: errno %d (%s)\n",
	      errno,strerror(errno));
      exit(EXIT_FAILURE);
    }
    if(Verbose > 1)
      fprintf(stdout,"grandchild %d waitpid status %d\n",grandchild,status);
    
    if(!WIFEXITED(status)){
      if(Verbose > 1){
	if(WIFSIGNALED(status))
	  fprintf(stdout,"grandchild %d terminated by signal %d\n",grandchild,WTERMSIG(status));
      }
    }
    if(!Keep_wav){
      if(Verbose)
	fprintf(stdout,"unlink(%s)\n",sp->filename);
      unlink(sp->filename);
      sp->filename[0] = '\0';
    }
    exit(EX_OK); // child exits
  }
  if(Verbose > 1)
    fprintf(stdout,"spawned child %d\n",child);
}
