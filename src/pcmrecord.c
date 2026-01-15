/**
@file
@author Phil Karn, KA9Q
@brief Record, stream, or launch commands with RTP streams as input
@verbatim
This program reads one or more RTP streams from a multicast group and either writes them into a file, streams (one of them) onto standard output, or invokes a command for each stream and pipes the RTP data into it. PCM streams are written as-is (except that big-endian PCM is converted to little-endian). Opus streams are placed in a standard Ogg container.

Command-line options:
 --stdout | --catmode | -c: write one stream to stdout. If --ssrc is not specified, selects the first one found and ignores the rest
 --source <source-specific name or address>
 --directory | -d <directory>: directory root in which to write files<
 --exec | -e '<command args ...>': Execute the specified command for each stream and pipe to it. Several macros expanded as shown when found in the arguments:
        $$: insert a literal '$'
        $c: number of channels (1 or 2)
        $d: description string from the radiod front end
        $f: encoding ("s16le", "s16be", "f32le", "opus", "none")
        $h: receive frequency in decimal hertz
        $k: receive frequency in decimal kilohertz
        $m: receive frequency in decimal megahertz
        $r: sample rate, integer Hz
        $s: ssrc (unsigned decimal integer)

 --flush|-f: Flush after each received packet. Increases Ogg container overhead; little need for this writing files
 -8|-4|-w: convenience flags for FT8, FT4 and WSPR modes. Sets --length,--jt and --pad
 --pad|-P: Align first file in time to multiple of length, pad start with silence (implied by --jt)
 --jt|-j: Use K1JT format file names
 --locale <locale>: Set locale. Default is $LANG
 --mintime|--minfiletime|-m: minimum file duration, in sec. Files shorter than this are deleted when closed
 --raw|-r: Don't emit .WAV header for PCM files; ignored with Opus (Ogg is needed to delimit frames in a stream)
 --subdirectories|--subdirs|-s': Create subdirectories when writing files: ssrc/year/month/day/filename
 --timeout|-t <seconds>: Close file after idle period (default 20 sec)
 --verbose|-v: Increase verbosity level
 --max_length|-x|--lengthlimit|--limit|-L <seconds>: maximum file duration, seconds. When --pad is set, pad first file to duration boundary
 --ssrc <ssrc>: Select one SSRC (recommended for --stdout)
 --version|-V: display command version
@endverbatim
 */

// Read and record PCM/WAV and Ogg Opus audio streams
// Now with --stdout option to send (one) stream to standard output, eventually to replace pcmcat
// Also with --exec option to pipe stream into command, to replace pcmspawn
// Copyright 2021-2024 Phil Karn, KA9Q
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
#include <inttypes.h>
#include <ogg/ogg.h>
#include <sys/file.h>

#include "misc.h"
#include "attr.h"
#include "multicast.h"
#include "radio.h"

// size of stdio buffer for disk I/O. 8K is probably the default, but we have this for possible tuning
#define BUFFERSIZE (8192) // probably the same as default
#define RESEQ 64 // size of resequence queue. Probably excessive; WiFi reordering is rarely more than 4-5 packets

// Simplified .wav file header
// http://soundfile.sapp.org/doc/WaveFormat/
struct wav {
  char ChunkID[4]; // "RIFF"
  int32_t ChunkSize; // Total file size minus 8
  char Format[4]; // "WAVE"

  char Subchunk1ID[4]; // "fmt "
  int32_t Subchunk1Size; // Chunk size minus 8
  int16_t AudioFormat;   // 1 = integer, 3 = float
  int16_t NumChannels;
  int32_t SampleRate; // Hz
  int32_t ByteRate;
  int16_t BlockAlign;
  int16_t BitsPerSample; // end of subchunk1 header in typical wav header

  // adding additional chunk fields to the wav file header to support 32 bit FP
  // see https://www.mmsp.ece.mcgill.ca/Documents/AudioFormats/WAVE/WAVE.html
  int16_t ExtensionChunkSize;
  int16_t ValidBitsPerSample;
  int32_t ChannelMask;
  char Subformat[16];
  char FactID[4];
  uint32_t FactSize;
  uint32_t SamplesLength;

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

  char filename[PATH_MAX+10];  // allow room for suffixes
  bool can_seek;               // file is regular; can seek on it
  bool exit_after_close;       // Exit after closing stdout

  uint32_t ssrc;               // RTP stream source ID
  struct rtp_state rtp_state;

  // information obtained from status stream
  struct channel chan;
  struct frontend frontend;

  double last_frequency;       // Detect changes to trigger Ogg Opus stream restarts
  char last_preset[32];

  int type;                    // RTP payload type (with marker stripped)
  int channels;                // 1 (PCM_MONO) or 2 (PCM_STEREO)
  unsigned int samprate;
  enum encoding encoding;

  // Ogg container state
  ogg_stream_state oggState;   // For ogg Opus
  uint8_t opus_toc;            // Last opus TOC (first) byte, for packet loss concealment
  int64_t granulePosition;
  int packetCount;
  struct reseq {
    struct rtp_header rtp;
    uint8_t *data;
    size_t size;
    bool inuse;
  } reseq[RESEQ];              // Reseqencing queue

  FILE *fp;                    // File being recorded
  void *iobuffer;              // Big buffer to reduce write rate
  struct timespec last_active; // time of last file activity
  int64_t starting_offset;     // First actual sample in file past wav header (for time alignment)
  bool no_offset;              // Don't offset except on first file in series (with -L option)

  bool substantial_file;       // At least one substantial segment has been seen
  uint64_t current_segment_samples; // total samples in this segment without skips in timestamp
  uint64_t samples_written;
  uint64_t total_file_samples;
  uint64_t samples_remaining;   // Samples remaining before file is closed; 0 means indefinite
  int64_t file_time;
  bool complete;
};

#define SIZE_LIMIT 1
#define SESSION_CLOSE 2
#define IDLE_TIMEOUT 3

static double SubstantialFileTime = 0.2;  // Don't record bursts < 250 ms unless they're between two substantial segments
static double Max_length = 0; // Length of recording in seconds; 0 = unlimited
int Verbose;
static char PCM_mcast_address_text[256];
static int64_t Timeout = 20; // 20 seconds max idle time before file close
static char const *Recordings = ".";
static bool Subdirs; // Place recordings in subdirectories by SSID
static char const *Locale;
static uint32_t Ssrc; // SSRC, when manually specified
static bool Catmode = false; // sending one channel to standard output
static bool Flushmode = false; // Flush ogg packets after each write; also fflush unless writing to file
static const char *Command = NULL;
static bool Jtmode = false;
static bool Raw = false;
static char const *Source;
static struct sockaddr_storage *Source_socket; // Remains NULL if Source == NULL
static bool Prefix_source; // Prepend 192.168.42.4:1234_ to file name
static bool Padding = false;
static bool Reset_time = false;

const char *App_path;
static int Input_fd,Status_fd;
static struct session *Sessions;
int Mcast_ttl;

static void closedown(int a);
static void input_loop(void);
static void process_status(int);
static void process_data(int);
static void scan_sessions(void);

static void cleanup(void);
static int session_file_init(struct session *sp,struct sockaddr const *sender,int64_t timestamp);
static int close_session(struct session **spp);
static int close_file(struct session *sp,char const *reason);
static uint8_t *encodeTagString(uint8_t *out,size_t size,const char *string);
static int start_ogg_opus_stream(struct session *sp);
static int emit_ogg_opus_tags(struct session *sp);
static int emit_opus_silence(struct session * const sp,int samples,bool plc_ok);
static int end_ogg_opus_stream(struct session *sp);
static int ogg_flush(struct session *sp);
static int start_wav_stream(struct session *sp);
static int end_wav_stream(struct session *sp);
static int send_wav_queue(struct session * const sp,bool flush);
static int send_opus_queue(struct session * const sp,bool flush);
static int send_queue(struct session * const sp,bool flush);

static struct option Options[] = {
  {"ft8", no_argument, NULL, '8'}, // synonym for --jt --lengthlimit 15
  {"ft4", no_argument, NULL, '4'}, // synonym for --jt --lengthlimit 7.5
  {"wspr", no_argument, NULL, 'w'}, // synonym for --jt --lengthlimit 120
  {"catmode", no_argument, NULL, 'c'}, // Send single stream to stdout
  {"stdout", no_argument, NULL, 'c'},
  {"directory", required_argument, NULL, 'd'},
  {"exec", required_argument, NULL, 'e'},
  {"flush", no_argument, NULL, 'f'},   // Quickly fflush in --stdout mode to minimize delay
  {"jt", no_argument, NULL, 'j'},      // Use K1JT format file names
  {"locale", required_argument, NULL, 'l'},
  {"minfiletime", required_argument, NULL, 'm'},
  {"mintime", required_argument, NULL, 'm'},
  {"source", required_argument, NULL, 'o'},
  {"prefix-source", no_argument, NULL, 'p' }, // Prefix file names with source socket
  {"reset", no_argument, NULL, 'R'},  // Detect clock skew and reset
  {"raw", no_argument, NULL, 'r' },
  {"subdirectories", no_argument, NULL, 's'},
  {"subdirs", no_argument, NULL, 's'},
  {"timeout", required_argument, NULL, 't'},
  {"verbose", no_argument, NULL, 'v'},
  {"lengthlimit", required_argument, NULL, 'L'}, // Segment files by wall clock time
  {"length", required_argument, NULL, 'L'},
  {"limit", required_argument, NULL, 'L'},
  {"pad", no_argument, NULL, 'P'},
  {"ssrc", required_argument, NULL, 'S'},
  {"version", no_argument, NULL, 'V'},
  {"max_length", required_argument, NULL, 'x'},
  {NULL, no_argument, NULL, 0},
};
static char Optstring[] = ":cd:e:fjl:m:o:PprRsS:t:vL:Vx:48w";

int main(int argc,char *argv[]){
  App_path = argv[0];

  // Defaults
  Locale = getenv("LANG");

  int c;
  while((c = getopt_long(argc,argv,Optstring,Options,NULL)) != EOF){
    switch(c){
    case 'p':
      Prefix_source = true;
      break;
    case '4':
      Jtmode = true;
      Max_length = 7.5;
      Padding = true;
      Reset_time = true;
      break;
    case '8':
      Jtmode = true;
      Max_length = 15.0;
      Padding = true;
      Reset_time = true;
      break;
    case 'w':
      Jtmode = true;
      Max_length = 120;
      Padding = true;
      Reset_time = true;
      break;
    case 'c':
      Catmode = true;
      break;
    case 'e':
      Command = optarg;
      break;
    case 'f':
      Flushmode = true; // Flush ogg opus streams after every frame
      break;
    case 'j':
      Jtmode = true;
      Padding = true;
      Reset_time = true;
      break;
    case 'd':
      Recordings = optarg;
      break;
    case 'o':
      Source = optarg;
      break;
    case 'l':
      Locale = optarg;
      break;
    case 'm':
      if(optarg)
	SubstantialFileTime = fabs(strtod(optarg,NULL));
      break;
    case 'R':
      Reset_time = true;
      break;
    case 'r':
      Raw = true;
      break;
    case 'S':
      if(optarg){
	char *ptr = NULL;
	long x = strtol(optarg,&ptr,0);
	if(ptr != optarg)
	  Ssrc = (uint32_t)x;
      }
      break;
    case 's':
      Subdirs = true;
      break;
    case 't':
      if(optarg){
	char *ptr = NULL;
	int64_t x = strtoll(optarg,&ptr,0);
	if(ptr != optarg)
	  Timeout = x;
      }
      break;
    case 'v':
      Verbose++;
      break;
    case 'L':
    case 'x':
      if(optarg)
	Max_length = fabs(strtod(optarg,NULL));
      break;
    case 'P':
      Padding = true;
      break;
    case 'V':
      VERSION();
      exit(EX_OK);
    default:
      fprintf(stderr,"Usage: %s [-c|--catmode|--stdout] [-r|--raw] [-e|--exec command] [-f|--flush] [-s] [-d directory] [-l locale] [-L maxtime] [-t timeout] [-j|--jt] [-v] [-m sec] [-x|--max_length max_file_time, no sync, oneshot] [-o|--source <source-name-or-address>] PCM_multicast_address\n",argv[0]);
      exit(EX_USAGE);
      break;
    }
  }
  setlocale(LC_ALL,Locale);
  if(optind >= argc){
    fprintf(stderr,"Specify PCM_mcast_address_text_address\n");
    exit(EX_USAGE);
  }
  strlcpy(PCM_mcast_address_text,argv[optind],sizeof(PCM_mcast_address_text));
  setlinebuf(stderr); // In case we're redirected to a file

  if(Catmode && Command != NULL){
    fprintf(stderr,"--exec supersedes --stdout\n");
    Catmode = false;
  }
  if((Catmode || Command != NULL) && (Subdirs || Jtmode || Max_length != 0 || Padding)){
    fprintf(stderr,"--stdout and --exec supersede --subdirs, --jtmode, --max-length, --length and --pad\n");
    Subdirs = false;
    Jtmode = false;
    Max_length = 0;
    Padding = false;
  }

  if(Subdirs && Jtmode){
    fprintf(stderr,"--jtmode supersedes --subdirs\n");
    Subdirs = false;
  }
  if(Source != NULL){
    Source_socket = calloc(1,sizeof(struct sockaddr_storage));
    resolve_mcast(Source,Source_socket,0,NULL,0,0);
  }
  // Set up input socket for multicast data stream from front end
  {
    struct sockaddr_storage sock;
    char iface[1024];
    resolve_mcast(PCM_mcast_address_text,&sock,DEFAULT_RTP_PORT,iface,sizeof(iface),0);
    Input_fd = listen_mcast(Source_socket,&sock,iface);
    resolve_mcast(PCM_mcast_address_text,&sock,DEFAULT_STAT_PORT,iface,sizeof(iface),0);
    Status_fd = listen_mcast(Source_socket,&sock,iface);
  }
  if(Status_fd == -1 || Input_fd == -1){
    fprintf(stderr,"Can't set up PCM input, exiting\n");
    exit(EX_IOERR);
  }
  int n = 1 << 20; // 1 MB
  if(setsockopt(Input_fd,SOL_SOCKET,SO_RCVBUF,&n,sizeof(n)) == -1)
    perror("setsockopt");

  // Graceful signal catch
  signal(SIGPIPE,closedown); // Should catch the --exec or --stdout receiving process terminating
  signal(SIGINT,closedown);
  signal(SIGKILL,closedown);
  signal(SIGQUIT,closedown);
  signal(SIGTERM,closedown);

  atexit(cleanup);
  if(strlen(Recordings) > 0 && chdir(Recordings) != 0){
    fprintf(stderr,"Can't change to directory %s: %s\n",Recordings,strerror(errno));
    exit(EX_CANTCREAT);
  }
  input_loop(); // Doesn't return

  exit(EX_OK);
}


// Read both data and status from RTP network socket, assemble blocks of samples
// Doing both in one thread avoids a lot of synchronization problems with the session structure, since both write it
static void input_loop(){
  while(true){
    // Receive status or data
    struct pollfd pfd[2] = {
      {
	.fd = Input_fd,
	.events = POLLIN,
      },
      {
	.fd = Status_fd,
	.events = POLLIN,
      },
    };
    int const n = poll(pfd,2,1000); // Wait 1 sec max so we can scan active session list
    if(n < 0)
      break; // error of some kind - should we exit or retry?

    if(pfd[1].revents & (POLLIN|POLLPRI))
      process_status(Status_fd);

    if(pfd[0].revents & (POLLIN|POLLPRI))
      process_data(Input_fd);

    scan_sessions();
  }
}
static void process_status(int fd){
  // Process status packet, if present
  uint8_t buffer[PKTSIZE];
  struct sockaddr sender;
  socklen_t socksize = sizeof(sender);
  ssize_t const length = recvfrom(fd, buffer, sizeof buffer, 0, &sender,&socksize);
  if(length <= 0){    // ??
    perror("recvfrom");
    return; // Some sort of error
  }
#if 0
  if(Source){
    // Backstop for kernel source filtering, which doesn't always work (e.g., on loopback)
    // Make this work for IPv6 too
    struct sockaddr_in *sin = (struct sockaddr_in *)&sender;
    struct sockaddr_in *src = (struct sockaddr_in *)Source_socket;
    if(sin->sin_family != src->sin_family || sin->sin_addr.s_addr != src->sin_addr.s_addr){
      fprintf(stderr,"wanted %s got %s\n",formatsock(Source_socket,false),formatsock(&sender,false));
      return; // Source filtering isn't working right
    }
  }
#endif
  if(buffer[0] != STATUS)
    return;
  // Extract just the SSRC to see if the session exists
  // NB! Assumes same IP source address *and UDP source port* for status and data
  // This is only true for recent versions of radiod, after the switch to unconnected output sockets
  // But older versions don't send status on the output channel anyway, so no problem
  struct channel chan = {0}; // Must be cleared, decode_radio_status() may not write every field
  struct frontend frontend = {0}; // ditto
  decode_radio_status(&frontend,&chan,buffer+1,length-1);
  chan.preset[sizeof chan.preset - 1] = '\0';
  frontend.description[sizeof frontend.description - 1] = '\0';

  if(Ssrc != 0 && chan.output.rtp.ssrc != Ssrc)
    return; // Unwanted session, but still clear any data packets

  // Look for existing session
  // Everything must match, or we create a different session & file
  struct session *sp;
  for(sp = Sessions;sp != NULL;sp=sp->next){
    if(sp->ssrc == chan.output.rtp.ssrc
       && sp->type == chan.output.rtp.type
       && address_match(&sp->sender,&sender)
       && getportnumber(&sp->sender) == getportnumber(&sender))
      break;
  }
  if(sp != NULL && sp->prev != NULL){
    // Move to top of list to speed later lookups
    sp->prev->next = sp->next;
    if(sp->next != NULL)
      sp->next->prev = sp->prev;
    sp->next = Sessions;
    sp->prev = NULL;
    Sessions = sp;
  }
  if(sp == NULL){
    // Create session and initialize
    sp = calloc(1,sizeof(*sp));
    if(sp == NULL)
      return; // Unlikely

    sp->prev = NULL;
    sp->next = Sessions;
    if(sp->next)
      sp->next->prev = sp;
    Sessions = sp;
    if(Catmode && Ssrc == 0){
      Ssrc = chan.output.rtp.ssrc; // Latch onto the first ssrc we see, ignore others
    }
  }
  // Wav can't change channels or samprate mid-stream, so if they're going to change we
  // should probably add an option to force stereo and/or some higher sample rate.
  // OggOpus can restart the stream with the new parameters, so it's not a problem
  sp->ssrc = chan.output.rtp.ssrc;
  sp->type = chan.output.rtp.type;
  sp->channels = chan.output.channels;
  sp->encoding = chan.output.encoding;
  sp->samprate = (sp->encoding == OPUS || sp->encoding == OPUS_VOIP) ? OPUS_SAMPRATE : chan.output.samprate;
  sp->sender = sender;
  sp->chan = chan;
  sp->frontend = frontend;
  // Ogg (containing opus) can concatenate streams with new metadata, so restart when it changes
  // WAV files don't even have this metadata, so ignore changes
  if((sp->encoding == OPUS || sp->encoding == OPUS_VOIP)
     && (sp->last_frequency != sp->chan.tune.freq || strncmp(sp->last_preset,sp->chan.preset,sizeof(sp->last_preset)))){
    end_ogg_opus_stream(sp);
    start_ogg_opus_stream(sp);
    emit_ogg_opus_tags(sp);
  }
}
static void process_data(int fd){
  // Process data packet, if any
  uint8_t buffer[PKTSIZE];
  struct sockaddr sender;
  socklen_t socksize = sizeof(sender);
  ssize_t size = recvfrom(fd,buffer,sizeof(buffer),0,&sender,&socksize);
  if(size <= 0){    // ??
    perror("recvfrom");
    return;
  }
#if 0
  if(Source){
    // Backstop for kernel source filtering, which doesn't always work (e.g., on loopback)
    // Make this work for IPv6 too
    struct sockaddr_in *sin = (struct sockaddr_in *)&sender;
    struct sockaddr_in *src = (struct sockaddr_in *)Source_socket;
    if(sin->sin_family != src->sin_family || sin->sin_addr.s_addr != src->sin_addr.s_addr){
      fprintf(stderr,"wanted %s got %s\n",formatsock(Source_socket,false),formatsock(&sender,false));
      return; // Source filtering isn't working right
    }
  }
#endif
  if(size < RTP_MIN_SIZE)
    return; // Too small for RTP, ignore

  struct rtp_header rtp;
  uint8_t const * const dp = (uint8_t *)ntoh_rtp(&rtp,buffer);
  if(rtp.pad){
    // Remove padding
    size -= dp[size-1];
    rtp.pad = 0;
  }
  if(size <= 0)
    return; // Bogus RTP header

  size -= (dp - buffer);

  if(Ssrc != 0 && rtp.ssrc != Ssrc)
    return;

  // Sessions are defined by the tuple {ssrc, payload type, sending IP address, sending UDP port}
  struct session *sp;
  for(sp = Sessions;sp != NULL;sp=sp->next){
    if(sp->ssrc == rtp.ssrc
       && sp->type == rtp.type
       && address_match(&sp->sender,&sender)
       && getportnumber(&sp->sender) == getportnumber(&sender))
      break;
  }
  // If a matching session is not found, drop packet and wait for first status packet to create it
  // This is a change from previous behavior without status when the first RTP packet would create it
  // This is the only way to work with dynamic payload types since we need the status info
  // We can't even process RTP timestamps without knowing how big a frame is
  if(sp == NULL)
    return;

  if(sp->prev != NULL){
    // Move to top of list to speed later lookups
    sp->prev->next = sp->next;
    if(sp->next != NULL)
      sp->next->prev = sp->prev;
    sp->next = Sessions;
    sp->prev = NULL;
    Sessions = sp;
  }
  clock_gettime(CLOCK_REALTIME,&sp->last_active);
  if(sp->fp == NULL && !sp->complete){
    int64_t sender_time = sp->chan.clocktime + (int64_t)BILLION * (UNIX_EPOCH - GPS_UTC_OFFSET);
    sender_time += (int64_t)BILLION * (int32_t)(rtp.timestamp - sp->chan.output.time_snap) / sp->samprate;

    if(session_file_init(sp,&sender,sender_time) != 0)
      return;

    if(sp->encoding == OPUS || sp->encoding == OPUS_VOIP){
      if(Raw)
	fprintf(stderr,"--raw ignored on Ogg Opus streams\n");
      start_ogg_opus_stream(sp);
      emit_ogg_opus_tags(sp);
      if(sp->starting_offset != 0)
	emit_opus_silence(sp,sp->starting_offset,false);
    } else {
      if(!Raw)
	start_wav_stream(sp); // Don't emit wav header in --raw
      int const framesize = sp->channels *
	(sp->encoding == F32LE ? sizeof(float)
	 : sp->encoding == F32BE ? sizeof(float)
	 : sp->encoding == S16LE ? sizeof(int16_t)
	 : sp->encoding == S16BE ? sizeof(int16_t)
#ifdef HAS_FLOAT16
	 : sp->encoding == F16LE ? sizeof(float16_t)
	 : sp->encoding == F16BE ? sizeof(float16_t)
#endif
	 : 0);
      if(framesize == 0)
	return; // invalid, can't process

      if(sp->fp != NULL){
	if(sp->can_seek){
	  fseeko(sp->fp,framesize * sp->starting_offset,SEEK_CUR);
	} else {
	  // Emit zero padding
	  uint8_t zeroes[4096] = {0};
	  int bytesleft = sp->starting_offset * framesize;
	  while(bytesleft > 0){
	    int bytes = min(bytesleft,4096);
	    fwrite(zeroes,1,bytes,sp->fp);
	    bytesleft -= bytes;
	  }
	}
      }
    }
  }
  // Output stream now ready to go
  if(sp->rtp_state.odd_seq_set){
    if(rtp.seq == sp->rtp_state.odd_seq){
      // Sender probably restarted; flush queue and start over
      send_queue(sp,true);
      sp->rtp_state.init = false;
    } else
      sp->rtp_state.odd_seq_set = false;
  }
  if(!sp->rtp_state.init){
    sp->rtp_state.seq = rtp.seq;
    sp->rtp_state.timestamp = rtp.timestamp;
    sp->rtp_state.init = true;
    sp->rtp_state.odd_seq_set = false;
    if(Verbose > 1)
      fprintf(stderr,"ssrc %u init seq %u timestamp %u\n",rtp.ssrc,rtp.seq,rtp.timestamp);
  }
  // Place packet into proper place in resequence ring buffer
  int16_t const seqdiff = rtp.seq - sp->rtp_state.seq;

  if(seqdiff < 0){
    // old, drop
    if(Verbose > 1)
      fprintf(stderr,"ssrc %u drop old sequence %u timestamp %u bytes %ld\n",rtp.ssrc,rtp.seq,rtp.timestamp,size);
    sp->rtp_state.dupes++;
    // But sender may have restarted so remember it
    sp->rtp_state.odd_seq = rtp.seq + 1;
    sp->rtp_state.odd_seq_set = true;
    return;
  } else if(seqdiff >= RESEQ){
    // Give up waiting for the lost frame, flush what we have
    // Could also be a restart, but treat it the same
    if(Verbose > 1)
      fprintf(stderr,"ssrc %u flushing with drops\n",rtp.ssrc);
    send_queue(sp,true);
    if(Verbose > 1)
      fprintf(stderr,"ssrc %u reset & queue sequence %u timestamp %u bytes %ld\n",rtp.ssrc,rtp.seq,rtp.timestamp,size);
  }
  if(Verbose > 2)
    fprintf(stderr,"ssrc %u queue sequence %u timestamp %u bytes %ld\n",rtp.ssrc,rtp.seq,rtp.timestamp,size);

  // put into circular queue
  sp->rtp_state.odd_seq_set = false;
  int const qi = rtp.seq % RESEQ;
  struct reseq * const qp = &sp->reseq[qi];
  qp->inuse = true;
  qp->rtp = rtp;
  qp->data = malloc(size);
  qp->size = size;

  if(sp->encoding == S16BE){
    // Flip endianness from big-endian on network to little endian wanted by .wav
    // byteswap.h is linux-specific; need to find a portable way to get the machine instructions

    int16_t const * const samples = (int16_t *)dp;
    int16_t * const wp = (int16_t *)qp->data;
    size_t const samp_count = size / sizeof(int16_t);
    for(size_t n = 0; n < samp_count; n++)
      wp[n] = (uint16_t)bswap_16((uint16_t)samples[n]);
  } else {
    memcpy(qp->data,dp,size); // copy everything else into circular queue as-is
  }
  send_queue(sp,false); // Send what we now can
  // If output is pipe, flush right away to minimize latency
  if(sp->fp && !sp->can_seek && 0 != fflush(sp->fp))
    fprintf(stderr,"flush failed on '%s', %s\n",sp->filename,strerror(errno));

  if(sp->samples_remaining <= 0)
    close_file(sp,"size limit"); // Don't reset RTP here so we won't lose samples on the next file
}
static void scan_sessions(){
  // Walk through session list, close idle files
  // Leave sessions forever in case traffic starts again?
  struct timespec now;
  clock_gettime(CLOCK_REALTIME,&now);

  struct session *next = NULL;
  for(struct session *sp = Sessions;sp != NULL; sp = next){
    next = sp->next; // save in case sp is closed
    // Don't close session waiting for first activity
    if(sp->last_active.tv_sec == 0 || sp->fp == NULL)
      continue;
    double const idle_time = now.tv_sec - sp->last_active.tv_sec + (now.tv_nsec - sp->last_active.tv_nsec) * 1.0e-9;
    if(idle_time >= Timeout){
      // Close idle file
      close_file(sp,"idle timeout"); // sp will be NULL
      if(sp->exit_after_close)
	exit(EX_OK); // if writing to a pipe
      sp->rtp_state.init = false; // reinit rtp on next packet so we won't emit lots of silence
    }
  }
}

static void closedown(int a){
  if(Verbose)
    fprintf(stderr,"%s: caught signal %d: %s\n",App_path,a,strsignal(a));

  cleanup();
  exit(EX_OK);  // Will call cleanup()
}

// Write out any partial Ogg Opus pages
// Note: just closes current Ogg page, doesn't actually flush stdio output stream
static int ogg_flush(struct session *sp){
  if(sp == NULL || sp->fp == NULL || (sp->encoding != OPUS && sp->encoding != OPUS_VOIP))
    return -1;

  ogg_page oggPage;
  int count = 0;
  while (ogg_stream_flush(&sp->oggState,&oggPage)){
    fwrite(oggPage.header, 1, oggPage.header_len, sp->fp);
    fwrite(oggPage.body, 1, oggPage.body_len, sp->fp);
    count++;
  }
  return count;
}

// These produced by a test program I wrote. All are in CELT, mono
static uint8_t OpusSilence25[] = {0xe0,0xff,0xfe}; // 2.5 ms Silence
static uint8_t OpusSilence5[] = {0xe8,0xff,0xfe}; // 5 ms Silence
static uint8_t OpusSilence10[] = {0xf0,0xff,0xfe}; // 10 ms Silence
static uint8_t OpusSilence20[] = {0xf8,0xff,0xfe}; // 20 ms Silence
static uint8_t OpusSilence40[] = {0xf9,0xff,0xfe,0xff,0xfe}; // 40 ms Silence (2x 20 ms silence frames)
static uint8_t OpusSilence60[] = {0xfb,0x03,0xff,0xfe,0xff,0xfe,0xff,0xfe}; // 60 ms Silence (3 x 20ms silence frames)

static int emit_opus_silence(struct session * const sp,int samples,bool plc_ok){
  if(sp == NULL || sp->fp == NULL || (sp->encoding != OPUS && sp->encoding != OPUS_VOIP))
    return -1;

  if(Verbose > 1)
    fprintf(stderr,"%u: emitting %d frames of silence\n",sp->ssrc,samples);

  ogg_packet oggPacket = { 0 }; // b_o_s and e_o_s are 0
  int samples_since_flush = 0;
  int plc_samples_generated = 0;
  uint8_t buffer[128] = {0}; // much longer than needed
  int length = 0;
  while(samples > 0){
    int chunk = min(samples,2880); // 60 ms is 2880 samples @ 48 kHz
    chunk = min(chunk,2880); // limit to 60 ms of either silence or PLC per packet
    // To save a little space in long silent intervals, emit the largest frame that will fit
    // Are we allowed to emit PLC, and have we not already sent 60 ms of it?
    if(plc_ok && plc_samples_generated < 2880){
      // Emit PLC until 60 ms, then switch to silence if there's more
      int code = sp->opus_toc >> 3; // high 5 bit code for codec mode and frame duration
      // Rewrite the code with our chosen duration
      if(code < 12){
	// Silk-only numbers 0-11
	int ms = chunk / 48;  // samples -> ms
	code = (code & ~0x3) | (ms/20);  // replace duration in last 2 bits: 10, 20, 40 or 60 ms
      } else if(code < 14){
	// hybrid SWB, 12-13
	chunk = min(chunk,960); // limit to 20 ms
	int ms = chunk / 48;
	code = (code & ~0x1) | (ms/20);   // 10 or 20 ms (0 or 1)
      } else if(code < 16){
	// hybrid FB 14-15
	chunk = min(chunk,960); // limit to 20 ms
	int ms = chunk / 48;
	code = (code & ~0x1) | (ms / 20); // 10 or 20 ms (0 or 1)
      } else {
	// CELT-only codes 16-31
	chunk = min(chunk,960); // limit to 20 ms
	int ms10 = 10 * chunk / 48; // tenths of ms
	code = (code & ~0x3) | (ms10 > 100 ? 3 : ms10 / 50); // 2.5, 5, 10 or 20
      }
      buffer[0] = (code << 3)| 0x00;
      length = 1;
      plc_samples_generated += chunk;
      int n = opus_packet_get_nb_samples(buffer,length,OPUS_SAMPRATE);
#ifndef NDEBUG
      int spf = opus_packet_get_samples_per_frame(buffer, OPUS_SAMPRATE);
      int nf  = opus_packet_get_nb_frames(buffer, length);
      // Check suggested by ChatGPT when I encode multi-frame PLC
      assert(n == spf * nf);
#endif
      if(n == OPUS_BAD_ARG){
	fprintf(stderr,"Bad generated Opus PLC TOC! ssrc %u saved toc 0x%x (%d), generated toc 0x%x (%d), intended duration %d samples (%.1lf ms)\n",
		sp->ssrc,sp->opus_toc,sp->opus_toc,buffer[0],buffer[0],chunk,(double)chunk/48.);
      } else if (n == OPUS_INVALID_PACKET){
	fprintf(stderr,"Invalid generated Opus packet! ssrc %u saved toc 0x%x (%d), generated toc 0x%x (%d), intended duration %d samples (%.1lf ms)\n",
		sp->ssrc,sp->opus_toc,sp->opus_toc,buffer[0],buffer[0],chunk,(double)chunk/48.);
      } else if(n != chunk){
	fprintf(stderr,"Opus PLC Length error! ssrc %u saved toc 0x%x (%d), generated toc 0x%x (%d), intended duration %d samples (%.1lf ms) actual %d samples (%.1lf ms)\n",
		sp->ssrc,sp->opus_toc,sp->opus_toc,buffer[0],buffer[0],chunk,(double)chunk/48.,n,(double)n/48.);
      }
      if(Verbose > 2)
	fprintf(stderr,"ssrc %u emit plc %.1lf ms\n",sp->ssrc,chunk / 48.);
    } else if(chunk >= 2880){
      length = sizeof OpusSilence60;
      memcpy(buffer,OpusSilence60,length);
    } else if(chunk >= 1920){
      chunk = 1920;
      length = sizeof OpusSilence40;
      memcpy(buffer,OpusSilence40,length);
    } else if(chunk >= 960){
      chunk = 960;
      length = sizeof OpusSilence20;
      memcpy(buffer,OpusSilence20,length);
    } else if(chunk >= 480){
      chunk = 480;
      length = sizeof OpusSilence10;
      memcpy(buffer,OpusSilence10,length);
    } else if(chunk >= 240){
      chunk = 240;
      length = sizeof OpusSilence5;
      memcpy(buffer,OpusSilence5,length);
    } else {
      chunk = 120;
      length = sizeof OpusSilence25;
      memcpy(buffer,OpusSilence25,length);
    }
    // Can we copy the stereo bit into the silence messages?
    //    buffer[0] |= (sp->opus_toc & 0x04);
    oggPacket.packet = buffer;
    oggPacket.bytes = length;
    oggPacket.packetno = sp->packetCount++; // Increment packet number
    sp->granulePosition += chunk; // points to end of this packet
    oggPacket.granulepos = sp->granulePosition; // Granule position
    int const ret = ogg_stream_packetin(&sp->oggState, &oggPacket);	  // Add the packet to the Ogg stream
    (void)ret;
    assert(ret == 0);

    sp->rtp_state.timestamp += chunk; // also ready for next
    sp->total_file_samples += chunk;
    sp->samples_written += chunk;
    sp->samples_remaining -= chunk;
    samples -= chunk;
    samples_since_flush += chunk;
    if(Flushmode || samples_since_flush >= OPUS_SAMPRATE){
      // Write at least once per second to keep opusinfo from complaining, and vlc progress from sticking
      samples_since_flush = 0;
      ogg_flush(sp);
    }
  }
  return 0;
}


static int send_queue(struct session * const sp,bool const flush){
  if(sp == NULL)
    return -1;
  if(sp->encoding == OPUS || sp->encoding == OPUS_VOIP)
    send_opus_queue(sp,flush);
  else
    send_wav_queue(sp,flush);

  // If file length is not limited, keep resetting it so it won't expire
  if(Max_length == 0)
    sp->samples_remaining = INT64_MAX;

  return 0;
}

// if !flush, send whatever's on the queue, up to the first missing segment
// if flush, empty the entire queue, skipping empty entries
static int send_opus_queue(struct session * const sp,bool const flush){
  if(sp == NULL || sp->fp == NULL || (sp->encoding != OPUS && sp->encoding != OPUS_VOIP))
    return -1;

  // Anything on the resequencing queue we can now process?
  int count = 0;
  for(int i=0; i < RESEQ; i++,sp->rtp_state.seq++){
    if(sp->samples_remaining <= 0)
      break; // Can't send any more in this file
    struct reseq * const qp = &sp->reseq[sp->rtp_state.seq % RESEQ];
    if(!qp->inuse){
      if(!flush)
	break; // Stop on first empty entry if we're not resynchronizing
      // Slot was empty
      // Instead of emitting one frame of silence here, we emit it when the next real frame arrives
      // so we know for sure how much to send
      //
      sp->rtp_state.drops++;
      continue;
    }
    int32_t jump = (int32_t)(qp->rtp.timestamp - sp->rtp_state.timestamp);
    if(jump > 0){
      // Timestamp jumped since last frame
      // Catch up by emitting silence padding
      if((uint64_t)jump > sp->samples_remaining)
	jump = (int32_t)sp->samples_remaining; // can't overflow in conversion

      if(Verbose > 2 || (Verbose > 1  && flush))
	fprintf(stderr,"timestamp jump %d samples\n",jump);

      // Send packet loss concealment only if there's a jump in the sequence number
      // to indicate an actual packet loss. Don't send a PLC if the sender had merely
      // paused due to DTX or a squelch closure, that causes annoying artifacts
      bool const plc_ok = (qp->rtp.seq != sp->rtp_state.seq);
      emit_opus_silence(sp,jump,plc_ok); // Might emit more if not on a frame boundary
      sp->current_segment_samples = 0; // gap resets
      sp->rtp_state.timestamp += jump; // also ready for next
      sp->total_file_samples += jump;
      sp->samples_written += jump;
      // If samples_remaining goes to zero, we'll break out of the loop below
      sp->samples_remaining -= jump;
    }
    if(sp->samples_remaining <= 0)
      break;
    // end of timestamp jump catch-up, send actual packets on queue
    // We have to send the whole queue entry, so we might go past the file samples remaining
    opus_int32 samples = opus_packet_get_nb_samples(qp->data,(opus_int32)qp->size,OPUS_SAMPRATE); // Number of 48 kHz samples
    sp->granulePosition += samples; // Adjust the granule position to point to end of this packet
    sp->opus_toc = qp->data[0]; // save toc byte in case we need it for PLC
    ogg_packet oggPacket = { // b_o_s and e_o_s are 0
      .packetno = sp->packetCount++, // Increment packet number
      .granulepos = sp->granulePosition, // Granule position
      .packet = qp->data,
      .bytes = qp->size
    };
    if(Verbose > 2 || (Verbose > 1  && flush))
      fprintf(stderr,"ssrc %u writing from rtp sequence %u, timestamp %u: bytes %ld samples %d granule %lld\n",
	      sp->ssrc,sp->rtp_state.seq,sp->rtp_state.timestamp,oggPacket.bytes,samples,
	      (long long)oggPacket.granulepos);

    sp->rtp_state.timestamp += samples; // also ready for next
    sp->total_file_samples += samples;
    sp->current_segment_samples += samples;
    if(sp->current_segment_samples >= SubstantialFileTime * sp->samprate)
      sp->substantial_file = true;

    sp->samples_written += samples;
    sp->samples_remaining -= samples; // Might go negative

    int const ret = ogg_stream_packetin(&sp->oggState, &oggPacket);
    (void)ret;
    assert(ret == 0);
    {
      ogg_page oggPage;
      while (ogg_stream_pageout(&sp->oggState, &oggPage)){
	fwrite(oggPage.header, 1, oggPage.header_len, sp->fp);
	fwrite(oggPage.body, 1, oggPage.body_len, sp->fp);
      }
    }
    FREE(qp->data); // OK if NULL
    qp->size = 0;
    qp->inuse = false;
    count++;
  }
  if(Flushmode)
    ogg_flush(sp); // Absolute minimum latency
  return count;
}
// if !flush, send whatever's on the queue, up to the first missing segment
// if flush, empty the entire queue, skipping empty entries
static int send_wav_queue(struct session * const sp,bool flush){
  if(sp == NULL || sp->fp == NULL)
    return -1;

  // Anything on the resequencing queue we can now process?
  int count = 0;
  int const framesize = sp->channels *
    ( sp->encoding == F32LE ? sizeof(float)
      : sp->encoding == F32BE ? sizeof(float)
      : sp->encoding == S16BE ? sizeof(int16_t)
      : sp->encoding == S16LE ? sizeof(int16_t)
      : 0);
  if(framesize == 0)
    return -1;
  // bytes per sample time
  for(int i=0; i < RESEQ; i++, sp->rtp_state.seq++){
    if(sp->samples_remaining <= 0)
      break; // Can't send any more in this file
    struct reseq * const qp = &sp->reseq[sp->rtp_state.seq % RESEQ];
    if(!qp->inuse){
      if(!flush)
	break; // Stop on first empty entry if we're not resynchronizing
      else
	continue;
    }
    int32_t jump = (int32_t)(qp->rtp.timestamp - sp->rtp_state.timestamp);
    if(jump > 0){
      // Timestamp jumped since last frame
      // Catch up by emitting silence padding
      if((uint64_t)jump > sp->samples_remaining) // confirmed positive
	jump = (int32_t)sp->samples_remaining; // Can't overflow
      assert(jump > 0); // already checked sp->samples_remaining above
      if(Verbose > 2 || (Verbose > 1  && flush))
	fprintf(stderr,"timestamp jump %d frames\n",jump);
      if(sp->can_seek)
	fseeko(sp->fp,framesize * jump,SEEK_CUR);
      else {
	int bytesleft = jump * framesize;
	uint8_t zeroes[4096] = {0};
	while(bytesleft > 0){
	  int bytes = min(bytesleft,4096);
	  fwrite(zeroes,1,bytes,sp->fp);
	  bytesleft -= bytes;
	}
      }
      sp->current_segment_samples = 0; // gap resets
      sp->rtp_state.timestamp += jump; // also ready for next
      sp->total_file_samples += jump;
      sp->samples_written += jump;
      // If samples_remaining goes to zero, we'll break out of the loop below
      sp->samples_remaining -= jump;
    }
    // end of timestamp jump catch-up, send actual packets on queue
    size_t const avail_frames = qp->size / framesize;  // One PCM frame per sample time
    size_t frames = avail_frames;
    if(frames > sp->samples_remaining)
      frames = sp->samples_remaining;

    if(frames <= 0)
      break;
    if(Verbose > 2 || (Verbose > 1  && flush))
      fprintf(stderr,"writing from rtp sequence %u, timestamp %u: bytes %ld frames %ld\n",
	      sp->rtp_state.seq,sp->rtp_state.timestamp,framesize * frames,frames);

    fwrite(qp->data,framesize,frames,sp->fp);
    sp->rtp_state.timestamp += frames; // get ready for next
    sp->total_file_samples += frames;
    sp->current_segment_samples += frames;
    if(sp->current_segment_samples >= SubstantialFileTime * sp->samprate)
      sp->substantial_file = true;
    sp->samples_written += frames;
    sp->samples_remaining -= frames;

    if(frames != avail_frames){
      // Sent only part of this entry before running out of space in the file
      // Keep the rest, shift it up so we'll get it next time
      memmove(qp->data, qp->data + frames * framesize, qp->size - frames * framesize);
      qp->size -= frames * framesize;
      break; // don't increment the expected sequence number, we'll work on this in the next file
    } else {
      FREE(qp->data); // OK if NULL
      qp->size = 0;
      qp->inuse = false;
    }
    count++;
  }
  return count;
}

static void cleanup(void){
  while(Sessions){
    // Flush and close each write stream
    // Be anal-retentive about freeing and clearing stuff even though we're about to exit
    struct session *next_s = Sessions->next;
    close_session(&Sessions); // Sessions will be NULL
    Sessions = next_s;
  }
}
static int session_file_init(struct session *sp,struct sockaddr const *sender,int64_t timestamp){
  if(sp->fp != NULL)
    return 0;

  char const *file_encoding = encoding_string(sp->encoding == S16BE ? S16LE : sp->encoding);
  if(Catmode){
    sp->fp = stdout;
    sp->can_seek = false; // Can't seek on a pipe
    sp->exit_after_close = true; // Single shot
    strlcpy(sp->filename,"[stdout]",sizeof(sp->filename));
    if(Verbose)
      fprintf(stderr,"receiving %s ssrc %u samprate %d channels %d encoding %s freq %'.3lf preset %s\n",
	      sp->frontend.description,
	      sp->ssrc,sp->chan.output.samprate,sp->channels,file_encoding,sp->chan.tune.freq, // use rx sample rate even for opus
	      sp->chan.preset);
    return 0;
  } else if(Command != NULL){
    // Substitute parameters as specified
    sp->can_seek = false; // Can't seek on a pipe
    sp->exit_after_close = false; // Runs forever, closing individual pipes on timeout
    sp->filename[0] = '\0';
    char command_copy[2048]; // Don't overwrite program args
    strlcpy(command_copy,Command,sizeof(command_copy));
    char *cp = command_copy;
    char const *a;
    while((a = strsep(&cp,"$")) != NULL){
      strlcat(sp->filename,a,sizeof(sp->filename));
      if(cp != NULL && strlen(cp) > 0){
	char temp[256];
	switch(*cp++){
	case '$':
	  snprintf(temp,sizeof(temp),"$");
	  break;
	case 'd':
	  snprintf(temp,sizeof(temp),"%s",sp->frontend.description);
	  break;
	case 'h':
	  snprintf(temp,sizeof(temp),"%.1lf",sp->chan.tune.freq);
	  break;
	case 'k':
	  snprintf(temp,sizeof(temp),"%.4lf",sp->chan.tune.freq/1000.);
	  break;
	case 'm':
	  snprintf(temp,sizeof(temp),"%.7lf",sp->chan.tune.freq/1000000.);
	  break;
	case 'c':
	  snprintf(temp,sizeof(temp),"%d",sp->channels);
	  break;
	case 'r':
	  snprintf(temp,sizeof(temp),"%d",sp->chan.output.samprate); // rx sample rate even for Opus
	  break;
	case 's':
	  snprintf(temp,sizeof(temp),"%u",sp->ssrc);
	  break;
	case 'f':
	  snprintf(temp,sizeof temp, "%s", encoding_string(sp->encoding));
	  break;
	default:
	  break;
	}
	size_t r = strlcat(sp->filename,temp,sizeof(sp->filename));
	if(r >= sizeof sp->filename){
	  fprintf(stderr,"filename overflow\n");
	  return -1;
	}
      }
    }
    if(Verbose)
      fprintf(stderr,"%s ssrc %u: executing %s\n",sp->frontend.description,sp->ssrc,sp->filename);
    sp->fp = popen(sp->filename,"w");
    if(sp->fp == NULL){
      fprintf(stderr,"ssrc %u: cannot start %s, exiting",sp->ssrc,sp->filename);
      exit(EX_CANTCREAT); // Will probably fail for all others too, just give up
    }
    return 0;
  }
  // Else create a file
  sp->exit_after_close = false;
  char const *suffix = ".raw";
  if(!Raw){
    switch(sp->encoding){
    case S16BE:
    case S16LE:
    case F32LE:
    case F32BE:
      suffix = ".wav";
      break;
    case F16BE:
    case F16LE:
      suffix = ".f16"; // Non standard! But gotta do something with it for now
      break;
    case OPUS_VOIP:
    case OPUS:
      suffix = ".opus";
      break;
    default:
      suffix = ".raw";
      break;
    }
  }
  // Use time provided by caller
  // Calling the clock here can make the file name a second or two late if the system is heavily loaded
  sp->file_time = timestamp;
  sp->starting_offset = 0;
  sp->samples_remaining = INT64_MAX; // unlimited unless it gets lowered below

  if(Max_length > 0){
    int64_t const period = llrint(1e9 * Max_length); // Period/length in ns
    int64_t period_start_ns = (timestamp / period) * period;  // time since epoch to start of current period
    int64_t skip_ns = timestamp % period;

    if(Padding && !sp->no_offset){ // Not really supported on opus yet
      // Pad start of first file with zeroes
      int64_t const offset = llrint((double)sp->samprate * skip_ns * 1e-9); // Samples to skip
      // Adjust file time to start of current period and pad to first sample
      sp->file_time = period_start_ns; // file starts at beginning of period
      sp->starting_offset = offset;
      sp->total_file_samples += offset; // count as part of file
      if(Verbose > 1)
	fprintf(stderr,"ssrc %lu padding %lf sec %lld samples\n",
		(unsigned long)sp->ssrc,
		(double)skip_ns * 1e-9,
		(long long)offset);

      sp->samples_remaining = llrint(Max_length * sp->samprate) - offset;
      sp->no_offset = true; // Only on the first file
    } else if(Reset_time){
      // On subsequent files, adjust this file size to align the end to the period boundary
      if(skip_ns > period / 2){
	// More than halfway through, go to the next interval and
	period_start_ns += period;
	skip_ns -= period;
      }
      intmax_t const offset = llrint((double)sp->samprate * skip_ns * 1e-9); // Samples to skip
      sp->samples_remaining = llrint(Max_length * sp->samprate) - offset;
    }
  }
  char filename[PATH_MAX] = {0}; // file pathname except for suffix. Must clear so strlen(filename) will always work

  if(Prefix_source){
    size_t const r = snprintf(filename, sizeof filename, "%s_", formatsock(&sp->sender,false));
    if(r >= sizeof filename){
      // Extremely unlikely, but I'm paranoid
      fprintf(stderr,"filename overflow 2\n");
      return -1;
    }
  }
  if(Jtmode){
    // K1JT-format file names in flat directory
    // Round time to nearest second since it will often bobble +/-
    // Accurate time will still be written in the user.unixstarttime attribute
    time_t const seconds = (sp->file_time + 500000000) / BILLION;
    struct tm tm;
    gmtime_r(&seconds,&tm);
    size_t space = sizeof filename - strlen(filename);
    size_t r = snprintf(filename + strlen(filename), space,"%4d%02d%02dT%02d%02d%02dZ_%.0lf_%s",
	     tm.tm_year+1900,
	     tm.tm_mon+1,
	     tm.tm_mday,
	     tm.tm_hour,
	     tm.tm_min,
	     tm.tm_sec,
	     sp->chan.tune.freq,
	     sp->chan.preset);
    if(r >= space)
      return -1; // Too long, unterminated
  } else { // not Jtmode
    // not JT; filename is yyyymmddThhmmss.sZ + digit + suffix
    // digit is inserted only if needed to make file unique
    // Round time to nearest 1/10 second
    int64_t const deci_seconds = sp->file_time / 100000000; // 10 million
    time_t const seconds = deci_seconds/10;
    int const tenths = deci_seconds % 10;
    struct tm tm;
    gmtime_r(&seconds,&tm);

    if(Subdirs){
      // Create directory path
      char dir[PATH_MAX] = {0}; // not really necessary to clear, but I'm paranoid
      errno = 0; // successful mkdir won't clear it, don't want a spurious strerror message if the snprintf failed
      size_t r = snprintf(dir,sizeof dir,"%u",sp->ssrc);
      if(r >= sizeof dir){
	fprintf(stderr,"snprintf overflow 1\n");
	return -1;
      }
      if(mkdir(dir,0777) == -1 && errno != EEXIST){
	fprintf(stderr,"can't create directory %s: %s\n",dir,strerror(errno));
	return -1;
      }
      r = snprintf(dir,sizeof dir,"%u/%d",sp->ssrc,tm.tm_year+1900);
      if(r >= sizeof dir){
	fprintf(stderr,"snprintf overflow 2\n");
	return -1;
      }
      if(mkdir(dir,0777) == -1 && errno != EEXIST){
	fprintf(stderr,"can't create directory %s: %s\n",dir,strerror(errno));
	return -1;
      }
      r = snprintf(dir,sizeof dir,"%u/%d/%d",sp->ssrc,tm.tm_year+1900,tm.tm_mon+1);
      if(r >= sizeof dir){
	fprintf(stderr,"snprintf overflow 3\n");
	return -1;
      }
      if(mkdir(dir,0777) == -1 && errno != EEXIST){
	fprintf(stderr,"can't create directory %s: %s\n",dir,strerror(errno));
	return -1;
      }
      r = snprintf(dir,sizeof dir,"%u/%d/%d/%d",sp->ssrc,tm.tm_year+1900,tm.tm_mon+1,tm.tm_mday);
      if(r >= sizeof dir){
	fprintf(stderr,"snprintf overflow 4\n");
	return -1;
      }
      if(mkdir(dir,0777) == -1 && errno != EEXIST){
	fprintf(stderr,"can't create directory %s: %s\n",dir,strerror(errno));
	return -1;
      }
      // yyyy-mm-dd-hh:mm:ss.s so it will sort properly
      size_t space = sizeof filename - strlen(filename);
      r = snprintf(filename + strlen(filename), space,
	       "%u/%d/%d/%d/",
	       sp->ssrc,
	       tm.tm_year+1900,
	       tm.tm_mon+1,
	       tm.tm_mday);
      if(r >= space){
	fprintf(stderr,"snprintf overflow 5\n");
	return -1;
      }
    }
    // create file in specified directory
    size_t space = sizeof filename - strlen(filename);
    size_t r = snprintf(filename + strlen(filename),space,
	     "%uk%4d-%02d-%02dT%02d:%02d:%02d.%dZ",
	     sp->ssrc,
	     tm.tm_year+1900,
	     tm.tm_mon+1,
	     tm.tm_mday,
	     tm.tm_hour,
	     tm.tm_min,
	     tm.tm_sec,
	     tenths);
    if(r >= space){
      fprintf(stderr,"snprintf overflow 6\n");
      return -1;
    }
  }
  // create a temp file (eg, foo.wav.tmp)
  // Some error and logging messages use the suffix, some don't, but hey
  int fd = -1;
  for(int tries = 0; tries < 10; tries++){
    size_t r;
    if(tries == 0) // Insert digit only if necessary
      r = snprintf(sp->filename,sizeof sp->filename,"%s%s",filename,suffix);
    else
      r = snprintf(sp->filename,sizeof sp->filename, "%s%d%s",filename,tries,suffix);
    if(r >= sizeof sp->filename){
      fprintf(stderr,"snprintf overflow 7\n");
      return -1;
    }
    char tempfile[PATH_MAX+50] = {0};
    r = snprintf(tempfile,sizeof tempfile,"%s.tmp",sp->filename);
    if(r >= sizeof tempfile){
      fprintf(stderr,"snprintf overflow 8\n");
      return -1;
    }
    fd = open(tempfile,O_RDWR|O_CREAT|O_EXCL|O_NONBLOCK,0644);
    if(fd != -1) // If too long, open will fail with ENAMETOOLONG
      break;
    fprintf(stderr,"create %s failed: %s\n",tempfile,strerror(errno));
  }
  if(fd == -1){
    fprintf(stderr,"Giving up creating temp file, redirecting to /dev/null\n");
    // This could be because two SSRCs are tuned to the same frequency and writing the same spool file
    // Leave the temp file(s), the spool reader will clean it out if it's an old fragment
    fd = open("/dev/null",O_RDWR|O_NONBLOCK);
    if(fd == -1){
      // Something is seriously wrong
      fprintf(stderr,"Can't open /dev/null: %s\n",strerror(errno));
      return -1;
    }
    strlcpy(sp->filename,"/dev/null",sizeof sp->filename);
  }
  sp->fp = fdopen(fd,"r+");
  if(sp->fp == NULL){
    fprintf(stderr,"fdopen(%d,a) failed: %s\n",fd,strerror(errno));
    sp->can_seek = false;
    return -1;
  }
  sp->can_seek = true; // Ordinary file
  // We byte swap S16BE to S16LE, so change the tag
  if(Verbose){
    fprintf(stderr,"%s creating '%s' %d s/s %s %s %'.3lf Hz %s",
	    sp->frontend.description,
	    sp->filename,sp->chan.output.samprate, // original rx samprate for opus
	    sp->channels == 1 ? "mono" : "stereo",
	    file_encoding,sp->chan.tune.freq,
	    sp->chan.preset);
    if(sp->starting_offset > 0)
      fprintf(stderr," offset %lld",(long long)sp->starting_offset);
    fprintf(stderr," from %s\n",formatsock(&sp->sender,false));
  }

  if(strcmp(sp->filename,"/dev/null") != 0){
    //    sp->iobuffer = malloc(BUFFERSIZE);
    //    setbuffer(sp->fp,sp->iobuffer,BUFFERSIZE);

    attrprintf(fd,"encoding","%s",file_encoding);
    attrprintf(fd,"samprate","%u",sp->samprate);
    attrprintf(fd,"channels","%d",sp->channels);
    attrprintf(fd,"ssrc","%u",sp->ssrc);
    attrprintf(fd,"frequency","%.3lf",sp->chan.tune.freq);
    attrprintf(fd,"preset","%s",sp->chan.preset);
    attrprintf(fd,"source","%s",formatsock(sender,false));
    attrprintf(fd,"multicast","%s",PCM_mcast_address_text);
    imaxdiv_t r = imaxdiv(sp->file_time,BILLION);
    attrprintf(fd,"unixstarttime","%lld.%09lld",(long long)r.quot, (long long)r.rem);

    if(strlen(sp->frontend.description) > 0)
      attrprintf(fd,"description","%s",sp->frontend.description);

    if(sp->starting_offset != 0)
      attrprintf(fd,"starting offset","%lld",sp->starting_offset);

    if(sp->chan.demod_type == LINEAR_DEMOD && !sp->chan.linear.agc)
      attrprintf(fd,"gain","%.3f",voltage2dB(sp->chan.output.gain));
  }
  return 0;
}

static int close_session(struct session **spp){
  if(spp == NULL)
    return -1;
  struct session *sp = *spp;

  if(sp == NULL)
    return -1;

  close_file(sp,"session closed");

  if(sp->exit_after_close)
    exit(EX_OK); // if writing to anything but an ordinary file

  if(sp->prev)
    sp->prev->next = sp->next;
  else
    Sessions = sp->next;
  if(sp->next)
    sp->next->prev = sp->prev;
  // when the Max_length (-x) option is used, valgrind has
  // intermittently reported unfree'd allocations in the resequencing
  // queue at program exit. Not sure what only -x is affected.
  for(int i=0;i < RESEQ;i++)
    FREE(sp->reseq[i].data);

  FREE(sp);
  return 0;
}


// Close a file, update .wav header
// If the file is not "substantial", just delete it
static int close_file(struct session *sp,char const *reason){
  if(sp == NULL)
    return -1;

  if(sp->fp == NULL)
    return 0;

  if(sp->encoding == OPUS || sp->encoding == OPUS_VOIP)
    end_ogg_opus_stream(sp);
  else
    end_wav_stream(sp);

  if(Catmode)
    fclose(sp->fp); // Just close stdout
  else if(Command != NULL)
    pclose(sp->fp);
  else { // regular file temporary
    if(Verbose){
      fprintf(stderr,"%s closing '%s' %'.1f sec",
	      sp->frontend.description,
	      sp->filename, // might be blank
	      (double)sp->samples_written / sp->samprate);
      if(reason != NULL)
	fprintf(stderr," (%s)\n",reason);

      if(Verbose > 1 && (sp->rtp_state.dupes != 0 || sp->rtp_state.drops != 0))
	fprintf(stderr,"ssrc %u dupes %llu drops %llu\n",sp->ssrc,(long long unsigned)sp->rtp_state.dupes,(long long unsigned)sp->rtp_state.drops);
    }
    if(strcmp(sp->filename,"/dev/null") != 0){
      char tempfile[PATH_MAX+50];
      snprintf(tempfile,sizeof tempfile,"%s.tmp",sp->filename); // Recover actual name of file we're working on
      if(sp->substantial_file){
	int fd = fileno(sp->fp);
	attrprintf(fd,"samples written","%lld",sp->samples_written);
	attrprintf(fd,"total samples","%lld",sp->total_file_samples);
	fclose(sp->fp);
	rename(tempfile,sp->filename);    // Atomic rename, after everything else
      } else {
	// File is too short to keep, delete
	fclose(sp->fp);
	if(unlink(tempfile) != 0)
	  fprintf(stderr,"Can't unlink %s: %s\n",tempfile,strerror(errno));
	if(Verbose)
	  fprintf(stderr,"deleting %s %'.1f sec\n",tempfile,(double)sp->samples_written / sp->samprate);
      }
    }
  } // end of else regular file
  // Do this for all closures (stdout, command, file)
  // Note: leaves RTP state and resequence queue intact for next file
  sp->fp = NULL;
  //  FREE(sp->iobuffer);
  sp->filename[0] = '\0';
  sp->samples_written = 0;
  sp->total_file_samples = 0;
  sp->current_segment_samples = 0;
  sp->file_time = 0;
  return 0;
}

static int start_ogg_opus_stream(struct session *sp){
  if(sp == NULL || sp->fp == NULL)
    return -1;
  // Create Ogg container with Opus codec
  int const serial = rand(); // Unique stream serial number
  if (ogg_stream_init(&sp->oggState, serial) != 0) {
    fprintf(stderr, "Failed to initialize Ogg stream.\n");
    return -1;
  }
  sp->granulePosition = 0;
  sp->packetCount = 0;

  struct {
    char head[8];  // Signature
    uint8_t version;
    uint8_t channels;
    int16_t preskip;
    uint32_t samprate;
    int16_t gain;
    uint8_t map_family;
  } const opusHeader = {
    .head = "OpusHead",
    .version = 1,
    // Some decoders get confused when the channel count or sample rate changes in a stream, so always say we're emitting 48kHz stereo.
    // Opus won't use more bits when the input is actually mono and/or at a lower rate
    .channels = 2,
    .preskip = 312,
    .samprate = OPUS_SAMPRATE,
    .gain = 0,
    .map_family = 0
  };
  ogg_packet idPacket = {
    .packet = (unsigned char *)&opusHeader,
    .bytes = 19,
    .b_o_s = 1,
    .e_o_s = 0,
    .granulepos = 0,
    .packetno = sp->packetCount++
  };
  // Add the identification header to the Ogg stream
  ogg_stream_packetin(&sp->oggState, &idPacket);
  ogg_flush(sp);
  return 0;
}
static int emit_ogg_opus_tags(struct session *sp){
  if(sp == NULL || sp->fp == NULL)
    return -1;

  if(ogg_stream_check(&sp->oggState))
    return -1;

  // fill this in with sender ID (ka9q-radio, etc, frequency, mode, etc etc)
  uint8_t opusTags[2048] = { "OpusTags" }; // Variable length, not easily represented as a structure
  uint8_t *wp = opusTags + 8;
  wp = encodeTagString(wp,sizeof(opusTags) - (wp - opusTags),"KA9Q-radio"); // Vendor - don't bother computing actual remaining sp
  int32_t *np = (int32_t *)wp;
  *np++ = 8; // Number of tags follows
  wp = (uint8_t *)np;
  {
    char temp[256];
    snprintf(temp,sizeof(temp),"ENCODER=KA9Q radiod - %s",opus_get_version_string());
    wp = encodeTagString(wp,sizeof(opusTags) - (wp - opusTags),temp);
  }
  // We can get called whenever the status changes, so use the current wall clock, not the file creation time (sp->file_time)
  struct timespec now;
  clock_gettime(CLOCK_REALTIME,&now);
  struct tm tm;
  gmtime_r(&now.tv_sec,&tm);

  char datestring[128];
  char timestring[128];
  snprintf(datestring,sizeof(datestring),"%4d-%02d-%02d",
	   tm.tm_year+1900,
	   tm.tm_mon+1,
	   tm.tm_mday);

  snprintf(timestring,sizeof(timestring),"%02d:%02d:%02d.%03d UTC",
	   tm.tm_hour,
	   tm.tm_min,
	   tm.tm_sec,
	   (int)(now.tv_nsec / 1000000));
  {
    char temp[256];
    snprintf(temp,sizeof(temp),"TITLE=%s ssrc %u: %'.3lf Hz %s, %s %s",
	     sp->frontend.description,
	     sp->ssrc,sp->chan.tune.freq,sp->chan.preset,
	     datestring,timestring);
    wp = encodeTagString(wp,sizeof(opusTags) - (wp - opusTags),temp);
  }
  {
    char temp[256];
    snprintf(temp,sizeof(temp),"TIME=%s",timestring);
    wp = encodeTagString(wp,sizeof(opusTags) - (wp - opusTags),temp);
  }
  {
    char temp[256];
    snprintf(temp,sizeof(temp),"DATE=%s",datestring);
    wp = encodeTagString(wp,sizeof(opusTags) - (wp - opusTags),temp);
  }
  {
    char temp[256];
    snprintf(temp,sizeof(temp),"DESCRIPTION=%s",sp->frontend.description);
    wp = encodeTagString(wp,sizeof(opusTags) - (wp - opusTags),temp);
  }
  {
    char temp[256];
    snprintf(temp,sizeof(temp),"SSRC=%u",sp->ssrc);
    wp = encodeTagString(wp,sizeof(opusTags) - (wp - opusTags),temp);
  }
  {
    char temp[256];
    snprintf(temp,sizeof(temp),"FREQUENCY=%.3lf",sp->chan.tune.freq);
    wp = encodeTagString(wp,sizeof(opusTags) - (wp - opusTags),temp);
  }
  {
    char temp[256];
    snprintf(temp,sizeof(temp),"PRESET=%s",sp->chan.preset);
    wp = encodeTagString(wp,sizeof(opusTags) - (wp - opusTags),temp);
  }
  ogg_packet tagsPacket = {
    .packet = opusTags,
    .bytes = wp - opusTags,
    .b_o_s = 0,             // Not the beginning of the stream
    .e_o_s = 0,             // Not the end of the stream
    .granulepos = sp->granulePosition,        // No granule position for metadata
    .packetno = sp->packetCount++          // Second packet in the stream
  };

  ogg_stream_packetin(&sp->oggState, &tagsPacket);
  ogg_flush(sp);
  // Remember so we'll detect changes
  sp->last_frequency = sp->chan.tune.freq;
  strlcpy(sp->last_preset,sp->chan.preset,sizeof(sp->last_preset));
  return 0;
}
static int end_ogg_opus_stream(struct session *sp){
  if(sp == NULL || sp->fp == NULL)
    return -1;

  if(ogg_stream_check(&sp->oggState))
    return -1;

  // Terminate ogg Opus file
  // Write an empty packet with the end bit set
  ogg_packet endPacket = {
    .packet = OpusSilence20,
    .bytes = sizeof(OpusSilence20),
    .b_o_s = 0,
    .e_o_s = 1,    // End of stream flag
    .granulepos = sp->granulePosition, // Granule position, doesn't change
    .packetno = sp->packetCount++ // Increment packet number (not actually used again)
  };

  // Add the packet to the Ogg stream
  int const ret = ogg_stream_packetin(&sp->oggState, &endPacket);
  (void)ret;
  assert(ret == 0);
  // Flush the stream to ensure packets are written
  ogg_flush(sp);
  ogg_stream_clear(&sp->oggState);
  return 0;
}


// Encode a string as {length,string}, with length in 4 bytes, little endian, string without terminating null
// Return pointer to first unused byte in output
// Used in writing Ogg tags
static uint8_t *encodeTagString(uint8_t *out,size_t size,const char *string){
  if(out == NULL || string == NULL || size <= sizeof(uint32_t))
    return out;

  size_t const len = strlen(string);
  uint32_t * const wp = (uint32_t *)out;
  *wp = (uint32_t)len;
  uint8_t *sp = out + sizeof(uint32_t);
  size -= sizeof(uint32_t);
  memcpy(sp,string,min(len,size));
  sp += min(len,size);
  return sp;
}
// Write WAV header at start of file
// Leave file positioned after header
static int start_wav_stream(struct session *sp){
  if(sp == NULL || sp->fp == NULL)
    return -1;

  // fill in the auxi chunk (start time, center frequency)
  time_t tt = sp->file_time / BILLION;
  struct tm tm;
  gmtime_r(&tt,&tm);

  // Construct and write .wav header, skipping size fields
  struct wav header = {
    .ChunkID = "RIFF",
    .ChunkSize = 0xffffffff, // Temporary
    .Format = "WAVE",
    .Subchunk1ID = "fmt ",
    .Subchunk1Size = 40,
    .NumChannels = (int16_t)sp->channels,
    .SampleRate = sp->samprate,
    .ExtensionChunkSize = 22,
    .SubChunk2ID = "data",
    .Subchunk2Size = 0xffffffff, // Temporary
    // appears to be needed for FP
    .FactID = "fact",
    .FactSize = 4,
    .SamplesLength = 0xffffffff,
    .AuxID = "auxi",
    .AuxSize = 164,
    .CenterFrequency = (int32_t)sp->chan.tune.freq,
    //  header.AuxUknown is zeroed
    .StartYear = (int16_t)tm.tm_year+1900,
    .StartMon = (int16_t)tm.tm_mon+1,
    .StartDOW = (int16_t)tm.tm_wday,
    .StartDay = (int16_t)tm.tm_mday,
    .StartHour = (int16_t)tm.tm_hour,
    .StartMinute = (int16_t)tm.tm_min,
    .StartSecond = (int16_t)tm.tm_sec,
    .StartMillis = (int16_t)((sp->file_time / 1000000) % 10),
  };
  switch(sp->encoding){
  default:
    return -1;
  case S16LE:
  case S16BE:
    header.AudioFormat = 1;
    header.BitsPerSample = 8 * sizeof(int16_t);
    header.ByteRate = sp->samprate * sp->channels * sizeof(int16_t);
    header.BlockAlign = (int16_t)(sp->channels * sizeof(int16_t));
    break;
  case F32LE:
  case F32BE:
    header.AudioFormat = 3;
    header.BitsPerSample = 8 * sizeof(float);
    header.ByteRate = sp->samprate * sp->channels * sizeof(float);
    header.BlockAlign = (int16_t)(sp->channels * sizeof(float));
    break;
  case F16LE:
  case F16BE:
    header.AudioFormat = 0; // What should go here for IEEE 16-bit float?
    header.BitsPerSample = 8 * 2;
    header.ByteRate = sp->samprate * sp->channels * 2; // should be sizeof(float16)
    header.BlockAlign = (int16_t)(sp->channels * 2);
    break;
  }

  if(sp->can_seek)
    rewind(sp->fp); // should be at BOF but make sure
  fwrite(&header,sizeof(header),1,sp->fp);
  return 0;
}
// Update wav header with now-known size and end auxi information
static int end_wav_stream(struct session *sp){
  if(sp == NULL || sp->fp == NULL)
    return -1;

  if(!sp->can_seek)
    return 0; // Can't seek back to the beginning on a pipe

  rewind(sp->fp);
  struct wav header;
  if(fread(&header,sizeof(header),1,sp->fp) != 1)
    return -1;

  struct stat statbuf;
  if(fstat(fileno(sp->fp),&statbuf) != 0){
    fprintf(stderr,"fstat(%d) [%s] failed! %s\n",fileno(sp->fp),sp->filename,strerror(errno));
    return -1;
  }
  header.ChunkSize = (int32_t)(statbuf.st_size - 8);
  header.Subchunk2Size = (int32_t)(statbuf.st_size - sizeof(header));

  // write number of samples (or is it frames?) into the fact chunk
  header.SamplesLength = (uint32_t)sp->samples_written;

  // write end time into the auxi chunk
  struct timespec now;
  clock_gettime(CLOCK_REALTIME,&now);
  struct tm tm;
  gmtime_r(&now.tv_sec,&tm);
  header.StopYear = (int16_t)tm.tm_year + 1900;
  header.StopMon = (int16_t)tm.tm_mon + 1;
  header.StopDOW = (int16_t)tm.tm_wday;
  header.StopDay = (int16_t)tm.tm_mday;
  header.StopHour = (int16_t)tm.tm_hour;
  header.StopMinute = (int16_t)tm.tm_min;
  header.StopSecond = (int16_t)tm.tm_sec;
  header.StopMillis = (int16_t)(now.tv_nsec / 1000000);

  time_t tt = sp->file_time / BILLION;
  gmtime_r(&tt,&tm);
  header.StartYear = (int16_t)tm.tm_year + 1900;
  header.StartMon = (int16_t)tm.tm_mon + 1;
  header.StartDOW = (int16_t)tm.tm_wday;
  header.StartDay = (int16_t)tm.tm_mday;
  header.StartHour = (int16_t)tm.tm_hour;
  header.StartMinute = (int16_t)tm.tm_min;
  header.StartSecond = (int16_t)tm.tm_sec;
  header.StartMillis = (int16_t)(((sp->file_time / 1000000) % 10));

  rewind(sp->fp);
  if(fwrite(&header,sizeof(header),1,sp->fp) != 1)
    return -1;
  return 0;
}
