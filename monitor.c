// Listen to multicast group(s), send audio to local sound device via portaudio
// Copyright 2018-2023 Phil Karn, KA9Q

#define _GNU_SOURCE 1
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <sys/stat.h>
#include <opus/opus.h>
#include <portaudio.h>
#include <ncurses.h>
#include <locale.h>
#include <signal.h>
#include <getopt.h>
#include <iniparser/iniparser.h>
#if __linux__
#include <bsd/string.h>
#include <alsa/asoundlib.h>
#else
#include <string.h>
#endif
#include <sysexits.h>
#include <poll.h>

#include "conf.h"
#include "config.h"
#include "misc.h"
#include "multicast.h"
#include "radio.h"
#include "iir.h"
#include "morse.h"

// Global constants
#define MAX_MCAST 20          // Maximum number of multicast addresses
#define BUFFERSIZE (1<<19)    // about 10.92 sec at 48 kHz - must be power of 2 times page size (4k)!
static float const Latency = 0.02; // chunk size for audio output callback
static float const Tone_period = 0.24; // PL tone integration period
#define NSESSIONS 1500

// Names of config file sections
static char const *Radio = "radio";
static char const *Audio = "audio";
static char const *Repeater = "repeater";
static char const *Display = "display";

// Command line/config file/interactive command parameters
static char const *Tx_on = "set_xcvr txon";
static char const *Tx_off = "set_xcvr txoff";
static int DAC_samprate = 48000;   // Actual hardware output rate
static int Update_interval = 100;  // Default time in ms between display updates
char const *App_path;
int Verbose;                       // Verbosity flag
static char const *Config_file;
static bool Quiet;                 // Disable curses
static bool Quiet_mode;            // Toggle screen activity after starting
static float Playout = 100;
static bool Constant_delay;
static bool Start_muted;
static bool Auto_position = true;  // first will be in the center
static int64_t Repeater_tail;
static char const *Cwid = "de nocall/r"; // Make this configurable!
static double ID_pitch = 800.0;
static double ID_level = -29.0;
static double ID_speed = 18.0;
static float Gain = 0; // unity gain by default
static bool Notch;
static char *Mcast_address_text[MAX_MCAST]; // Multicast address(es) we're listening to
static char const *Audiodev = "";    // Name of audio device; empty means portaudio's default
static int Position; // auto-position streams
static bool Auto_sort;
// IDs must be at least every 10 minutes per FCC 97.119(a)
static int64_t Mandatory_ID_interval;
// ID early when carrier is about to drop, to avoid stepping on users
static int64_t Quiet_ID_interval;
static int Dit_length; 
static int Channels = 2;
static char const *Init;

// Global variables that regularly change
static int64_t Last_xmit_time;
static int64_t Last_id_time;
static float *Output_buffer;
static int Buffer_length; // Bytes left to play out, max BUFFERSIZE
static volatile unsigned int Rptr;   // callback thread read pointer, *frames*
static volatile unsigned int Wptr;   // For monitoring length of output queue
static volatile bool PTT_state;
static uint64_t Audio_callbacks;
static unsigned long Audio_frames;
static volatile int64_t LastAudioTime;
static int32_t Portaudio_delay;
static pthread_t Repeater_thread;
static pthread_cond_t PTT_cond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t PTT_mutex = PTHREAD_MUTEX_INITIALIZER;
static int Nfds;                     // Number of streams
static pthread_mutex_t Sess_mutex = PTHREAD_MUTEX_INITIALIZER;
static PaStream *Pa_Stream;          // Portaudio stream handle
static int inDevNum;                 // Portaudio's audio output device index
static int64_t Start_time;
static pthread_mutex_t Stream_mutex = PTHREAD_MUTEX_INITIALIZER; // Control access to stream start/stop
static PaTime Start_pa_time;
static PaTime Last_callback_time;
static int Invalids;
static int64_t Last_error_time;
static int Nsessions;
static struct session *Sessions[NSESSIONS];
static bool Terminate;
static bool Voting;

int Mcast_ttl; // for decode_radio_status(); not really needed here


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

  uint32_t last_timestamp;  // Last timestamp seen
  unsigned int wptr;        // current write index into output PCM buffer, *frames*
  int playout;              // Initial playout delay, frames
  long long last_active;    // GPS time last active
  long long last_start;     // GPS time at last transition to active from idle
  float tot_active;         // Total PCM time, ns
  float active;             // Seconds we've been active (only when queue has stuff)

  OpusDecoder *opus;        // Opus codec decoder handle, if needed
  int frame_size;
  int bandwidth;            // Audio bandwidth
  struct goertzel tone_detector[N_tones];
  int tone_samples;
  float current_tone;       // Detected tone frequency

  int samprate;
  int channels;             // Channels (1 or 2)
  float gain;               // Gain; 1 = 0 dB
  float pan;                // Stereo position: 0 = center; -1 = full left; +1 = full right

  unsigned long packets;    // RTP packets for this session
  unsigned long empties;    // RTP but no data
  unsigned long lates;
  unsigned long earlies;
  unsigned long resets;
  unsigned long reseqs;

  bool terminate;            // Set to cause thread to terminate voluntarily
  bool muted;
  bool reset;                // Set to force output timing reset on next packet
  bool now_active;           // for convenience of painting output
  
  char id[32];
  bool notch_enable;         // Enable PL removal notch
  struct iir iir_left;
  struct iir iir_right;  
  float notch_tone;
  struct channel chan;
  struct frontend frontend;

};

static void load_id(void);
static void cleanup(void);
static void *display(void *);
static void reset_session(struct session *sp,uint32_t timestamp);
static struct session *lookup_session(struct sockaddr_storage const *,uint32_t);
static struct session *create_session(void);
static int sort_session_active(void),sort_session_total(void);
static int close_session(struct session **);
static int pa_callback(void const *,void *,unsigned long,PaStreamCallbackTimeInfo const *,PaStreamCallbackFlags,void *);
static void *decode_task(void *x);
static void *sockproc(void *arg);
static void *repeater_ctl(void *arg);
static char const *lookupid(uint32_t ssrc);
static float make_position(int);
static bool kick_output();
static inline int modsub(unsigned int const a, unsigned int const b, int const modulus){
  int diff = (int)a - (int)b;
  if(diff > modulus)
    return diff % modulus; // Unexpectedly large, just do it the slow way

  if(diff > modulus/2)
   return  diff - modulus;

  if(diff < -modulus)
    return diff % modulus; // Unexpectedly small

  if(diff < -modulus/2)
    diff += modulus;

  return diff;
}


static char Optstring[] = "CI:LR:Sac:f:g:p:qr:su:vnV";
static struct  option Options[] = {
   {"center", no_argument, NULL, 'C'},
   {"input", required_argument, NULL, 'I'},
   {"list-audio", no_argument, NULL, 'L'},
   {"device", required_argument, NULL, 'R'},
   {"autosort", no_argument, NULL, 'S'},
   {"channels", required_argument, NULL, 'c'},
   {"config", required_argument, NULL, 'f'},
   {"gain", required_argument, NULL, 'g'},
   {"playout", required_argument, NULL, 'p'},
   {"quiet", no_argument, NULL, 'q'},
   {"samprate",required_argument,NULL,'r'},
   {"update", required_argument, NULL, 'u'},
   {"verbose", no_argument, NULL, 'v'},
   {"notch", no_argument, NULL, 'n'},
   {"version", no_argument, NULL, 'V'},
   {"voting", no_argument, NULL, 's'},
   {NULL, 0, NULL, 0},
};


#ifdef __linux__
// Get rid of those fucking ALSA error messages that clutter the screen
static void alsa_error_handler(const char *file, int line, const char *function, int err, const char *fmt, ...){
  return;
}
#endif

int main(int argc,char * const argv[]){
  App_path = argv[0];
  setlocale(LC_ALL,getenv("LANG"));
  tzset();

  // Parse command line for config file, read first so it can be overriden by command line args
  int c;
  while((c = getopt_long(argc,argv,Optstring,Options,NULL)) != -1){
    switch(c){
    case 'f':
      Config_file = optarg;
      break;
    case 'V':
      VERSION();
      exit(EX_OK);
    default:
      break;
    }
  }
  if(Config_file){
    dictionary *Configtable = iniparser_load(Config_file);
    if(Configtable == NULL){
     fprintf(stdout,"Can't load config file %s\n",Config_file);
      exit(EX_NOINPUT);
    }
    DAC_samprate = config_getint(Configtable,Audio,"samprate",DAC_samprate);
    Channels = config_getint(Configtable,Audio,"channels",Channels);
    char const *audiodev = config_getstring(Configtable,"audio","device",NULL);
    if(audiodev)
      Audiodev = strdup(audiodev);
    // Add validity checking

    Gain = config_getfloat(Configtable,Audio,"gain",Gain);
    Cwid = strdup(config_getstring(Configtable,Repeater,"id","NOCALL"));
    // 600 sec is 10 minutes, max ID interval per FCC 97.119(a)
    int const period = config_getint(Configtable,Repeater,"period",600);
    int pperiod = config_getint(Configtable,Repeater,"pperiod",period/2);
    if(pperiod > period)
      pperiod = period;
    Mandatory_ID_interval = period * BILLION;
    Quiet_ID_interval = pperiod * BILLION;
    ID_pitch = config_getfloat(Configtable,Repeater,"pitch",ID_pitch);
    ID_level = config_getfloat(Configtable,Repeater,"level",ID_level);
    Notch = config_getboolean(Configtable,Audio,"notch",Notch);
    Quiet = config_getboolean(Configtable,Display,"quiet",Quiet);
    if(config_getboolean(Configtable,Audio,"center",false))
      Auto_position = false;

    Auto_sort = config_getboolean(Configtable,Display,"autosort",Auto_sort);
    Update_interval = config_getint(Configtable,Display,"update",Update_interval);
    Playout = config_getfloat(Configtable,Audio,"playout",Playout);
    Repeater_tail = config_getfloat(Configtable,Repeater,"tail",Repeater_tail);
    Verbose = config_getboolean(Configtable,Display,"verbose",Verbose);
    char const *txon = config_getstring(Configtable,Radio,"txon",NULL);
    char const *txoff = config_getstring(Configtable,Radio,"txoff",NULL);
    if(txon)
      Tx_on = strdup(txon);
    if(txoff)
      Tx_off = strdup(txoff);

    char const *init = config_getstring(Configtable,Radio,"init",NULL);
    if(init)
      Init = strdup(init);

    char const *input = config_getstring(Configtable,Audio,"input",NULL);
    if(input)
      Mcast_address_text[Nfds++] = strdup(input);
    iniparser_freedict(Configtable);
  }
  // Rescan args to override config file
  bool list_audio = false;
  optind = 0; // reset getopt()
  while((c = getopt_long(argc,argv,Optstring,Options,NULL)) != -1){
    switch(c){
    case 'c':
      Channels = strtol(optarg,NULL,0);
      break;
    case 'f':
      break; // Ignore this time
    case 'g':
      Gain = strtof(optarg,NULL);
      break;
    case 'n':
      Notch = true;
      break;
    case 'p':
      Playout = strtof(optarg,NULL);
      break;
    case 'q': // No ncurses
      Quiet = true;
      break;
    case 'r':
      DAC_samprate = strtol(optarg,NULL,0);
      break;
    case 'u':
      Update_interval = strtol(optarg,NULL,0);
      break;
    case 'v':
      Verbose = true;
      break;
    case 'I':
      if(Nfds == MAX_MCAST){
	fprintf(stderr,"Too many multicast addresses; max %d\n",MAX_MCAST);
      } else 
	Mcast_address_text[Nfds++] = optarg;
      break;
    case 'L':
      list_audio = true;
      break;
    case 'R':
      Audiodev = optarg;
      break;
    case 'S':
      Auto_sort = true;
      break;
    case 's':
      Voting = true;
      Auto_position = false; // Disables 
      break;
    default:
      fprintf(stderr,"Usage: %s -L\n",App_path);
      fprintf(stderr,"       %s [-a] [-c channels] [-f config_file] [-g gain] [-p playout] [-q] [-r samprate] [-u update] [-v]\
[-I mcast_address] [-R audiodev] [-S] [mcast_address ...]\n",App_path);
      exit(EX_USAGE);
    }
  }
  if(list_audio){
    // On stdout, not stderr, so we can toss ALSA's noisy error messages
    PaError r = Pa_Initialize();
    if(r != paNoError){
      fprintf(stderr,"Portaudio error: %s\n",Pa_GetErrorText(r));
      return r;
    }
    printf("Audio devices:\n");
    int numDevices = Pa_GetDeviceCount();
    for(int inDevNum=0; inDevNum < numDevices; inDevNum++){
      const PaDeviceInfo *deviceInfo = Pa_GetDeviceInfo(inDevNum);
      printf("%d: %s\n",inDevNum,deviceInfo->name);
    }
    Pa_Terminate();
    exit(EX_OK);
  }

  if(Channels != 1 && Channels != 2){
    fprintf(stderr,"Channels = %d invalid; defaulting to 2\n",Channels);
    Channels = 2;
  }
  if(Auto_position && Channels != 2){
    fprintf(stderr,"Auto_position requires 2 channels\n");
    Auto_position = false;
  }

  // Also accept groups without -I option
  for(int i=optind; i < argc; i++){
    if(Nfds == MAX_MCAST){
      fprintf(stderr,"Too many multicast addresses; max %d\n",MAX_MCAST);
    } else 
      Mcast_address_text[Nfds++] = argv[i];
  }
  if(Nfds == 0){
    fprintf(stderr,"At least one input group required, exiting\n");
    exit(EX_USAGE);
  }

  if(Init != NULL)
    (void) - system(Init);

  if(Cwid != NULL){
    // Operating as a repeater controller; initialize
    // Make these settable parameters
    // -29 dB is -15 + (-14).
    // -15 dBFS is the target level of the FM demodulator
    // -14 dB is 1 kHz ID deviation divided by 5 kHz peak deviation
    Dit_length = init_morse(ID_speed,ID_pitch,ID_level,DAC_samprate);
  }    
#ifdef __linux__
  // Get rid of those fucking ALSA error messages that clutter the screen
  snd_lib_error_set_handler(alsa_error_handler);
#endif

  PaError r = Pa_Initialize();
  if(r != paNoError){
    fprintf(stderr,"Portaudio error: %s\n",Pa_GetErrorText(r));
    return r;
  }
  atexit(cleanup); // Make sure Pa_Terminate() gets called

  load_id();
  char *nextp = NULL;
  int d;
  int numDevices = Pa_GetDeviceCount();
  if(Audiodev == NULL || strlen(Audiodev) == 0){
    // not specified; use default
    inDevNum = Pa_GetDefaultOutputDevice();
  } else if(d = strtol(Audiodev,&nextp,0),nextp != Audiodev && *nextp == '\0'){
    if(d >= numDevices){
      fprintf(stderr,"%d is out of range, use %s -L for a list\n",d,App_path);
      exit(EX_USAGE);
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
    exit(EX_IOERR);
  }

  // Create portaudio stream.
  // Runs continuously, playing silence until audio arrives.
  // This allows multiple streams to be played on hosts that only support one
  Output_buffer = mirror_alloc(BUFFERSIZE * Channels * sizeof(*Output_buffer)); // Must be power of 2 times page size
  memset(Output_buffer,0,BUFFERSIZE * Channels * sizeof(*Output_buffer)); // Does mmap clear its initial memory? Not sure

  PaStreamParameters outputParameters;
  memset(&outputParameters,0,sizeof(outputParameters));
  outputParameters.channelCount = Channels;
  outputParameters.device = inDevNum;
  outputParameters.sampleFormat = paFloat32;
  outputParameters.suggestedLatency = Latency; // 0 doesn't seem to be a good value on OSX, lots of underruns and stutters

  r = Pa_OpenStream(&Pa_Stream,
		    NULL,
		    &outputParameters,
		    DAC_samprate,
		    paFramesPerBufferUnspecified, // seems to be 31 on OSX
		    //SAMPPCALLBACK,
		    0,
		    pa_callback,
		    NULL);

  if(r != paNoError){
    fprintf(stderr,"Portaudio error: %s, exiting\n",Pa_GetErrorText(r));      
    exit(EX_IOERR);
  }

  if(Repeater_tail != 0)
    pthread_create(&Repeater_thread,NULL,repeater_ctl,NULL); // Repeater mode active

  // Spawn one thread per address
  // All have to succeed in resolving their targets or we'll exit
  // This allows a restart when started automatically from systemd before avahi is fully running
  pthread_t sockthreads[Nfds];
  for(int i=0; i<Nfds; i++)
    pthread_create(&sockthreads[i],NULL,sockproc,Mcast_address_text[i]);

  Last_error_time = gps_time_ns();

  // Become the display thread
  if(!Quiet){
    display(NULL);
  } else {
    while(!Terminate)
      sleep(1);
  }
  exit(EX_OK); // calls cleanup() to clean up Portaudio and ncurses. Can't happen...
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

  int status_fd;
  {
    char iface[1024];
    struct sockaddr sock;
    resolve_mcast(mcast_address_text,&sock,DEFAULT_STAT_PORT,iface,sizeof(iface));
    status_fd = listen_mcast(&sock,iface);
  }
  struct packet *pkt = NULL;
  
  realtime();
  // Main loop begins here
  while(!Terminate){
    int const nfds = 2;
    struct pollfd fds[nfds];
    fds[0].fd = input_fd;
    fds[0].events = POLLIN;
    fds[0].revents = 0;
    fds[1].fd = status_fd;
    fds[1].events = POLLIN;
    fds[1].revents = 0;
    int n = poll(fds,nfds,100); // timeout to poll terminate flag
    if(n < 0)
      perror("poll");
    if(n <= 0)
      continue;

    struct sockaddr_storage sender;
    socklen_t socksize = sizeof(sender);

    if(fds[1].revents & (POLLIN|POLLPRI)){
      // Got a status packet
      uint8_t buffer[PKTSIZE];
      int length = recvfrom(status_fd,buffer,sizeof(buffer),0,(struct sockaddr *)&sender,&socksize);
      if(buffer[0] != STATUS) // not status, ignore
	continue;

      struct channel chan;
      struct frontend frontend;
      decode_radio_status(&frontend,&chan,buffer+1,length-1);
      uint32_t ssrc = chan.output.rtp.ssrc;
      pthread_mutex_lock(&Sess_mutex); // Protect Nsessions
      struct session *sp = lookup_session(&sender,ssrc);
      pthread_mutex_unlock(&Sess_mutex);
      if(!sp){
	// Status arrived before first RTP; create and init session
	pthread_mutex_lock(&Sess_mutex); // Protect Nsessions
	sp = create_session();
	pthread_mutex_unlock(&Sess_mutex); // Protect Nsessions
	if(!sp){
	  fprintf(stderr,"No room!!\n");
	  continue;
	}
	char const *id = lookupid(ssrc); // rather than freq? resolve this.
	if(id)
	  strlcpy(sp->id,id,sizeof(sp->id));
	if(Auto_position)
	  sp->pan = make_position(Position++);
	else
	  sp->pan = 0;     // center by default
	pthread_cond_init(&sp->qcond,NULL);
	pthread_mutex_init(&sp->qmutex,NULL);
	sp->ssrc = chan.output.rtp.ssrc;
	memcpy(&sp->sender,&sender,sizeof(sender)); // Bind to specific host and sending port
	sp->gain = powf(10.,0.05 * Gain);    // Start with global default
	sp->notch_enable = Notch;
	sp->muted = Start_muted;
	sp->dest = mcast_address_text;
	sp->last_timestamp = chan.output.rtp.timestamp;
	sp->rtp_state.seq = chan.output.rtp.seq;
	sp->reset = true;
	sp->active = 0;
	sp->type = chan.output.rtp.type; // Check range 0-127?
	sp->samprate = chan.output.samprate;
	if(PT_table[chan.output.rtp.type].encoding == OPUS)
	  sp->samprate = DAC_samprate;
	
	for(int j=0; j < N_tones; j++)
	  init_goertzel(&sp->tone_detector[j],PL_tones[j]/(float)sp->samprate);
	
	if(pthread_create(&sp->task,NULL,decode_task,sp) == -1){
	  perror("pthread_create");
	  close_session(&sp);
	  continue;
	}
      }
      memcpy(&sp->chan,&chan,sizeof(sp->chan));
      sp->chan.inuse = true;
      memcpy(&sp->frontend,&frontend,sizeof(sp->frontend));
      continue; // next packet
    }

    if(!(fds[0].revents & (POLLIN|POLLPRI)))
      continue; // Not ready to receive a data packet
    // Need a new packet buffer?
    if(!pkt)
      pkt = malloc(sizeof(*pkt));
    // Zero these out to catch any uninitialized derefs
    pkt->next = NULL;
    pkt->data = NULL;
    pkt->len = 0;
    
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
    uint8_t const *dp = ntoh_rtp(&pkt->rtp,pkt->content);
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
      pthread_cond_init(&sp->qcond,NULL);
      pthread_mutex_init(&sp->qmutex,NULL);

      sp->ssrc = pkt->rtp.ssrc;
      memcpy(&sp->sender,&sender,sizeof(sender)); // Bind to specific host and sending port
      char const *id = lookupid(pkt->rtp.ssrc);
      if(id)
	strlcpy(sp->id,id,sizeof(sp->id));
      if(Auto_position)
	sp->pan = make_position(Position++);
      else
	sp->pan = 0;     // center by default
      sp->gain = powf(10.,0.05 * Gain);    // Start with global default
      sp->notch_enable = Notch;
      sp->muted = Start_muted;
      sp->dest = mcast_address_text;
      sp->last_timestamp = pkt->rtp.timestamp;
      sp->rtp_state.seq = pkt->rtp.seq;
      sp->reset = true;
      sp->type = pkt->rtp.type;
      if(sp->type < 0 || sp->type > 127)
	continue; // Invalid payload type?
      sp->samprate = PT_table[sp->type].samprate;
      if(PT_table[sp->type].encoding == OPUS)
	sp->samprate = DAC_samprate;

      for(int j=0; j < N_tones; j++)
	init_goertzel(&sp->tone_detector[j],PL_tones[j]/(float)sp->samprate);

      if(pthread_create(&sp->task,NULL,decode_task,sp) == -1){
	perror("pthread_create");
	close_session(&sp);
	continue;
      }
    }
    // Insert onto queue sorted by sequence number, wake up thread
    struct packet *q_prev = NULL;
    struct packet *qe = NULL;
    pthread_mutex_lock(&sp->qmutex);
    for(qe = sp->queue; qe && pkt->rtp.seq >= qe->rtp.seq; q_prev = qe,qe = qe->next)
      ;
    
    if(qe)
      sp->reseqs++;   // Not the last on the list
    pkt->next = qe;
    if(q_prev)
      q_prev->next = pkt;
    else
      sp->queue = pkt; // Front of list
    pkt = NULL;        // force new packet to be allocated
    long long t = gps_time_ns();
    if(t - sp->last_active > BILLION){
      // Transition from idle to active 
      sp->last_start = t;
    }
    sp->last_active = t;
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
    FREE(pkt);
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

  int consec_lates = 0;
  int consec_earlies = 0;
  float *bounce = NULL;

  // Main loop; run until asked to quit
  while(!sp->terminate && !Terminate){
    struct packet *pkt = NULL;
    // Wait for packet to appear on queue
    pthread_mutex_lock(&sp->qmutex);
    while(!sp->queue){
      int64_t const increment = 100000000; // 100 ms
      // pthread_cond_timedwait requires UTC clock time! Undefined behavior around a leap second...
      struct timespec ts;
      ns2ts(&ts,utc_time_ns() + increment);
      int r = pthread_cond_timedwait(&sp->qcond,&sp->qmutex,&ts); // Wait 100 ms max so we pick up terminates
      if(r != 0){
	if(r == EINVAL)
	  Invalids++;
	pthread_mutex_unlock(&sp->qmutex);
	goto endloop;// restart loop, checking terminate flags
      }
    }
    // Peek at first packet on queue; is it in sequence?
    if(sp->queue->rtp.seq != sp->rtp_state.seq){
      // No. If we've got plenty in the playout buffer, sleep to allow some packet resequencing in the input thread.
      // Strictly speaking, we will resequence ourselves below with the RTP timestamp. But that works properly only with stateless
      // formats like PCM. Opus is stateful, so it's better to resequence input packets (using the RTP sequence #) when possible.
      float queue = (float)modsub(sp->wptr,Rptr,BUFFERSIZE) / DAC_samprate;
      if(queue > Latency + 0.1){ // 100 ms for scheduling latency?
	pthread_mutex_unlock(&sp->qmutex);
	struct timespec ss;
	ns2ts(&ss,(int64_t)(1e9 * (queue - (Latency + 0.1))));
	nanosleep(&ss,NULL);
	goto endloop;
      }
      // else the playout queue is close to draining, accept out of sequence packet anyway
    }
    pkt = sp->queue;
    sp->queue = pkt->next;
    pkt->next = NULL;
    pthread_mutex_unlock(&sp->qmutex);
    sp->packets++; // Count all packets, regardless of type
    if(sp->type != pkt->rtp.type) // Handle transitions both ways
      sp->type = pkt->rtp.type;

    if((int16_t)(pkt->rtp.seq - sp->rtp_state.seq) > 0){ // Doesn't really handle resequencing
      if(!pkt->rtp.marker){
	sp->rtp_state.drops++; // Avoid spurious drops when session is recreated after silence
	Last_error_time = gps_time_ns();
      }
      if(sp->opus)
	opus_decoder_ctl(sp->opus,OPUS_RESET_STATE); // Reset decoder
    }
    sp->rtp_state.seq = pkt->rtp.seq + 1;
    if(!sp->muted && pkt->rtp.marker){
      // beginning of talk spurt, resync
      reset_session(sp,pkt->rtp.timestamp); // Updates sp->wptr
    }
    int upsample = 1;

    // decode Opus or PCM into bounce buffer
    if(PT_table[sp->type].encoding == OPUS){
      // Execute Opus decoder even when muted to keep its state updated
      if(!sp->opus){
	int error;
	
	// Decode Opus to the selected sample rate
	sp->opus = opus_decoder_create(DAC_samprate,Channels,&error);
	if(error != OPUS_OK)
	  fprintf(stderr,"opus_decoder_create error %d\n",error);

	assert(sp->opus);
      }
      sp->channels = Channels;
      sp->samprate = DAC_samprate;
      // Opus RTP timestamps always referenced to 48 kHz
      int const r0 = opus_packet_get_nb_samples(pkt->data,pkt->len,48000);
      if(r0 == OPUS_INVALID_PACKET || r0 == OPUS_BAD_ARG)
	goto endloop;

      int const r1 = opus_packet_get_nb_samples(pkt->data,pkt->len,DAC_samprate);
      if(r1 == OPUS_INVALID_PACKET || r1 == OPUS_BAD_ARG)
	goto endloop;

      assert(r1 >= 0);
      sp->frame_size = r1;
      int const r2 = opus_packet_get_bandwidth(pkt->data);
      if(r2 == OPUS_INVALID_PACKET || r2 == OPUS_BAD_ARG)
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
      size_t const bounce_size = sizeof(*bounce) * sp->frame_size * sp->channels;
      assert(bounce == NULL); // detect possible memory leaks
      bounce = malloc(bounce_size);
      int const samples = opus_decode_float(sp->opus,pkt->data,pkt->len,bounce,bounce_size,0);
      if(samples != sp->frame_size)
	fprintf(stderr,"samples %d frame-size %d\n",samples,sp->frame_size);
    } else { // PCM
      // Test for invalidity
      int const samprate = samprate_from_pt(sp->type);
      if(samprate == 0)
	goto endloop;
      sp->samprate = samprate;
      upsample = DAC_samprate / sp->samprate; // Upsample lower PCM samprates to output rate (should be cleaner; what about decimation?)
      sp->bandwidth = sp->samprate / 2000;    // in kHz allowing for Nyquist
      sp->channels = channels_from_pt(sp->type); // channels in packet (not portaudio output buffer)
      
      if(sp->samprate <= 0 || sp->channels <= 0 || sp->channels > 2)
	goto endloop;
      sp->frame_size = pkt->len / (sizeof(int16_t) * sp->channels); // mono/stereo samples in frame
      if(sp->frame_size <= 0)
	goto endloop;
      int16_t const * const data_ints = (int16_t *)&pkt->data[0];	
      assert(bounce == NULL);
      bounce = malloc(sizeof(*bounce) * sp->frame_size * sp->channels);
      for(int i=0; i < sp->channels * sp->frame_size; i++)
	bounce[i] = SCALE16 * (int16_t)ntohs(data_ints[i]);
    }
    // Run PL tone decoders
    // Disable if display isn't active and autonotching is off
    // Fed audio that might be discontinuous or out of sequence, but it's a pain to fix
    if(sp->notch_enable) {
      for(int i=0; i < sp->frame_size; i++){
	float s;
	if(sp->channels == 2)
	  s = 0.5 * (bounce[2*i] + bounce[2*i+1]); // Mono sum
	else // sp->channels == 1
	  s = bounce[i];
	
	for(int j = 0; j < N_tones; j++)
	  update_goertzel(&sp->tone_detector[j],s);
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
      if(sp->current_tone != 0 && sp->notch_tone != sp->current_tone){
	// New or changed tone
	sp->notch_tone = sp->current_tone;
	setIIRnotch(&sp->iir_right,sp->current_tone/sp->samprate);
	setIIRnotch(&sp->iir_left,sp->current_tone/sp->samprate);
      }
    } // sp->notch_enable

    // Count samples and frames and advance write pointer even when muted
    sp->tot_active += (float)sp->frame_size / sp->samprate;
    sp->active += (float)sp->frame_size / sp->samprate;

    if(sp->muted)
      goto endloop; // No more to do with this frame

    kick_output(); // Ensure Rptr is current
    // Sequence number processing and write pointer updating
    if(modsub(sp->wptr,Rptr,BUFFERSIZE) < 0){
      sp->lates++;
      if(++consec_lates < 3 || Constant_delay)
	goto endloop; // Drop packet as late
      // 3 or more consecutive lates triggers a reset
      sp->reset = true;
    }
    consec_lates = 0;
    if(modsub(sp->wptr,Rptr,BUFFERSIZE) > BUFFERSIZE/4){
      sp->earlies++;
      if(++consec_earlies < 3)
	goto endloop; // Drop if just a few
      sp->reset = true; // should this happen if Constant_delay is set?
    }
    consec_earlies = 0;
    if(sp->reset)
      reset_session(sp,pkt->rtp.timestamp); // Resets sp->wptr and last_timestamp
    else {
      // Normal packet, relative adjustment to write pointer
      // Can difference in timestamps be negative? Cast it anyway
      // Opus always counts timestamps at 48 kHz so this breaks when DAC_samprate is not 48 kHz
      // For opus, sp->wptr += (int32_t)(pkt->rtp.timestamp - sp->last_timestamp) * DAC_samprate / 48000;
      sp->wptr += (int32_t)(pkt->rtp.timestamp - sp->last_timestamp) * upsample;
      sp->wptr &= (BUFFERSIZE-1);
      sp->last_timestamp = pkt->rtp.timestamp;
    }
    
    if(Channels == 2){
      /* Compute gains and delays for stereo imaging
	 Extreme gain differences can make the source sound like it's inside an ear
	 This can be uncomfortable in good headphones with extreme panning
	 -6dB for each channel in the center
	 when full to one side or the other, that channel is +6 dB and the other is -inf dB */
      float const left_gain = sp->gain * (1 - sp->pan)/2;
      float const right_gain = sp->gain * (1 + sp->pan)/2;
      /* Delay less favored channel 0 - 1.5 ms max (determined
	 empirically) This is really what drives source localization
	 in humans. The effect is so dramatic even with equal levels
	 you have to remove one earphone to convince yourself that the
	 levels really are the same! */
      int const left_delay = (sp->pan > 0) ? round(sp->pan * .0015 * DAC_samprate) : 0; // Delay left channel
      int const right_delay = (sp->pan < 0) ? round(-sp->pan * .0015 * DAC_samprate) : 0; // Delay right channel
      
      assert(left_delay >= 0 && right_delay >= 0);
      
      // Mix bounce buffer into output buffer read by portaudio callback
      // Simplified by mirror buffer wrap
      int left_index = 2 * (sp->wptr + left_delay);
      int right_index = 2 * (sp->wptr + right_delay) + 1;
      
      for(int i=0; i < sp->frame_size; i++){
	float left,right;
	if(sp->channels == 1){
	  // Mono input, put on both channels
	  left = bounce[i];
	  if(sp->notch_enable && sp->notch_tone > 0)
	    left = applyIIRnotch(&sp->iir_left,left);
	  right = left;
	} else {
	  // stereo input
	  left = bounce[2*i];
	  right = bounce[2*i+1];
	  if(sp->notch_enable && sp->notch_tone > 0){
	    left = applyIIRnotch(&sp->iir_left,left);
	    right = applyIIRnotch(&sp->iir_right,right);
	  }
	}
	// Not the cleanest way to upsample the sample rate, but it works
	for(int j=0; j < upsample; j++){
	  Output_buffer[left_index] += left * left_gain;
	  Output_buffer[right_index] += right * right_gain;
	  left_index += 2;
	  right_index += 2;
	}
	if(modsub(right_index/2,Wptr,BUFFERSIZE) > 0)
	   Wptr = right_index / 2; // samples to frames; For verbose mode
      }
    } else { // Channels == 1, no panning
      int64_t index = sp->wptr;
      for(int i=0; i < sp->frame_size; i++){
	float s;
	if(sp->channels == 1){
	  s = bounce[i];
	} else {
	  // Downmix to mono
	  s = 0.5 * (bounce[2*i] + bounce[2*i+1]);
	}
	if(sp->notch_enable && sp->notch_tone > 0)
	  s = applyIIRnotch(&sp->iir_left,s);
	// Not the cleanest way to upsample the sample rate, but it works
	for(int j=0; j < upsample; j++){
	  Output_buffer[index++] += s * sp->gain;
	}
	if(modsub(index,Wptr,BUFFERSIZE) > 0)
	   Wptr = index; // For verbose mode
      }
    } // Channels == 1

  endloop:;
    FREE(bounce);
    FREE(pkt);
  } // !sp->terminate
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
  bool help = false;
  int last_best_session = 0;

  while(!Terminate){
    assert(first_session >= 0);
    assert(first_session == 0 || first_session < Nsessions);
    assert(current >= -1);
    assert(current == -1 || current < Nsessions); // in case Nsessions is 0

    // Start screen update
    move(0,0);
    clrtobot();
    addstr("KA9Q Multicast Audio Monitor:");
    for(int i=0;i<Nfds;i++)
      printw(" %s",Mcast_address_text[i]);
    addstr("\n");

    if(help){
      char path [PATH_MAX];
      dist_path(path,sizeof(path),"monitor-help.txt");
      FILE *fp = fopen(path,"r");
      if(fp != NULL){
	size_t size = 1024;
	char *line = malloc(size);
	while(getline(&line,&size,fp) != -1)
	  addstr(line);

	FREE(line);
	fclose(fp);
	fp = NULL;
      }
    }

    if(Quiet_mode){
      addstr("Hit 'q' to resume screen updates\n");
    } else {
      // First header line
      if(Repeater_tail != 0){
	if(Last_id_time != 0)
	  printw("Last ID: %lld sec",(long long)((gps_time_ns() - Last_id_time) / BILLION));
	if(PTT_state)
	  addstr(" PTT On");
	else if(Last_xmit_time != 0)
	  printw(" PTT Off; Last xmit: %lld sec",(long long)((gps_time_ns() - Last_xmit_time) / BILLION));
	printw("\n");
      }
      if(Constant_delay)
	printw("Constant delay ");

      if(Start_muted)
	printw("**Starting new sessions muted** ");

      if(Voting)
	printw("SNR Voting enabled ");

      int y,x;
      getyx(stdscr,y,x);
      if(x != 0)
	printw("\n");

      if(Auto_sort)
	sort_session_active();

      sessions_per_screen = LINES - getcury(stdscr) - 1;
      
      // This mutex protects Sessions[] and Nsessions. Instead of holding the
      // lock for the entire display loop, we make a copy.
      pthread_mutex_lock(&Sess_mutex);
      assert(Nsessions <= NSESSIONS);
      int Nsessions_copy = Nsessions;
      struct session *Sessions_copy[NSESSIONS];
      memcpy(Sessions_copy,Sessions,Nsessions * sizeof(Sessions_copy[0]));
      if(Nsessions == 0)
	current = -1; // Not sure how this can happen, but in case
      if(current == -1 && Nsessions > 0)
	current = 0; // Session got created, make it current
      pthread_mutex_unlock(&Sess_mutex);
      
      // Flag active sessions
      long long time = gps_time_ns();
      for(int session = first_session; session < Nsessions_copy; session++){
	struct session *sp = Sessions_copy[session];
	sp->now_active = (time - sp->last_active) < BILLION/2;
      }
      if(Verbose){
	// Measure skew between sampling clock and UNIX real time (hopefully NTP synched)
	double pa_seconds = Pa_GetStreamTime(Pa_Stream) - Start_pa_time;
	int q = modsub(Wptr,Rptr,BUFFERSIZE);
	double qd = (double) q / DAC_samprate;
	double rate = Audio_frames / pa_seconds;
	
	printw("Playout %.0f ms, latency %d ms, queue %.3lf sec, D/A rate %'.3lf Hz,",Playout,Portaudio_delay,qd,rate);
	printw(" (%+.3lf ppm),",1e6 * (rate / DAC_samprate - 1));
	// Time since last packet drop on any channel
	printw(" Error-free sec %'.1lf\n",(1e-9*(gps_time_ns() - Last_error_time)));
      }
      // Show channel statuses
      getyx(stdscr,y,x);
      int row_save = y;
      int col_save = x;

      // dB column
      mvprintw(y++,x,"%4s","dB");
      for(int session = first_session; session < Nsessions_copy; session++,y++){
	struct session *sp = Sessions_copy[session];
	mvprintw(y,x,"%+4.0lf",sp->muted ? -INFINITY : voltage2dB(sp->gain));
      }
      x += 5;
      y = row_save;
      if(Auto_position){
	// Pan column
	mvprintw(y++,x," Pan");
	for(int session = first_session; session < Nsessions_copy; session++,y++){
	  struct session *sp = Sessions_copy[session];
	  mvprintw(y,x,"%4d",(int)roundf(100*sp->pan));
	}
	x += 4;
	y = row_save;
      }

      // SSRC
      mvprintw(y++,x,"%9s","SSRC");
      for(int session = first_session; session < Nsessions_copy; session++,y++){
	struct session *sp = Sessions_copy[session];
	mvprintw(y,x,"%9d",sp->ssrc);
      }
      x += 10;
      y = row_save;

      if(Notch){
	mvprintw(y++,x,"%5s","Tone");
	for(int session = first_session; session < Nsessions_copy; session++,y++){
	  struct session *sp = Sessions_copy[session];
	  if(sp->notch_enable == false || sp->current_tone == 0)
	    continue;
	  
	  mvprintw(y,x,"%5.1f",sp->current_tone);
	}
	x += 6;
	y = row_save;
	
	mvprintw(y++,x,"%5s","Notch");
	for(int session = first_session; session < Nsessions_copy; session++,y++){
	  struct session *sp = Sessions_copy[session];
	  if(sp->notch_enable == false || sp->notch_tone == 0)
	    continue;
	  
	  mvprintw(y,x,"%5.1f",sp->notch_tone);
	}
	x += 6;
	y = row_save;
      }
      mvprintw(y++,x,"%12s","Freq");
      for(int session = first_session; session < Nsessions_copy; session++,y++){
	struct session *sp = Sessions_copy[session];
	if(sp->chan.tune.freq != 0)
	  mvprintw(y,x,"%'12.0lf",sp->chan.tune.freq);
      }
      x += 13;
      y = row_save;
      
      mvprintw(y++,x,"%5s","Mode");
      for(int session = first_session; session < Nsessions_copy; session++,y++){
	struct session *sp = Sessions_copy[session];
	mvprintw(y,x,"%5s",sp->chan.preset);
      }
      x += 6;
      y = row_save;

      mvprintw(y++,x,"%5s","SNR");
      float snrs[Nsessions_copy]; // Keep SNRs for voting decisions
      for(int session = first_session; session < Nsessions_copy; session++,y++){
	struct session *sp = Sessions_copy[session];
	struct channel *chan = &sp->chan;
	float const noise_bandwidth = fabsf(chan->filter.max_IF - chan->filter.min_IF);
	float sig_power = chan->sig.bb_power - noise_bandwidth * chan->sig.n0;
	if(sig_power < 0)
	  sig_power = 0; // Avoid log(-x) = nan
	float const sn0 = sig_power/chan->sig.n0;
	float const snr = power2dB(sn0/noise_bandwidth);
	snrs[session] = sp->now_active ? snr : -INFINITY;
	if(!isnan(snr))
	  mvprintw(y,x,"%5.1f",snr);
      }
      // Find the best with 1 dB hysteresis - should it be configurable?
      int best_session = last_best_session;
      for(int i = 0; i < Nsessions_copy; i++){
	if(snrs[i] > snrs[last_best_session] + 1.0)
	   best_session = i;
      }
      last_best_session = best_session;

      x += 6;
      y = row_save;

      int longest = 0;
      mvprintw(y++,x,"%-30s","ID");
      for(int session = first_session; session < Nsessions_copy; session++,y++){
	struct session *sp = Sessions_copy[session];
	int len = strlen(sp->id);
	if(len > longest)
	  longest = len;
	mvprintw(y,x,"%-30s",sp->id);
      }
      x += longest;
      y = row_save;

      mvprintw(y++,x,"%10s","Total");
      for(int session = first_session; session < Nsessions_copy; session++,y++){
	struct session *sp = Sessions_copy[session];
	char total_buf[100];
	mvprintw(y,x,"%10s",ftime(total_buf,sizeof(total_buf),sp->tot_active));
      }
      x += 11;
      y = row_save;

      mvprintw(y++,x,"%10s","Cur/idle");
      for(int session = first_session; session < Nsessions_copy; session++,y++){
	struct session *sp = Sessions_copy[session];
	char buf[100];
	if(sp->now_active)
	  mvprintw(y,x,"%10s",ftime(buf,sizeof(buf),sp->active));
	else {
	  sp->active = 0; // Clear accumulated value
	  float idle_sec = (time - sp->last_active) / BILLION;
	  mvprintw(y,x,"%10s",ftime(buf,sizeof(buf),idle_sec));   // Time idle since last transmission
	}
      }
      x += 11;
      y = row_save;

      mvprintw(y++,x,"%6s","Queue");
      for(int session = first_session; session < Nsessions_copy; session++,y++){
	struct session *sp = Sessions_copy[session];
	if(!sp->now_active)
	  continue;

	int d = modsub(sp->wptr,Rptr,BUFFERSIZE); // Unplayed samples on queue
	int queue_ms = d > 0 ? 1000 * d / DAC_samprate : 0; // milliseconds
	if(sp->now_active && !sp->muted)
	  mvprintw(y,x,"%6d",queue_ms);   // Time idle since last transmission
      }
      x += 7;
      y = row_save;

      if(Verbose){
	// Opus/pcm
	mvprintw(y++,x,"Type");
	for(int session = first_session; session < Nsessions_copy; session++,y++){
	  struct session *sp = Sessions_copy[session];
	  
	  if(sp->samprate != 0)
	    mvprintw(y,x,"%4s",PT_table[sp->type].encoding == OPUS ? "Opus" : "PCM");
	}
	x += 5;
	y = row_save;
	// frame size, ms
	mvprintw(y++,x,"%3s","ms");
	for(int session = first_session; session < Nsessions_copy; session++,y++){
	  struct session *sp = Sessions_copy[session];
	  mvprintw(y,x,"%3d",(1000 * sp->frame_size/sp->samprate)); // frame size, ms
	}
	x += 6;
	y = row_save;
	  
	// channels
	mvprintw(y++,x,"%3s","ch");
	for(int session = first_session; session < Nsessions_copy; session++,y++){
	  struct session *sp = Sessions_copy[session];
	  mvprintw(y,x,"%3d",sp->channels);
	}
	x += 4;
	y = row_save;

	// BW
	mvprintw(y++,x,"%3s","bw");
	for(int session = first_session; session < Nsessions_copy; session++,y++){
	  struct session *sp = Sessions_copy[session];
	  mvprintw(y,x,"%3d",sp->bandwidth);
	}
	x += 4;
	y = row_save;
	
	// Packets
	mvprintw(y++,x,"%12s","Packets");
	for(int session = first_session; session < Nsessions_copy; session++,y++){
	  struct session *sp = Sessions_copy[session];
	  mvprintw(y,x,"%12lu",sp->packets);
	}
	x += 13;
	y = row_save;
	
	// Resets
	mvprintw(y++,x,"%7s","resets");
	for(int session = first_session; session < Nsessions_copy; session++,y++){
	  struct session *sp = Sessions_copy[session];
	  mvprintw(y,x,"%7lu",sp->resets);
	}
	x += 8;
	y = row_save;
	
	// BW
	mvprintw(y++,x,"%6s","drops");
	for(int session = first_session; session < Nsessions_copy; session++,y++){
	  struct session *sp = Sessions_copy[session];
	  mvprintw(y,x,"%'6llu",(unsigned long long)sp->rtp_state.drops);
	}
	x += 7;
	y = row_save;
	
	// Lates
	mvprintw(y++,x,"%6s","lates");
	for(int session = first_session; session < Nsessions_copy; session++,y++){
	  struct session *sp = Sessions_copy[session];
	  mvprintw(y,x,"%6lu",sp->lates);
	}
	x += 7;
	y = row_save;
	
	// BW
	mvprintw(y++,x,"%6s","reseq");
	for(int session = first_session; session < Nsessions_copy; session++,y++){
	  struct session *sp = Sessions_copy[session];
	  mvprintw(y,x,"%6lu",sp->reseqs);
	}
	x += 7;
	y = row_save;

	// Sockets
	mvprintw(y++,x,"%s","sockets");
	for(int session = first_session; session < Nsessions_copy; session++,y++){
	  struct session *sp = Sessions_copy[session];
	  mvprintw(y,x,"%s -> %s",formatsock(&sp->sender),sp->dest);
	}
      }
      // Embolden the active lines
      attr_t attrs;
      short pair;
      attr_get(&attrs, &pair, NULL);
      for(int session = first_session; session < Nsessions_copy; session++){
	struct session *sp = Sessions_copy[session];
	
	attr_t attr = A_NORMAL;
	attr |= session == current ? A_UNDERLINE : 0;
	attr |= sp->now_active ? A_BOLD : 0;

	// 1 adjusts for the titles
	// only underscore to just before the socket entry since it's variable length
	mvchgat(1 + row_save + session,col_save,x,attr,pair,NULL);
      }
      // End of display writing
      // Experimental voting feature - mute all but best SNR
      if(Voting){
	for(int session = first_session; session < Nsessions_copy; session++,y++){
	  struct session *sp = Sessions_copy[session];
	  sp->muted = (session == best_session) ? false : true;
	}
      }
    }

    // process keyboard commands only if there's something to act on
    int const c = getch(); // Waits for 'update interval' ms if no input
    if(c == EOF)
      continue; // No key hit; don't lock and unlock Sess_mutex
    
    // Not all of these commands require locking, but it's easier to just always do it
    pthread_mutex_lock(&Sess_mutex); // Re-lock after time consuming getch() (which includes a refresh)
    // Since we unlocked & relocked, Nsessions might have changed (incremented) again
    if(Nsessions == 0)
      current = -1;
    if(Nsessions > 0 && current == -1)
      current = 0;
    switch(c){
    case 'Q': // quit program
      Terminate = true;
      break;
    case 'v':
      Verbose = !Verbose;
      break;
    case 'C':
      Constant_delay = !Constant_delay;
      break;
    case 'A': // Start all new sessions muted
      Start_muted = !Start_muted;
      break;
    case 'U': // Unmute all sessions, resetting any that were muted
      for(int i = 0; i < Nsessions; i++){
	struct session *sp = Sessions[i];
	if(sp->muted){
	  sp->reset = true; // Resynchronize playout buffer (output callback may have paused)
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
      help = !help;
      break;
    case 's': // Sort sessions by most recently active (or longest active)
      sort_session_active();
      break;
    case 'S':
      Auto_sort = !Auto_sort;
      break;
    case 't': // Sort sessions by most recently active (or longest active)
      sort_session_total();
      break;
    case 'N':
      Notch = true;
      for(int i=0; i < Nsessions; i++){
	struct session *sp = Sessions[i];
	if(sp != NULL && !sp->notch_enable){
	  sp->notch_enable = true;
	}
      }
      break;
    case 'n':
      Notch = true;
      if(current >= 0){
	if(!Sessions[current]->notch_enable)
	  Sessions[current]->notch_enable = true;
      }
      break;
    case 'R': // Reset all sessions
      for(int i=0; i < Nsessions;i++)
	Sessions[i]->reset = true;
      break;	
    case 'f':
      if(current >= 0)
	Sessions[current]->notch_enable = false;
      break;
    case 'F':
      Notch = false;
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
	  sp->reset = true; // Resynchronize playout buffer (output callback may have paused)
	  sp->muted = false;
	}
      }
      break;
    case 'm': // Mute current session
      if(current >= 0)
	Sessions[current]->muted = true;
      break;
    case 'r':
      // Manually reset playout queue
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
  sp->last_timestamp = timestamp;
  sp->playout = Playout * DAC_samprate/1000;
  sp->wptr = (Rptr + sp->playout) & (BUFFERSIZE-1);
}

// sort callback for sort_session_active() for comparing sessions by most recently active (or currently longest active)
static int scompare(void const *a, void const *b){
  struct session const * const s1 = *(struct session **)a;
  struct session const * const s2 = *(struct session **)b;
  
  if(s1->active > 0 && s2->active > 0){
    // Fuzz needed because active sessions are updated when packets arrive
    if(fabsf(s1->active - s2->active) < 0.5)
      return 0; // Equal within 1/2 sec
    if(s1->active > s2->active)
      return -1; // Longer active lower
    else
      return +1;
  }
  if(s1->active <= 0 && s2->active > 0)
    return +1; // Active always lower than inactive
  if(s1->active >= 0 && s2->active < 0)
    return -1;

  // Both inactive
  if(s1->last_active > s2->last_active)
    return -1;
  else
    return +1;
  // Chances of equality are nil
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
  struct session * sp = *p;
  if(sp == NULL)
    return -1;
  assert(Nsessions > 0);
  
  // Remove from table
  for(int i = 0; i < Nsessions; i++){
    if(Sessions[i] == sp){
      Nsessions--;
      memmove(&Sessions[i],&Sessions[i+1],(Nsessions-i) * sizeof(Sessions[0]));
      pthread_cond_destroy(&sp->qcond);
      pthread_mutex_destroy(&sp->qmutex);
      FREE(sp);
      *p = NULL;
      return 0;
    }
  }
  assert(0); // get here only if not found, which shouldn't happen
  return -1;
}

// passed to atexit, invoked at exit
// must not call exit() to avoid looping
static void cleanup(void){
  if(Repeater_tail != 0 && Tx_off != NULL){
    int r __attribute__((unused));
    r = system(Tx_off);
  }
  Pa_StopStream(Pa_Stream);
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
  Audio_callbacks++;
  Audio_frames += framesPerBuffer;

  if(!outputBuffer)
    return paAbort; // can this happen??
  
  Last_callback_time = timeInfo->currentTime;
  assert(framesPerBuffer < BUFFERSIZE); // Make sure ring buffer is big enough
  // Delay within Portaudio in milliseconds
  Portaudio_delay = 1000. * (timeInfo->outputBufferDacTime - timeInfo->currentTime);

  // Use mirror buffer to simplify wraparound. Count is in bytes = Channels * frames * sizeof(float)
  int const bytecount = Channels * framesPerBuffer * sizeof(*Output_buffer);
  memcpy(outputBuffer,&Output_buffer[Channels*Rptr],bytecount);
  // Zero what we just copied
  memset(&Output_buffer[Channels*Rptr],0,bytecount);
  Rptr += framesPerBuffer;
  Rptr &= (BUFFERSIZE-1);
  Buffer_length -= framesPerBuffer;
#if 0
  int q = modsub(Wptr,Rptr,BUFFERSIZE);
  if(q < 0)
    return paComplete;
  return (Buffer_length <= 0) ?  paComplete : paContinue;
#endif
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
  float samples[60 * Dit_length];
  kick_output(); // Start output stream if it was stopped, so we can get current Rptr
  uint32_t wptr = (Rptr + ((long)Playout * DAC_samprate))/1000;
  wptr &= (BUFFERSIZE-1);

  // Don't worry about wrap during write, the mirror will handle it
  for(char const *cp = Cwid; *cp != '\0'; cp++){
    int const samplecount = encode_morse_char(samples,(wchar_t)*cp);
    if(samplecount <= 0)
      break;
    if(Channels == 2){
      for(int i=0;i<samplecount;i++){
	Output_buffer[2*wptr] += samples[i];
	Output_buffer[(2*wptr++ + 1)] += samples[i];
      }
      if(modsub(wptr/2,Wptr,BUFFERSIZE) > 0)
	 Wptr = wptr / 2;
    } else { // Channels == 1
      for(int i=0;i<samplecount;i++)
	Output_buffer[wptr++] += samples[i];
      if(modsub(wptr,Wptr,BUFFERSIZE) > 0)
	 Wptr = wptr;
    }
    kick_output(); // In case it has already drained; the ID could be quite long
    int64_t const sleeptime = BILLION * samplecount / DAC_samprate;
    struct timespec ts;
    ns2ts(&ts,sleeptime);
    nanosleep(&ts,NULL);    // Wait for it to play out
  }
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

  while(!Terminate){
    // Wait for audio output; set in kick_output()
    pthread_mutex_lock(&PTT_mutex);
    while(!PTT_state)
      pthread_cond_wait(&PTT_cond,&PTT_mutex);
    pthread_mutex_unlock(&PTT_mutex);

    // Turn transmitter on
    if(Tx_on != NULL)
      (void) - system(Tx_on);
    if(Quiet){ // curses display is not on
      // debugging only, temp
      char result[1024];
      fprintf(stdout,"%s: PTT On\n",
	      format_gpstime(result,sizeof(result),LastAudioTime));
    }
    while(true){
      int64_t now = gps_time_ns();
      // When are we required to ID?
      if(now >= Last_id_time + Mandatory_ID_interval){
	// must ID on top of users to satisfy FCC max ID interval
	Last_id_time = now;
	send_cwid();
	now = gps_time_ns(); // send_cwid() has delays
      }
      int64_t const drop_time = LastAudioTime + BILLION * Repeater_tail;
      if(now >= drop_time)
	break;
      
      // Sleep until possible end of timeout, or next mandatory ID, whichever is first
      int64_t const sleep_time = min(drop_time,Last_id_time + Mandatory_ID_interval) - now;
      if(sleep_time > 0){
	struct timespec ts;
	ns2ts(&ts,sleep_time);
	nanosleep(&ts,NULL);
      }
    }
    // time to drop transmitter
    // See if we can ID early before dropping, to avoid a mandatory ID on the next transmission
    int64_t now = gps_time_ns();
    if(now > Last_id_time + Mandatory_ID_interval / 2){
      Last_id_time = now;
      send_cwid();
      now = gps_time_ns();
    }
    pthread_mutex_lock(&PTT_mutex);
    PTT_state = false;
    pthread_mutex_unlock(&PTT_mutex);
    Last_xmit_time = gps_time_ns();
    if(Quiet){
      // debug only, temp
      char result[1024];
      fprintf(stdout,"%s: PTT Off\n",format_gpstime(result,sizeof(result),gps_time_ns()));
    }
    if(Tx_off != NULL)
      (void) - system(Tx_off);
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
  x += 1; // Force first position to be in center, which is the default with a single stream
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

// Start output stream if it was off; reset idle timeout on output audio stream activity
// Return true if we (re)started it
bool kick_output(){
  bool restarted = false;
  pthread_mutex_lock(&Stream_mutex);
  if(!Pa_IsStreamActive(Pa_Stream)){
    // Start it up
    if(!Pa_IsStreamStopped(Pa_Stream))
      Pa_StopStream(Pa_Stream); // it was in limbo

    Start_time = gps_time_ns();
    Start_pa_time = Pa_GetStreamTime(Pa_Stream); // Stream Time runs continuously even when stream stopped
    Audio_frames = 0;
    // Adjust Rptr for the missing time we were asleep, but only
    // if this isn't the first time
    // This will break if someone goes back in time and starts this program at precisely 00:00:00 UTC on 1 Jan 1970 :-)
    if(Last_callback_time != 0){
      Rptr += DAC_samprate * (Start_pa_time - Last_callback_time);
      Rptr &= (BUFFERSIZE-1);
    }

    int r = Pa_StartStream(Pa_Stream); // Immediately triggers the first callback
    if(r != paNoError){
      fprintf(stderr,"Portaudio error: %s, aborting\n",Pa_GetErrorText(r));
      abort();
    }
    restarted = true;
  }
  Buffer_length = BUFFERSIZE; // (Continue to) run for at least the length of the ring buffer
  pthread_mutex_unlock(&Stream_mutex);

  // Key up the repeater if it's configured and not already on
  if(Repeater_tail != 0){
    LastAudioTime = gps_time_ns();
    pthread_mutex_lock(&PTT_mutex);
    if(!PTT_state){
      PTT_state = true;
      pthread_cond_signal(&PTT_cond); // Notify the repeater control thread to ID and run drop timer
    }
    pthread_mutex_unlock(&PTT_mutex);
  
  }
  return restarted;
}
