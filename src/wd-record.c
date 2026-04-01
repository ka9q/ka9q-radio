/**
@file pcmrecord.c
@author Phil Karn, KA9Q
@brief Record, stream, or launch commands with RTP streams as input
@verbatim
This program reads one or more RTP streams from a multicast group and either writes them into a file, streams (one of them) onto standard output, or invokes a command for each stream and pipes the RTP data into it. PCM streams are written as-is (except that big-endian PCM is converted to little-endian). Opus streams are placed in a standard Ogg container.

Command-line options:
 --stdout | --catmode | -c: write one stream to stdout. If --ssrc is not specified, selects the first one found and ignores the rest
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
 --jt|-j: Use K1JT format file names
 --locale <locale>: Set locale. Default is $LANG
 --mintime|--minfiletime|-m: minimum file duration, in sec. Files shorter than this are deleted when closed
 --raw|-r: Don't emit .WAV header for PCM files; ignored with Opus (Ogg is needed to delimit frames in a stream)
 --subdirectories|--subdirs|-s': Create subdirectories when writing files: ssrc/year/month/day/filename
 --timeout|-t <seconds>: Close file after idle period (default 20 sec)
 --verbose|-v: Increase verbosity level
 --lengthlimit|--limit|-L <seconds>: maximum file duration, seconds. When new file is created, round down to previous start of interval and pad with silence (for JT decoding)
 --ssrc <ssrc>: Select one SSRC (recommended for --stdout)
 --version|-V: display command version
 --max_length|-x: <seconds> maximum file duration, in seconds. Don't pad the wav file with silence. Exit when all files have reached max duration.
 --wd_mode|-W: wsprdeamon mode, sync start to multiple of --lengthlimit (defaults to 60 seconds if omitted), and also implies --jt file name format
@endverbatim
 */

// Read and record PCM/WAV and Ogg Opus audio streams
// Now with --stdout option to send (one) stream to standard output, eventually to replace pcmcat
// Also with --exec option to pipe stream into command, to replace pcmspawn
// Copyright 2021-2024 Phil Karn, KA9Q

// Read and record PCM audio streams
// Copyright 2021 Phil Karn, KA9Q
//
// Modified "wspr-decoded" to "wd-record" to record 1 minute .wav files, synchronized to the UTC
// second by Clint Turner, KA7OEI for use with the "WSPRDaemon" code by Rob Robinett, AI6VN.
//
// July 14, 2023 Rob Robinett, AI6VN.  Modified Clint's code to fully support creating the 1 minute long .wav files needed by WD
//
// TO DO:
//  - Cleanup from previous "wspr-decoded" version (e.g. remove unneeded variables/code)
//

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
#include <stdarg.h>
#include <syslog.h>
#include <sys/mman.h>
#include <complex.h>

#include "misc.h"
#include "attr.h"
#include "multicast.h"
#include "radio.h"
#include "status.h"

#include <math.h>

typedef struct {
    double x1, x2;
    double y1, y2;
} BiquadState;

/* Initialize notch filter at 500 Hz, fs = 16 kHz, pole radius r */
void notch500_init(double r, double *b0, double *b1, double *b2,
                   double *a1, double *a2, BiquadState *st, double sample_rate)
{
  double fs  = sample_rate;
  double f0  = 500.0;
  double w0  = 2.0 * M_PI * f0 / fs;    // pi/16
  double c   = cos(w0);                 // ~0.980785

  *b0 = 1.0;
  *b1 = -2.0 * c;
  *b2 = 1.0;

  *a1 = -2.0 * r * c;
  *a2 = r * r;

  st->x1 = st->x2 = st->y1 = st->y2 = 0.0;
}

/* Process one real sample through the biquad */
static inline double notch500_process(double x,
                                      double b0, double b1, double b2,
                                      double a1, double a2,
                                      BiquadState *st)
{
    double y = b0 * x + b1 * st->x1 + b2 * st->x2
                     - a1 * st->y1   - a2 * st->y2;

    st->x2 = st->x1;
    st->x1 = x;
    st->y2 = st->y1;
    st->y1 = y;
    return y;
}

BiquadState stI, stQ;
double b0, b1, b2, a1, a2;

// size of stdio buffer for disk I/O. 8K is probably the default, but we have this for possible tuning
#define BUFFERSIZE (8192) // probably the same as default
#define RESEQ 64 // size of resequence queue. Probably excessive; WiFi reordering is rarely more than 4-5 packets
#define OPUS_SAMPRATE 48000 // Opus always operates at 48 kHz virtual sample rate

enum sync_state_t
{
  sync_state_startup,           // any second; waiting for data to arrive in second :59
  sync_state_armed,             // second :59; waiting for data to arrive in second :00 to sync
  sync_state_active,            // recording data to file, wait for final samples to complete file
  sync_state_resync,
  sync_state_done,
};

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

  char filename[PATH_MAX];
  bool can_seek;               // file is regular; can seek on it

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
  int64_t granulePosition;
  int packetCount;
  struct reseq {
    struct rtp_header rtp;
    uint8_t *data;
    int size;
    bool inuse;
  } reseq[RESEQ];              // Reseqencing queue


  FILE *fp;                    // File being recorded
  void *iobuffer;              // Big buffer to reduce write rate
  int64_t last_active;         // gps time of last activity
  int64_t starting_offset;     // First actual sample in file past wav header (for time alignment)
  bool no_offset;              // Don't offset except on first file in series (with -L option)

  bool substantial_file;       // At least one substantial segment has been seen
  int64_t current_segment_samples; // total samples in this segment without skips in timestamp
  int64_t samples_written;
  int64_t total_file_samples;
  int64_t samples_remaining;   // Samples remaining before file is closed; 0 means indefinite
  struct timespec file_time;
  bool complete;

  enum sync_state_t sync_state;
  struct timespec end_time;
  struct timespec wd_file_time;
  uint32_t next_expected_rtp_ts;
  uint16_t next_expected_rtp_seq;
  uint32_t max_tx_queue;
  uint32_t max_rx_queue;
  uint32_t max_drops;
  uint32_t last_block_drops;
  bool session_errors_init;     // true once the session has run wd_check() at least once to set the next expected ts,seq

  float last_angle;
  uint32_t last_edge;
  uint32_t start_ts;
  int64_t start_timesnap;
  uint32_t start_sequence;
};

static struct {
  float noise_bandwidth;
  float sig_power;
  float sn0;
  float snr;
  int64_t pll_start_time;
  double pll_start_phase;
} Local;

struct sync_diag_t {
  char multicast[32];
  uint32_t magic;
  uint32_t version;
  uint32_t pid;
  uint32_t reserved;

  uint64_t start_ns;
  uint64_t updated_ns;
  uint64_t datagrams;
  uint64_t dropped_blocks;
  uint64_t seq_errors;
  uint64_t ts_errors;
  uint64_t pps_ok;
  uint64_t pps_noise;
  uint64_t pps_consecutive;
  double sync_frequency;
  float sync_snr;
};

struct sync_diag_t *sync_diags;
static int sync_diags_fd = -1;

static float SubstantialFileTime = 0.2;  // Don't record bursts < 250 ms unless they're between two substantial segments
static double FileLengthLimit = 0; // Length of file in seconds; 0 = unlimited
static double max_length = 0; // Length of recording in seconds; 0 = unlimited
static const double Tolerance = 1.0; // tolerance for starting time in sec when FileLengthLimit is active
int Verbose;
static char PCM_mcast_address_text[256];
static int64_t Timeout = 20; // 20 seconds max idle time before file close
static char const *Recordings = ".";
static bool Subdirs; // Place recordings in subdirectories by SSID
static char const *Locale;
static uint32_t Ssrc; // SSRC, when manually specified
static bool Catmode = false; // sending one channel to standard output
static bool Flushmode = false; // Flush after each packet when writing to standard output
static const char *Command = NULL;
static bool Jtmode = false;
static bool Raw = false;
static bool wd_mode = false;
static int force_sample_rate_error = 0;
static char const *wd_error_log = 0;
static double wd_tolerance_seconds = 2.0;
static uint32_t sync_ssrc = 0;
static float sync_frequency = 0;
static bool sync_record = false;
static uint32_t sync_start_ts;
static int32_t sync_pretrigger;
static char* radio_mcast_group = NULL;
static bool no_output = false;
static bool filter_500 = false;
static bool leaky_folding = false;
static double leaky_folding_filter = 0.99;
static bool notch_filter_initialized = false;

const char *App_path;
static int Input_fd,Status_fd,Control_fd;
static struct session *Sessions;
int Mcast_ttl;
struct sockaddr Metadata_dest_socket;
struct sockaddr mcast_dest_sock;
static char const *Source;
static struct sockaddr_storage *Source_socket; // Remains NULL if Source == NULL

static void closedown(int a);
static void input_loop(void);
static void cleanup(void);
int session_file_init(struct session *sp,struct sockaddr const *sender);
static int close_session(struct session **spp);
static int close_file(struct session *sp);
static uint8_t *encodeTagString(uint8_t *out,size_t size,const char *string);
static int start_ogg_opus_stream(struct session *sp);
static int emit_ogg_opus_tags(struct session *sp);
static int end_ogg_opus_stream(struct session *sp);
static int ogg_flush(struct session *sp);
static int start_wav_stream(struct session *sp);
static int end_wav_stream(struct session *sp);
static int send_wav_queue(struct session * const sp,bool flush);
static int send_opus_queue(struct session * const sp,bool flush);
static int send_queue(struct session * const sp,bool flush);

static struct option Options[] = {
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
  {"raw", no_argument, NULL, 'r' },
  {"subdirectories", no_argument, NULL, 's'},
  {"subdirs", no_argument, NULL, 's'},
  {"timeout", required_argument, NULL, 't'},
  {"verbose", no_argument, NULL, 'v'},
  {"lengthlimit", required_argument, NULL, 'L'},
  {"limit", required_argument, NULL, 'L'},
  {"ssrc", required_argument, NULL, 'S'},
  {"sync-ssrc", required_argument, NULL, 1001},
  {"sync-frequency", required_argument, NULL, 1002},
  {"sync-record", no_argument, NULL, 1003},
  {"sync-pretrigger", required_argument, NULL, 1004},
  {"no-output", no_argument, NULL, 1005},
  {"filter-500", no_argument, NULL, 1006},
  {"leaky-folding", no_argument, NULL, 1007},
  {"leaky-folding-filter", required_argument, NULL, 1008},
  {"version", no_argument, NULL, 'V'},
  {"max_length", required_argument, NULL, 'x'},
  {"wd_mode", no_argument, NULL, 'W'},
  {"error", required_argument, NULL, 'E'},
  {"wd_errors", required_argument, NULL, 'q'},
  {"wd_tolerance", required_argument, NULL, 'Y'},
  {NULL, no_argument, NULL, 0},
};
static char Optstring[] = "cd:e:fjl:m:o:rsS:t:vL:Vx:WE:q:Y:";

int main(int argc,char *argv[]){
  App_path = argv[0];
  openlog(App_path, LOG_PID | LOG_CONS, LOG_USER);
  // Defaults
  Locale = getenv("LANG");

  int c;
  while((c = getopt_long(argc,argv,Optstring,Options,NULL)) != EOF){
    switch(c){
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
      break;
    case 'd':
      Recordings = optarg;
      break;
    case 'l':
      Locale = optarg;
      break;
    case 'm':
      SubstantialFileTime = fabsf(strtof(optarg,NULL));
      break;
    case 'o':
      Source = optarg;
      break;
    case 'r':
      Raw = true;
      break;
    case 'S':
      {
        char *ptr;
        uint32_t x = strtol(optarg,&ptr,0);
        if(ptr != optarg)
          Ssrc = x;
      }
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
      FileLengthLimit = fabsf(strtof(optarg,NULL));
      break;
    case 'x':
      max_length = fabsf(strtof(optarg,NULL));
      break;
    case 'V':
      VERSION();
      fputs("wsprdaemon mode (-W): v0.15\n",stdout);
      exit(EX_OK);
    case 'W':
      wd_mode = true;
      Jtmode = true;
      if (0 == FileLengthLimit){
        FileLengthLimit = 60;
      }
      break;
    case 'E':
      {
        char *ptr;
        int32_t x = strtol(optarg,&ptr,0);
        if(ptr != optarg)
          force_sample_rate_error = x;
      }
      fprintf(stderr,"Warning: sample count error forced to %+d samples\n",force_sample_rate_error);
      break;
    case 'q':
      wd_error_log = optarg;
      break;
    case 'Y':
      wd_tolerance_seconds = fabsf(strtof(optarg,NULL));
      break;
    case 1001:
      {
        char *ptr;
        int32_t x = strtol(optarg,&ptr,0);
        if(ptr != optarg)
          sync_ssrc = x;
      }
      fprintf(stderr,"bpsk sync signal on SSRC %u\n",sync_ssrc);
      break;
    case 1002:
      {
        char *ptr;
        float x = strtod(optarg,&ptr);
        if(ptr != optarg)
          sync_frequency = x;
      }
      fprintf(stderr,"bpsk sync signal at %.0f Hz\n",sync_frequency);
      break;
    case 1003:
      sync_record = true;
      fprintf(stderr,"recording bpsk sync channel\n");
      break;
    case 1004:
      {
        char *ptr;
        int32_t x = strtol(optarg,&ptr,0);
        if(ptr != optarg)
          sync_pretrigger = x;
      }
      fprintf(stderr,"bpsk pretrigger at %d samples\n",sync_pretrigger);
      break;
    case 1005:
      no_output = true;
      break;
    case 1006:
      filter_500 = true;
      fprintf(stderr,"500 Hz notch filter engaged\n");
      break;
    case 1007:
      leaky_folding = true;
      fprintf(stderr,"Leaky folding engaged\n");
      break;
    case 1008:
      {
        char *ptr;
        double x = strtod(optarg,&ptr);
        if(ptr != optarg)
          leaky_folding_filter = x;
      }
      if ((leaky_folding_filter >= 1.0) || (leaky_folding_filter <= 0.0))
        leaky_folding_filter = 0.99;
      fprintf(stderr,"Leaky folding filter set to %.4f\n",leaky_folding_filter);
      break;
    default:
      fprintf(stderr,"Usage: %s [-c|--catmode|--stdout] [-r|--raw] [-e|--exec command] [-f|--flush] [-s] [-d directory] [-l locale] [-L maxtime] [-t timeout] [-j|--jt] [-v] [-m sec] [-x|--max_length max_file_time, no sync, oneshot] [--wd_mode|-W] PCM_multicast_address\n",argv[0]);
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
  if((Catmode || Command != NULL) && (Subdirs || Jtmode)){
    fprintf(stderr,"--stdout and --exec supersede --subdirs and --jtmode\n");
    Subdirs = false;
    Jtmode = false;
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
    struct sockaddr_storage sock = {0};
    char iface[1024] = {0};
    resolve_mcast(PCM_mcast_address_text,&mcast_dest_sock,DEFAULT_RTP_PORT,iface,sizeof(iface),0);
    Input_fd = listen_mcast(Source_socket,&mcast_dest_sock,iface);
    resolve_mcast(PCM_mcast_address_text,&sock,DEFAULT_STAT_PORT,iface,sizeof(iface),0);
    /* fprintf(stderr,"mcast group: %s\n",PCM_mcast_address_text); */
    /* fprintf(stderr,"mcast port: %d\n",DEFAULT_STAT_PORT); */
    /* fprintf(stderr,"mcast iface: %s\n",iface); */
    Status_fd = listen_mcast(Source_socket,&sock,iface);
    Control_fd = -1;
  }
  if(Input_fd == -1){
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

static double time_diff(struct timespec x,struct timespec y){
  double xd = (1.0e-9 * x.tv_nsec) + x.tv_sec;
  double yd = (1.0e-9 * y.tv_nsec) + y.tv_sec;
  return xd - yd;
}

static const char *wd_time(){
  struct timespec now;
  clock_gettime(CLOCK_REALTIME,&now);
  struct tm *tm_now = gmtime(&now.tv_sec);;
  static char timebuff[256];
  size_t s = strftime(timebuff,sizeof(timebuff),"%a %d %b %Y %H:%M:%S",tm_now);
  if (s) {
    snprintf(&timebuff[s],sizeof(timebuff)-s,".%03lu UTC: ", now.tv_nsec / 1000000);
  }
  return timebuff;
}

void wd_log(int v_level,const char *format,...) __attribute__ ((format (printf, 2, 3)));

void wd_log(int v_level,const char *format,...){
  if (Verbose < v_level){
    return;
  }
  va_list args;
  va_start(args,format);
  char *msg;
  if (vasprintf(&msg,format,args) >= 0){
    FILE *f = stderr;
    if ((wd_error_log) && (strlen(wd_error_log)))
      f = fopen(wd_error_log,"a");
    if (NULL == f){
      f = stderr;
    }
    fputs(wd_time(),f);
    fputs(msg,f);
    if (stderr != f){
      fclose(f);
    }
    FREE(msg);
  }
  va_end(args);
}

static void clear_queue_counters(struct session * const sp){
  sp->max_tx_queue = 0;
  sp->max_rx_queue = 0;
  sp->max_drops = 0;
}

static void wd_check(struct session * const sp,int buffer_size,struct rtp_header *rtp){
  /* if (!sp->session_errors_init){ */
  /*   wd_log(0,"wd_check(): SSRC %u first check of session?\n", */
  /*          sp->ssrc); */
  /* } */
  __atomic_fetch_add(&sync_diags->datagrams,1,__ATOMIC_RELAXED);

  // track sequence numbers and report if we see one out of order (except the first datagram of file)
  if ((sp->session_errors_init) && (rtp->seq != sp->next_expected_rtp_seq)){
    __atomic_fetch_add(&sync_diags->seq_errors,1,__ATOMIC_RELAXED);
    wd_log(0,"Weird rtp.seq: expected %u, received %u (delta %d) on SSRC %d (tx %u, rx %u, drops %u)\n",
           sp->next_expected_rtp_seq,
           rtp->seq,
           (int16_t)(rtp->seq - sp->next_expected_rtp_seq),
           sp->ssrc,
           sp->max_tx_queue,
           sp->max_rx_queue,
           sp->max_drops);
  }
  sp->next_expected_rtp_seq = rtp->seq + 1;    // next expected RTP sequence number

  int framesize = sp->channels * (sp->encoding == F32LE ? 4 : 2); // bytes per sample time
  int frames = buffer_size / framesize;  // One frame per sample time

  // is the rtp.timestamp value what we expect from the last datagram (don't log on first datagram of file)
  if ((sp->session_errors_init) && (rtp->timestamp != sp->next_expected_rtp_ts)){
    __atomic_fetch_add(&sync_diags->ts_errors,1,__ATOMIC_RELAXED);
    wd_log(0,"Weird rtp.timestamp: expected %u, received %u (delta %d) on SSRC %d (tx %u, rx %u, drops %u)\n",
           sp->next_expected_rtp_ts,
           rtp->timestamp,
           rtp->timestamp - sp->next_expected_rtp_ts,
           sp->ssrc,
           sp->max_tx_queue,
           sp->max_rx_queue,
           sp->max_drops);
  }
  sp->next_expected_rtp_ts = rtp->timestamp + frames;    // next expected RTP timestamp

  // if the output filter dropped a block, emit a warning
  if ((sp->session_errors_init) && (sp->last_block_drops != sp->chan.filter.out.block_drops)){
    __atomic_fetch_add(&sync_diags->dropped_blocks,(sp->chan.filter.out.block_drops - sp->last_block_drops),__ATOMIC_RELAXED);
    wd_log(0,"Weird block_drops: expected %u, received %u on SSRC %d (tx %u, rx %u, drops %u)\n",
           sp->last_block_drops,
           sp->chan.filter.out.block_drops,
           sp->ssrc,
           sp->max_tx_queue,
           sp->max_rx_queue,
           sp->max_drops);
  }
  sp->last_block_drops = sp->chan.filter.out.block_drops;
  sp->session_errors_init = true;
  __atomic_store_n(&sync_diags->updated_ns,gps_time_ns(),__ATOMIC_RELAXED);
}

int64_t calculated_starting_timesnap(struct session * const sp, uint32_t rtp_timestamp){
        /* int64_t sender_time = sp->chan.clocktime + (int64_t)BILLION * (UNIX_EPOCH - GPS_UTC_OFFSET); */
        int64_t sender_time = sp->chan.clocktime + (int64_t)BILLION * (UNIX_EPOCH);
        sender_time += (int64_t)BILLION * (int32_t)(rtp_timestamp - sp->chan.output.time_snap) / sp->samprate;
        return sender_time;
}

static int wd_write(struct session * const sp,void *samples,int buffer_size,struct timespec now){
  if(NULL == sp->fp)
    return -1;

  int framesize = sp->channels * (sp->encoding == F32LE ? 4 : 2); // bytes per sample time
  int frames = buffer_size / framesize;  // One frame per sample time

  /* static int xc = 80; */
  /* if (xc) { */
  /*    wd_log(0,"%u bytes %u frames %u samples, %u bytes/sample\n",buffer_size,frames,frames*sp->channels,(sp->encoding == F32LE ? 4 : 2)); */
  /*    xc--; */
  /* } */

  // check time of first sample: if it's more than +/- x seconds from expected, force resync on next file
  if (0 == sp->total_file_samples){
    struct timespec expected_start = now;
    expected_start.tv_nsec = 0;
    expected_start.tv_sec += (time_t)(FileLengthLimit / 2);
    expected_start.tv_sec /= (time_t)(FileLengthLimit);
    expected_start.tv_sec *= (time_t)(FileLengthLimit);

    if (fabs(time_diff(expected_start,now)) >= wd_tolerance_seconds){
      wd_log(1,"First sample %.3f s off...resync at next interval on SSRC %d (tx %u, rx %u, drops %u)\n",
             time_diff(expected_start,now),
             sp->ssrc,
             sp->max_tx_queue,
             sp->max_rx_queue,
             sp->max_drops);
      sp->sync_state = sync_state_resync;
    }
    clear_queue_counters(sp);
  }

  int partial_frames = frames;
  if (partial_frames > sp->samples_remaining){
     wd_log(1,"SSRC %u Too many frames in this packet! %ld remain, %u this packet\n",
	    sp->ssrc,
	    sp->samples_remaining,
	    partial_frames);
     partial_frames = sp->samples_remaining;
  }

  fwrite(samples,framesize,partial_frames,sp->fp);
  sp->total_file_samples += partial_frames;
  sp->current_segment_samples += partial_frames;
  if(sp->current_segment_samples >= SubstantialFileTime * sp->samprate)
    sp->substantial_file = true;
  sp->samples_written += partial_frames;
  sp->samples_remaining -= partial_frames;

  // if we wrote a partial, finish up in the new file
  if (partial_frames < frames){
    wd_log(1,"SSRC %u frames: %d partial %d samples %p %ld new samples %p %ld\n",
           sp->ssrc,
           frames,
           partial_frames,
           samples,
           (long int)samples,
           (void*)((float*)samples + (partial_frames * sp->channels)),
           (long int)(void*)((float*)samples + (partial_frames * sp->channels)));
    close_file(sp);

    // start new file
    sp->start_ts = sp->rtp_state.timestamp + partial_frames;
    sp->start_sequence = sp->rtp_state.seq;
    sp->start_timesnap = calculated_starting_timesnap(sp,sp->start_ts);
    session_file_init(sp,&sp->sender);
    sp->sync_state = sync_state_active;
    wd_log(1,"SSRC %u set start ts to %u (RTP TS %u, partial %u) wd_write()\n",
           sp->ssrc,
           sp->start_ts,
           sp->rtp_state.timestamp,
           partial_frames);

    // spit out the estimated start time of the stream, based on sample rate and RTP timestamp, ignoring rollovers
    wd_log(1, "SSRC %u start partial file with seq %u timestamp %u, estimated stream start is %u s ago\n",
           sp->ssrc,
           sp->rtp_state.seq,
           sp->rtp_state.timestamp,
           sp->rtp_state.timestamp / sp->samprate);

    start_wav_stream(sp);
    sp->file_time = now;
    wd_log(1,"SSRC %u starting in the middle of a packet. partial_frames = %d, frames = %d, samples = %p (%ld)\n",
           sp->ssrc,
           partial_frames,frames,samples,(long int)samples);
    samples = (void*)((float*) samples + (partial_frames * sp->channels));
    partial_frames = frames - partial_frames;
    wd_log(1,"SSRC %u Starting in the middle of a packet. partial_frames = %d, frames = %d, samples = %p (%ld)\n",
           sp->ssrc,
           partial_frames,
           frames,samples,
           (long int)samples);

    fwrite(samples,framesize,partial_frames,sp->fp);
    sp->total_file_samples += partial_frames;
    sp->current_segment_samples += partial_frames;
    if(sp->current_segment_samples >= SubstantialFileTime * sp->samprate)
      sp->substantial_file = true;
    sp->samples_written += partial_frames;
    sp->samples_remaining -= partial_frames;
  }

  /* if (sp->samples_remaining <= sp->samprate * 2){ */
  /*   return -1; */
  /* } */

  if(sp->samples_remaining <= 0)
  {
    // hit sample count, close file and create the next one
    close_file(sp);
    return 1;           // tell state machine to create the next file
  }
  return 0;
}

static FILE *udp_stats_file = 0;

static bool grab_queue_stats(uint32_t *tx_queue_depth,uint32_t *rx_queue_depth,uint32_t *drops){
  if (AF_INET != mcast_dest_sock.sa_family)
    return false;

  if (0 == udp_stats_file){
    udp_stats_file = fopen("/proc/net/udp","r");
  }

  if (udp_stats_file){
    struct sockaddr_in const *sin = (struct sockaddr_in *)&mcast_dest_sock;
    char *src_addr;
    if (asprintf(&src_addr,"%08X:%04X",(sin->sin_addr.s_addr),ntohs(sin->sin_port)) >= 0){
      char *line = NULL;
      size_t len = 0;
      ssize_t nread;
      fseek(udp_stats_file,0,SEEK_SET);
      while ((nread = getline(&line,&len,udp_stats_file)) != -1){
        strtok(line," ");
        char *a = strtok(0," ");
        if (0 == strcmp(src_addr, a)){
          strtok(0," ");
          strtok(0," ");
          char *tq = strtok(0,":");
          char *rq = strtok(0," ");
          strtok(0," ");
          strtok(0," ");
          strtok(0," ");
          strtok(0," ");
          strtok(0," ");
          strtok(0," ");
          strtok(0," ");
          char *d = strtok(0," ");
          *drops = strtoul(d,0,10);
          *tx_queue_depth = strtoul(tq,0,16);
          *rx_queue_depth = strtoul(rq,0,16);
          FREE(src_addr);
          FREE(line);
          return true;
        }
      }
      FREE(src_addr);
      FREE(line);
    }
    return false;
  }
  return false;
}

static void wd_state_machine(struct session * const sp,struct sockaddr const *sender,void *samples,int buffer_size){
  if (!wd_mode || NULL == sp){
    return;
  }
  int status;
  struct timespec now;
  clock_gettime(CLOCK_REALTIME,&now);

  int seconds = now.tv_sec % (time_t)FileLengthLimit;

  // update max queue depth and drops (really only need this per channel group, not per session)
  uint32_t tx = 0;
  uint32_t rx = 0;
  uint32_t d = 0;
  if (grab_queue_stats(&tx,&rx,&d)){
    if (tx > sp->max_tx_queue)
      sp->max_tx_queue = tx;
    if (rx > sp->max_rx_queue)
      sp->max_rx_queue = rx;
    if (d > sp->max_drops)
      sp->max_drops = d;
  }

  int framesize = sp->channels * (sp->encoding == F32LE ? 4 : 2); // bytes per sample time
  uint32_t packet_start_ts = sp->rtp_state.timestamp;
  uint32_t packet_stop_ts = packet_start_ts + (buffer_size / framesize);

  if ((0 != sync_ssrc) && (sync_start_ts >= packet_start_ts) && (sync_start_ts < packet_stop_ts)){
    // PPS sync mode
    wd_log(1,"SSRC %u sync start at RTP ts %u: this packet is %u - %u\n",
           sp->ssrc,
           sync_start_ts,
           packet_start_ts,
           packet_stop_ts);
  }

  switch(sp->sync_state){
  default:
  case sync_state_startup:
    // spin until we see samples arrive in second 59
    if (seconds == (FileLengthLimit - 1)){
      // data arrived in second 59, so go to armed state
      sp->sync_state = sync_state_armed;
    }
    break;

  case sync_state_armed:
    // drop samples until we're in second 0 or see the PPS edge

    bool start = false;
    if (0 == sync_ssrc){
      // wd mode, no PPS sync channel, start with first datagram in second 0
      if (0 == seconds){
        start = true;
        sync_start_ts = packet_start_ts;
      }
    } else {
      /* fprintf(stderr,"ssrc %u armed start ts: %u packet %u - %u!\n",sp->ssrc,sync_start_ts,packet_start_ts,packet_stop_ts); */
      // PPS sync mode, only start when the PPS is in this datagram
      if ((sync_start_ts >= packet_start_ts) && (sync_start_ts < packet_stop_ts)){
        start=true;
      }
    }

    if (true == start){
      // either first datagram in :00 or PPS detected in this datagram--start recording
      sp->sync_state = sync_state_active;

      if(sp->fp == NULL && !sp->complete){
        // create new file in second :00
        sp->wd_file_time.tv_sec = 0;
        sp->start_sequence = sp->rtp_state.seq;
        sp->start_timesnap = calculated_starting_timesnap(sp,sync_start_ts);
        sp->start_ts = sync_start_ts;
        session_file_init(sp,sender);
        sp->sync_state = sync_state_active;

        // spit out the estimated start time of the stream, based on sample rate and RTP timestamp, ignoring rollovers
        wd_log(1, "SSRC %u start file with seq %u timestamp %u, estimated stream start is %u s ago\n",
               sp->ssrc,
               sp->rtp_state.seq,
               sync_start_ts,
               sync_start_ts / sp->samprate);

        start_wav_stream(sp);
        sp->file_time = now;

        uint32_t frame_offset = sync_start_ts - packet_start_ts;
        /* printf("buffer at %p (%lu), length %u -- ",samples,(unsigned long int)samples,buffer_size); */
        float * new_samples = (float*) samples;
        new_samples += (frame_offset * sp->channels);
        buffer_size -= (frame_offset * framesize);
        /* printf("buffer at %p (%lu), length %u\n",new_samples,(unsigned long int)new_samples,buffer_size); */
        sp->start_ts = sp->rtp_state.timestamp + frame_offset;
        wd_log(1,"SSRC %u set start ts to %u (RTP TS %u, partial %u) wd_state_machine()\n",
               sp->ssrc,
               sp->start_ts,
               sp->rtp_state.timestamp,
               frame_offset);


        if (0 != wd_write(sp,new_samples,buffer_size,now)){
          // something went wrong...should we delete the file?
          sp->sync_state = sync_state_startup;
          close_file(sp);
        }
      }
    }
    break;

  case sync_state_active:
    if(NULL == sp->fp){
      sp->sync_state = sync_state_startup;
      return;
    }

    // save to file until error or file is complete
    status = wd_write(sp,samples,buffer_size,now);

    if (-1 == status){
      // something went wrong...should we delete the file?
      sp->sync_state = sync_state_startup;
      close_file(sp);
    }
    else if (1 == status){
      // file complete, start new file next time
      sp->sync_state = sync_state_done;
    }
    break;

  case sync_state_done:
    // last time through the file was complete, so start a new one
    sp->start_sequence = sp->rtp_state.seq;
    sp->start_timesnap = calculated_starting_timesnap(sp,sp->rtp_state.timestamp);
    sp->start_ts = sp->rtp_state.timestamp;
    session_file_init(sp,sender);
    sp->sync_state = sync_state_active;

    // spit out the estimated start time of the stream, based on sample rate and RTP timestamp, ignoring rollovers
    wd_log(1, "Start file on SSRC %d with seq %u timestamp %u, estimated stream start is %u s ago\n",
           sp->ssrc,
           sp->rtp_state.seq,
           sp->rtp_state.timestamp,
           sp->rtp_state.timestamp / sp->samprate);

    start_wav_stream(sp);
    sp->file_time = now;

    // save to file until error or file is complete
    status = wd_write(sp,samples,buffer_size,now);

    if (-1 == status){
      // something went wrong...should we delete the file?
      sp->sync_state = sync_state_startup;
      close_file(sp);
    }
    else if (1 == status){
      // file complete, start new file next time
      sp->sync_state = sync_state_done;

    }
    break;

  case sync_state_resync:
    // record short file until we can resync at next :00
    if(NULL == sp->fp){
      sp->sync_state = sync_state_startup;
      return;
    }

    // tricky...if samples arrive too fast, we could start a new file in :59, which
    // would trigger a resync, but then it'd quickly go to :00 and the short file would be less
    // than a second, leading to duplicate file names! Argh.
    // Maybe only create the new file once the short file is at least half full?
    if ((0 == seconds) && (sp->total_file_samples > sp->samples_remaining)) {
      // first packet in :00, resync and start clean after the short file
      close_file(sp);
      sp->wd_file_time.tv_sec = 0;
      sp->start_sequence = sp->rtp_state.seq;
      sp->start_timesnap = calculated_starting_timesnap(sp,sp->rtp_state.timestamp);
      sp->start_ts = sp->rtp_state.timestamp;
      session_file_init(sp,sender);
      sp->sync_state = sync_state_active;

      // spit out the estimated start time of the stream, based on sample rate and RTP timestamp, ignoring rollovers
      wd_log(1, "Resync file on SSRC %d with seq %u timestamp %u, estimated stream start is %u s ago\n",
             sp->ssrc,
             sp->rtp_state.seq,
             sp->rtp_state.timestamp,
             sp->rtp_state.timestamp / sp->samprate);

      start_wav_stream(sp);
      sp->file_time = now;
    }
    if (0 != wd_write(sp,samples,buffer_size,now)){
      // something went wrong...should we delete the file?
      sp->sync_state = sync_state_startup;
      close_file(sp);
    }
    break;
  }
}

void log_printf(const char* format, ...){
  va_list args;
  va_start(args, format);
  char* buff;
  if (vasprintf(&buff, format, args)>=0)
  {
    syslog(LOG_INFO, "%s", buff);
    free(buff);
  }
  va_end(args);
}

static uint32_t pps_consecutive = 0;
static uint32_t pps_ok = 0;
static uint32_t pps_noise = 0;

static void fix_mode(struct session * const sp){
  if (Control_fd >= 0){
    // Probably need a rate limit so we don't hammer radiod
    // anything else that needs to be config'd? IQ, float, AGC off, gain 0 dB?
    uint8_t cmdbuffer[PKTSIZE];
    uint8_t *bp = cmdbuffer;
    *bp++ = CMD; // Command

    encode_int(&bp,OUTPUT_SSRC,sp->ssrc); // Specific SSRC
    int sent_tag = arc4random();
    encode_int(&bp,COMMAND_TAG,sent_tag); // Append a command tag
    encode_string(&bp,PRESET,"iq",strlen("iq"));
    encode_int(&bp,OUTPUT_ENCODING,F32LE);
    encode_float(&bp,GAIN,0);
    encode_int(&bp,AGC_ENABLE,false); // Turn off AGC for manual gain
    float low = (-(double)sp->samprate/2) + 50;
    /* float low = -sp->samprate + 50; */
    encode_float(&bp,LOW_EDGE, low);
    float high = (sp->samprate/2) - 50;
    fprintf(stderr,"SSRC %u, samprate %u: set filter:%.0f to %.0f Hz\n", sp->ssrc, sp->samprate, low, high);
    encode_float(&bp,HIGH_EDGE,high);
    encode_eol(&bp);
    int command_len = bp - cmdbuffer;

    if(send(Control_fd, cmdbuffer, command_len, 0) != command_len){
      fprintf(stderr,"Control command send error: %s\n",strerror(errno));
    } else {
      fprintf(stderr,"Control command sent ok.\n");
    }
  }
}

static void bpsk_state_machine(struct session * const sp,struct sockaddr const */*sender*/,void *samples,int buffer_size,int64_t sender_time){
  if (NULL == sp){
    return;
  }

  if (filter_500){
    if (!notch_filter_initialized){
      notch500_init(0.99, &b0, &b1, &b2, &a1, &a2, &stI, sp->samprate);
      notch500_init(0.99, &b0, &b1, &b2, &a1, &a2, &stQ, sp->samprate);
      notch_filter_initialized = true;
    }
  }

  static float complex acc[32000];
  static uint32_t acc_i = 0;

  {
    static bool wrong_mode_warning = false;
    if ((strcmp("iq",sp->chan.preset)) || (2 != sp->channels) || (F32LE != sp->encoding)){
      if (!wrong_mode_warning){
        fprintf(stderr,"SSRC %u mode %s channels %d encoding %s unsupported! Must be 2 channel IQ float\n",
                sp->ssrc,
                sp->chan.preset,
                sp->channels,
                encoding_string(sp->encoding));
      }
      wrong_mode_warning = true;
      fix_mode(sp);
      return;
    } else {
      if (wrong_mode_warning){
        fprintf(stderr,"SSRC %u mode/encoding/channels now fixed\n",sp->ssrc);
      }
      wrong_mode_warning = false;
    }
  }

  // don't even bother if SNR is <8 dB or so
  /* if (Local.snr < 8) */
  /*   return; */

  struct timespec now;
  clock_gettime(CLOCK_REALTIME,&now);

  // read through the samples and calculate the arg of each (assuming IQ for now)
  int framesize = sp->channels * (sp->encoding == F32LE ? 4 : 2); // bytes per sample time
  uint32_t frames = buffer_size / framesize;  // One frame per sample time
  float complex* s=(float complex*)samples;
  /* wd_log(0,"%d frames of %d bytes each, size: %d, IQ samples: %lu\n", */
  /*        frames,framesize,buffer_size,buffer_size/sizeof(complex float)); */
  for(uint32_t i = 0;i < frames;++i){
    uint32_t ts = sp->rtp_state.timestamp + i;
    float complex sample = s[i];

    // Per sample:
    /* double I_in, Q_in;   // your input IQ */
    if (filter_500){
      double I_out = notch500_process(creal(sample), b0, b1, b2, a1, a2, &stI);
      double Q_out = notch500_process(cimag(sample), b0, b1, b2, a1, a2, &stQ);
      sample = I_out + Q_out * I;
    }

    if (leaky_folding){
      acc[acc_i] = (acc[acc_i] * leaky_folding_filter) + sample;
      sample = acc[acc_i] * (1.0 - leaky_folding_filter);
      acc_i = (acc_i + 1) % (sp->samprate * 2);
      if (0 == acc_i){

        /* int64_t st = calculated_starting_timesnap(sp, ts); */
        char buff[128];
        lldiv_t const ut = lldiv(sender_time,BILLION);
        int ms = llabs(ut.rem / 1000000);
        time_t t=time(0);
        wd_log(1,"SSRC: %u SNR: %.1f dB PPS ok: %u noise: %u consecutive: %u sync TS: %u now TS: %u ms: %u %s",
                sp->ssrc,
                Local.snr,
                pps_ok,
                pps_noise,
                pps_consecutive,
                sync_start_ts,
                ts,
                ms,
                ctime(&t));

        static int downcount=0;
        if ((Verbose >0) &&(!(downcount++ % 30))){
          sprintf(buff,"%d.raw",downcount-1);
          FILE *f=fopen(buff, "wb");
          if (f){
            fwrite(acc,sizeof(acc),1,f);
            fclose(f);
          }
        }
      }
    }


    /* lldiv_t const ut = lldiv(sender_time,BILLION); */
    /* // if we're within +/- 100 ms of the second, run the comparison */
    /* int ms = abs(ut.rem / 1000000); */
    /* if (ms >= 100) */
    /*   continue; */

    float angle = 180.0 * cargf(sample) / M_PI;
    float angle_diff=angle - sp->last_angle;
    if ((fabs(angle_diff) > 90.0) && (fabs(angle_diff) < 270.0)){
      bool noisy = false;

      // if the pulse isn't +/- 5 samples from the expected position, modulo sample rate, call it noise
      int32_t delta = (ts % sp->samprate) - (sp->last_edge % sp->samprate);
      if (abs(delta) > 10)
        noisy=true;

      // or if the pulse is <99% of a second?
      if ((ts - sp->last_edge) < ((sp->samprate * 99) / 100))
        noisy = true;

      /* if ((ts - sp->last_edge) > ((sp->samprate * 101) / 100)) */
      /*   noisy = true; */

      if (noisy){
        ++pps_noise;
        pps_consecutive = 0;
      } else{
        ++pps_ok;
        ++pps_consecutive;
      }

      /* printf("%s%ld %8u %.0f Hz %10u %6u %6d %8u %+6.1f %+6.1f %3.1f dB %6u %s %ld %6u %6u\n",wd_time(),now.tv_sec,sp->ssrc,sp->chan.tune.freq,ts,ts % sp->samprate,delta,ts / sp->samprate,angle,angle-sp->last_angle,Local.snr,ts - sp->last_edge,noisy?"noise?!":"",sender_time,pps_ok,pps_noise); */
      /* printf("Time                                         SSRC     Freq        RTP TS       Offset       Seconds Phase  Diff   SNR     Delta\n"); */

      // check if this PPS edge is +/- 0.4 seconds from top of minute -1 second, to arm the wsprdaemon sync start thing
      struct timespec expected_start = now;
      expected_start.tv_nsec = 0;
      /* expected_start.tv_sec += (time_t)(FileLengthLimit / 2); */
      /* expected_start.tv_sec /= (time_t)(FileLengthLimit); */
      /* expected_start.tv_sec *= (time_t)(FileLengthLimit); */
      /* expected_start.tv_sec -= 1; */
      expected_start.tv_sec += (time_t)(FileLengthLimit);
      expected_start.tv_sec /= (time_t)(FileLengthLimit);
      expected_start.tv_sec *= (time_t)(FileLengthLimit);

      if (pps_consecutive >= 1){
        wd_log(1,"SSRC %u edge at ts %u, modulo samp_rate: %u, time delta: %.6f, now: %ld, next: %ld, cons: %u sync: %u\n",
               sp->ssrc,
               ts,
               ts % sp->samprate,
               time_diff(now,expected_start),
                 now.tv_sec,
               expected_start.tv_sec,
               pps_consecutive,
               sync_start_ts);
      }

      /* if (fabs(time_diff(expected_start,now)) < 0.4){ */
      if (1) {
        if (pps_consecutive >= 10){
          // anti-race: There's no assurance that this sync SSRC will run before the others
          // This means the starting ts could be advanced one minute before any of the other sessions
          // have a chance to start recording. Not seeing an easy/clean fix.
          // Maybe we don't update if it's exactly one minute past the last
          // We can't really start in second 0 anyway, because we can't be sure this SSRC runs first.
          if (0 == ((expected_start.tv_sec - now.tv_sec) % (time_t)FileLengthLimit)){
            wd_log(1,"skip this starting TS update to avoid a race with the other sessions\n");
          }
          else{
            sync_start_ts = ts + (sp->samprate * (expected_start.tv_sec - now.tv_sec));
            sync_start_ts += sync_pretrigger;
          }
          wd_log(1,"SSRC %u sync start at next PPS (RTP ts %u)? Time delta: %.3f s, modulo samp_rate: %u, now: %ld, next: %ld\n",
                 sp->ssrc,
                 sync_start_ts,
                 time_diff(now,expected_start),
                 ts % sp->samprate,
                 now.tv_sec,
                 expected_start.tv_sec);
        }
      }
      fflush(0);
      sp->last_edge=ts;
    }
    sp->last_angle=angle;
  }

  // log interesting data once/minute
  if (59 == (now.tv_sec % 60)){
    static long int last_s = 0;
    if (now.tv_sec != last_s){
      log_printf("SSRC %u PPS ok: %u PPS noise: %u consecutive ok: %u sync at TS %u last edge: %u",
                 sp->ssrc,
                 pps_ok,
                 pps_noise,
                 pps_consecutive,
                 sync_start_ts,
                 sp->last_edge);
      last_s = now.tv_sec;
    }
  }

  __atomic_store_n(&sync_diags->pps_ok,pps_ok,__ATOMIC_RELAXED);
  __atomic_store_n(&sync_diags->pps_noise,pps_noise,__ATOMIC_RELAXED);
  __atomic_store_n(&sync_diags->pps_consecutive,pps_consecutive,__ATOMIC_RELAXED);
  __atomic_fetch_add(&sync_diags->datagrams,1,__ATOMIC_RELAXED);
  __atomic_store(&sync_diags->sync_frequency,&sp->chan.tune.freq,__ATOMIC_RELAXED);
  __atomic_store(&sync_diags->sync_snr,&Local.snr,__ATOMIC_RELAXED);
  __atomic_store_n(&sync_diags->updated_ns,gps_time_ns(),__ATOMIC_RELAXED);
}

static void closedown(int a){
  if(Verbose)
    fprintf(stderr,"%s: caught signal %d: %s\n",App_path,a,strsignal(a));

  if (wd_mode){
    char buff[NAME_MAX+1];
    snprintf(buff,NAME_MAX,"/pcmrecord.bpsk-%u",getpid());
    shm_unlink(buff);
  }
  cleanup();
  exit(EX_OK);  // Will call cleanup()
}

// Write out any partial Ogg Opus pages
static int ogg_flush(struct session *sp){
  if(sp == NULL || sp->fp == NULL || sp->encoding != OPUS)
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

// These produced by a test program I wrote. All are in CELT, 48 kHz mono
static uint8_t OpusSilence25[] = {0xe0,0xff,0xfe}; // 2.5 ms Silence
static uint8_t OpusSilence5[] = {0xe8,0xff,0xfe}; // 5 ms Silence
static uint8_t OpusSilence10[] = {0xf0,0xff,0xfe}; // 10 ms Silence
static uint8_t OpusSilence20[] = {0xf8,0xff,0xfe}; // 20 ms Silence
static uint8_t OpusSilence40[] = {0xf9,0xff,0xfe,0xff,0xfe}; // 40 ms Silence (2x 20 ms silence frames)
static uint8_t OpusSilence60[] = {0xfb,0x03,0xff,0xfe,0xff,0xfe,0xff,0xfe}; // 60 ms Silence (3 x 20ms silence frames)

static int emit_opus_silence(struct session * const sp,int samples){
  if(sp == NULL || sp->fp == NULL || sp->encoding != OPUS)
    return -1;

  if(Verbose > 1)
    fprintf(stderr,"%d: emitting %d frames of silence\n",sp->ssrc,samples);
  ogg_packet oggPacket = {0}; // Not really necessary
  oggPacket.b_o_s = 0;
  oggPacket.e_o_s = 0;    // End of stream flag

  int samples_since_flush = 0;
  while(samples > 0){
    int chunk = min(samples,2880); // 60 ms is 2880 samples @ 48 kHz
    // To save a little space in long silent intervals, emit the largest frame that will fit
    if(chunk >= 2880){
      chunk = 2880;
      oggPacket.packet = OpusSilence60;
      oggPacket.bytes = sizeof(OpusSilence60);
    } else if(chunk >= 1920){
      chunk = 1920;
      oggPacket.packet = OpusSilence40;
      oggPacket.bytes = sizeof(OpusSilence40);
    } else if(chunk >= 960){
      chunk = 960;
      oggPacket.packet = OpusSilence20;
      oggPacket.bytes = sizeof(OpusSilence20);
    } else if(chunk >= 480){
      chunk = 480;
      oggPacket.packet = OpusSilence10;
      oggPacket.bytes = sizeof(OpusSilence10);
    } else if(chunk >= 240){
      chunk = 240;
      oggPacket.packet = OpusSilence5;
      oggPacket.bytes = sizeof(OpusSilence5);
    } else {
      chunk = 120;
      oggPacket.packet = OpusSilence25;
      oggPacket.bytes = sizeof(OpusSilence25);
    }
    oggPacket.packetno = sp->packetCount++; // Increment packet number
    sp->granulePosition += chunk; // points to end of this packet
    oggPacket.granulepos = sp->granulePosition; // Granule position
    int ret = ogg_stream_packetin(&sp->oggState, &oggPacket);	  // Add the packet to the Ogg stream
    (void)ret;
    assert(ret == 0);

    sp->rtp_state.timestamp += chunk; // also ready for next
    sp->total_file_samples += chunk;
    sp->samples_written += chunk;
    if((FileLengthLimit != 0) || (max_length != 0))
      sp->samples_remaining -= chunk;
    samples -= chunk;
    samples_since_flush += chunk;
  }
  if(Flushmode || samples_since_flush >= OPUS_SAMPRATE){
    // Write at least once per second to keep opusinfo from complaining, and vlc progress from sticking
    samples_since_flush = 0;
    ogg_flush(sp);
  }
  return 0;
}


static int send_queue(struct session * const sp,bool flush){
  if(sp->encoding == OPUS)
    return send_opus_queue(sp,flush);
  else
    return send_wav_queue(sp,flush);
}

// if !flush, send whatever's on the queue, up to the first missing segment
// if flush, empty the entire queue, skipping empty entries
static int send_opus_queue(struct session * const sp,bool flush){
  if(sp == NULL || sp->fp == NULL || sp->encoding != OPUS)
    return -1;

  // Anything on the resequencing queue we can now process?
  int count = 0;
  for(int i=0; i < RESEQ; i++,sp->rtp_state.seq++){
    struct reseq *qp = &sp->reseq[sp->rtp_state.seq % RESEQ];
    if(!qp->inuse && !flush)
      break; // Stop on first empty entry if we're not resynchronizing

    if(qp->inuse){
      ogg_packet oggPacket = {0};
      int samples = opus_packet_get_nb_samples(qp->data,qp->size,OPUS_SAMPRATE); // Number of 48 kHz samples

      int32_t jump = (int32_t)(qp->rtp.timestamp - sp->rtp_state.timestamp);
      if(jump > 0){
	// Timestamp jumped since last frame
	// Catch up by emitting silence padding
	if(Verbose > 2 || (Verbose > 1  && flush))
	  fprintf(stderr,"timestamp jump %d samples\n",jump);

	emit_opus_silence(sp,jump);
	sp->current_segment_samples = 0; // gap resets
      }
      // end of timestamp jump catch-up, send actual packets on queue
      oggPacket.packetno = sp->packetCount++; // Increment packet number
      // Adjust the granule position
      sp->granulePosition += samples; // points to end of this packet
      oggPacket.granulepos = sp->granulePosition; // Granule position
      oggPacket.packet = qp->data;
      oggPacket.bytes = qp->size;
      sp->rtp_state.timestamp += samples; // also ready for next
      sp->total_file_samples += samples;
      sp->current_segment_samples += samples;
      if(sp->current_segment_samples >= SubstantialFileTime * sp->samprate)
	sp->substantial_file = true;

      sp->samples_written += samples;
      if((FileLengthLimit != 0) || (max_length != 0))
	sp->samples_remaining -= samples;

      if(Verbose > 2 || (Verbose > 1  && flush))
	fprintf(stderr,"ssrc %u writing from rtp sequence %u, timestamp %u: bytes %ld samples %d granule %lld\n",
		sp->ssrc,sp->rtp_state.seq,sp->rtp_state.timestamp,oggPacket.bytes,samples,
		(long long)oggPacket.granulepos);

      int ret = ogg_stream_packetin(&sp->oggState, &oggPacket);
      (void)ret;
      assert(ret == 0);
      {
	ogg_page oggPage;
	while (ogg_stream_pageout(&sp->oggState, &oggPage)){
	  fwrite(oggPage.header, 1, oggPage.header_len, sp->fp);
	  fwrite(oggPage.body, 1, oggPage.body_len, sp->fp);
	}
      }
    } else {
      // Slot was empty
      // Instead of emitting one frame of silence here, we emit it above when the next real frame arrives
      // so we know for sure how much to send
      //
      sp->rtp_state.drops++;
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
  int framesize = sp->channels * (sp->encoding == F32LE ? 4 : 2); // bytes per sample time
  for(int i=0; i < RESEQ; i++,sp->rtp_state.seq++){
    struct reseq *qp = &sp->reseq[sp->rtp_state.seq % RESEQ];
    if(!qp->inuse && !flush)
      break; // Stop on first empty entry if we're not resynchronizing

    int frames = qp->size / framesize;  // One frame per sample time
    if(qp->inuse){
      int jump = (int32_t)(qp->rtp.timestamp - sp->rtp_state.timestamp);
      if(jump > 0){
	// Timestamp jumped since last frame
	// Catch up by emitting silence padding
	if(Verbose > 2 || (Verbose > 1  && flush))
	  fprintf(stderr,"timestamp jump %d frames\n",jump);
	if(sp->can_seek)
	  fseeko(sp->fp,framesize * jump,SEEK_CUR);
	else {
	  unsigned char *zeroes = calloc(jump,framesize); // Don't use too much stack space
	  fwrite(zeroes,framesize,jump,sp->fp);
	  FREE(zeroes);
	}
	sp->current_segment_samples = 0; // gap resets
	sp->rtp_state.timestamp += jump; // also ready for next
	sp->total_file_samples += jump;
	sp->samples_written += jump;
	if((FileLengthLimit != 0) || (max_length != 0))
	  sp->samples_remaining -= jump;
      }
      // end of timestamp jump catch-up, send actual packets on queue
      fwrite(qp->data,framesize,frames,sp->fp);
      sp->rtp_state.timestamp += frames; // also ready for next
      sp->total_file_samples += frames;
      sp->current_segment_samples += frames;
      if(sp->current_segment_samples >= SubstantialFileTime * sp->samprate)
	sp->substantial_file = true;
      sp->samples_written += frames;
      if((FileLengthLimit != 0) || (max_length != 0))
	sp->samples_remaining -= frames;

      if(Verbose > 2 || (Verbose > 1  && flush))
	fprintf(stderr,"writing from rtp sequence %u, timestamp %u: bytes %d frames %d\n",
		sp->rtp_state.seq,sp->rtp_state.timestamp,framesize * frames,frames);

      FREE(qp->data); // OK if NULL
      qp->size = 0;
      qp->inuse = false;
      count++;
    }
  }
  return count;
}

void extract_source(uint8_t const * const buffer,int length){
  uint8_t const *cp = buffer;

  while(cp - buffer < length){
    enum status_type const type = *cp++; // increment cp to length field

    if(type == EOL)
      break; // End of list

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
    if(cp - buffer + optlen >= length)
      break; // Invalid length

    switch(type){
    case EOL: // Shouldn't get here
      goto done;

    case STATUS_DEST_SOCKET:
    {
      if (NULL == radio_mcast_group){
        struct sockaddr_storage sock;
        radio_mcast_group = strdup(formatsock(decode_socket(&sock,cp,optlen),false));
        /* fprintf(stderr,"radio mcast_group: %s\n",radio_mcast_group); */

        char iface[1024];
        struct sockaddr Metadata_dest_socket;

        resolve_mcast(radio_mcast_group,&Metadata_dest_socket,DEFAULT_STAT_PORT,iface,sizeof(iface),0);
        Control_fd = connect_mcast(&Metadata_dest_socket,iface,1,48);
        /* if(Control_fd < 0){ */
        /*   fprintf(stderr,"Control connection failed!\n"); */
        /* } else { */
        /*   fprintf(stderr,"Control connection ok\n"); */
        /* } */
      }
      break;
    }

    default:
      break;
    }
    cp += optlen;
  }
  done:
}

static void gen_locals(struct channel *channel){
  Local.noise_bandwidth = fabsf(channel->filter.max_IF - channel->filter.min_IF);
  Local.sig_power = channel->sig.bb_power - Local.noise_bandwidth * channel->sig.n0;
  if(Local.sig_power < 0)
    Local.sig_power = 0; // Avoid log(-x) = nan
  Local.sn0 = Local.sig_power/channel->sig.n0;
  Local.snr = power2dB(Local.sn0/Local.noise_bandwidth);
}

static void check_stream_skew(int64_t clocktime_ns){
  if (!wd_mode)
    return;

  struct session *sp;
  static int64_t last_skew_ns = 0;

  // run about 1 Hz
  if ((clocktime_ns - last_skew_ns) >= BILLION){
    last_skew_ns = clocktime_ns;
    int64_t min_stream_start_ns = INT64_MAX;
    for(sp = Sessions;sp != NULL;sp = sp->next){
      if(sp->chan.output.samprate>0){
	int64_t idle = clocktime_ns - sp->last_active;
        int64_t stream_start_ns = sp->chan.clocktime - (((int64_t)sp->chan.output.time_snap * BILLION) / sp->chan.output.samprate);
        if ((stream_start_ns < min_stream_start_ns) && (idle < BILLION))
          min_stream_start_ns = stream_start_ns;
      }
    }
    for(sp = Sessions;sp != NULL;sp=sp->next){
      // output starting RTP TS and GPS time
      char buff[256];
      if(sp->chan.output.samprate>0){
	int64_t idle = clocktime_ns - sp->last_active;
        int64_t stream_start_ns = sp->chan.clocktime - (((int64_t)sp->chan.output.time_snap * BILLION) / sp->chan.output.samprate);
        int64_t skew_ms = (stream_start_ns - min_stream_start_ns) / (1000 * 1000);
        if (idle < BILLION){
          wd_log((skew_ms >= 20) ? 0 : 1, "SSRC %8u: snap: %u skew: %4ld ms stream start: %s\n",
                 sp->chan.output.rtp.ssrc,
                 sp->chan.output.time_snap,
                 skew_ms,
                 format_gpstime(buff,256,stream_start_ns));
        }
      }
    }
  }
}

// Read both data and status from RTP network socket, assemble blocks of samples
// Doing both in one thread avoids a lot of synchronization problems with the session structure, since both write it
static void input_loop(){
  struct sockaddr sender;
  int64_t last_scan_time = 0;
  while(true){
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

      if (NULL == radio_mcast_group){
        extract_source(buffer+1,length-1);
      }

      if ((sync_ssrc) && (chan.output.rtp.ssrc == sync_ssrc)){
        // status packet for the BPSK sync channel, so calc SNR stats
        gen_locals(&chan);
      }


      if(Ssrc != 0 && chan.output.rtp.ssrc != Ssrc)
	goto statdone; // Unwanted session, but still clear any data packets

      check_stream_skew(chan.clocktime);

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
	  goto statdone; // unlikely

	sp->prev = NULL;
	sp->next = Sessions;
	if(sp->next)
	  sp->next->prev = sp;
	Sessions = sp;
	if(Catmode && Ssrc == 0){
	  Ssrc = chan.output.rtp.ssrc; // Latch onto the first ssrc we see, ignore others
	}

        // init diag structure if not already
        if (-1 == sync_diags_fd){
          char buff[NAME_MAX+1];
          snprintf(buff,NAME_MAX,"/pcmrecord.bpsk-%u",getpid());
          sync_diags_fd = shm_open(buff,O_RDWR | O_CREAT | O_TRUNC,0600);
          ftruncate(sync_diags_fd,sizeof(*sync_diags));
          sync_diags = mmap(NULL,sizeof(*sync_diags),PROT_READ | PROT_WRITE,MAP_SHARED,sync_diags_fd,0);
          if (((void*)-1) == sync_diags){
            fprintf(stderr,"Couldn't mmap diagnostic area!\n");
          } else {
            memset(sync_diags,0,sizeof(*sync_diags));
            sync_diags->magic = 0xefbe000a;
            sync_diags->version = 1;
            sync_diags->pid = getpid();
            strlcpy(sync_diags->multicast, PCM_mcast_address_text, sizeof(sync_diags->multicast));
            sync_diags->start_ns = gps_time_ns();
            sync_diags->updated_ns = gps_time_ns();
          }
          close(sync_diags_fd);
        }
      }
      // Wav can't change channels or samprate mid-stream, so if they're going to change we
      // should probably add an option to force stereo and/or some higher sample rate.
      // OggOpus can restart the stream with the new parameters, so it's not a problem
      sp->ssrc = chan.output.rtp.ssrc;
      sp->type = chan.output.rtp.type;
      sp->channels = chan.output.channels;
      sp->encoding = chan.output.encoding;
      sp->samprate = (sp->encoding == OPUS) ? OPUS_SAMPRATE : chan.output.samprate;
      memcpy(&sp->sender,&sender,sizeof(sp->sender));
      memcpy(&sp->chan,&chan,sizeof(sp->chan));
      memcpy(&sp->frontend,&frontend,sizeof(sp->frontend));
      // Ogg (containing opus) can concatenate streams with new metadata, so restart when it changes
      // WAV files don't even have this metadata, so ignore changes
      if(sp->encoding == OPUS){
	if(sp->last_frequency != sp->chan.tune.freq
	   || strncmp(sp->last_preset,sp->chan.preset,sizeof(sp->last_preset))){
	  end_ogg_opus_stream(sp);
	  start_ogg_opus_stream(sp);
	  emit_ogg_opus_tags(sp);
	}
      }
    }
  statdone:;
    if(pfd[0].revents & (POLLIN|POLLPRI)){
      uint8_t buffer[PKTSIZE];
      socklen_t socksize = sizeof(sender);
      int size = recvfrom(Input_fd,buffer,sizeof(buffer),0,&sender,&socksize);
      if(size <= 0){    // ??
	perror("recvfrom");
	goto datadone; // Some sort of error, quit
      }
      if(size < RTP_MIN_SIZE)
	goto datadone; // Too small for RTP, ignore

      struct rtp_header rtp;
      uint8_t *dp = (uint8_t *)ntoh_rtp(&rtp,buffer);
      if(rtp.pad){
	// Remove padding
	size -= dp[size-1];
	rtp.pad = 0;
      }
      if(size <= 0)
	goto datadone; // Bogus RTP header

      size -= (dp - buffer);

      if(Ssrc != 0 && rtp.ssrc != Ssrc)
	goto datadone;

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
	goto datadone;

      if(sp->prev != NULL){
	// Move to top of list to speed later lookups
	sp->prev->next = sp->next;
	if(sp->next != NULL)
	  sp->next->prev = sp->prev;
	sp->next = Sessions;
	sp->prev = NULL;
	Sessions = sp;
      }

      wd_check(sp,size,&rtp);

      if(sp->fp == NULL && !sp->complete && !wd_mode){
        sp->start_ts = rtp.timestamp;
	session_file_init(sp,&sender);
	if(sp->encoding == OPUS){
	  if(Raw)
	    fprintf(stderr,"--raw ignored on Ogg Opus streams\n");
	  start_ogg_opus_stream(sp);
	  emit_ogg_opus_tags(sp);
	  if(sp->starting_offset != 0)
	    emit_opus_silence(sp,sp->starting_offset);
	} else {
	  if(!Raw)
	    start_wav_stream(sp); // Don't emit wav header in --raw
	  int framesize = sp->channels * (sp->encoding == F32LE ? 4 : 2);
	  if(sp->can_seek){
	    fseeko(sp->fp,framesize * sp->starting_offset,SEEK_CUR);
	  } else {
	    // Emit zero padding
	    unsigned char *zeroes = calloc(sp->starting_offset,framesize); // Don't use too much stack space
	    fwrite(zeroes,framesize,sp->starting_offset,sp->fp);
	    FREE(zeroes);
	  }
	}
      }
      sp->last_active = gps_time_ns();

      if ((sync_frequency) && (sp->chan.tune.freq == sync_frequency)){
        sync_frequency = 0.0;
        sync_ssrc = rtp.ssrc;
      }

      if ((sync_ssrc) && (rtp.ssrc == sync_ssrc)){
	sp->rtp_state.seq = rtp.seq;
	sp->rtp_state.timestamp = rtp.timestamp;

        int64_t sender_time = sp->chan.clocktime + (int64_t)BILLION * (UNIX_EPOCH - GPS_UTC_OFFSET);
        sender_time += (int64_t)BILLION * (int32_t)(rtp.timestamp - sp->chan.output.time_snap) / sp->samprate;

        bpsk_state_machine(sp,&sender,dp,size,sender_time);
        if (!sync_record)
          goto datadone;
      }

      if (no_output)
        goto datadone;

      if (wd_mode){
        /* static int dc=0; */
        /* if (! (++dc % 100)){ */
        /*   printf("wd_mode(): SSRC %u\n",sp->ssrc); */
        /* } */
	sp->rtp_state.seq = rtp.seq;
	sp->rtp_state.timestamp = rtp.timestamp;
        if(sp->encoding == S16BE){
          // Flip endianness from big-endian on network to little endian wanted by .wav
          // byteswap.h is linux-specific; need to find a portable way to get the machine instructions
          int16_t const * const samples = (int16_t *)dp;
          int16_t *wp = (int16_t *)dp;
          int samp_count = size / sizeof(int16_t);
          for(int n = 0; n < samp_count; n++)
            wp[n] = bswap_16((uint16_t)samples[n]);
        }
        wd_state_machine(sp,&sender,dp,size);
        goto datadone;
      }

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
	  fprintf(stderr,"ssrc %u drop old sequence %u timestamp %u bytes %d\n",rtp.ssrc,rtp.seq,rtp.timestamp,size);
	sp->rtp_state.dupes++;
	// But sender may have restarted so remember it
	sp->rtp_state.odd_seq = rtp.seq + 1;
	sp->rtp_state.odd_seq_set = true;
	goto datadone;
      } else if(seqdiff >= RESEQ){
	// Give up waiting for the lost frame, flush what we have
	// Could also be a restart, but treat it the same
	if(Verbose > 1)
	  fprintf(stderr,"ssrc %u flushing with drops\n",rtp.ssrc);
	send_queue(sp,true);
	if(Verbose > 1)
	  fprintf(stderr,"ssrc %u reset & queue sequence %u timestamp %u bytes %d\n",rtp.ssrc,rtp.seq,rtp.timestamp,size);
      }
      if(Verbose > 2)
	fprintf(stderr,"ssrc %u queue sequence %u timestamp %u bytes %d\n",rtp.ssrc,rtp.seq,rtp.timestamp,size);

      // put into circular queue
      sp->rtp_state.odd_seq_set = false;
      int qi = rtp.seq % RESEQ;
      struct reseq * const qp = &sp->reseq[qi];
      qp->inuse = true;
      qp->rtp = rtp;
      qp->data = malloc(size);
      qp->size = size;

      if(sp->encoding == S16BE){
	// Flip endianness from big-endian on network to little endian wanted by .wav
	// byteswap.h is linux-specific; need to find a portable way to get the machine instructions

	int16_t const * const samples = (int16_t *)dp;
	int16_t *wp = (int16_t *)qp->data;
	int samp_count = size / sizeof(int16_t);
	for(int n = 0; n < samp_count; n++)
	  wp[n] = bswap_16((uint16_t)samples[n]);
      } else {
	memcpy(qp->data,dp,size); // copy everything else into circular queue as-is
      }
      send_queue(sp,false); // Send what we now can
      // If output is pipe, flush right away to minimize latency
      if(!sp->can_seek)
	if(0 != fflush(sp->fp)){
	  fprintf(stderr,"flush failed on '%s', %s\n",sp->filename,strerror(errno));
	}
      if(((FileLengthLimit != 0) || (max_length != 0)) && sp->samples_remaining <= 0)
	close_file(sp); // Don't reset RTP here so we won't lose samples on the next file

    } // end of packet processing
  datadone:;
    // Walk through list, close idle files
    // Leave sessions forever in case traffic starts again?
    int64_t current_time = gps_time_ns();
    if(current_time > last_scan_time + BILLION){
      last_scan_time = current_time;
      struct session *next;
      for(struct session *sp = Sessions;sp != NULL; sp = next){
	next = sp->next; // save in case sp is closed
	int64_t idle = current_time - sp->last_active;
	if(idle > Timeout * BILLION){
	  // Close idle file
	  close_file(sp); // sp will be NULL
	  sp->rtp_state.init = false; // reinit rtp on next packet so we won't emit lots of silence
	}
      }
    }
  }
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

int session_file_init(struct session *sp,struct sockaddr const *sender){
  if(sp->fp != NULL)
    return 0;
  sp->starting_offset = 0;
  sp->samples_remaining = 0;

  char const *file_encoding = encoding_string(sp->encoding == S16BE ? S16LE : sp->encoding);
  if(Catmode){
    sp->fp = stdout;
    if(Verbose)
      fprintf(stderr,"receiving %s ssrc %u samprate %d channels %d encoding %s freq %'.3lf preset %s\n",
	      sp->frontend.description,
	      sp->ssrc,sp->chan.output.samprate,sp->channels,file_encoding,sp->chan.tune.freq, // use rx sample rate even for opus
	      sp->chan.preset);
    return 0;
  } else if(Command != NULL){
    // Substitute parameters as specified
    sp->filename[0] = '\0';
    char command_copy[2048]; // Don't overwrite program args
    strlcpy(command_copy,Command,sizeof(command_copy));
    char *cp = command_copy;
    char *a;
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
	strlcat(sp->filename,temp,sizeof(sp->filename));
      }
    }
    if(Verbose)
      fprintf(stderr,"%s ssrc %u: executing %s\n",sp->frontend.description,sp->ssrc,sp->filename);
    sp->fp = popen(sp->filename,"w");
    if(sp->fp == NULL){
      fprintf(stderr,"ssrc %u: cannot start %s, exiting",sp->ssrc,sp->filename);
      exit(1); // Will probably fail for all others too, just give up
    }
    return 0;
  }
  // Else create a file
  char const *suffix = ".raw";
  if(!Raw){
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
  }
  struct timespec now;
  clock_gettime(CLOCK_REALTIME,&now);
  struct timespec file_time = now; // Default to actual time when length limit is not set
  if (wd_mode){
    if (sp->wd_file_time.tv_sec){
      // not the first file in the series, so +60 (well, FileLengthLimit) seconds from last file time
      sp->wd_file_time.tv_sec += FileLengthLimit;
    } else {
      // first file in series, use current time to name it
      sp->wd_file_time = file_time;
    }
  } else {
    // not wd mode, use current time
    sp->file_time = file_time;
  }

  if((FileLengthLimit > 0) && (!wd_mode)){ // Not really supported on opus yet
    // Pad start of first file with zeroes
#if 0
    struct tm const * const tm_now = gmtime(&now.tv_sec);
    fprintf(stderr,"time now = %4d-%02d-%02dT%02d:%02d:%02d.%dZ\n",
	     tm_now->tm_year+1900,
	     tm_now->tm_mon+1,
	     tm_now->tm_mday,
	     tm_now->tm_hour,
	     tm_now->tm_min,
	     tm_now->tm_sec,
	    (int)(now.tv_nsec / 100000000)); // 100 million, i.e., convert to tenths of a sec
#endif
    // Do time calculations with a modified epoch to avoid overflow problems
    int const epoch = 1704067200; // seconds between Jan 1 1970 00:00:00 UTC (unix epoch) and Jan 1 2024 00:00:00
    intmax_t now_ns = BILLION * (now.tv_sec - epoch) + now.tv_nsec; // ns since Jan 2024
    intmax_t limit = FileLengthLimit * BILLION;
    imaxdiv_t r = imaxdiv(now_ns,limit);
    intmax_t start_ns = r.quot * limit; // ns since epoch
    intmax_t skip_ns = now_ns - start_ns;

    if(!sp->no_offset && skip_ns > (int64_t)(Tolerance * BILLION) && (int64_t)(limit - skip_ns) > Tolerance * BILLION){
      // Adjust file time to previous multiple of specified limit size and pad start to first sample
      sp->no_offset = true; // Only on first file of session with -L
      imaxdiv_t f = imaxdiv(start_ns,BILLION);
      file_time.tv_sec = f.quot + epoch; // restore original epoch
      file_time.tv_nsec = f.rem;
      sp->file_time = file_time;
      sp->starting_offset = (sp->samprate * skip_ns) / BILLION;
      sp->total_file_samples += sp->starting_offset;
#if 0
      fprintf(stderr,"padding %lf sec %ld samples\n",
	      (float)skip_ns / BILLION,
	      sp->starting_offset);
#endif
    }
    sp->samples_remaining = (FileLengthLimit * sp->samprate) - sp->starting_offset;
  }
  if (max_length > 0)
    sp->samples_remaining = max_length * sp->samprate;

  if (wd_mode){
    sp->starting_offset = 0;
    sp->total_file_samples = 0;
    sp->samples_remaining = FileLengthLimit * sp->samprate;
    sp->samples_remaining += force_sample_rate_error;
    sp->sync_state = sync_state_startup;

    // hack the file start time to be in sequence, even if it's wrong
    file_time.tv_sec = sp->wd_file_time.tv_sec;
    file_time.tv_nsec = sp->wd_file_time.tv_nsec;
    //wd_log(1,"Override new file name using %ld.%03ld\n",file_time.tv_sec,file_time.tv_nsec/1000000);
    sp->last_block_drops = sp->chan.filter.out.block_drops;
  }

  if(Jtmode){
    //  K1JT-format file names in flat directory
    // Round time to nearest second
    time_t seconds = file_time.tv_sec;
    if(file_time.tv_nsec > BILLION/2)
      seconds++;
    struct tm const * const tm = gmtime(&seconds);
    snprintf(sp->filename,sizeof(sp->filename),"%4d%02d%02dT%02d%02d%02dZ_%.0lf_%s%s",
	     tm->tm_year+1900,
	     tm->tm_mon+1,
	     tm->tm_mday,
	     tm->tm_hour,
	     tm->tm_min,
	     tm->tm_sec,
	     sp->chan.tune.freq,
	     sp->chan.preset,
	     suffix);
  } else {
    // Round time to nearest 1/10 second
    imaxdiv_t f = imaxdiv(file_time.tv_nsec,100000000); // 100 million to get deci-seconds
    if(f.rem >= 50000000) // 50 million
      f.quot++; // round up to next deci second
    long long deci_seconds = f.quot + (long long)10 * file_time.tv_sec;
    f = imaxdiv(deci_seconds,10); // seconds, tenths
    time_t seconds = f.quot;
    int tenths = f.rem;
    struct tm const * const tm = gmtime(&seconds);
    sp->filename[0] = '\0';

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
      // yyyy-mm-dd-hh:mm:ss.s so it will sort properly
      snprintf(sp->filename,sizeof(sp->filename),
	       "%u/%d/%d/%d/",
	       sp->ssrc,
	       tm->tm_year+1900,
	       tm->tm_mon+1,
	       tm->tm_mday);
    }
    // create file in specified directory
    char *start = sp->filename + strlen(sp->filename);
    int size = sizeof(sp->filename) - strlen(sp->filename);
    snprintf(start,size,
	     "%uk%4d-%02d-%02dT%02d:%02d:%02d.%dZ%s",
	     sp->ssrc,
	     tm->tm_year+1900,
	     tm->tm_mon+1,
	     tm->tm_mday,
	     tm->tm_hour,
	     tm->tm_min,
	     tm->tm_sec,
	     tenths,
	     suffix);
  }
  sp->fp = fopen(sp->filename,"w++");
  sp->last_active = gps_time_ns();

  if(sp->fp == NULL){
    fprintf(stderr,"can't create/write file '%s': %s\n",sp->filename,strerror(errno));
    return -1;
  }
  {
    struct stat statbuf;
    if(fstat(fileno(sp->fp),&statbuf) != 0){
      fprintf(stderr,"stat(%s) failed: %s\n",sp->filename,strerror(errno));
    } else {
      switch(statbuf.st_mode & S_IFMT){
      case S_IFREG:
	sp->can_seek = true;
	break;
      default:
	sp->can_seek = false;
	break;
      }
    }
  }
  // We byte swap S16BE to S16LE, so change the tag
  if(Verbose){
    fprintf(stderr,"%s creating %s %d s/s %s %s %'.3lf Hz %s TS %u",
	    sp->frontend.description,
	    sp->filename,sp->chan.output.samprate, // original rx samprate for opus
	    sp->channels == 1 ? "mono" : "stereo",
	    file_encoding,sp->chan.tune.freq,
	    sp->chan.preset,
	    sp->start_ts);
    if(sp->starting_offset > 0)
      fprintf(stderr," offset %lld",(long long)sp->starting_offset);
    fputc('\n',stderr);
  }

  sp->iobuffer = malloc(BUFFERSIZE);
  setbuffer(sp->fp,sp->iobuffer,BUFFERSIZE);

  int const fd = fileno(sp->fp);
  fcntl(fd,F_SETFL,O_NONBLOCK); // Let's see if this keeps us from losing data

  attrprintf(fd,"encoding","%s",file_encoding);
  attrprintf(fd,"samprate","%u",sp->samprate);
  attrprintf(fd,"channels","%d",sp->channels);
  attrprintf(fd,"ssrc","%u",sp->ssrc);
  attrprintf(fd,"frequency","%.3lf",sp->chan.tune.freq);
  attrprintf(fd,"preset","%s",sp->chan.preset);
  attrprintf(fd,"source","%s",formatsock(sender,false));
  attrprintf(fd,"multicast","%s",PCM_mcast_address_text);
  attrprintf(fd,"unixstarttime","%ld.%09ld",(long)now.tv_sec,(long)now.tv_nsec);

  attrprintf(fd,"Start RTP timestamp","%u",sp->start_ts);
  attrprintf(fd,"Start RTP seq","%u",sp->start_sequence);
  attrprintf(fd,"Start timesnap","%.6f s",1.0e-9 * sp->start_timesnap);
  attrprintf(fd,"Pretrigger","%d",sync_pretrigger);

  if(strlen(sp->frontend.description) > 0)
    attrprintf(fd,"description","%s",sp->frontend.description);

  if(sp->starting_offset != 0)
    attrprintf(fd,"starting offset","%lld",sp->starting_offset);

  if(sp->chan.demod_type == LINEAR_DEMOD && !sp->chan.linear.agc)
    attrprintf(fd,"gain","%.3f",voltage2dB(sp->chan.output.gain));
  return 0;
}

static int close_session(struct session **spp){
  if(spp == NULL)
    return -1;
  struct session *sp = *spp;

  if(sp == NULL)
    return -1;

  close_file(sp);
  if(sp->prev)
    sp->prev->next = sp->next;
  else
    Sessions = sp->next;
  if(sp->next)
    sp->next->prev = sp->prev;
  // when the max_length (-x) option is used, valgrind has
  // intermittently reported unfree'd allocations in the resequencing
  // queue at program exit. Not sure what only -x is affected.
  for(int i=0;i < RESEQ;i++){
    FREE(sp->reseq[i].data);
  }
  FREE(sp);
  return 0;
}

// Close a file, update .wav header
// If the file is not "substantial", just delete it
static int close_file(struct session *sp){
  if(sp == NULL)
    return -1;

  if(sp->fp == NULL)
    return 0;

  if(sp->encoding == OPUS)
    end_ogg_opus_stream(sp);
  else
    end_wav_stream(sp);

  if(Verbose){
    fprintf(stderr,"%s closing '%s' %'.1f sec\n",
	    sp->frontend.description,
	    sp->filename, // might be blank
            (float)sp->samples_written / sp->samprate);
  }
  if(Verbose > 1 && (sp->rtp_state.dupes != 0 || sp->rtp_state.drops != 0))
    fprintf(stderr,"ssrc %u dupes %llu drops %llu\n",sp->ssrc,(long long unsigned)sp->rtp_state.dupes,(long long unsigned)sp->rtp_state.drops);

  if(sp->can_seek){
    if(sp->substantial_file){ // Don't bother for non-substantial files
      int fd = fileno(sp->fp);
      attrprintf(fd,"samples written","%lld",sp->samples_written);
      attrprintf(fd,"total samples","%lld",sp->total_file_samples);
      struct timespec now;
      clock_gettime(CLOCK_REALTIME,&now);
      attrprintf(fd,"end time","%ld.%09ld",(long)now.tv_sec,(long)now.tv_nsec);
      attrprintf(fd,"elapsed","%.6f",time_diff(now,sp->file_time));
      attrprintf(fd,"Start RTP timestamp","%u",sp->start_ts);

      if (wd_mode){
        attrprintf(fd,"drift","%.6f",time_diff(sp->file_time,sp->wd_file_time));
        if (sync_ssrc)
          attrprintf(fd,"PPS RTP timestamp","%u",sync_start_ts);

        wd_log(1,"SSRC %u close file at %ld.%09ld, %.6f s elapsed, %.6f drift\n",
               sp->ssrc,
               (long)now.tv_sec,
               (long)now.tv_nsec,
               time_diff(now,sp->file_time),
               time_diff(sp->file_time,sp->wd_file_time));
      }
    } else if(strlen(sp->filename) > 0){
      if(unlink(sp->filename) != 0)
	fprintf(stderr,"Can't unlink %s: %s\n",sp->filename,strerror(errno));
      if(Verbose)
	fprintf(stderr,"deleting %s %'.1f sec\n",sp->filename,
		(float)sp->samples_written / sp->samprate);
    }
  }
  if(Command != NULL)
    pclose(sp->fp);
  else
    fclose(sp->fp);
  sp->fp = NULL;
  FREE(sp->iobuffer);
  sp->filename[0] = '\0';
  sp->samples_written = 0;
  sp->total_file_samples = 0;
  sp->current_segment_samples = 0;

  if (0 == max_length)
    return 0;

  sp->complete = true;        // don't create multiple files in max length mode
  // check to see if all sessions are complete...if so, exit
  for(sp = Sessions;sp != NULL;sp = sp->next){
    if(!sp->complete){
      return 0;
    }
  }
  // max length active, all sessions are complete, exit
  exit(EX_OK);  // Will call cleanup()
  return 0;
}

static int start_ogg_opus_stream(struct session *sp){
  if(sp == NULL || sp->fp == NULL)
    return -1;
  // Create Ogg container with Opus codec
  int serial = rand(); // Unique stream serial number
  if (ogg_stream_init(&sp->oggState, serial) != 0) {
    fprintf(stderr, "Failed to initialize Ogg stream.\n");
    return -1;
  }
  sp->granulePosition = 0;
  sp->packetCount = 0;

  struct {
    char head[8];
    uint8_t version;
    uint8_t channels;
    int16_t preskip;
    uint32_t samprate;
    int16_t gain;
    uint8_t map_family;
  } opusHeader;

  memset(&opusHeader,0,sizeof(opusHeader));
  memcpy(opusHeader.head, "OpusHead", 8);  // Signature
  opusHeader.version = 1;                  // Version
  // Some decoders get confused when the channel count or sample rate changes in a stream, so always say we're emitting 48kHz stereo.
  // Opus won't use more bits when the input is actually mono and/or at a lower rate
  opusHeader.channels = 2;
  opusHeader.preskip = 312;
  opusHeader.samprate = OPUS_SAMPRATE;
  opusHeader.gain = 0;
  opusHeader.map_family = 0;

  ogg_packet idPacket;
  idPacket.packet = (unsigned char *)&opusHeader;
  idPacket.bytes = 19; // sizeof(opusHeader) pads up, can't use!
  idPacket.b_o_s = 1;  // Beginning of stream
  idPacket.e_o_s = 0;
  idPacket.granulepos = 0; // always zero
  idPacket.packetno = sp->packetCount++;
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
  uint8_t opusTags[2048]; // Variable length, not easily represented as a structure
  memset(opusTags,0,sizeof(opusTags));
  memcpy(opusTags,"OpusTags",8);
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
  struct timespec now;
  clock_gettime(CLOCK_REALTIME,&now);
  struct tm const * const tm = gmtime(&now.tv_sec);

  char datestring[128];
  char timestring[128];
  snprintf(datestring,sizeof(datestring),"%4d-%02d-%02d",
	   tm->tm_year+1900,
	   tm->tm_mon+1,
	   tm->tm_mday);

  snprintf(timestring,sizeof(timestring),"%02d:%02d:%02d.%03d UTC",
	   tm->tm_hour,
	   tm->tm_min,
	   tm->tm_sec,
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
  ogg_packet tagsPacket;
  tagsPacket.packet = opusTags;
  tagsPacket.bytes = wp - opusTags;
  tagsPacket.b_o_s = 0;             // Not the beginning of the stream
  tagsPacket.e_o_s = 0;             // Not the end of the stream
  tagsPacket.granulepos = sp->granulePosition;        // No granule position for metadata
  tagsPacket.packetno = sp->packetCount++;          // Second packet in the stream

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
  ogg_packet endPacket;
  endPacket.packet = OpusSilence20;
  endPacket.bytes = sizeof(OpusSilence20);
  endPacket.b_o_s = 0;
  endPacket.e_o_s = 1;    // End of stream flag
  endPacket.granulepos = sp->granulePosition; // Granule position
  sp->granulePosition += 0; // Doesn't change
  endPacket.packetno = sp->packetCount++; // Increment packet number (not actually used again)

  // Add the packet to the Ogg stream
  int ret = ogg_stream_packetin(&sp->oggState, &endPacket);
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

  size_t len = strlen(string);
  uint32_t *wp = (uint32_t *)out;
  *wp = len;
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

  // Write .wav header, skipping size fields
  struct wav header;
  memset(&header,0,sizeof(header));
  memcpy(header.ChunkID,"RIFF", 4);
  header.ChunkSize = 0xffffffff; // Temporary
  memcpy(header.Format,"WAVE",4);
  memcpy(header.Subchunk1ID,"fmt ",4);
  header.Subchunk1Size = 40; // ??????
  switch(sp->encoding){
  default:
    return -1;
  case S16LE:
  case S16BE:
    header.AudioFormat = 1;
    header.BitsPerSample = 16;
    header.ByteRate = sp->samprate * sp->channels * 2;
    header.BlockAlign = sp->channels * 2;;
    break;
  case F32LE:
    header.AudioFormat = 3;
    header.BitsPerSample = 32;
    header.ByteRate = sp->samprate * sp->channels * 4;
    header.BlockAlign = sp->channels * 4;
    break;
  case F16LE:
    header.AudioFormat = 0; // What should go here for IEEE 16-bit float?
    header.BitsPerSample = 16;
    header.ByteRate = sp->samprate * sp->channels * 2;
    header.BlockAlign = sp->channels * 2;;
    break;
  }

  header.NumChannels = sp->channels;
  header.SampleRate = sp->samprate;
  header.ExtensionChunkSize = 22;
  memcpy(header.SubChunk2ID,"data",4);
  header.Subchunk2Size = 0xffffffff; // Temporary

  //  appears to be needed for FP
  memcpy(header.FactID,"fact",4);
  header.FactSize = 4;
  header.SamplesLength = 0xffffffff;

  // fill in the auxi chunk (start time, center frequency)
  struct timespec now;
  clock_gettime(CLOCK_REALTIME,&now);
  struct tm const * const tm = gmtime(&now.tv_sec);

  memcpy(header.AuxID, "auxi", 4);
  header.AuxSize=164;
  header.StartYear=tm->tm_year+1900;
  header.StartMon=tm->tm_mon+1;
  header.StartDOW=tm->tm_wday;
  header.StartDay=tm->tm_mday;
  header.StartHour=tm->tm_hour;
  header.StartMinute=tm->tm_min;
  header.StartSecond=tm->tm_sec;
  header.StartMillis=(int16_t)(now.tv_nsec / 1000000);
  header.CenterFrequency= sp->chan.tune.freq;
  memset(header.AuxUknown, 0, 128);
  if(sp->can_seek)
    rewind(sp->fp); // should be at BOF but make sure
  fwrite(&header,sizeof(header),1,sp->fp);
  sp->last_active = gps_time_ns();
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
  header.ChunkSize = statbuf.st_size - 8;
  header.Subchunk2Size = statbuf.st_size - sizeof(header);

  // write number of samples (or is it frames?) into the fact chunk
  header.SamplesLength = sp->samples_written;

  // write end time into the auxi chunk
  struct timespec now;
  clock_gettime(CLOCK_REALTIME,&now);
  struct tm const * tm = gmtime(&now.tv_sec);
  header.StopYear=tm->tm_year+1900;
  header.StopMon=tm->tm_mon+1;
  header.StopDOW=tm->tm_wday;
  header.StopDay=tm->tm_mday;
  header.StopHour=tm->tm_hour;
  header.StopMinute=tm->tm_min;
  header.StopSecond=tm->tm_sec;
  header.StopMillis=(int16_t)(now.tv_nsec / 1000000);

  tm = gmtime(&sp->file_time.tv_sec);
  header.StartYear = tm->tm_year + 1900;
  header.StartMon = tm->tm_mon + 1;
  header.StartDOW = tm->tm_wday;
  header.StartDay = tm->tm_mday;
  header.StartHour = tm->tm_hour;
  header.StartMinute = tm->tm_min;
  header.StartSecond = tm->tm_sec;
  header.StartMillis = (int16_t)(sp->file_time.tv_nsec / 1000000);

  rewind(sp->fp);
  if(fwrite(&header,sizeof(header),1,sp->fp) != 1)
    return -1;
  sp->last_active = gps_time_ns();
  return 0;
}
