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
#include <fcntl.h>
#include <time.h>
#include <sys/socket.h>
#include <stdatomic.h>

#include "conf.h"
#include "config.h"
#include "misc.h"
#include "multicast.h"
#include "radio.h"
#include "iir.h"
#include "morse.h"
#include "status.h"
#include "monitor.h"

// Could be (obscure) config file parameters
double const Latency = 0.02; // chunk size for audio output callback
double const Tone_period = 0.24; // PL tone integration period

// Voting hysteresis table. Small at low SNR, larger at large SNR to minimize pointless switching
// When the current SNR is 'snr', don't switch to another channel unless it's at least 'hysteresis' dB stronger
#define HSIZE (7)
struct {
  double snr;
  double hysteresis;
} Hysteresis_table[HSIZE] = {
  // Must be in descending order
  {30.0, 5.0},
  {20.0, 3.0},
  {12.0, 2.0},
  {10.0, 1.0}, // Roughly full quieting
  {8.0, 0.5},
  {0.0, 0.0},  // Squelch probably won't be open anyway
  {-10.0, 0.0}
};

// Names of config file sections
char const *Radio = "radio";
char const *Audio = "audio";
char const *Repeater = "repeater";
char const *Display = "display";

// Command line/config file/interactive command parameters
int DAC_samprate = 48000;   // Actual hardware output rate
char const *App_path;
int Verbose = 0;                    // Verbosity flag
char const *Config_file;
bool Quiet = false;                 // Disable curses
bool Quiet_mode = false;            // Toggle screen activity after starting
double Playout = 0.1; // default 100 ms
bool Constant_delay = false;
bool Start_muted = false;
bool Auto_position = true;  // first will be in the center
double Gain = 0; // unity gain by default
bool Notch = false;
char *Mcast_address_text[MAX_MCAST]; // Multicast address(es) we're listening to
char const *Audiodev = "";    // Name of audio device; empty means portaudio's default
bool Voting = false;
int Channels = 2;
char const *Init;
//double GoodEnoughSNR = 20.0; // FM SNR considered "good enough to not be worth changing
char const *Pipe;
char const *Source; // Source specific multicast, if used

// Global variables that regularly change
int Output_fd = -1; // Output network socket, if any
struct sockaddr_in Dest_socket;
int64_t Last_xmit_time;
_Atomic uint64_t Output_time;  // Relative output time in frames
_Atomic uint64_t Callbacks;
_Atomic uint64_t Audio_frames;
_Atomic int64_t LastAudioTime;
_Atomic unsigned Callback_quantum;
_Atomic uint64_t Output_total;
_Atomic double Output_level; // Output level, mean square
double Portaudio_delay;
pthread_t Repeater_thread;
int Nfds;                     // Number of streams
pthread_mutex_t Sess_mutex = PTHREAD_MUTEX_INITIALIZER;
PaStream *Pa_Stream;          // Portaudio stream handle
int inDevNum;                 // Portaudio's audio output device index
int64_t Start_time;
PaTime Start_pa_time;
_Atomic PaTime Last_callback_time;
int64_t Last_error_time;
int Nsessions;
struct session Sessions[NSESSIONS];
_Atomic bool Terminate;
struct session const * _Atomic Best_session; // Session with highest SNR
int Mcast_ttl;
void *output_thread(void *p);
struct sockaddr_in *Source_socket;
int Callback_blocksize = 960; // 960 samples = 20 ms @ 48k
uint64_t Wait_timeout;
uint64_t Wait_successful;
uint64_t Waits;

static char Optstring[] = "CI:P:LR:Sb:c:f:g:o:p:qr:su:vnV";
static struct  option Options[] = {
   {"center", no_argument, NULL, 'C'},
   {"input", required_argument, NULL, 'I'},
   {"list-audio", no_argument, NULL, 'L'},
   {"device", required_argument, NULL, 'R'},
   {"autosort", no_argument, NULL, 'S'},
   {"blocksize", required_argument, NULL, 'b'},
   {"channels", required_argument, NULL, 'c'},
   {"config", required_argument, NULL, 'f'},
   {"gain", required_argument, NULL, 'g'},
   {"notch", no_argument, NULL, 'n'},
   {"source", required_argument, NULL, 'o'},
   {"pipe", required_argument, NULL, 'P'},
   {"playout", required_argument, NULL, 'p'},
   {"quiet", no_argument, NULL, 'q'},
   {"samprate",required_argument,NULL,'r'},
   {"voting", no_argument, NULL, 's'},
   {"update", required_argument, NULL, 'u'},
   {"verbose", no_argument, NULL, 'v'},
   {"version", no_argument, NULL, 'V'},
   {NULL, 0, NULL, 0},
};


#ifdef __linux__
// Get rid of those fucking ALSA error messages that clutter the screen
static void alsa_error_handler(const char *file, int line, const char *function, int err, const char *fmt, ...){
  (void)file; // no args used
  (void)line;
  (void)function;
  (void)err;
  (void)fmt;

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
    char const *audiodev = config_getstring(Configtable,Audio,"device",NULL);
    if(audiodev)
      Audiodev = strdup(audiodev);
    // Add validity checking

#if __linux__
    Pipe = config_getstring(Configtable,Audio,"pipe",NULL);
#endif

    Gain = config_getdouble(Configtable,Audio,"gain",Gain);
    Cwid = strdup(config_getstring(Configtable,Repeater,"id","NOCALL"));
    // 600 sec is 10 minutes, max ID interval per FCC 97.119(a)
    int const period = config_getint(Configtable,Repeater,"period",600);
    int pperiod = config_getint(Configtable,Repeater,"pperiod",period/2);
    if(pperiod > period)
      pperiod = period;
    Mandatory_ID_interval = period * BILLION;
    Quiet_ID_interval = pperiod * BILLION;
    ID_pitch = config_getdouble(Configtable,Repeater,"pitch",ID_pitch);
    ID_level = config_getdouble(Configtable,Repeater,"level",ID_level);
    Notch = config_getboolean(Configtable,Audio,"notch",Notch);
    Quiet = config_getboolean(Configtable,Display,"quiet",Quiet);
    if(config_getboolean(Configtable,Audio,"center",false))
      Auto_position = false;

    Callback_blocksize = config_getint(Configtable,Audio,"blocksize",Callback_blocksize);
    Auto_sort = config_getboolean(Configtable,Display,"autosort",Auto_sort);
    Update_interval = config_getint(Configtable,Display,"update",Update_interval);
    Playout = config_getdouble(Configtable,Audio,"playout",Playout) / 1000.; // convert ms to sec
    Repeater_tail = config_getdouble(Configtable,Repeater,"tail",Repeater_tail);
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
    Configtable = NULL;
  }
  // Rescan args to override config file
  bool list_audio = false;
  optind = 0; // reset getopt()
  while((c = getopt_long(argc,argv,Optstring,Options,NULL)) != -1){
    switch(c){
    case 'b':
      Callback_blocksize = atoi(optarg);
      break;
    case 'c':
      Channels = atoi(optarg);
      break;
    case 'f':
      break; // Ignore this time
    case 'g':
      Gain = strtod(optarg,NULL);
      break;
    case 'n':
      Notch = true;
      break;
    case 'o':
      Source = optarg; // source specific multicast; only take packets from this source
      break;
    case 'p':
      Playout = strtod(optarg,NULL) / 1000.;
      break;
    case 'q': // No ncurses
      Quiet = true;
      break;
    case 'r':
      DAC_samprate = atoi(optarg);
      break;
    case 'u':
      Update_interval = atoi(optarg);
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
#if __linux__
    case 'P':
      Pipe = optarg;
      break;
#endif
    case 'S':
      Auto_sort = true;
      break;
    case 's':
      Voting = true;
      Auto_position = false; // Disables
      break;
    default:
      fprintf(stderr,"Usage: %s -L\n",App_path);
      fprintf(stderr,"       %s [-c channels] [-f config_file] [-g gain] [-p playout] [-q] [-r samprate] [-u update] [-v] \
[-I mcast_address] [-R audiodev|-P pipename] [-S] [-o|--source <source-name-or-address>] [mcast_address ...]\n",App_path);
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
  atomic_init(&Terminate,false);

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
  load_id();
  // Callback continuously scans sessions, merging their audio
  // if active into the output stream
  // This allows multiple streams to be played on hosts that only support one

  if(Pipe != NULL){
#if __linux__
    pthread_t pipethread;
    pthread_create(&pipethread,NULL,output_thread,NULL);
#endif
  } else {

    // Use portaudio
    PaError r = Pa_Initialize();
    if(r != paNoError){
      fprintf(stderr,"Portaudio error: %s\n",Pa_GetErrorText(r));
      return r;
    }
    atexit(cleanup); // Make sure Pa_Terminate() gets called

    char *nextp = NULL;
    long d;
    int numDevices = Pa_GetDeviceCount();
    if(Audiodev == NULL || strlen(Audiodev) == 0){
      // not specified; use default
      inDevNum = Pa_GetDefaultOutputDevice();
    } else if(d = strtol(Audiodev,&nextp,0),nextp != Audiodev && *nextp == '\0'){
      if(d >= numDevices){
	fprintf(stderr,"%ld is out of range, use %s -L for a list\n",d,App_path);
	exit(EX_USAGE);
      }
      inDevNum = (int)d;
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
    PaStreamParameters outputParameters = {
      .channelCount = Channels,
      .device = inDevNum,
      .sampleFormat = paFloat32,
      .suggestedLatency = Latency // 0 doesn't seem to be a good value on OSX, lots of underruns and stutters
    };

    r = Pa_OpenStream(&Pa_Stream,
		      NULL,
		      &outputParameters,
		      DAC_samprate,
		      Callback_blocksize,
		      0,
		      pa_callback,
		      NULL);

    if(r != paNoError){
      fprintf(stderr,"Portaudio error: %s, exiting\n",Pa_GetErrorText(r));
      exit(EX_IOERR);
    }
  }  // !Network (use Portaudio)



  if(Repeater_tail != 0)
    pthread_create(&Repeater_thread,NULL,repeater_ctl,NULL); // Repeater mode active

  if(Source != NULL){
    Source_socket = calloc(1,sizeof(struct sockaddr_storage));
    resolve_mcast(Source,Source_socket,0,NULL,0,0);
  }

  // Spawn one thread per address
  // All have to succeed in resolving their targets or we'll exit
  // This allows a restart when started automatically from systemd before avahi is fully running
  pthread_t datathreads[Nfds];
  pthread_t statthreads[Nfds];
  for(int i=0; i<Nfds; i++){
    pthread_create(&datathreads[i],NULL,dataproc,Mcast_address_text[i]);
    pthread_create(&statthreads[i],NULL,statproc,Mcast_address_text[i]);
  }
  Last_error_time = gps_time_ns();

  // Spawn the display thread so it isn't charged with everybody's CPU time
  // This seems to be a change in recent Linux kernels and/or the 'top' command
  // Drove me up the wall trying to find out why this thread was spending so much CPU doing nothing!
  if(!Quiet){
    pthread_t display_thread;
    pthread_create(&display_thread,NULL,display,NULL);
  }
  while(!atomic_load_explicit(&Terminate,memory_order_acquire))
    sleep(1);

  exit(EX_OK); // calls cleanup() to clean up Portaudio and ncurses. Can't happen...
}

// Sets global Best_session if we have the highest SNR
void vote(struct session const *sp){
  assert(sp != NULL);
  if(!inuse(sp) || muted(sp))
    return;

  pthread_mutex_lock(&Sess_mutex);
  struct session const *best = atomic_load_explicit(&Best_session,memory_order_acquire);
  if(best == NULL || !inuse(best) || muted(best)){
    atomic_store_explicit(&Best_session,sp,memory_order_release); // they abdicated; grab the throne
    pthread_mutex_unlock(&Sess_mutex);
    return;
  }
  // Don't take it from a running session unless we're sufficiently better
  for(int i=0; i < HSIZE;i++){
    if(sp->snr <= Hysteresis_table[i].snr)
      continue;
    if(sp->snr > best->snr + Hysteresis_table[i].hysteresis){
      atomic_store_explicit(&Best_session,sp,memory_order_release); // we won
      break;
    }
  }
  pthread_mutex_unlock(&Sess_mutex);
}

// Receive status multicasts on output multicast groups, update local states
void *statproc(void *arg){
  char const *mcast_address_text = (char *)arg;
  {
    char name[100];
    snprintf(name,sizeof(name),"stat %s",mcast_address_text);
    pthread_setname(name);
  }

  int status_fd;
  {
    char iface[1024];
    struct sockaddr sock;
    resolve_mcast(mcast_address_text,&sock,DEFAULT_STAT_PORT,iface,sizeof(iface),0);
    status_fd = listen_mcast(Source_socket,&sock,iface);
  }
  if(status_fd == -1)
    pthread_exit(NULL);

  // Main loop begins here - does not need to be realtime?
  uint8_t *buffer = malloc(PKTSIZE);
  while(!atomic_load_explicit(&Terminate,memory_order_acquire)){
    struct sockaddr_storage sender;
    socklen_t socksize = sizeof(sender);
    ssize_t length = recvfrom(status_fd,buffer,PKTSIZE,0,(struct sockaddr *)&sender,&socksize);
    if(buffer[0] != STATUS) // not status, ignore
      continue;

    // Extract just the SSRC to see if the session exists
    // NB! Assumes same IP source address *and UDP source port* for status and data
    // This is only true for recent versions of radiod, after the switch to unconnected output sockets
    // But older versions don't send status on the output channel anyway, so no problem
    uint32_t ssrc = get_ssrc(buffer+1,length-1);
    struct session *sp = lookup_or_create_session(&sender,ssrc);
    if(!sp){
      fprintf(stderr,"No room!!\n");
      continue;
    }
    if(sp->last_active == 0)
      sp->last_active = gps_time_ns(); // Keep active time calc from blowing up before data packet arrives

    // Decode directly into local copy, as not every parameter is updated in every status message
    // Decoding into a temp copy and then memcpy would write zeroes into unsent parameters
    // Lock and signal the queue so the data handler can atomically wait for the squelch to open
    decode_radio_status(&sp->frontend,&sp->chan,buffer+1,length-1);
    // chan.output.power is in dBFS so no signal is -Infinity
    if(!isfinite(sp->chan.output.power))
      sp->squelch_open = false; // only turned off here; turned on in the data path

    // Cache payload-type/channel count/sample rate/encoding association for use by data thread
    sp->type = sp->chan.output.rtp.type & 0x7f;
    sp->pt_table[sp->type].encoding = sp->chan.output.encoding;
    sp->pt_table[sp->type].samprate = sp->chan.output.samprate;
    sp->pt_table[sp->type].channels = sp->chan.output.channels;
    // Lookup channel ID if its not already set
    // The data decode thread will change it if there's a tone and an entry for it
    char const *id = lookupid(sp->chan.tune.freq,sp->notch_tone); // Any or no tone
    if(id){
      strlcpy(sp->id,id,sizeof(sp->id));
    } else if((id = lookupid(sp->chan.tune.freq,0.0)) != NULL){
      // entry with no tone?
      strlcpy(sp->id,id,sizeof(sp->id));
    } else
      sp->id[0] = '\0';

    // Update SNR calculation (not sent explicitly)
    double const noise_bandwidth = fabs(sp->chan.filter.max_IF - sp->chan.filter.min_IF);
    double sig_power = sp->chan.sig.bb_power - noise_bandwidth * sp->chan.sig.n0;
    if(sig_power < 0)
      sig_power = 0; // Avoid log(-x) = nan
    double const sn0 = sig_power/sp->chan.sig.n0;
    sp->snr = power2dB(sn0/noise_bandwidth);
    vote(sp);
  }
  FREE(buffer);
  return NULL;
}

// Look up session, or if it doesn't exist, create it.
// Executes atomically
int Session_creates = 0;

struct session *lookup_or_create_session(struct sockaddr_storage const *sender,const uint32_t ssrc){
  int first_idle = -1;
  pthread_mutex_lock(&Sess_mutex);
  for(int i = 0; i < NSESSIONS; i++){
    struct session * const sp = Sessions + i;
    if(!inuse(sp)){
      if(first_idle == -1)
	first_idle = i; // in case we need to create it
      continue;
    }
    if(sp->ssrc == ssrc
       && address_match(sender,&sp->sender)
       //       && getportnumber(&sp->sender) == getportnumber(&sender)
       ){
      pthread_mutex_unlock(&Sess_mutex);
      return sp;
    }
  }
  // Assign an empty spot
  if(first_idle == -1)
    return NULL;

  struct session * const sp = Sessions + first_idle;
  memset(sp,0,sizeof(struct session));

  Session_creates++;
  atomic_init(&sp->terminate,false);
  sp->ssrc = ssrc;
  memcpy(&sp->sender,sender,sizeof(sp->sender));
  pthread_cond_init(&sp->qcond,NULL);
  pthread_mutex_init(&sp->qmutex,NULL);
  atomic_store_explicit(&sp->inuse,true,memory_order_release);
  pthread_mutex_unlock(&Sess_mutex);
  return sp; // caller will set sp->inuse
}

int close_session(struct session *sp){
  assert(sp != NULL);
  if(sp == NULL)
    return -1;

  assert(sp >= Sessions && sp < Sessions + NSESSIONS);
  if(sp == Best_session)
    Best_session = NULL;

  // Tell it to commit hari-kari
  atomic_store_explicit(&sp->terminate,true,memory_order_release);
  pthread_mutex_lock(&sp->qmutex);
  pthread_cond_broadcast(&sp->qcond);   // Try to get its attention
  pthread_mutex_unlock(&sp->qmutex); // Done modifying session table

  // Thread now cleans itself up
  return 0;
}

// passed to atexit, invoked at exit
// must not call exit() to avoid looping
void cleanup(void){
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

static float Softclip_mem[2];

// Portaudio callback - transfer data (if any) to provided buffer
// This has to be lock-free; no mutexes allowed, so _Atomic variables are used to communicate with the producer threads
// The session blocks are static, not malloced, in case the callback accesses them for one call before it sees the sp->inuse flag drop.
// Even if the callback reads a dead buffer, it won't generate any output for it because its write pointer will have stopped advancing
int pa_callback(void const *inputBuffer, void *outputBuffer,
		       unsigned long const framesPerBuffer,
		       PaStreamCallbackTimeInfo const * timeInfo,
		       PaStreamCallbackFlags statusFlags,
		       void *userData){
  (void)inputBuffer; // Unused
  (void)statusFlags;
  (void)userData;


  if(!outputBuffer)
    return paAbort; // can this happen??

  assert(framesPerBuffer != 0);
  // Informational stuff
  atomic_store_explicit(&Last_callback_time,timeInfo->currentTime,memory_order_release);
  atomic_store_explicit(&Callback_quantum,framesPerBuffer,memory_order_release);
  Portaudio_delay = timeInfo->outputBufferDacTime - timeInfo->currentTime;

  // Output frame clock at beginning of our buffer
  uint64_t const rptr = atomic_load_explicit(&Output_time,memory_order_relaxed); // we control it
  // base sample index into all session output buffers
  int const base = (Channels * rptr) & (BUFFERSIZE-1);
  // We'll be summing into it, so clear it first
  float * const buffer = (float *)outputBuffer;
  memset(buffer, 0, framesPerBuffer * Channels * sizeof (float));

  int64_t total = 0;
#if TEST_TONE
  double freq = 2. / 64.;
  double phase = freq * ((rptr) % 64); // 64 frames/cycle = 750 Hz
  double complex os = cispi(phase);

  double complex step = cispi(freq);

  for(unsigned int i=0; i < framesPerBuffer; i++){
    buffer[2*i] += 0.1 * creal(os); // -20dBFS
    buffer[2*i+1] += 0.1 * cimag(os);
    os *= step;
  }
#endif

  // If voting, look only at the leader.
  // Otherwise scan the whole list, summing all active sessions
  // finally a real use for do {} while();
  struct session const *sp = Voting ?
    atomic_load_explicit(&Best_session,memory_order_acquire) : Sessions;

  do {
    if(sp == NULL)
      break; // Voting, and no one has claimed the prize yet

    if(!inuse(sp) || muted(sp) || sp->buffer == NULL)
      continue; // Don't consider

    unsigned long frames = framesPerBuffer;
    uint64_t const wptr = atomic_load_explicit(&sp->wptr,memory_order_acquire);
    // careful with unsigned arithmetic
    int start = 0;
    if(wptr <= rptr){
      int late = rptr - wptr; // guaranteed zero or positive
      if(late * Channels >= BUFFERSIZE)
	continue;      // all of it is late
      else
	start = late * Channels; // trim the front to keep it from backward wrapping and being played 1 buffer later
    } else if(Channels * (wptr - rptr + frames) > BUFFERSIZE){
      if(Channels * (wptr - rptr) > BUFFERSIZE)
	continue; // all is too early
      frames = BUFFERSIZE / Channels - (wptr - rptr); // Trim the end from forward wrapping, prevent early play
    }
    for(unsigned int j = start; j < Channels*frames; j++)
      buffer[j] += sp->buffer[BINDEX(base,j)];

    total += frames;
  } while(!Voting && ++sp < Sessions + NSESSIONS);

  // Sum up all the energy we've written in this callback
  double energy = 0;
  for(unsigned int j=0; j < Channels * framesPerBuffer;j++)
    energy += buffer[j] * buffer[j];

  energy /= Channels * framesPerBuffer;
  atomic_store_explicit(&Output_level,energy,memory_order_relaxed);
  opus_pcm_soft_clip(buffer,(int)framesPerBuffer,Channels,Softclip_mem);
  atomic_fetch_add_explicit(&Audio_frames,framesPerBuffer,memory_order_release);
  atomic_fetch_add_explicit(&Output_time,framesPerBuffer,memory_order_release);
  atomic_fetch_add_explicit(&Output_total,total,memory_order_release);
  atomic_fetch_add_explicit(&Callbacks,1,memory_order_release);
  return paContinue;
}

#if __linux__
// Macos doesn't support clock_nanosleep(); find a substitute
// Thread used instead of Portaudio callback when sending to network
// Sends raw 16-bit PCM stereo at 48kHz; send to named pipe and opusenc, etc
// Rewritten for new output architecture 23 Dec 2025, not yet tested
void *output_thread(void *p){
  (void)p;
  Output_fd = open(Pipe,O_WRONLY,0666);

  struct timespec next;
  clock_gettime(CLOCK_MONOTONIC,&next);

  // Grab 20 milliseconds stereo @ 48 kHz
  int const frames = .02 * DAC_samprate;
  int const samples = frames * Channels;

  int16_t *pcm_buffer = malloc(samples * sizeof *pcm_buffer);
  float *out_buffer = malloc(samples * sizeof *out_buffer);
  assert(out_buffer != NULL);
  while(1){

    memset(out_buffer, 0, samples * sizeof *out_buffer);
    int rptr = atomic_load_explicit(&Output_time,memory_order_relaxed);
    for(int i=0; i < NSESSIONS; i++){
      struct session *sp = Sessions + i;
      if(!inuse(sp))
	continue;

      int64_t wptr = atomic_load_explicit(&sp->wptr,memory_order_acquire);
      int64_t count = samples;
      if(wptr <= rptr)
	count = 0; // he's empty
      if(count > wptr - rptr)
	count = wptr - rptr; // limit to what he's got

      int const base = (Channels * rptr) & (BUFFERSIZE-1);

      for(int j = 0; j < count; j++){
	out_buffer[j] += sp->buffer[BINDEX(base,j)];
      }
    }
    atomic_store_explicit(&Output_time,rptr + frames,memory_order_release);
    double energy = 0;
    for(int j=0; j < samples; j++)
      energy += out_buffer[j] * out_buffer[j];

    energy /= samples;
    atomic_store_explicit(&Output_level,energy,memory_order_relaxed);

    opus_pcm_soft_clip(out_buffer,frames,Channels,Softclip_mem);
    for(int j = 0; j < samples; j++){
      double s = 32768 * out_buffer[j];
      pcm_buffer[j] = s > 32767 ? 32767 : s < -32767 ? -32767 : s; // clip
    }
    int r = write(Output_fd,pcm_buffer,samples * sizeof *pcm_buffer);
    if(r <= 0){
      if(Output_fd != -1)
	close(Output_fd);
      Output_fd = open(Pipe,O_WRONLY,0666); // could retry, but don't bother
    }
    // Schedule next transmission in 20 ms
    next.tv_nsec += 20000000;
    while(next.tv_nsec >= BILLION){
      next.tv_nsec -= BILLION;
      next.tv_sec++;
    }
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
  }
}
#endif
