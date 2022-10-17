// $Id: monitor.c,v 1.188 2022/10/13 19:41:42 karn Exp $
// Listen to multicast group(s), send audio to local sound device via portaudio
// Copyright 2018 Phil Karn, KA9Q
#define _GNU_SOURCE 1
#include <assert.h>
#include <errno.h>
#include <complex.h> // test
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <limits.h>

#include <opus/opus.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <netdb.h>
#include <portaudio.h>
#include <ncurses.h>
#include <locale.h>
#include <signal.h>
#include <getopt.h>
#if __linux__
#include <bsd/string.h>
#else
#include <string.h>
#endif

#include "conf.h"
#include "misc.h"
#include "multicast.h"
#include "iir.h"
#include "morse.h"


// Global config variables
static int Samprate = 48000;  // Now applies only to hardware output
#define MAX_MCAST 20          // Maximum number of multicast addresses
#define BUFFERSIZE (1<<19)    // about 10.92 sec at 48 kHz stereo - must be power of 2!!
#define MAXSIZE  5760         // 120 ms @ 48K for biggest Opus frame
static float const SCALE = 1./SHRT_MAX;
static const float Latency = 0.02; // chunk size for audio output callback
static const float Tone_period = 0.24; // PL tone integration period

// Command line parameters
static int Update_interval = 100;  // Default time in ms between display updates
static bool List_audio;            // List audio output devices and exit
const char *App_path;
int Verbose;                       // Verbosity flag
static bool Quiet;                 // Disable curses
static bool Quiet_mode;            // Toggle screen activity after starting
static float Playout = 100;
static bool Start_muted;
static bool Auto_position;
static bool PTT_state = false;
static long long LastAudioTime;
static pthread_t Repeater_thread;
static pthread_cond_t PTT_cond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t PTT_mutex = PTHREAD_MUTEX_INITIALIZER;
static long long Repeater_tail;
static char const *Cwid = "de nocall/r"; // Make this configurable!
static long long Last_id_time = 0;
static int Dit_length; 
static bool Autonotch = 0;

// IDs must be at least every 10 minutes per FCC 97.119(a)
static long long const MANDATORY_ID_INTERVAL = 600 * BILLION;
// ID early when carrier is about to drop, to avoid stepping on users
static long long const QUIET_ID_INTERVAL = (MANDATORY_ID_INTERVAL / 2);


// Global variables
static char *Mcast_address_text[MAX_MCAST]; // Multicast address(es) we're listening to
static char Audiodev[256];           // Name of audio device; empty means portaudio's default
static int Nfds;                     // Number of streams
static pthread_mutex_t Sess_mutex = PTHREAD_MUTEX_INITIALIZER;
static PaStream *Pa_Stream;          // Portaudio stream handle
static int inDevNum;                 // Portaudio's audio output device index
static long long Start_time;
static PaTime Start_pa_time;

static pthread_mutex_t Output_mutex = PTHREAD_MUTEX_INITIALIZER;
static float Output_buffer[BUFFERSIZE][2]; // Decoded audio output, written by processing thread and read by PA callback
static volatile long long Rptr;            // Unwrapped read pointer (bug: will overflow in 6 million years)
#if 0
static PaTime Last_callback_time;
#endif

static int Invalids;
static bool Help;
static long long Last_error_time;
static int Position; // auto-position streams
static bool Auto_sort;

// All the tones from various groups, including special NATO 150 Hz tone
static float PL_tones[] = {
     67.0,  69.3,  71.9,  74.4,  77.0,  79.7,  82.5,  85.4,  88.5,  91.5,
     94.8,  97.4, 100.0, 103.5, 107.2, 110.9, 114.8, 118.8, 123.0, 127.3,
    131.8, 136.5, 141.3, 146.2, 150.0, 151.4, 156.7, 159.8, 162.2, 165.5,
    167.9, 171.3, 173.8, 177.3, 179.9, 183.5, 186.2, 189.9, 192.8, 196.6,
    199.5, 203.5, 206.5, 210.7, 213.8, 218.1, 221.3, 225.7, 229.1, 233.6,
    237.1, 241.8, 245.5, 250.3, 254.1
};

#define N_tones (sizeof(PL_tones)/sizeof(PL_tones[0]))

struct session {
  struct sockaddr_storage sender;
  char const *dest;

  pthread_t task;           // Thread reading from queue and running decoder
  struct packet *queue;     // Incoming RTP packets
  pthread_mutex_t qmutex;   // Mutex protecting packet queue
  pthread_cond_t qcond;     // Condition variable for arrival of new packet

  struct rtp_state rtp_state; // Incoming RTP session state
  uint32_t ssrc;            // RTP Sending Source ID
  int type;                 // RTP type (10,11,20,111)

  uint32_t start_timestamp; // First RTP timestamp in stream, or after reset
  long long start_rptr;     // First output callback read pointer in stream, or after reset
  long long timestamp_upper; // Upper bits of virtual timestamp (if greater than 2^32)
  volatile long long wptr;           // current write pointer into output PCM buffer
  int playout;              // Initial playout delay, samples

  float bounce[MAXSIZE][2]; // This is uncomfortably large for the stack on some machines
  OpusDecoder *opus;        // Opus codec decoder handle, if needed
  int frame_size;
  int bandwidth;            // Audio bandwidth
  struct goertzel tone_detector[N_tones];
  int tone_samples;
  float current_tone;       // Detected tone frequency

  float active;  // Seconds we've been active (only when queue has stuff)
  int samprate;
  int channels;             // Channels (1 or 2)
  float gain;               // Gain; 1 = 0 dB
  float pan;                // Stereo position: 0 = center; -1 = full left; +1 = full right

  float tot_active; // Total PCM time, sec
  unsigned long packets;    // RTP packets for this session
  unsigned long empties;    // RTP but no data
  unsigned long long lates;
  unsigned long long earlies;
  unsigned long long resets;

  bool terminate;            // Set to cause thread to terminate voluntarily
  bool muted;
  bool reset;                // Set to force output timing reset on next packet
  
  char id[32];
  bool notch_enable;         // Enable PL removal notch
  struct iir iir_left;
  struct iir iir_right;  
  float notch_tone;
};
#define NSESSIONS 1500
static int Nsessions;
static struct session *Sessions[NSESSIONS];

static void load_id(void);
static void cleanup(void);
static void closedown(int);
static void *display(void *);
static void reset_session(struct session *sp,uint32_t timestamp);
static struct session *lookup_session(const struct sockaddr_storage *,uint32_t);
static struct session *create_session(void);
static int sort_session_active(void),sort_session_total(void);
static int close_session(struct session **);
static int pa_callback(const void *,void *,unsigned long,const PaStreamCallbackTimeInfo*,PaStreamCallbackFlags,void *);
static void *decode_task(void *x);
static void *sockproc(void *arg);
static void *repeater_ctl(void *arg);
static char const *lookupid(uint32_t ssrc);
static float make_position(int);


static char Optstring[] = "i:I:LR:Sap:qr:t:u:vn";
static struct  option Options[] = {
   {"pcm_in", required_argument, NULL, 'I'},
   {"opus_in", required_argument, NULL, 'I'},
   {"list-audio", no_argument, NULL, 'L'},
   {"audio-dev", required_argument, NULL, 'R'},
   {"autosort", no_argument, NULL, 'S'},
   {"auto-position", no_argument, NULL, 'a'},
   {"id", required_argument, NULL, 'i'},
   {"playout", required_argument, NULL, 'p'},
   {"quiet", no_argument, NULL, 'q'},
   {"samprate",required_argument,NULL,'r'},
   {"repeater", required_argument, NULL, 't'},
   {"update", required_argument, NULL, 'u'},
   {"verbose", no_argument, NULL, 'v'},
   {"notch", no_argument, NULL, 'n'},

   {NULL, 0, NULL, 0},
};


int main(int argc,char * const argv[]){
  App_path = argv[0];
  {
    // Try to improve our priority, then drop root
    int prio = getpriority(PRIO_PROCESS,0);
    setpriority(PRIO_PROCESS,0,prio - 15);
    if(seteuid(getuid()) != 0)
      perror("seteuid");
  }    
  setlocale(LC_ALL,getenv("LANG"));

  int c;
  while((c = getopt_long(argc,argv,Optstring,Options,NULL)) != -1){
    switch(c){
    case 'n':
      Autonotch = true;
      break;
    case 'i':
      Cwid = optarg;
      break;
    case 't':
      Repeater_tail = BILLION * fabsf(strtof(optarg,NULL));
      break;
    case 'L':
      List_audio = true;
      break;
    case 'R':
      strlcpy(Audiodev,optarg,sizeof(Audiodev));
      break;
    case 'v':
      Verbose++;
      break;
    case 'I':
      if(Nfds == MAX_MCAST){
	fprintf(stderr,"Too many multicast addresses; max %d\n",MAX_MCAST);
      } else 
	Mcast_address_text[Nfds++] = optarg;
      break;
    case 'q': // No ncurses
      Quiet = true;
      break;
    case 'u':
      Update_interval = strtol(optarg,NULL,0);
      break;
    case 'r':
      Samprate = strtol(optarg,NULL,0);
      break;
    case 'p':
      Playout = strtod(optarg,NULL);
      break;
    case 'a':
      Auto_position = true;
      break;
    case 'S':
      Auto_sort = true;
      break;
    default:
      fprintf(stderr,"Usage: %s [-a] [-v] [-q] [-L] [-u update] [-R audio_device] [-p|-P playout_delay_ms] [-r samprate] -I mcast_address [-I mcast_address]\n",argv[0]);
      exit(1);
    }
  }
  // Also accept groups without -I option
  for(int i=optind; i < argc; i++){
    if(Nfds == MAX_MCAST){
      fprintf(stderr,"Too many multicast addresses; max %d\n",MAX_MCAST);
    } else 
      Mcast_address_text[Nfds++] = argv[i];
  }

  if(Repeater_tail != 0){
    // Operating as a repeater controller; initialize
    // Make these settable parameters
    // -29 dB is -15 + (-14).
    // -15 dBFS is the target level of the FM demodulator
    // -14 dB is 1 kHz ID deviation divided by 5 kHz peak deviation
    Dit_length = init_morse(18.0,800.0,-29.0,48000.0);
  }    
  tzset();
  PaError r = Pa_Initialize();
  if(r != paNoError){
    fprintf(stderr,"Portaudio error: %s\n",Pa_GetErrorText(r));
    return r;
  }
  atexit(cleanup);

  if(List_audio){
    // On stdout, not stderr, so we can toss ALSA's noisy error messages
    printf("Audio devices:\n");
    int numDevices = Pa_GetDeviceCount();
    for(int inDevNum=0; inDevNum < numDevices; inDevNum++){
      const PaDeviceInfo *deviceInfo = Pa_GetDeviceInfo(inDevNum);
      printf("%d: %s\n",inDevNum,deviceInfo->name);
    }
    exit(0);
  }
  if(Nfds == 0){
    fprintf(stderr,"At least one input group required, exiting\n");
    exit(1);
  }
  load_id();
  char *nextp = NULL;
  int d;
  int numDevices = Pa_GetDeviceCount();
  if(strlen(Audiodev) == 0){
    // not specified; use default
    inDevNum = Pa_GetDefaultOutputDevice();
  } else if(d = strtol(Audiodev,&nextp,0),nextp != Audiodev && *nextp == '\0'){
    if(d >= numDevices){
      fprintf(stderr,"%d is out of range, use %s -L for a list\n",d,argv[0]);
      exit(1);
    }
    inDevNum = d;
  } else {
    for(inDevNum=0; inDevNum < numDevices; inDevNum++){
      const PaDeviceInfo *deviceInfo = Pa_GetDeviceInfo(inDevNum);
      if(strcmp(deviceInfo->name,Audiodev) == 0)
	break;
    }
  }
  if(inDevNum == paNoDevice){
    fprintf(stderr,"Portaudio: no available devices, exiting\n");
    exit(1);
  }

  // Create portaudio stream.
  // Runs continuously, playing silence until audio arrives.
  // This allows multiple streams to be played on hosts that only support one
  PaStreamParameters outputParameters;
  memset(&outputParameters,0,sizeof(outputParameters));
  outputParameters.channelCount = 2;
  outputParameters.device = inDevNum;
  outputParameters.sampleFormat = paFloat32;
  outputParameters.suggestedLatency = Latency; // 0 doesn't seem to be a good value on OSX, lots of underruns and stutters
  
  // Clear output buffer
  // Should already be cleared at startup, but there's often a loud burst of noise after startup
  // So maybe something is polluting it
  memset(Output_buffer,0,sizeof(Output_buffer));

  r = Pa_OpenStream(&Pa_Stream,
		    NULL,
		    &outputParameters,
		    Samprate,
		    paFramesPerBufferUnspecified, // seems to be 31 on OSX
		    //SAMPPCALLBACK,
		    0,
		    pa_callback,
		    NULL);

  if(r != paNoError){
    fprintf(stderr,"Portaudio error: %s, exiting\n",Pa_GetErrorText(r));      
    exit(1);
  }

  if(Repeater_tail != 0)
    pthread_create(&Repeater_thread,NULL,repeater_ctl,NULL); // Repeater mode active

  // Do this at the last minute at startup since the upcall will come quickly
  r = Pa_StartStream(Pa_Stream);
  if(r != paNoError){
    fprintf(stderr,"Portaudio error: %s, exiting\n",Pa_GetErrorText(r));
    exit(1);
  }
  Start_pa_time = Pa_GetStreamTime(Pa_Stream);
  Start_time = gps_time_ns();
  Last_error_time = Start_time;

  // Spawn one thread per address
  // All have to succeed in resolving their targets or we'll exit
  // This allows a restart when started automatically from systemd before avahi is fully running
  pthread_t sockthreads[Nfds];
  for(int i=0; i<Nfds; i++)
    pthread_create(&sockthreads[i],NULL,sockproc,Mcast_address_text[i]);

  // Graceful signal catch
  signal(SIGPIPE,closedown); // keeps repeater thread from crashing if cwd crashes
  signal(SIGINT,closedown);
  signal(SIGKILL,closedown);
  signal(SIGQUIT,closedown);
  signal(SIGTERM,closedown);
  signal(SIGHUP,closedown);
  signal(SIGPIPE,SIG_IGN);

  // Become the display thread
  if(!Quiet){
    display(NULL);
  } else {
    while(true)
      sleep(1000);
  }

  // won't actually get here
  echo();
  nocbreak();
  endwin();
  exit(0);
}

static void *sockproc(void *arg){
  char const *mcast_address_text = (char *)arg;
  {
    char name[100];
    snprintf(name,sizeof(name),"mon %s",mcast_address_text);
    pthread_setname(name);
  }

  int input_fd;
  {
    char iface[1024];
    struct sockaddr sock;
    resolve_mcast(mcast_address_text,&sock,DEFAULT_RTP_PORT,iface,sizeof(iface));
    input_fd = listen_mcast(&sock,iface);
  }

  if(input_fd == -1)
    pthread_exit(NULL);
  struct packet *pkt = NULL;
  
  // Main loop begins here
  while(1){

    // Need a new packet buffer?
    if(!pkt)
      pkt = malloc(sizeof(*pkt));
    // Zero these out to catch any uninitialized derefs
    pkt->next = NULL;
    pkt->data = NULL;
    pkt->len = 0;
    
    struct sockaddr_storage sender;
    socklen_t socksize = sizeof(sender);
    int size = recvfrom(input_fd,&pkt->content,sizeof(pkt->content),0,(struct sockaddr *)&sender,&socksize);
    
    if(size == -1){
      if(errno != EINTR){ // Happens routinely, e.g., when window resized
	perror("recvfrom");
	usleep(1000);
      }
      continue;  // Reuse current buffer
    }
    if(size <= RTP_MIN_SIZE)
      continue; // Must be big enough for RTP header and at least some data
    
    // Convert RTP header to host format
    unsigned char const *dp = ntoh_rtp(&pkt->rtp,pkt->content);
    pkt->data = dp;
    pkt->len = size - (dp - pkt->content);
    if(pkt->rtp.pad){
      pkt->len -= dp[pkt->len-1];
      pkt->rtp.pad = 0;
    }
    if(pkt->len <= 0)
      continue; // Used to be an assert, but would be triggered by bogus packets
    
    // Find appropriate session; create new one if necessary
    pthread_mutex_lock(&Sess_mutex); // Protect Nsessions
    struct session *sp = lookup_session(&sender,pkt->rtp.ssrc);
    pthread_mutex_unlock(&Sess_mutex);
    if(!sp){
      // Not found
      pthread_mutex_lock(&Sess_mutex); // Protect Nsessions
      sp = create_session();
      pthread_mutex_unlock(&Sess_mutex); // Protect Nsessions
      if(!sp){
	fprintf(stderr,"No room!!\n");
	continue;
      }
      sp->ssrc = pkt->rtp.ssrc;
      char const *id = lookupid(pkt->rtp.ssrc);
      if(id)
	strlcpy(sp->id,id,sizeof(sp->id));
      if(Auto_position)
	sp->pan = make_position(Position++);
      else
	sp->pan = 0;     // center by default
      sp->gain = 1;    // 0 dB by default
      sp->notch_enable = Autonotch;
      sp->muted = Start_muted;
      sp->dest = mcast_address_text;
      sp->start_rptr = Rptr;
      sp->start_timestamp = pkt->rtp.timestamp;
      sp->rtp_state.seq = pkt->rtp.seq;
      sp->reset = true;
      sp->samprate = samprate_from_pt(pkt->rtp.type);
      for(int j=0; j < N_tones; j++)
	init_goertzel(&sp->tone_detector[j],PL_tones[j]/(float)sp->samprate);

      pthread_mutex_init(&sp->qmutex,NULL);
      pthread_cond_init(&sp->qcond,NULL);
      if(pthread_create(&sp->task,NULL,decode_task,sp) == -1){
	perror("pthread_create");
	close_session(&sp);
	continue;
      }
    }
    // Copy sender, in case the port number changed
    memcpy(&sp->sender,&sender,sizeof(sender));
    
    // Insert onto queue sorted by sequence number, wake up thread
    struct packet *q_prev = NULL;
    struct packet *qe = NULL;
    pthread_mutex_lock(&sp->qmutex);
    for(qe = sp->queue; qe && pkt->rtp.seq >= qe->rtp.seq; q_prev = qe,qe = qe->next)
      ;
    
    pkt->next = qe;
    if(q_prev)
      q_prev->next = pkt;
    else
      sp->queue = pkt; // Front of list
    pkt = NULL;        // force new packet to be allocated
    // wake up decoder thread
    pthread_cond_signal(&sp->qcond);
    pthread_mutex_unlock(&sp->qmutex);
  }      
  return NULL;
}

static void decode_task_cleanup(void *arg){
  struct session *sp = (struct session *)arg;
  assert(sp);

  pthread_mutex_destroy(&sp->qmutex);
  pthread_cond_destroy(&sp->qcond);

  if(sp->opus){
    opus_decoder_destroy(sp->opus);
    sp->opus = NULL;
  }
  struct packet *pkt_next;
  for(struct packet *pkt = sp->queue; pkt; pkt = pkt_next){
    pkt_next = pkt->next;
    free(pkt);
    pkt = NULL;
  }
}

// Thread to decode incoming RTP packets for each session
static void *decode_task(void *arg){
  struct session *sp = (struct session *)arg;
  assert(sp);

  {
    char name[100];
    snprintf(name,sizeof(name),"dec %u",sp->ssrc);
    pthread_setname(name);
  }
  pthread_cleanup_push(decode_task_cleanup,arg);


  int late_rate = 0;
  int consec_futures = 0;

  // Main loop; run until asked to quit
  while(!sp->terminate){

    struct packet *pkt = NULL;
    // Wait for packet to appear on queue
    pthread_mutex_lock(&sp->qmutex);
    while(!sp->queue){
      long long const increment = 100000000; // 100 ns
      // pthread_cond_timedwait requires UTC clock time! Undefined behavior around a leap second...
      struct timespec ts;
      ns2ts(&ts,gps_time_ns() + increment + BILLION * (UNIX_EPOCH - GPS_UTC_OFFSET));
      int r = pthread_cond_timedwait(&sp->qcond,&sp->qmutex,&ts); // Wait 100 ms max so we pick up terminates
      if(r != 0){
	if(r == EINVAL)
	  Invalids++;
	pthread_mutex_unlock(&sp->qmutex);
	goto endloop;// restart loop, checking terminate flag
      }
    }
    pkt = sp->queue;
    sp->queue = pkt->next;
    pkt->next = NULL;
    pthread_mutex_unlock(&sp->qmutex);
    sp->packets++; // Count all packets, regardless of type
    if(sp->type != pkt->rtp.type) // Handle transitions both ways
      sp->type = pkt->rtp.type;

    sp->samprate = samprate_from_pt(sp->type);
    int upsample = 1;
    if(sp->samprate != 0)
      upsample = Samprate / sp->samprate; // Upsample lower PCM samprates to 48 kHz output (should be cleaner)

    if(pkt->rtp.seq != sp->rtp_state.seq){
      if(!pkt->rtp.marker){
	sp->rtp_state.drops++; // Avoid spurious drops when session is recreated after silence
	Last_error_time = gps_time_ns();
      }
      if(sp->opus)
	opus_decoder_ctl(sp->opus,OPUS_RESET_STATE); // Reset decoder
    } else {
      /* Handle wraparound in timestamp (unlikely but possible in long-lived stream)
	 This can still fail if there's an outage more than 2^31 samples long without a mark (seems unlikely)
	 Do only when packet is in sequence as heuristic check */
      while(sp->timestamp_upper + (int32_t)(pkt->rtp.timestamp - sp->start_timestamp) < 0)
	sp->timestamp_upper += (1LL << 32);

    }
    sp->rtp_state.seq = pkt->rtp.seq + 1;
    
    if(pkt->rtp.marker){
      // beginning of talk spurt, resync
      sp->active = 0; // reset active
      reset_session(sp,pkt->rtp.timestamp); // Updates sp->wptr
    }

    // decode Opus or PCM into bounce buffer
    if(sp->type == OPUS_PT){
      // Execute Opus decoder even when muted to keep its state updated
      if(!sp->opus){
	int error;
	
	// Always decode Opus to 48 kHz stereo, ignoring original sample rate
	sp->opus = opus_decoder_create(Samprate,2,&error);
	assert(sp->opus);
      }
      sp->channels = 2; // always decode as stereo, 48 kHz
      sp->samprate = Samprate;
      int const r1 = opus_packet_get_nb_samples(pkt->data,pkt->len,Samprate);
      if(r1 == OPUS_INVALID_PACKET)
	goto endloop;
      sp->frame_size = r1;
      int const r2 = opus_packet_get_bandwidth(pkt->data);
      if(r2 == OPUS_INVALID_PACKET)
	goto endloop;
      switch(r2){
      case OPUS_BANDWIDTH_NARROWBAND:
	sp->bandwidth = 4;
	break;
      case OPUS_BANDWIDTH_MEDIUMBAND:
	sp->bandwidth = 6;
	break;
      case OPUS_BANDWIDTH_WIDEBAND:
	sp->bandwidth = 8;
	break;
      case OPUS_BANDWIDTH_SUPERWIDEBAND:
	sp->bandwidth = 12;
	break;
      default:
      case OPUS_BANDWIDTH_FULLBAND:
	sp->bandwidth = 20;
	break;
      }
      int samples = opus_decode_float(sp->opus,pkt->data,pkt->len,sp->bounce[0],sizeof(sp->bounce),0); // fec?
      if(samples != sp->frame_size)
	fprintf(stderr,"samples %d frame-size %d\n",samples,sp->frame_size);

      assert(samples == r1);
    } else { // PCM -- should do this only if not muted
      // Test for invalidity
      sp->samprate = samprate_from_pt(sp->type);
      sp->bandwidth = sp->samprate / 2000;
      sp->channels = channels_from_pt(sp->type);
      if(sp->samprate <= 0 || sp->channels <= 0 || sp->channels > 2)
	goto endloop;
      sp->frame_size = pkt->len / (sizeof(int16_t) * sp->channels); // mono/stereo samples in frame
      if(sp->frame_size <= 0)
	goto endloop;

      signed short const *data_ints = (signed short *)&pkt->data[0];	
      for(int i=0; i < sp->frame_size && i < MAXSIZE; i++){
	assert((void *)data_ints >= (void *)&pkt->data[0] && (void *)data_ints < (void *)(&pkt->data[0] + pkt->len));
	float left = SCALE * (signed short)ntohs(*data_ints++);
	float right;
	if(sp->channels == 2){
	  right = SCALE * (signed short)ntohs(*data_ints++);
	} else {
	  right = left;
	}
	sp->bounce[i][0] = left;
	sp->bounce[i][1] = right;
      }
    }
    // Run PL tone decoders
    // Disable if display isn't active, since no other use is made of them
    if(!Quiet) {
      for(int i=0; i < sp->frame_size; i++){
	float s = sp->bounce[i][0] + sp->bounce[i][1]; // Mono sum
	for(int j = 0; j < N_tones; j++){
	  update_goertzel(&sp->tone_detector[j],s);
	}
      }
      sp->tone_samples += sp->frame_size;
      if(sp->tone_samples >= Tone_period * sp->samprate){
	sp->tone_samples = 0;
	int pl_tone_index = -1;
	float strongest_tone_energy = 0;
	float total_energy = 0;
	for(int j=0; j < N_tones; j++){
	  float energy = cnrmf(output_goertzel(&sp->tone_detector[j]));
	  total_energy += energy;
	  reset_goertzel(&sp->tone_detector[j]);
	  if(energy > strongest_tone_energy){
	    strongest_tone_energy = energy;
	    pl_tone_index = j;
	  }
	}
	if(2*strongest_tone_energy > total_energy && pl_tone_index >= 0){
	  // Tone must be > -3dB relative to total of all tones
	  sp->current_tone = PL_tones[pl_tone_index];
	} else
	  sp->current_tone = 0;
      } // End of tone observation period
    }
    /* Find where to write in circular output buffer
     * This is updated even when muted so the activity display will work */
    sp->wptr = sp->start_rptr + upsample * (sp->timestamp_upper + pkt->rtp.timestamp - sp->start_timestamp) + sp->playout;
    bool keyup = false; // Don't do printf while holding locks
    // Protect Output_buffer and Rptr, which are modified in portaudio callback
    pthread_mutex_lock(&Output_mutex);

    if(sp->wptr < Rptr){
      sp->lates++;
      // More than 2 lates in 10 packets triggers a reset
      if((late_rate += 10) < 20){
	pthread_mutex_unlock(&Output_mutex);
	goto endloop; // Drop packet as late
      }
      late_rate = 0;
      sp->reset = true;
    }
    if(late_rate > 0)
      late_rate--;
    if(sp->wptr > Rptr + BUFFERSIZE/2){
      sp->earlies++;
      if(++consec_futures < 3){
	pthread_mutex_unlock(&Output_mutex);
	goto endloop;
      }
      sp->reset = true;
    }
    consec_futures = 0;
    if(sp->reset)
      reset_session(sp,pkt->rtp.timestamp); // Updates sp->wptr

    if(sp->current_tone != 0 && sp->notch_tone != sp->current_tone){
      // New or changed tone
      sp->notch_tone = sp->current_tone;
      setIIRnotch(&sp->iir_right,sp->current_tone/sp->samprate);
      setIIRnotch(&sp->iir_left,sp->current_tone/sp->samprate);
    }
    if(!sp->muted){
      /* Compute gains and delays for stereo imaging
	 Extreme gain differences can make the source sound like it's inside an ear
	 This can be uncomfortable in good headphones with extreme panning
	 -6dB for each channel in the center
	 when full to one side or the other, that channel is +6 dB and the other is -inf dB */
      float const left_gain = sp->gain * (1 - sp->pan)/2;
      float const right_gain = sp->gain * (1 + sp->pan)/2;
      /* Delay less favored channel 0 - 1.5 ms max (determined
	 empirically) This is really what drives source localization
	 in humans The effect is so dramatic even with equal levels
	 you have to remove one earphone to convince yourself that the
	 levels really are the same */
      int const left_delay = (sp->pan > 0) ? round(sp->pan * .0015 * Samprate) : 0; // Delay left channel
      int const right_delay = (sp->pan < 0) ? round(-sp->pan * .0015 * Samprate) : 0; // Delay right channel

      assert(left_delay >= 0 && right_delay >= 0);

      // Mix bounce buffer into output buffer read by portaudio callback
      unsigned int left_index = sp->wptr + left_delay;
      unsigned int right_index = sp->wptr + right_delay;
      
      for(int i=0; i < sp->frame_size; i++){
	float left = sp->bounce[i][0] * left_gain;
	float right = sp->bounce[i][1] * right_gain;
	if(sp->notch_enable && sp->notch_tone > 0){
	  left = applyIIRnotch(&sp->iir_left,left);
	  right = applyIIRnotch(&sp->iir_right,right);
	}
	// Not the cleanest way to upsample the sample rate, but it works
	for(int j=0; j < upsample; j++){
	  Output_buffer[left_index++ & (BUFFERSIZE-1)][0] += left;
	  Output_buffer[right_index++ & (BUFFERSIZE-1)][1] += right;
	}
      }
      LastAudioTime = gps_time_ns();

      pthread_mutex_lock(&PTT_mutex);
      if(Repeater_tail != 0 && !PTT_state){
	PTT_state = true;
	keyup = true;
	pthread_cond_signal(&PTT_cond);
      }
      pthread_mutex_unlock(&PTT_mutex);
    }
    pthread_mutex_unlock(&Output_mutex); // Done with Output_buffer and Rptr
    if(Quiet && keyup){
      // debugging only, temp
      char result[1024];
      fprintf(stdout,"%s: PTT On\n",
	      format_gpstime(result,sizeof(result),LastAudioTime));
    }

    // Count samples and frames and advance write pointer even when muted
    sp->tot_active += (float)sp->frame_size / sp->samprate;
    sp->active += (float)sp->frame_size / sp->samprate;

    if(sp->frame_size > 0){
      sp->wptr += upsample * sp->frame_size; // increase displayed queue in status screen
#if 0
      float queue = (sp->wptr - Rptr) / Samprate;
      // Pause to allow some packet resequencing in the input thread
      if(queue > Latency){
	useconds_t pause_time = (useconds_t)(1e6 * (queue - Latency) / 2);
	usleep(pause_time);
      }
#endif
    }
  endloop:;
    if(pkt)
      free(pkt);
    pkt = NULL;
  }
  pthread_cleanup_pop(1);
  return NULL;
}

// Use ncurses to display streams
static void *display(void *arg){

  pthread_setname("display");

  if(initscr() == NULL){
    fprintf(stderr,"initscr() failed, disabling control/display thread\n");
    pthread_exit(NULL);
  }
  keypad(stdscr,TRUE);
  timeout(Update_interval);
  cbreak();
  noecho();

  int first_session = 0;
  int sessions_per_screen = 0;
  int current = -1; // No current session

  while(1){
    assert(first_session >= 0);
    assert(first_session == 0 || first_session < Nsessions);
    assert(current >= -1);
    assert(current == -1 || current < Nsessions); // in case Nsessions is 0

    move(0,0);
    clrtobot();
    addstr("KA9Q Multicast Audio Monitor:");
    for(int i=0;i<Nfds;i++)
      printw(" %s",Mcast_address_text[i]);
    addstr("\n");

    if(Help){
      char path [PATH_MAX];
      dist_path(path,sizeof(path),"monitor-help.txt");
      FILE *fp = fopen(path,"r");
      if(fp != NULL){
	size_t size = 1024;
	char *line = malloc(size);
	while(getline(&line,&size,fp) != -1){
	  addstr(line);
	}
	fclose(fp);
	fp = NULL;
      }
    }

    if(Start_muted)
      addstr("**Starting new sessions muted**\n");

    if(Quiet_mode){
      addstr("Hit 'q' to resume screen updates\n");
    } else {
      // First header line
      if(Repeater_tail != 0){
	if(PTT_state)
	  addstr("PTT On;  ");
	else
	  addstr("PTT Off; ");
	if(Last_id_time != 0)
	  printw("Last ID: %lld sec",(gps_time_ns() - Last_id_time) / BILLION);
      }
      int y,x;
      getyx(stdscr,y,x);
      x = 65; // work around unused variable warning
      mvaddstr(y,x,"------- Activity --------");
      if(Verbose)
	addstr(" Play  ----Codec----     ---------------RTP--------------------------");	
      addstr("\n");    
      
      // Second header line
      addstr("  dB Pan     SSRC  Tone Notch ID                                 Total   Current      Idle");
      if(Verbose)
	addstr(" Queue Type ms ch BW     packets resets drops lates early Source/Dest");
      addstr("\n");
      
      if(Auto_sort)
	sort_session_active();

      sessions_per_screen = LINES - getcury(stdscr) - 1;
      
      /* This mutex protects Sessions[] and Nsessions. Instead of holding the
	 lock for the entire display loop, we make a copy.
      */

      int Nsessions_copy;
      pthread_mutex_lock(&Sess_mutex);
      assert(Nsessions <= NSESSIONS);
      Nsessions_copy = Nsessions;
      struct session *Sessions_copy[NSESSIONS];
      memcpy(Sessions_copy,Sessions,Nsessions * sizeof(Sessions_copy[0]));
      if(Nsessions == 0)
	current = -1; // Not sure how this can happen, but in case
      if(current == -1 && Nsessions > 0)
	current = 0; // Session got created, make it current
      pthread_mutex_unlock(&Sess_mutex);
      
      for(int session = first_session; session < Nsessions_copy; session++){
	struct session *sp = Sessions_copy[session];
	
	// embolden entire line if active
	int queue = 1000 * (sp->wptr - Rptr) / Samprate;
	int idle = sp->wptr < Rptr ? (Rptr - sp->wptr) / Samprate : 0;
	
	if(queue > 0)
	  attr_on(A_BOLD,NULL);
	
	// Stand out gain and pan on currently selected channel
	if(session == current)
	  attr_on(A_STANDOUT,NULL);
	
	printw("%+4.0lf%4d",sp->muted ? -INFINITY : voltage2dB(sp->gain),(int)roundf(100*sp->pan));
	attr_off(A_STANDOUT,NULL);
	{
	  char id[100];
	  char identifier[31];
	  
	  // Truncate ID field to 30 places
	  strlcpy(identifier,sp->id,sizeof(identifier));
	  
	  printw("%9u %5.1f %5.1f %-30s%10.0f%10.0f%10s",
		 sp->ssrc,
		 sp->current_tone,
		 sp->notch_enable ? sp->notch_tone : 0,
		 identifier,
		 sp->tot_active, // Total active time, sec
		 sp->active,    // active time in current transmission, sec
		 ftime(id,sizeof(id),idle));   // Time idle since last transmission
	  
	}
	if(Verbose){
	  printw("%6d%5s%3d%3d%3d",
		 queue > 0 ? queue : 0, // Playout buffer length, fractional sec
		 sp->type == OPUS_PT ? "Opus" : "PCM",
		 (1000 * sp->frame_size/sp->samprate), // frame size, ms
		 sp->channels,
		 sp->bandwidth); // Bandwidth, kHz 
	  
	  printw("%'12lu",sp->packets);
	  printw("%'7llu",sp->resets);
	  printw("%'6llu",sp->rtp_state.drops);
	  printw("%'6llu",sp->lates);
	  printw("%'6llu",sp->earlies);
	  
	  // printable version of socket addresses and ports
	  if(sp->dest){ // Might not be allocated yet, if we got dispatched during the nameinfo() call
	    printw(" %s -> %s",formatsock(&sp->sender),sp->dest);
	  }
	}
	attr_off(A_BOLD,NULL); // In case it was on (channel is active)
	addstr("\n");
	if(getcury(stdscr) >= LINES-1) // Can't be at beginning, because y will clip at LINES!
	  break;
      } // end session loop
      
      if(Verbose){
	// Measure skew between sampling clock and UNIX real time (hopefully NTP synched)
	double unix_seconds = (gps_time_ns() - Start_time) * 1e-9;
	double pa_seconds = Pa_GetStreamTime(Pa_Stream) - Start_pa_time;
	printw("D/A clock error: %+.3lf ppm ",1e6 * (pa_seconds / unix_seconds - 1));
	// Time since last packet drop on any channel
	printw("Error-free seconds: %'.1lf\n",(1e-9*(gps_time_ns() - Last_error_time)));
	printw("Initial playout time: %.0f ms\n",Playout);
      }      
    }

    // process keyboard commands only if there's something to act on
    int c = getch(); // Pauses here
    
    // Not all of these commands require locking, but it's easier to just always do it
    pthread_mutex_lock(&Sess_mutex); // Re-lock after time consuming wgetch (which includes a refresh)
    // Since we unlocked & relocked, Nsessions might have changed (incremented) again
    if(Nsessions == 0)
      current = -1;
    if(Nsessions > 0 && current == -1)
      current = 0;
    switch(c){
    case 'v':
      Verbose = !Verbose;
      break;
    case 'A': // Start all new sessions muted
      Start_muted = !Start_muted;
      break;
    case 'U': // Unmute all sessions, resetting any that were muted
      for(int i = 0; i < Nsessions; i++){
	struct session *sp = Sessions[i];
	if(sp->muted){
	  sp->reset = true;
	  sp->muted = false;
	}
      }
      break;
    case 'M': // Mute all sessions
      for(int i = 0; i < Nsessions; i++){
	struct session *sp = Sessions[i];
	sp->muted = true;
      }
      break;
    case 'q':
      Quiet_mode = !Quiet_mode;
      break;
    case '\f':  // Screen repaint (formfeed, aka control-L)
      clearok(curscr,TRUE);
      break;
    case 'h': // Help screen
      Help = !Help;
      break;
    case 's': // Sort sessions by most recently active (or longest active)
      sort_session_active();
      break;
    case 't': // Sort sessions by most recently active (or longest active)
      sort_session_total();
      break;
    case 'N':
      for(int i=0; i < Nsessions; i++){
	struct session *sp = Sessions[i];
	if(sp != NULL && !sp->notch_enable){
	  sp->notch_enable = true;
	}
      }
      break;
    case 'n':
      if(current >= 0){
	if(!Sessions[current]->notch_enable)
	  Sessions[current]->notch_enable = true;
      }
      break;
    case 'f':
      if(current >= 0)
	Sessions[current]->notch_enable = false;
      break;
    case 'F':
      for(int i=0; i < Nsessions; i++){
	struct session *sp = Sessions[i];
	sp->notch_enable = false;
      }
      break;
    case KEY_RESIZE:
    case EOF:
      break;
    case KEY_NPAGE:
      if(first_session + sessions_per_screen < Nsessions){
	first_session += sessions_per_screen;
	current += sessions_per_screen;
	if(current > Nsessions-1)
	  current = Nsessions - 1;
      }
      break;
    case KEY_PPAGE:
      if(first_session - sessions_per_screen >= 0){
	first_session -= sessions_per_screen;
	current -= sessions_per_screen;
      }
      break;
    case KEY_HOME: // first session
      if(Nsessions > 0){
	current = 0;
	first_session = 0;
      }
      break;
    case KEY_END: // last session
      if(Nsessions > 0){
	current = Nsessions-1;
	first_session = max(0,Nsessions - sessions_per_screen);
      }
      break;
    case '\t':
    case KEY_DOWN:
      if(current >= 0 && current < Nsessions-1){
	current++;
	if(current >= first_session + sessions_per_screen - 1)
	  first_session++;
      }
      break;
    case KEY_BTAB:
    case KEY_UP:
      if(current > 0){
	current--;
	if(current < first_session)
	  first_session--;
      }
      break;
    case '=': // If the user doesn't hit the shift key (on a US keyboard) take it as a '+'
    case '+':
      if(current >= 0)
	Sessions[current]->gain *= 1.122018454; // +1 dB
      break;
    case '_': // Underscore is shifted minus
    case '-':
      if(current >= 0)
	Sessions[current]->gain /= 1.122018454; // -1 dB
      break;
    case KEY_LEFT:
      if(current >= 0)
	Sessions[current]->pan = max(Sessions[current]->pan - .01,-1.0);
      break;
    case KEY_RIGHT:
      if(current >= 0)
	Sessions[current]->pan = min(Sessions[current]->pan + .01,+1.0);
      break;
    case KEY_SLEFT: // Shifted left - decrease playout buffer 10 ms
      if(Playout >= -100){
	Playout -= 1;
	if(current >= 0)
	  Sessions[current]->reset = true;
      }
      break;
    case KEY_SRIGHT: // Shifted right - increase playout buffer 10 ms
      Playout += 1;
      if(current >= 0)
	Sessions[current]->reset = true;
      else
	beep();
      break;
    case 'u': // Unmute and reset current session
      if(current >= 0){
	struct session *sp = Sessions[current];
	if(sp->muted){
	  sp->reset = true;
	  sp->muted = false;
	}
      }
      break;
    case 'm': // Mute current session
      if(current >= 0)
	Sessions[current]->muted = true;
      break;
    case 'r':
      // Reset playout queue
      if(current >= 0)
	Sessions[current]->reset = true;
      break;
    case KEY_DC: // Delete
    case KEY_BACKSPACE:
    case 'd': // Delete current session
      if(Nsessions > 0){
	struct session *sp = Sessions[current];
	sp->terminate = true;
	// We have to wait for it to clean up before we close and remove its session
	pthread_join(sp->task,NULL);
	close_session(&sp); // Decrements Nsessions
	if(current >= Nsessions)
	  current = Nsessions-1; // -1 when no sessions
      }
      break;
    default: // Invalid command
      beep();
      break;
    }
    pthread_mutex_unlock(&Sess_mutex);
  }
  return NULL;
}
static void reset_session(struct session * const sp,uint32_t timestamp){
  sp->resets++;
  if(sp->opus)
    opus_decoder_ctl(sp->opus,OPUS_RESET_STATE); // Reset decoder
  sp->reset = false;
  sp->start_rptr = Rptr;
  sp->start_timestamp = timestamp; // Resynch as if new stream
  sp->timestamp_upper = 0;
  sp->playout = Playout * Samprate/1000;
  sp->wptr = Rptr + sp->playout;
}

// sort callback for sort_session_active() for comparing sessions by most recently active (or currently longest active)
static int scompare(void const *a, void const *b){
  struct session const * const s1 = *(struct session **)a;
  struct session const * const s2 = *(struct session **)b;
  
  if(s1->wptr > Rptr && s2->wptr > Rptr){
    // Both active

    // If within printout resolution (1 sec)
#define NOFUZZ 1
#if NOFUZZ
    if(fabsf(s1->active - s2->active) < 0.1) // equal within margin
      return 0;
#endif
    if(s1->active > s2->active)
      return -1;
    else 
      return +1; // Longer active above shorter active
  } else if(s1->wptr > Rptr && s2->wptr < Rptr){
    return -1; // all active sessions above all idle sessions
  } else if(s1->wptr < Rptr && s2->wptr > Rptr){
    return +1;
  
  // more recently idle sorts ahead of longer idle
  } else if(s1->wptr > s2->wptr){
    return -1;
  } else if(s1->wptr < s2->wptr){
    return 1;
  } else {
    return 0;
  }
}
// sort callback for sort_session() for comparing sessions by total time
static int tcompare(void const *a, void const *b){
  struct session const * const s1 = *(struct session **)a;
  struct session const * const s2 = *(struct session **)b;
  
#if NOFUZZ
  if(fabsf(s1->tot_active - s2->tot_active) < 0.1) // equal within margin
    return 0;
#endif
  if(s1->tot_active > s2->tot_active)
    return -1;
  return +1;
}

// Sort session list in increasing order of age
static int sort_session_active(void){
  qsort(Sessions,Nsessions,sizeof(Sessions[0]),scompare);
  return 0;
}
static int sort_session_total(void){
  qsort(Sessions,Nsessions,sizeof(Sessions[0]),tcompare);
  return 0;
}


static struct session *lookup_session(const struct sockaddr_storage *sender,const uint32_t ssrc){
  for(int i = 0; i < Nsessions; i++){
    struct session *sp = Sessions[i];
    if(sp->ssrc == ssrc && address_match(sender,&sp->sender))
      return sp;
  }
  return NULL;
}
// Create a new session, partly initialize
static struct session *create_session(void){
  struct session * const sp = calloc(1,sizeof(*sp));

  if(sp == NULL)
    return NULL; // Shouldn't happen on modern machines!

  // Put at end of list
  Sessions[Nsessions++] = sp;
  return sp;
}

static int close_session(struct session **p){
  if(p == NULL)
    return -1;
  struct session * const sp = *p;
  if(sp == NULL)
    return -1;
  assert(Nsessions > 0);
  
  // Remove from table
  for(int i = 0; i < Nsessions; i++){
    if(Sessions[i] == sp){
      Nsessions--;
      memmove(&Sessions[i],&Sessions[i+1],(Nsessions-i) * sizeof(Sessions[0]));
      free(sp);
      *p = NULL;
      return 0;
    }
  }
  assert(0); // get here only if not found, which shouldn't happen
  return -1;
}
static void closedown(int s){
  fprintf(stderr,"Signal %d, exiting\n",s);
  exit(0);
}


static void cleanup(void){
  Pa_Terminate();
  if(!Quiet){
    echo();
    nocbreak();
    endwin();
  }
}

// Portaudio callback - transfer data (if any) to provided buffer
static int pa_callback(void const *inputBuffer, void *outputBuffer,
		       unsigned long framesPerBuffer,
		       PaStreamCallbackTimeInfo const * timeInfo,
		       PaStreamCallbackFlags statusFlags,
		       void *userData){
  if(!outputBuffer)
    return paAbort; // can this happen??
  
#if 0
  if(Last_callback_time + 1.0 < timeInfo->currentTime){
    // We've been asleep for >1 sec. Reset everybody
    pthread_mutex_lock(&Sess_mutex);
    for(int i = 0; i < Nsessions; i++)
      Sessions[i]->reset = true;
    memset(Output_buffer,0,sizeof(Output_buffer)); // Wipe clean
    pthread_mutex_unlock(&Sess_mutex);
  }

  Last_callback_time = timeInfo->currentTime;
#endif

  assert(framesPerBuffer < BUFFERSIZE/2); // Make sure ring buffer is big enough
  float *out = outputBuffer;
  // I know they say you're not supposed to do any locks in the callback, but I don't see any alternative!
  pthread_mutex_lock(&Output_mutex);
  while(framesPerBuffer > 0){
    // chunk size = lesser of total amount needed or remainder of read buffer before wraparound
    unsigned long rptr = Rptr & (BUFFERSIZE-1);
    long chunk = min(framesPerBuffer,BUFFERSIZE-rptr);
    assert(chunk > 0);

    Rptr += chunk;
    framesPerBuffer -= chunk;
    memcpy(out,Output_buffer[rptr],chunk * sizeof(Output_buffer[0]));
    memset(Output_buffer[rptr],0,chunk * sizeof(Output_buffer[0]));
    out += chunk * 2;
  }
  pthread_mutex_unlock(&Output_mutex);
  return paContinue;
}

#if 0
// Send CWID through separate CW daemon (cwd)
// Use non-blocking IO; ignore failures
void send_cwid(void){

  if(Quiet){
    // Debug only, temp
    char result[1024];
    fprintf(stdout,"%s: CW ID started\n",format_gpstime(result,sizeof(result),gps_time_ns()));
  }
  int fd = open("/run/cwd/input",O_NONBLOCK|O_WRONLY);
  if(fd != -1){
    write(fd,Cwid,strlen(Cwid));
    close(fd);
  }
}
#else
// stub version that writes directly to local portaudio output buffer
void send_cwid(void){
  if(Quiet){
    // Debug only, temp
    char result[1024];
    fprintf(stdout,"%s: CW ID started\n",format_gpstime(result,sizeof(result),gps_time_ns()));
  }

  int16_t * const samples = malloc(60 * Dit_length * sizeof(samples[0]));
  pthread_mutex_lock(&Output_mutex);  
  int wptr = Rptr + (Playout * Samprate)/1000;
  pthread_mutex_unlock(&Output_mutex);

  for(char const *cp = Cwid; *cp != 0; cp++){
    int const samplecount = encode_morse_char(samples,(wchar_t)*cp);
    if(samplecount <= 0)
      break;
    pthread_mutex_lock(&Output_mutex);
    for(int i=0;i<samplecount;i++){
      Output_buffer[wptr & (BUFFERSIZE-1)][0] += SCALE * samples[i];
      Output_buffer[wptr++ & (BUFFERSIZE-1)][1] += SCALE * samples[i];
    }
    pthread_mutex_unlock(&Output_mutex);
    // Wait for it to play out
    long long const sleeptime = BILLION * samplecount / Samprate;
    struct timespec ts;
    ns2ts(&ts,sleeptime);
    nanosleep(&ts,NULL);
  }
  free(samples);
  if(Quiet){
    fprintf(stdout,"CW ID finished\n");
  }
}
#endif



// Repeater control for experimental multi-input repeater
// optional, run only if -t option is given
// Send CW ID at appropriate times
// Drop PTT some time after last write to audio output ring buffer
void *repeater_ctl(void *arg){
  pthread_setname("rptctl");
  while(1){
    // Wait for carrier to come up; set in main loop whenever audio buffer is written to
    pthread_mutex_lock(&PTT_mutex);
    while(!PTT_state)
      pthread_cond_wait(&PTT_cond,&PTT_mutex);
    pthread_mutex_unlock(&PTT_mutex);

    // Transmitter is on; are we required to ID?
    long long now = gps_time_ns();
    if(now > Last_id_time + MANDATORY_ID_INTERVAL){
      // must ID on top of users to satisfy FCC max ID interval
      Last_id_time = now;
      send_cwid();
      now = gps_time_ns(); // send_cwid() has delays
    }
    long long const drop_time = LastAudioTime + Repeater_tail;
    if(now < drop_time){
      // Sleep until possible end of timeout, or next mandatory ID, whichever is first
      long long const sleep_time = min(drop_time,Last_id_time + MANDATORY_ID_INTERVAL) - now;
      if(sleep_time > 0){
	struct timespec ts;
	ns2ts(&ts,sleep_time);
	nanosleep(&ts,NULL);
	continue;
      }
    }
    // Tail dropout timer expired
    // ID more often than 10 minutes to minimize IDing on top of users
    if(now > Last_id_time + QUIET_ID_INTERVAL){
      // update LastAudioTime here (even though it will be updated in the portaudio upcall)
      // so the PTT won't momentarily drop
      Last_id_time = now;
      // Send an ID
      // Use nonblocking I/O so we won't hang if the cwd fails for some reason
      // Ignore sigpipe signals too?
      send_cwid();
    } else {
      if(Quiet){
	// debug only, temp
	char result[1024];
	fprintf(stdout,"%s: PTT Off\n",format_gpstime(result,sizeof(result),gps_time_ns()));
      }
      pthread_mutex_lock(&PTT_mutex);
      PTT_state = false;
      pthread_mutex_unlock(&PTT_mutex);
    }
  }
  return NULL;
}


// Return an ascii string identifier indexed by ssrc
// Database in /usr/share/ka9q-radio/id.txt
struct idtable {
  uint32_t ssrc;
  char id[128];
};
#define IDSIZE 1024
static int Nid;
static struct idtable Idtable[IDSIZE];
static struct stat Last_stat;

static void load_id(void){
  char filename[PATH_MAX];
  dist_path(filename,sizeof(filename),ID);
  struct stat statbuf;
  stat(filename,&statbuf);
  if(statbuf.st_mtime != Last_stat.st_mtime)
    Nid = 0; // Force reload

  if(Nid == 0){
    // Load table
    FILE * const fp = fopen(filename,"r");
    if(fp == NULL)
      return;
    
    char line[1024];
    while(fgets(line,sizeof(line),fp)){
      chomp(line);
      char *ptr = NULL;
      if(line[0] == '#' || strlen(line) == 0)
	continue; // Comment
      assert(Nid < IDSIZE);
      Idtable[Nid].ssrc = strtol(line,&ptr,0);
      if(ptr == line)
	continue; // no parseable hex number
      
      while(*ptr == ' ' || *ptr == '\t')
	ptr++;
      int const len = strlen(ptr); // Length of ID field
      if(len > 0){ // Not null
	strlcpy(Idtable[Nid].id,ptr,sizeof(Idtable[Nid].id));
      }
      Nid++;
      if(Nid == IDSIZE){
	fprintf(stderr,"ID table overlow, size %d\n",Nid);
	break;
      }
    }
    fclose(fp);
  }
}

static char const *lookupid(uint32_t ssrc){
  for(int i=0; i < Nid; i++){
    if(Idtable[i].ssrc == ssrc)
      return Idtable[i].id;
  }
  return NULL;
}
// Assign pan position by reversing binary bits of counter
// Returns -1 to +1
static float make_position(int x){
  // Swap bit order
  int y = 0;
  const int w = 8;
  for(int i=0; i < w; i++){
    y = (y << 1) | (x & 1);
    x >>= 1;
  }
  // Scale
  return 0.5 * (((float)y / 128) - 1);
}
