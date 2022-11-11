// $Id: opussend.c,v 1.24 2022/08/05 06:35:10 karn Exp $
// Multicast local audio with Opus
// Copyright Feb 2018 Phil Karn, KA9Q
#define _GNU_SOURCE 1
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <opus/opus.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <signal.h>
#include <portaudio.h>

#include "misc.h"
#include "multicast.h"

// Global config constants
#define BUFFERSIZE (1<<18)    // Size of audio ring buffer in mono samples. 2^18 is 2.73 sec at 48 kHz stereo
                              // Defined as macro so the Audiodata[] declaration below won't bother some compilers
int const Samprate = 48000;   // Too hard to handle other sample rates right now
                              // Opus will notice the actual audio bandwidth, so there's no real cost to this
int const Channels = 2;       // Stereo - no penalty if the audio is actually mono, Opus will figure it out


// Command line params
char *Audiodev = "";
char *Mcast_output_address_text;  // Multicast address we're sending to
const char *App_path;
int Verbose;                  // Verbosity flag (currently unused)
// Opus codec params (with defaults)
float Opus_blocktime = 20;    // 20 ms, a reasonable default
int Opus_bitrate = 32;        // Opus stream audio bandwidth; default 32 kb/s
int Discontinuous = 0;        // Off by default
int Fec = 0;
int Mcast_ttl = 10;           // We're often routed
int IP_tos = 48;              // AF12 << 2
// End of config stuff

OpusEncoder *Opus;
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
#if 0 // Better done manually or in systemd?
  // Try to improve our priority
  int prio = getpriority(PRIO_PROCESS,0);
  prio = setpriority(PRIO_PROCESS,0,prio - 15);

  // Drop root if we have it
  if(seteuid(getuid()) != 0)
    perror("seteuid");
#endif

  setlocale(LC_ALL,getenv("LANG"));

  int c;
  int List_audio = 0;
  Mcast_ttl = 10; // By default, let Opus be routed
  while((c = getopt(argc,argv,"I:vR:B:o:xT:Lf:p:")) != EOF){
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
    case 'B':
      Opus_blocktime = strtod(optarg,NULL);
      break;
    case 'o':
      Opus_bitrate = strtol(optarg,NULL,0);
      break;
    case 'x':
      Discontinuous = 1;
      break;
    case 'f':
      Fec = strtol(optarg,NULL,0);
      break;
    default:
      fprintf(stderr,"Usage: %s [-x] [-v] [-o bitrate] [-B blocktime] [-I input_mcast_address] [-R output_mcast_address][-T mcast_ttl]\n",argv[0]);
      fprintf(stderr,"Defaults: %s -o %d -B %.1f -I %s -R %s -T %d\n",argv[0],Opus_bitrate,Opus_blocktime,Audiodev,Mcast_output_address_text,Mcast_ttl);
      exit(1);
    }
  }
  // Compute opus parameters
  if(Opus_blocktime != 2.5 && Opus_blocktime != 5
     && Opus_blocktime != 10 && Opus_blocktime != 20
     && Opus_blocktime != 40 && Opus_blocktime != 60
     && Opus_blocktime != 80 && Opus_blocktime != 100
     && Opus_blocktime != 120){
    fprintf(stderr,"opus block time must be 2.5/5/10/20/40/60/80/100/120 ms\n");
    fprintf(stderr,"80/100/120 supported only on opus 1.2 and later\n");
    exit(1);
  }
  int Opus_frame_size = round(Opus_blocktime * Samprate / 1000.);


  atexit(cleanup);


  // Set up audio input
  PaError r = Pa_Initialize();
  if(r != paNoError){
    fprintf(stderr,"Portaudio error: %s\n",Pa_GetErrorText(r));
    close(Output_fd);
    return r;
  }
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

  int inDevNum,d;
  char *nextp;
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
    fprintf(stderr,"Portaudio: no available devices\n");
    return -1;
  }


  PaStreamParameters inputParameters;
  memset(&inputParameters,0,sizeof(inputParameters));
  inputParameters.channelCount = Channels;
  inputParameters.device = inDevNum;
  inputParameters.sampleFormat = paFloat32;
  inputParameters.suggestedLatency = .001 * Opus_blocktime;
  
  PaStream *Pa_Stream;          // Portaudio stream handle
  r = Pa_OpenStream(&Pa_Stream,
		    &inputParameters,
		    NULL,       // No output stream
		    Samprate,
		    Opus_frame_size, // Read one Opus frame at a time
		    0,
		    pa_callback,
		    NULL);

  if(r != paNoError){
    fprintf(stderr,"Portaudio error: %s\n",Pa_GetErrorText(r));      
    close(Output_fd);
    exit(1);
  }
  r = Pa_StartStream(Pa_Stream);
  if(r != paNoError){
    fprintf(stderr,"Portaudio error: %s\n",Pa_GetErrorText(r));
    close(Output_fd);
    exit(1);
  }


  // Opus is specified to operate between 6 kb/s and 510 kb/s
  if(Opus_bitrate < 6000)
    Opus_bitrate *= 1000; // Assume it was given in kb/s
  if(Opus_bitrate > 510000)
    Opus_bitrate =  510000;

  int est_packet_size = round(Opus_bitrate * Opus_blocktime * .001/8);
  if(est_packet_size > 1500){
    fprintf(stderr,"Warning: estimated packet size %d bytes; IP framgmentation is likely\n",est_packet_size);
  }

  int error = 0;
  Opus = opus_encoder_create(Samprate,Channels,OPUS_APPLICATION_AUDIO,&error);
  if(error != OPUS_OK){
    fprintf(stderr,"opus_encoder_create error %d\n",error);
    exit(1);
  }

  error = opus_encoder_ctl(Opus,OPUS_SET_DTX(Discontinuous));
  if(error != OPUS_OK){
    fprintf(stderr,"opus_encoder_ctl set discontinuous %d: error %d\n",Discontinuous,error);
  }

  error = opus_encoder_ctl(Opus,OPUS_SET_BITRATE(Opus_bitrate));
  if(error != OPUS_OK){
    fprintf(stderr,"opus_encoder_ctl set bitrate %d: error %d\n",Opus_bitrate,error);
  }

  if(Fec){
    error = opus_encoder_ctl(Opus,OPUS_SET_INBAND_FEC(1));
    if(error != OPUS_OK)
      fprintf(stderr,"opus_encoder_ctl set FEC on error %d\n",error);
    error = opus_encoder_ctl(Opus,OPUS_SET_PACKET_LOSS_PERC(Fec));
    if(error != OPUS_OK)
      fprintf(stderr,"opus_encoder_ctl set FEC loss rate %d%% error %d\n",Fec,error);
  }


  // Always seems to return error -5 even when OK??
  error = opus_encoder_ctl(Opus,OPUS_FRAMESIZE_ARG,(int)Opus_frame_size);
  if(0 && error != OPUS_OK)
    fprintf(stderr,"opus_encoder_ctl set framesize %d (%.1lf ms): error %d\n",Opus_frame_size,Opus_blocktime,error);


  // Set up multicast transmit socket
  if(!Mcast_output_address_text){
    fprintf(stderr,"Must specify -R mcast_output_address\n");
    exit(1);
  }
  Output_fd = setup_mcast(Mcast_output_address_text,NULL,1,Mcast_ttl,IP_tos,0);
  if(Output_fd == -1){
    fprintf(stderr,"Can't set up output on %s: %s\n",Mcast_output_address_text,strerror(errno));
    exit(1);
  }
  // Set up to transmit Opus RTP/UDP/IP
  struct rtp_state rtp_state_out;
  memset(&rtp_state_out,0,sizeof(rtp_state_out));
  rtp_state_out.ssrc = gps_time_sec();

  // Graceful signal catch
  signal(SIGPIPE,closedown);
  signal(SIGINT,closedown);
  signal(SIGKILL,closedown);
  signal(SIGQUIT,closedown);
  signal(SIGTERM,closedown);
  signal(SIGPIPE,SIG_IGN);

  int rptr = 0;

  while(1){
    // Wait for audio input
    // I'd rather use pthread condition variables and signaling, but the portaudio people
    // say you shouldn't do that in a callback. So we poll.
    // Experimental "Zeno's paradox" delays to minimize number of loops without being too late
    // we first sleep for half the frame time, then a quarter, and so forth until we approach
    // the expected time of a new frame

    int delay = Opus_blocktime * 1000;
    while(signmod(Wptr - rptr) < Channels * Opus_frame_size){
      if(delay >= 200)
	delay /= 2; // Minimum sleep time 0.2 ms
      usleep(delay);
    }
    float bouncebuffer[Channels * Opus_frame_size];
    float *opus_input;
    if(rptr + Channels * Opus_frame_size > BUFFERSIZE){
      // wraps around; use bounce buffer
      memcpy(bouncebuffer,Audiodata + rptr,sizeof(float)*(BUFFERSIZE-rptr));
      memcpy(bouncebuffer + (BUFFERSIZE-rptr), Audiodata, sizeof(float) * (Channels * Opus_frame_size - (BUFFERSIZE-rptr)));
      opus_input = bouncebuffer;
    } else
      opus_input = Audiodata + rptr;

    rptr += Channels * Opus_frame_size;
    if(rptr >= BUFFERSIZE)
      rptr -= BUFFERSIZE;

    struct rtp_header rtp_hdr;
    memset(&rtp_hdr,0,sizeof(rtp_hdr));
    rtp_hdr.version = RTP_VERS;
    rtp_hdr.type = OPUS_PT; // Opus (not standard)
    rtp_hdr.seq = rtp_state_out.seq;
    rtp_hdr.ssrc = rtp_state_out.ssrc;
    rtp_hdr.timestamp = rtp_state_out.timestamp;

    unsigned char buffer[16384]; // Pick better number
    unsigned char *dp = buffer;
    dp = hton_rtp(dp,&rtp_hdr);

    int size = opus_encode_float(Opus,opus_input,Opus_frame_size,dp,sizeof(buffer) - (dp - buffer));
    if(!Discontinuous || size > 2){
      dp += size;
      send(Output_fd,buffer,dp - buffer,0);
      rtp_state_out.seq++; // Increment RTP sequence number only if packet is sent
      rtp_state_out.packets++;
      rtp_state_out.bytes += size;
    }
    rtp_state_out.timestamp += Opus_frame_size; // Always increments, even if we suppress the frame
  }
  opus_encoder_destroy(Opus);
  close(Output_fd);
  exit(0);
}

// Portaudio callback - encode and transmit audio
// You're supposed to avoid synchronization calls here, but they seem to work
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
  if(Opus != NULL)
    opus_encoder_destroy(Opus);
  Opus = NULL;
  
  if(Output_fd != -1)
    close(Output_fd);
  Output_fd = -1;
}

void closedown(int s){
  fprintf(stderr,"Signal %d\n",s);

  exit(0);
}

