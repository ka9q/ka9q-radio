// Read and record PCM audio streams
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
#include <ogg/ogg.h>

#include "misc.h"
#include "attr.h"
#include "multicast.h"
#include "radio.h"

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

  struct channel chan;
  struct frontend frontend;
  int type;                    // RTP payload type (with marker stripped)
  int channels;                // 1 (PCM_MONO) or 2 (PCM_STEREO)
  unsigned int samprate;       // implicitly 48 kHz in PCM
  enum encoding encoding;
  ogg_stream_state oggState;   // For ogg Opus
  bool oggfirst;               // Set after first ogg packet
  int64_t granulePosition;
  int last_packetsize;          // Samples in last Opus packet (used for error padding)
  int packetCount;
  struct reseq {
    struct rtp_header rtp;
    uint8_t *data;
    int size;
    bool inuse;
  } reseq[16];                 // Reseqencing queue


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

static int Input_fd,Status_fd;
static struct session *Sessions;
static int64_t Timeout = 20; // 20 seconds max idle time before file close
int Mcast_ttl;
struct sockaddr_storage Metadata_dest_socket;


static void closedown(int a);
static void input_loop(void);
static void cleanup(void);
int session_file_init(struct session *sp,struct sockaddr const *sender);
static int close_file(struct session **spp);
static uint8_t *encodeTagString(uint8_t *out,const char *string);

static struct option Options[] = {
  {"directory", required_argument, NULL, 'd'},
  {"locale", required_argument, NULL, 'l'},
  {"minfiletime", required_argument, NULL, 'm'},
  {"mintime", required_argument, NULL, 'm'},
  {"subdirectories", no_argument, NULL, 's'},
  {"subdirs", no_argument, NULL, 's'},
  {"timeout", required_argument, NULL, 't'},
  {"verbose", no_argument, NULL, 'v'},
  {"lengthlimit", required_argument, NULL, 'L'},
  {"limit", required_argument, NULL, 'L'},
  {"version", no_argument, NULL, 'V'},
  {NULL, no_argument, NULL, 0},
};
static char Optstring[] = "d:l:m:st:vL:V";

int main(int argc,char *argv[]){
  App_path = argv[0];

  // Defaults
  Locale = getenv("LANG");

  int c;
  while((c = getopt_long(argc,argv,Optstring,Options,NULL)) != EOF){
    switch(c){
    case 'd':
      Recordings = optarg;
      break;
    case 'l':
      Locale = optarg;
      break;
    case 'm':
      SubstantialFileTime = strtof(optarg,NULL);
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
      Verbose++;
      break;
    case 'L':
      FileLengthLimit = strtof(optarg,NULL);
      break;
    case 'V':
      VERSION();
      exit(EX_OK);
    default:
      fprintf(stderr,"Usage: %s [-s] [-d directory] [-l locale] [-L maxtime] [-t timeout] [-v] [-m sec] PCM_multicast_address\n",argv[0]);
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
  setlocale(LC_ALL,Locale);
  setlinebuf(stdout); // In case we're redirected to a file

  // Set up input socket for multicast data stream from front end
  {
    struct sockaddr_storage sock;
    char iface[1024];
    resolve_mcast(PCM_mcast_address_text,&sock,DEFAULT_RTP_PORT,iface,sizeof(iface),0);
    Input_fd = listen_mcast(&sock,iface);
    resolve_mcast(PCM_mcast_address_text,&sock,DEFAULT_STAT_PORT,iface,sizeof(iface),0);
    Status_fd = listen_mcast(&sock,iface);
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
  if(strlen(Recordings) > 0 && chdir(Recordings) != 0){
    fprintf(stderr,"Can't change to directory %s: %s\n",Recordings,strerror(errno));
    exit(EX_CANTCREAT);
  }
  input_loop(); // Doesn't return

  exit(EX_OK);
}

static void closedown(int a){
  if(Verbose)
    fprintf(stderr,"%s: caught signal %d: %s\n",App_path,a,strsignal(a));

  cleanup();
  exit(EX_OK);  // Will call cleanup()
}

static uint8_t OggSilence[3] = {0xf8,0xff,0xfe};


// if default_framesize == 0, send whatever's on the queue, up to the first missing segment
// otherwise, empty the entire queue, skipping entry entries.
int send_opus_queue(struct session * const sp,int default_framesize){
  // Anything on the resequencing queue we can now process?
  int count = 0;
  for(int i=0; i < 16; i++,sp->rtp_state.seq++){
    struct reseq *qp = &sp->reseq[sp->rtp_state.seq & 15];
    if(!qp->inuse && default_framesize == 0)
      break; // Stop here if we're not resynchronizing

    ogg_packet oggPacket;
    int samples;
    oggPacket.e_o_s = 0;    // End of stream flag
    oggPacket.packetno = sp->packetCount++; // Increment packet number
    if(qp->inuse){
      oggPacket.packet = qp->data;
      oggPacket.bytes = qp->size;
      samples = opus_packet_get_nb_samples(qp->data,qp->size,48000); // Number of 48 kHz samples
      sp->last_packetsize = samples;
      if(qp->rtp.timestamp != sp->rtp_state.timestamp){
	int jump = (int32_t)(qp->rtp.timestamp - sp->rtp_state.timestamp);
	sp->granulePosition += jump;
      }
    } else {
      oggPacket.packet = OggSilence;
      samples = default_framesize;
      oggPacket.bytes = sizeof(OggSilence);
    }
    // Adjust the granule position
    sp->granulePosition += samples; // points to end of this packet
    oggPacket.granulepos = sp->granulePosition; // Granule position
    sp->rtp_state.timestamp += samples; // also ready for next
    sp->total_file_samples += samples;
    sp->current_segment_samples += samples;
    sp->samples_written += samples;
    if(sp->current_segment_samples >= SubstantialFileTime * sp->samprate)
      sp->substantial_file = true;
    
    // Add the packet to the Ogg stream
    if(Verbose > 1 || (Verbose > 0 && default_framesize > 0))
      fprintf(stdout,"writing from rtp sequence %u, timestamp %u: bytes %ld samples %d granule %lld\n",
	      sp->rtp_state.seq,sp->rtp_state.timestamp,oggPacket.bytes,samples,
	      (long long)oggPacket.granulepos);
    
    int ret = ogg_stream_packetin(&sp->oggState, &oggPacket);
    if (ret != 0) {
      fprintf(stderr, "Failed to write packet to Ogg stream.\n");
      // what to do here?
    }
    // Flush the stream to ensure packets are written
    ogg_page oggPage;
    while (ogg_stream_pageout(&sp->oggState, &oggPage)) {
      fwrite(oggPage.header, 1, oggPage.header_len, sp->fp);
      fwrite(oggPage.body, 1, oggPage.body_len, sp->fp);
    }
    FREE(qp->data); // OK if NULL
    qp->size = 0;
    qp->inuse = false;
    count++;
  }
  return count;
}


// Read both data and status from RTP network socket, assemble blocks of samples
// Doing both in one thread avoids a lot of synchronization problems with the session structure, since both write it
static void input_loop(){
  struct sockaddr sender;
  while(true){
    int64_t current_time = gps_time_ns();

    // Receive status or data
    struct pollfd pfd[2];
    pfd[0].fd = Input_fd;
    pfd[1].fd = Status_fd;
    pfd[1].events = pfd[0].events = POLLIN;
    pfd[1].revents = pfd[0].revents = 0;

    int const n = poll(pfd,sizeof(pfd)/sizeof(pfd[0]),1000); // Wait 1 sec max so we can scan active session list
    if(n < 0)
      break; // error of some kind - should we exit or retry?
    if(pfd[1].revents & (POLLIN|POLLPRI)){
      // Process status packet
      uint8_t buffer[PKTSIZE];
      socklen_t socksize = sizeof(sender);
      int length = recvfrom(Status_fd,buffer,sizeof(buffer),0,&sender,&socksize);
      if(length <= 0){    // ??
	perror("recvfrom");
	goto statdone; // Some sort of error
      }
      if(buffer[0] != STATUS)
	goto statdone;
      // Extract just the SSRC to see if the session exists
      // NB! Assumes same IP source address *and UDP source port* for status and data
      // This is only true for recent versions of radiod, after the switch to unconnected output sockets
      // But older versions don't send status on the output channel anyway, so no problem
      struct channel chan;
      memset(&chan,0,sizeof(chan));
      struct frontend frontend;
      memset(&frontend,0,sizeof(frontend));
      decode_radio_status(&frontend,&chan,buffer+1,length-1);

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
      if(sp == NULL){
	// Create session and initialize
	sp = calloc(1,sizeof(*sp));
	if(sp == NULL)
	  goto statdone; // unlikely

	sp->ssrc = chan.output.rtp.ssrc;
	sp->type = chan.output.rtp.type;
	sp->channels = chan.output.channels;
	sp->encoding = chan.output.encoding;
	sp->samprate = chan.output.samprate;
	memcpy(&sp->sender,&sender,sizeof(sp->sender));
	memcpy(&sp->chan,&chan,sizeof(sp->chan));
	memcpy(&sp->frontend,&frontend,sizeof(sp->frontend));
	
	sp->prev = NULL;
	sp->next = Sessions;
	if(sp->next)
	  sp->next->prev = sp;
	Sessions = sp;
      }
    }

  statdone:;
    if(pfd[0].revents & (POLLIN|POLLPRI)){
      uint8_t buffer[PKTSIZE];
      socklen_t socksize = sizeof(sender);
      int size = recvfrom(Input_fd,buffer,sizeof(buffer),0,&sender,&socksize);
      if(size <= 0){    // ??
	perror("recvfrom");
	continue; // Some sort of error, quit
      }
      if(size < RTP_MIN_SIZE)
	continue; // Too small for RTP, ignore

      struct rtp_header rtp;
      uint8_t *dp = (uint8_t *)ntoh_rtp(&rtp,buffer);
      if(rtp.pad){
	// Remove padding
	size -= dp[size-1];
	rtp.pad = 0;
      }
      if(size <= 0)
	continue; // Bogus RTP header

      size -= (dp - buffer);

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
	continue;
      
      if(sp->fp == NULL)
	session_file_init(sp,&sender);

      if(sp->encoding == OPUS){
	if(!sp->rtp_state.init){
	  sp->rtp_state.seq = rtp.seq;
	  sp->rtp_state.timestamp = rtp.timestamp;
	  sp->rtp_state.init = true;
	  if(Verbose)
	    fprintf(stdout,"init seq %u timestamp %u\n",rtp.seq,rtp.timestamp);
	}
	int16_t const seqdiff = rtp.seq - sp->rtp_state.seq;
	if(seqdiff >= 0 && seqdiff < 16){
	  if(Verbose > 1)
	    fprintf(stdout,"queue sequence %u timestamp %u bytes %d\n",rtp.seq,rtp.timestamp,size);
	  int qi = rtp.seq & 15;
	  struct reseq * const qp = &sp->reseq[qi];
	  qp->inuse = true;
	  qp->rtp = rtp;
	  qp->data = malloc(size);
	  memcpy(qp->data,dp,size);
	  qp->size = size;
	  send_opus_queue(sp,0); // Transmit any pending packets
	} else if((int16_t)(rtp.seq - sp->rtp_state.seq) < 0){
	  // old duplicate, drop
	  if(Verbose)
	    fprintf(stdout,"drop old dupe sequence %u timestamp %u bytes %d\n",rtp.seq,rtp.timestamp,size);
	  // But could be a resynch, test for this ****
	} else if ((int16_t)(rtp.seq - sp->rtp_state.seq) > 15){
	  // too far ahead to resequence, flush what we have
	  // But could be a possible resync or long outage, test for this ****
	  if(Verbose)
	    fprintf(stdout,"flushing with drops\n");
	  send_opus_queue(sp,sp->last_packetsize); // use the last packet size for sample stuffing
	  if(Verbose)
	    fprintf(stdout,"reset & queue sequence %u timestamp %u bytes %d\n",rtp.seq,rtp.timestamp,size);

	  int qi = sp->rtp_state.seq & 15;
	  struct reseq * const qp = &sp->reseq[qi];
	  qp->inuse = true;
	  qp->rtp = rtp;
	  qp->data = malloc(size);
	  memcpy(qp->data,dp,size);
	  qp->size = size;
	  send_opus_queue(sp,0); // Transmit any pending packets
	}
      } else { // PCM
        if (0 == sp->samples_remaining)
          continue;
	int const samp_size = sp->encoding == F32LE ? 4 : 2;
	int const samp_count = size / samp_size;
	
	// A "sample" is a single audio sample, usually 16 bits.
	// A "frame" is the same as a sample for mono. It's two audio samples for stereo
	int const frame_count = samp_count / sp->channels; // 1 every sample period (e.g., 4 for stereo 16-bit)
	off_t const offset = rtp_process(&sp->rtp_state,&rtp,frame_count); // rtp timestamps refer to frames
	
	// Ignore stray packets with extreme timestamps beyond the session timeout
	// They should never be valid because the session will have closed first
	if(offset > sp->samprate * Timeout)
	  continue;
	
	// The seek offset relative to the current position in the file is the signed (modular) difference between
	// the actual and expected RTP timestamps. This should automatically handle
	// 32-bit RTP timestamp wraps, which occur every ~1 days at 48 kHz and only 6 hr @ 192 kHz
	// Should I limit the range on this?
	
	if(offset != 0){
	  int const r = fseeko(sp->fp,offset * samp_size * sp->channels,SEEK_CUR); // offset is in bytes - will this allocate new space? *****
	  if(r != 0)
	    continue; // Probably before start of file
	  if(offset > 0)
	    sp->current_segment_samples = 0;
	}
	sp->total_file_samples += samp_count + offset;
	sp->current_segment_samples += samp_count;
	sp->samples_written += samp_count;
	if(sp->current_segment_samples >= SubstantialFileTime * sp->samprate)
	  sp->substantial_file = true;
	
	if(sp->encoding == S16BE){
	  // Flip endianness from big-endian on network to little endian wanted by .wav
	  // byteswap.h is linux-specific; need to find a portable way to get the machine instructions
	  uint16_t wbuffer[samp_count];
	  int16_t const * const samples = (int16_t *)dp;
	  for(int n = 0; n < samp_count; n++)
	    wbuffer[n] = bswap_16((uint16_t)samples[n]);
	  fwrite(wbuffer,sizeof(*wbuffer),samp_count,sp->fp);
	} else if(sp->encoding != OPUS) {
	  fwrite(dp,1,size,sp->fp); // Just write raw bytes
	}
        sp->samples_remaining -= samp_count;
      }
      sp->last_active = gps_time_ns();
    } // end of packet processing

    // check to see if all sessions are complete
    bool all_complete = true;
    for(struct session *sp = Sessions;sp != NULL;sp = sp->next)
      all_complete &= (0 == sp->samples_remaining);

    if(all_complete){
      cleanup();
      exit(EX_OK);
    }
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
int session_file_init(struct session *sp,struct sockaddr const *sender){
  if(sp->fp != NULL)
    return 0;
  sp->samples_remaining = sp->samprate * FileLengthLimit * sp->channels; // If file is being limited in length
  // Create file
  // Should we append to existing files instead? If we try this, watch out for timestamp wraparound
  struct timespec now;
  clock_gettime(CLOCK_REALTIME,&now);
  struct tm const * const tm = gmtime(&now.tv_sec);
  // yyyy-mm-dd-hh:mm:ss so it will sort properly

  sp->fp = NULL;
  char const *suffix = ".wav";
  switch(sp->encoding){
  case S16BE:
  case S16LE:
  case F32LE:
    suffix = ".wav";
    break;
  case F16LE:
    suffix = ".f16"; // Non standard! But gotta do something with it for now
    break;
  case OPUS:
    suffix = ".opus";
    break;
  default:
    suffix = ".raw";
    break;
  }

  if(Subdirs){
    // Create directory path
    char dir[PATH_MAX];
    snprintf(dir,sizeof(dir),"%u",sp->ssrc);
    if(mkdir(dir,0777) == -1 && errno != EEXIST){
      fprintf(stderr,"can't create directory %s: %s\n",dir,strerror(errno));
      return -1;
    }
    snprintf(dir,sizeof(dir),"%u/%d",sp->ssrc,tm->tm_year+1900);
    if(mkdir(dir,0777) == -1 && errno != EEXIST){
      fprintf(stderr,"can't create directory %s: %s\n",dir,strerror(errno));
      return -1;
    }
    snprintf(dir,sizeof(dir),"%u/%d/%d",sp->ssrc,tm->tm_year+1900,tm->tm_mon+1);
    if(mkdir(dir,0777) == -1 && errno != EEXIST){
      fprintf(stderr,"can't create directory %s: %s\n",dir,strerror(errno));
      return -1;
    }
    snprintf(dir,sizeof(dir),"%u/%d/%d/%d",sp->ssrc,tm->tm_year+1900,tm->tm_mon+1,tm->tm_mday);
    if(mkdir(dir,0777) == -1 && errno != EEXIST){
      fprintf(stderr,"can't create directory %s: %s\n",dir,strerror(errno));
      return -1;
    }
    snprintf(sp->filename,sizeof(sp->filename),"%u/%d/%d/%d/%uk%4d-%02d-%02dT%02d:%02d:%02d.%dZ%s",
	     sp->ssrc,
	     tm->tm_year+1900,
	     tm->tm_mon+1,
	     tm->tm_mday,
	     sp->ssrc,
	     tm->tm_year+1900,
	     tm->tm_mon+1,
	     tm->tm_mday,
	     tm->tm_hour,
	     tm->tm_min,
	     tm->tm_sec,
	     (int)(now.tv_nsec / 100000000), // 100 million, i.e., convert to tenths of a sec
	     suffix);
  } else {
    // create file in current directory
    snprintf(sp->filename,sizeof(sp->filename),"%uk%4d-%02d-%02dT%02d:%02d:%02d.%dZ%s",
	     sp->ssrc,
	     tm->tm_year+1900,
	     tm->tm_mon+1,
	     tm->tm_mday,
	     tm->tm_hour,
	     tm->tm_min,
	     tm->tm_sec,
	     (int)(now.tv_nsec / 100000000),
	     suffix);
  }
  sp->fp = fopen(sp->filename,"w+");
  if(sp->fp == NULL){
    fprintf(stderr,"can't create/write file %s: %s\n",sp->filename,strerror(errno));
    return -1;
  }

  if(Verbose)
    fprintf(stdout,"creating %s ssrc %u samprate %d channels %d encoding %s freq %.3lf preset %s\n",
	    sp->filename,sp->ssrc,sp->samprate,sp->channels,encoding_string(sp->encoding),sp->chan.tune.freq,sp->chan.preset);

  sp->iobuffer = malloc(BUFFERSIZE);
  setbuffer(sp->fp,sp->iobuffer,BUFFERSIZE);

  int const fd = fileno(sp->fp);
  fcntl(fd,F_SETFL,O_NONBLOCK); // Let's see if this keeps us from losing data

  attrprintf(fd,"ssrc","%u",sp->ssrc);
  attrprintf(fd,"samplerate","%d",sp->samprate);
  attrprintf(fd,"channels","%d",sp->channels);

  attrprintf(fd,"sampleformat","%s",encoding_string(sp->encoding));
  attrprintf(fd,"frequency","%.3lf",sp->chan.tune.freq);
  attrprintf(fd,"preset","%s",sp->chan.preset);

  if(sp->encoding == OPUS){
    int serial = rand(); // Unique stream serial number
    if (ogg_stream_init(&sp->oggState, serial) != 0) {
      fprintf(stderr, "Failed to initialize Ogg stream.\n");
      return -1;
    }
    sp->granulePosition = 0;
    sp->packetCount = 0;

    unsigned char opusHeader[19];
    memcpy(opusHeader, "OpusHead", 8);  // Signature
    opusHeader[8] = 1;                  // Version
    opusHeader[9] = 2;                  // Channel count (e.g., stereo)
    opusHeader[10] = 56;                // Pre-skip (low byte) 312
    opusHeader[11] = 1;                 // Pre-skip (high byte)
    opusHeader[12] = 0x80;              // Input sample rate (48 kHz, low byte)
    opusHeader[13] = 0xBB;              // Input sample rate (48 kHz, 2nd byte)
    opusHeader[14] = 0x00;              // Input sample rate (48 kHz, 3rd byte)
    opusHeader[15] = 0x00;              // Input sample rate (48 kHz, high byte)
    opusHeader[16] = 0;                 // Output gain (low byte)
    opusHeader[17] = 0;                 // Output gain (high byte)
    opusHeader[18] = 0;                 // Mapping family (0 = single stream)
    
    ogg_packet idPacket;
    idPacket.packet = opusHeader;
    idPacket.bytes = sizeof(opusHeader);
    idPacket.b_o_s = 1;  // Beginning of stream
    idPacket.e_o_s = 0;
    idPacket.granulepos = 0; // always zero
    idPacket.packetno = sp->packetCount++;
    // Add the identification header to the Ogg stream
    ogg_stream_packetin(&sp->oggState, &idPacket);
    ogg_page oggPage;
    while (ogg_stream_flush(&sp->oggState, &oggPage)) {
      fwrite(oggPage.header, 1, oggPage.header_len, sp->fp);
      fwrite(oggPage.body, 1, oggPage.body_len, sp->fp);
    }
    // fill these in with sender ID (ka9q-radio, etc, frequency, mode, etc etc)
    uint8_t opusTags[2048];
    memset(opusTags,0,sizeof(opusTags));
    memcpy(opusTags,"OpusTags",8);
    uint8_t *wp = opusTags + 8;
    wp = encodeTagString(wp,"KA9Q-radio"); // Vendor
    int32_t *np = (int32_t *)wp;
    *np++ = 5; // Number of tags follows
    wp = (uint8_t *)np;
    char temp[64];
    snprintf(temp,sizeof(temp),"ENCODER=KA9Q radiod");
    wp = encodeTagString(wp,temp);
    snprintf(temp,sizeof(temp),"TITLE=ka9q-radio, ssrc %u: %'.3lf Hz %s",sp->ssrc,sp->chan.tune.freq,sp->chan.preset);
    wp = encodeTagString(wp,temp);
    snprintf(temp,sizeof(temp),"SSRC=%u",sp->ssrc);
    wp = encodeTagString(wp,temp);
    snprintf(temp,sizeof(temp),"FREQUENCY=%.3lf",sp->chan.tune.freq);
    wp = encodeTagString(wp,temp);
    snprintf(temp,sizeof(temp),"PRESET=%s",sp->chan.preset);
    wp = encodeTagString(wp,temp);
    
    ogg_packet tagsPacket;
    tagsPacket.packet = opusTags;
    tagsPacket.bytes = wp - opusTags;
    tagsPacket.b_o_s = 0;             // Not the beginning of the stream
    tagsPacket.e_o_s = 0;             // Not the end of the stream
    tagsPacket.granulepos = 0;        // No granule position for metadata
    tagsPacket.packetno = sp->packetCount++;          // Second packet in the stream
    
    ogg_stream_packetin(&sp->oggState, &tagsPacket);
    while (ogg_stream_flush(&sp->oggState, &oggPage)) {
      fwrite(oggPage.header, 1, oggPage.header_len, sp->fp);
      fwrite(oggPage.body, 1, oggPage.body_len, sp->fp);
    }
  } else { // Not opus
    // Write .wav header, skipping size fields
    memcpy(sp->header.ChunkID,"RIFF", 4);
    sp->header.ChunkSize = 0xffffffff; // Temporary
    memcpy(sp->header.Format,"WAVE",4);
    memcpy(sp->header.Subchunk1ID,"fmt ",4);
    sp->header.Subchunk1Size = 16; // ?????? 
    switch(sp->encoding){
    default: // Not sure what else to do. Opus files should be written in .opus files
    case S16LE:
    case S16BE:
      sp->header.AudioFormat = 1;
      sp->header.BitsPerSample = 16;
      sp->header.ByteRate = sp->samprate * sp->channels * 2;
      sp->header.BlockAlign = sp->channels * 2;;
      break;
    case F32LE:
      sp->header.AudioFormat = 3;    
      sp->header.BitsPerSample = 32;
      sp->header.ByteRate = sp->samprate * sp->channels * 4;
      sp->header.BlockAlign = sp->channels * 4;
      break;
    case F16LE:
      sp->header.AudioFormat = 0; // What should go here for IEEE 16-bit float?
      sp->header.BitsPerSample = 16;
      sp->header.ByteRate = sp->samprate * sp->channels * 2;
      sp->header.BlockAlign = sp->channels * 2;;
      break;
    }
    
    sp->header.NumChannels = sp->channels;
    sp->header.SampleRate = sp->samprate;
    
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
    sp->header.CenterFrequency= sp->chan.tune.freq;
    memset(sp->header.AuxUknown, 0, 128);
    
    fwrite(&sp->header,sizeof(sp->header),1,sp->fp);
    fflush(sp->fp); // get at least the header out there
  }
  char sender_text[NI_MAXHOST];
  // Don't wait for an inverse resolve that might cause us to lose data
  getnameinfo((struct sockaddr *)sender,sizeof(*sender),sender_text,sizeof(sender_text),NULL,0,NI_NOFQDN|NI_DGRAM|NI_NUMERICHOST);
  attrprintf(fd,"source","%s",sender_text);
  attrprintf(fd,"multicast","%s",PCM_mcast_address_text);

  attrprintf(fd,"unixstarttime","%ld.%09ld",(long)now.tv_sec,(long)now.tv_nsec);
  return 0;
}

// Close a session, update .wav header, remove from session table
// If the file is not "substantial", just delete it
static int close_file(struct session **spp){
  struct session *sp = *spp;

  if(spp == NULL || sp == NULL || sp->fp == NULL)
    return 0;

  if(sp->substantial_file){ // Don't bother for non-substantial files
    if(Verbose){
      fprintf(stdout,"closing %s %'.1f/%'.1f sec\n",sp->filename,
            (float)sp->samples_written / (sp->samprate * sp->channels),
            (float)sp->total_file_samples / (sp->samprate * sp->channels));
    }
    // Get final file size, write .wav header with sizes
    fflush(sp->fp);
    if(sp->encoding == OPUS){
      // Terminate ogg Opus file
      // Write an empty packet with the end bit set
	ogg_packet endPacket;
	endPacket.packet = OggSilence;
	endPacket.bytes = sizeof(OggSilence);
	endPacket.e_o_s = 1;    // End of stream flag
	endPacket.granulepos = sp->granulePosition; // Granule position
	sp->granulePosition += 0; // Doesn't change
	endPacket.packetno = sp->packetCount++; // Increment packet number (not actually used again)
	
	// Add the packet to the Ogg stream
	if (ogg_stream_packetin(&sp->oggState, &endPacket) != 0) {
	  fprintf(stderr, "Failed to write packet to Ogg stream.\n");
	  return -1;
	}
	// Flush the stream to ensure packets are written
	ogg_page finalPage;
	while (ogg_stream_flush(&sp->oggState, &finalPage)) {
	  fwrite(finalPage.header, 1, finalPage.header_len, sp->fp);
	  fwrite(finalPage.body, 1, finalPage.body_len, sp->fp);
	}
    } else { // Not opus
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
    }
    fflush(sp->fp);
    // RTP processing should be smarter about counting these.
    // Packets received out of order are counted as a drop and a dupe, but are harmless here
    if(Verbose && (sp->rtp_state.dupes != 0 || sp->rtp_state.drops != 0))
      printf("file %s dupes %llu drops %llu\n",sp->filename,(long long unsigned)sp->rtp_state.dupes,(long long unsigned)sp->rtp_state.drops);
  } else {
    unlink(sp->filename);
    if(Verbose)
      printf("deleting %s %'.1f/%'.1f sec\n",sp->filename,
            (float)sp->samples_written / (sp->samprate * sp->channels),
            (float)sp->total_file_samples / (sp->samprate * sp->channels));
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
static uint8_t *encodeTagString(uint8_t *out,const char *string){
  if(out == NULL)
    return NULL;
  if(string == NULL)
    return out;

  int len = strlen(string);
  uint32_t *wp = (uint32_t *)out;
  *wp = len;
  uint8_t *sp = out + sizeof(uint32_t);
  memcpy(sp,string,len);
  sp += len;
  return sp;
}
