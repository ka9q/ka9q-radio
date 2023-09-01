// Multicast local audio source with PCM
// Copyright April 2018 Phil Karn, KA9Q
#define _GNU_SOURCE 1
#include <assert.h>
#include <errno.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <limits.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <portaudio.h>
#include <arpa/inet.h>
#include <signal.h>
#include <sysexits.h>

#include "misc.h"
#include "multicast.h"

// Global config constants
#define BUFFERSIZE (1<<18)    // Size of audio ring buffer in mono samples. 2^18 is 2.73 sec at 48 kHz stereo
                              // Defined as macro so the Audiodata[] declaration below won't bother some compilers
int const Samprate = 48000;   // Too hard to handle other sample rates right now
int const Channels = 2;
#define FRAMESIZE 240         // 5 ms @ 48 kHz makes 960 bytes/packet
// End of config stuff


// Command line params
char *Audiodev = "";
char *Mcast_output_address_text = "";     // Multicast address we're sending to
const char *App_path;
int Verbose;                  // Verbosity flag (currently unused)
int Mcast_ttl = 1;
int IP_tos = 48; // AF12 << 2

// Global vars
int Output_fd = -1;
float Audiodata[BUFFERSIZE];
int Samples_available;
int Wptr;   // Write pointer for callback

static int pa_callback(const void *inputBuffer, void *outputBuffer,
		       unsigned long framesPerBuffer,
		       const PaStreamCallbackTimeInfo* timeInfo,
		       PaStreamCallbackFlags statusFlags,
		       void *userData);

void cleanup(void);
void closedown(int);

// Convert unsigned number modulo buffersize to a signed 2's complement
static inline int signmod(unsigned int const a){
  int y = a & (BUFFERSIZE-1);
  
  if(y >= BUFFERSIZE/2)
    y -= BUFFERSIZE;
  assert(y >= -BUFFERSIZE/2 && y < BUFFERSIZE/2);
  return y;
}

int main(int argc,char * const argv[]){
  App_path = argv[0];
  setlocale(LC_ALL,getenv("LANG"));

  int c;
  int List_audio = 0;
  while((c = getopt(argc,argv,"LT:vI:R:V")) != EOF){
    switch(c){
    case 'L':
      List_audio++;
      break;
    case 'p':
      IP_tos = strtol(optarg,NULL,0);
      break;
    case 'T':
      Mcast_ttl = strtol(optarg,NULL,0);
      break;
    case 'v':
      Verbose++;
      break;
    case 'I':
      Audiodev = optarg;
      break;
    case 'R':
      Mcast_output_address_text = optarg;
      break;
    case 'V':
      VERSION();
      exit(EX_OK);
    default:
      fprintf(stderr,"Usage: %s [-v] -I device [-R output_mcast_address][-T mcast_ttl]\n",argv[0]);
      exit(EX_USAGE);
    }
  }
  // Set up audio input
  PaError r = Pa_Initialize();
  if(r != paNoError){
    fprintf(stderr,"Portaudio error: %s\n",Pa_GetErrorText(r));
    close(Output_fd);
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
    exit(EX_OK);
  }

  int inDevNum,d;
  char *nextp;
  int numDevices = Pa_GetDeviceCount();
  if(strlen(Audiodev) == 0){
    // not specified; use default
    inDevNum = Pa_GetDefaultOutputDevice();
  } else if(d = strtol(Audiodev,&nextp,0),nextp != Audiodev && *nextp == '\0'){
    if(d >= numDevices){
      fprintf(stderr,"%d is out of range, use %s -L for a list\n",d,argv[0]);
      exit(EX_IOERR);
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
    fprintf(stderr,"Portaudio: no available devices\n");
    return -1;
  }


  PaStreamParameters inputParameters;
  memset(&inputParameters,0,sizeof(inputParameters));
  inputParameters.channelCount = Channels;
  inputParameters.device = inDevNum;
  inputParameters.sampleFormat = paFloat32;
  inputParameters.suggestedLatency = (double)FRAMESIZE / Samprate;
  
  PaStream *Pa_Stream;          // Portaudio stream handle
  r = Pa_OpenStream(&Pa_Stream,
		    &inputParameters,
		    NULL,       // No output stream
		    Samprate,
		    FRAMESIZE,        // 5 ms @ 48 kHz
		    0,
		    pa_callback,
		    NULL);

  if(r != paNoError){
    fprintf(stderr,"Portaudio error: %s\n",Pa_GetErrorText(r));      
    close(Output_fd);
    exit(EX_IOERR);
  }
  r = Pa_StartStream(Pa_Stream);
  if(r != paNoError){
    fprintf(stderr,"Portaudio error: %s\n",Pa_GetErrorText(r));
    close(Output_fd);
    exit(EX_IOERR);
  }


  // Set up multicast transmit socket
  Output_fd = setup_mcast(Mcast_output_address_text,NULL,1,Mcast_ttl,IP_tos,0);
  if(Output_fd == -1){
    fprintf(stderr,"Can't set up output on %s: %s\n",Mcast_output_address_text,strerror(errno));
    exit(EX_IOERR);
  }
  // Set up to transmit RTP/UDP/IP

  struct rtp_state rtp_state_out;
  memset(&rtp_state_out,0,sizeof(rtp_state_out));

  rtp_state_out.ssrc = utc_time_sec();
  // Graceful signal catch
  signal(SIGPIPE,closedown);
  signal(SIGINT,closedown);
  signal(SIGKILL,closedown);
  signal(SIGQUIT,closedown);
  signal(SIGTERM,closedown);
  signal(SIGPIPE,SIG_IGN);

  int rptr = 0;

  realtime();

  while(1){
    // Wait for audio input
    // I'd rather use pthread condition variables and signaling, but the portaudio people
    // say you shouldn't do that in a callback. So we poll.
    // Experimental "Zeno's paradox" delays to minimize number of loops without being too late
    // we first sleep for half the frame time, then a quarter, and so forth until we approach
    // the expected time of a new frame

    int delay = 1000; // 1 ms
    while(signmod(Wptr - rptr) < Channels * FRAMESIZE){
      if(delay >= 200)
	delay /= 2; // Minimum sleep time 0.2 ms
      usleep(delay);
    }
    struct rtp_header rtp_hdr;
    memset(&rtp_hdr,0,sizeof(rtp_hdr));
    rtp_hdr.version = RTP_VERS;
    rtp_hdr.type = PCM_STEREO_PT;
    rtp_hdr.seq = rtp_state_out.seq;
    rtp_hdr.ssrc = rtp_state_out.ssrc;
    rtp_hdr.timestamp = rtp_state_out.timestamp;

    uint8_t buffer[16384]; // Pick better number
    uint8_t *dp = buffer;
    dp = hton_rtp(dp,&rtp_hdr);
    int16_t *samples = (int16_t *)dp;
    for(int i=0; i < Channels * FRAMESIZE; i++){
      *samples++ = htons(scaleclip(Audiodata[rptr++]));
      rptr &= (BUFFERSIZE-1);
    }
    dp += Channels * FRAMESIZE * sizeof(*samples);
    send(Output_fd,buffer,dp - buffer,0); // should probably check return code
    rtp_state_out.packets++;
    rtp_state_out.bytes += Channels * FRAMESIZE * sizeof(int16_t);
    rtp_state_out.seq++;
    rtp_state_out.timestamp += FRAMESIZE;
  }
  close(Output_fd);
  exit(EX_OK);
}

// Portaudio callback - encode and transmit audio
// You're supposed to avoid synchronization calls here, which is very hard
static int pa_callback(const void *inputBuffer, void *outputBuffer,
		       unsigned long framesPerBuffer,
		       const PaStreamCallbackTimeInfo* timeInfo,
		       PaStreamCallbackFlags statusFlags,
		       void *userData){

  float *in = (float *)inputBuffer;
  assert(in != NULL);
    
  int count = Channels*framesPerBuffer;

  while(count--){
    Audiodata[Wptr++] = *in++;
    if(Wptr == BUFFERSIZE)
      Wptr = 0;
  }
  return paContinue;
}
void cleanup(void){
  Pa_Terminate();
  
  if(Output_fd != -1)
    close(Output_fd);
  Output_fd = -1;
}


void closedown(int s){
  fprintf(stderr,"signal %d\n",s);
  exit(EX_OK);
}


