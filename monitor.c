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
float const Latency = 0.02; // chunk size for audio output callback
float const Tone_period = 0.24; // PL tone integration period

// Voting hysteresis table. Small at low SNR, larger at large SNR to minimize pointless switching
// When the current SNR is 'snr', don't switch to another channel unless it's at least 'hysteresis' dB stronger
#define HSIZE (7)
struct {
  float snr;
  float hysteresis;
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
unsigned int DAC_samprate = 48000;   // Actual hardware output rate
char const *App_path;
int Verbose = 0;                    // Verbosity flag
char const *Config_file;
bool Quiet = false;                 // Disable curses
bool Quiet_mode = false;            // Toggle screen activity after starting
float Playout = 100;
bool Constant_delay = false;
bool Start_muted = false;
bool Auto_position = true;  // first will be in the center
float Gain = 0; // unity gain by default
bool Notch = false;
char *Mcast_address_text[MAX_MCAST]; // Multicast address(es) we're listening to
char const *Audiodev = "";    // Name of audio device; empty means portaudio's default
bool Voting = false;
int Channels = 2;
char const *Init;
//float GoodEnoughSNR = 20.0; // FM SNR considered "good enough to not be worth changing
char const *Pipe;
char const *Source; // Source specific multicast, if used

// Global variables that regularly change
int Output_fd = -1; // Output network socket, if any
struct sockaddr_in Dest_socket;
int64_t Last_xmit_time;
float *Output_buffer;
int Buffer_length; // Bytes left to play out, max BUFFERSIZE
volatile unsigned int Rptr;   // callback thread read pointer, *frames*
volatile unsigned int Wptr;   // For monitoring length of output queue
uint64_t Audio_callbacks;
unsigned long Audio_frames;
volatile int64_t LastAudioTime;
int32_t Portaudio_delay;
pthread_t Repeater_thread;
int Nfds;                     // Number of streams
pthread_mutex_t Sess_mutex = PTHREAD_MUTEX_INITIALIZER;
PaStream *Pa_Stream;          // Portaudio stream handle
int inDevNum;                 // Portaudio's audio output device index
int64_t Start_time;
PaTime Start_pa_time;
PaTime Last_callback_time;
int64_t Last_error_time;
int Nsessions;
struct session *Sessions[NSESSIONS];
bool Terminate;
struct session *Best_session; // Session with highest SNR
struct sockaddr Metadata_dest_socket;
int Mcast_ttl;
pthread_mutex_t Rptr_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t Rptr_cond = PTHREAD_COND_INITIALIZER;
void *output_thread(void *p);
struct sockaddr_in *Source_socket;

static char Optstring[] = "CI:P:LR:Sc:f:g:o:p:qr:su:vnV";
static struct  option Options[] = {
   {"center", no_argument, NULL, 'C'},
   {"input", required_argument, NULL, 'I'},
   {"list-audio", no_argument, NULL, 'L'},
   {"device", required_argument, NULL, 'R'},
   {"autosort", no_argument, NULL, 'S'},
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
    char const *audiodev = config_getstring(Configtable,"audio","device",NULL);
    if(audiodev)
      Audiodev = strdup(audiodev);
    // Add validity checking

#if __linux__
    Pipe = config_getstring(Configtable,"audio","pipe",NULL);
#endif

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
    Configtable = NULL;
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
    case 'o':
      Source = optarg; // source specific multicast; only take packets from this source
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
  // Create output circular buffer
  // Runs continuously, playing silence until audio arrives.
  // This allows multiple streams to be played on hosts that only support one
  Output_buffer = mirror_alloc(BUFFERSIZE * Channels * sizeof(*Output_buffer)); // Must be power of 2 times page size
  memset(Output_buffer,0,BUFFERSIZE * Channels * sizeof(*Output_buffer)); // Does mmap clear its initial memory? Not sure

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
  while(!Terminate)
    sleep(1);

  exit(EX_OK); // calls cleanup() to clean up Portaudio and ncurses. Can't happen...
}

// Update session now-active flags, pick session with highest SNR for voting
void vote(void){
  struct session *best = NULL;
  long long const time = gps_time_ns();

  pthread_mutex_lock(&Sess_mutex);
  for(int i = 0; i < Nsessions; i++){
    struct session * const sp = sptr(i);
    if(sp == NULL)
      continue;

    // Have we gotten anything in the last 500 ms?
    sp->now_active = (time - sp->last_active) < BILLION/2; // note: boolean expression
    if(!sp->now_active)
      sp->active = 0;

    if(sp->muted || !sp->now_active) // No recent audio, skip
      continue;

    if(best == NULL || sp->snr > best->snr)
      best = sp;
  }
  // Don't claim it unless we're sufficiently better (or there's nobody)
  if(Best_session == NULL || Best_session->muted || !Best_session->now_active)
    Best_session = best;
  else if(best != NULL){
    for(int i=0; i < HSIZE;i++){
      if(Best_session->snr > Hysteresis_table[i].snr){
	if(best->snr > Best_session->snr + Hysteresis_table[i].hysteresis)
	  Best_session = best;
	break;
      }
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
  while(!Terminate){
    struct sockaddr_storage sender;
    socklen_t socksize = sizeof(sender);
    int length = recvfrom(status_fd,buffer,PKTSIZE,0,(struct sockaddr *)&sender,&socksize);
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
    decode_radio_status(&sp->frontend,&sp->chan,buffer+1,length-1);
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
    float const noise_bandwidth = fabsf(sp->chan.filter.max_IF - sp->chan.filter.min_IF);
    float sig_power = sp->chan.sig.bb_power - noise_bandwidth * sp->chan.sig.n0;
    if(sig_power < 0)
      sig_power = 0; // Avoid log(-x) = nan
    float const sn0 = sig_power/sp->chan.sig.n0;
    sp->snr = power2dB(sn0/noise_bandwidth);
    vote();
  }
  FREE(buffer);
  return NULL;
}

// Look up session, or if it doesn't exist, create it.
// Executes atomically
struct session *lookup_or_create_session(const struct sockaddr_storage *sender,const uint32_t ssrc){
  pthread_mutex_lock(&Sess_mutex);
  for(int i = 0; i < Nsessions; i++){
    struct session * const sp = sptr(i);
    if(sp && sp->ssrc == ssrc && address_match(sender,&sp->sender)){
      pthread_mutex_unlock(&Sess_mutex);
      return sp;
    }
  }
  struct session * const sp = calloc(1,sizeof(*sp));
  if(sp == NULL){ // Shouldn't happen on modern machines!
    pthread_mutex_unlock(&Sess_mutex);
    return NULL;
  }

  // Put at end of list
  Sessions[Nsessions++] = sp;
  sp->init = false; // Wait for first RTP packet to set the rest up
  sp->ssrc = ssrc;
  memcpy(&sp->sender,sender,sizeof(sp->sender));

  pthread_cond_init(&sp->qcond,NULL);
  pthread_mutex_init(&sp->qmutex,NULL);
  pthread_mutex_unlock(&Sess_mutex);

  return sp;
}
int close_session(struct session **p){
  assert(p != NULL);
  if(p == NULL)
    return -1;
  struct session * sp = *p;
  assert(sp != NULL);
  if(sp == NULL)
    return -1;
  assert(Nsessions > 0);

  pthread_mutex_lock(&Sess_mutex);
  if(sp == Best_session)
    Best_session = NULL;

  // Remove from table
  int i = 0;
  for(i = 0; i < Nsessions; i++){
    if(Sessions[i] == sp)
      break;
  }
  if(i == Nsessions){
    // Not found
    assert(false);
    pthread_mutex_unlock(&Sess_mutex);
    return -1;
  }

  // Copy remaining session pointers down
  Nsessions--;
  assert(Nsessions >= i);
  memmove(&Sessions[i],&Sessions[i+1],(Nsessions-i) * sizeof(Sessions[0]));
  Sessions[Nsessions] = NULL; // Last entry no longer valid
  pthread_mutex_unlock(&Sess_mutex); // Done modifying session table
  // Thread now cleans itself up
  FREE(sp);
  *p = NULL;
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
int pa_callback(void const *inputBuffer, void *outputBuffer,
		       unsigned long framesPerBuffer,
		       PaStreamCallbackTimeInfo const * timeInfo,
		       PaStreamCallbackFlags statusFlags,
		       void *userData){
  (void)inputBuffer; // Unused
  (void)statusFlags;
  (void)userData;
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
  opus_pcm_soft_clip((float *)outputBuffer,framesPerBuffer,Channels,Softclip_mem);
  pthread_mutex_lock(&Rptr_mutex);
  Rptr += framesPerBuffer;
  Rptr &= (BUFFERSIZE-1);
  Buffer_length -= framesPerBuffer;
  pthread_cond_broadcast(&Rptr_cond);
  pthread_mutex_unlock(&Rptr_mutex);

#if 0
  int q = modsub(Wptr,Rptr,BUFFERSIZE);
  if(q < 0)
    return paComplete;
  return (Buffer_length <= 0) ?  paComplete : paContinue;
#endif
  return paContinue;
}

#if __linux__
// Macos doesn't support clock_nanosleep(); find a substitute
// Thread used instead of Portaudio callback when sending to network
// Sends raw 16-bit PCM stereo at 48kHz; send to named pipe and opusenc, etc
void *output_thread(void *p){
  (void)p;
  Output_fd = open(Pipe,O_WRONLY,0666);

  struct timespec next;
  clock_gettime(CLOCK_MONOTONIC,&next);

  while(1){
    // Grab 20 milliseconds stereo @ 48 kHz
    int frames = .02 * 48000;
    int samples = frames * Channels;
    int16_t out_buffer[samples];

    pthread_mutex_lock(&Rptr_mutex);
    float *buffer = &Output_buffer[Channels*Rptr];
    Rptr += frames;
    Rptr &= (BUFFERSIZE-1);
    Buffer_length -= frames;
    for(int i=0; i < samples; i++)
      out_buffer[i] = buffer[i] > 1 ? 32767 : buffer[i] < -1 ? -32767 : 32767 * buffer[i];
    memset(buffer,0,samples * sizeof *buffer);

    pthread_cond_broadcast(&Rptr_cond);
    pthread_mutex_unlock(&Rptr_mutex);

    int r = write(Output_fd,out_buffer,sizeof out_buffer);
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
