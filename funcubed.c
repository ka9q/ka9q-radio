// $Id: funcubed.c,v 1.8 2022/08/08 11:06:34 karn Exp $
// Read from AMSAT UK Funcube Pro and Pro+ dongles
// Multicast raw 16-bit I/Q samples
// Accept control commands from UDP socket
// rewritten to use portaudio July 2018
#define _GNU_SOURCE 1


#include <assert.h>
#include <limits.h>
#include <pthread.h>
#include <string.h>
#include <math.h>
#include <complex.h>
#include <stdio.h>
#include <stdarg.h>
#include <portaudio.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <signal.h>
#include <locale.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <syslog.h>
#include <errno.h>
#include <getopt.h>
#include <iniparser/iniparser.h>

#include "conf.h"
#include "fcd.h"
#include "misc.h"
#include "status.h"
#include "multicast.h"
#include "config.h"

struct sdrstate {
  // Stuff for sending commands
  void *phd;               // Opaque pointer to type hid_device
  uint8_t lna_gain;
  uint8_t mixer_gain;
  uint8_t if_gain;
  long long timestamp;
  double frequency;
  unsigned int intfreq;    // Nominal (uncorrected) tuner frequency
  float in_power;          // Running estimate of signal power

  // Smoothed error estimates
  complex float DC;      // DC offset
  float sinphi;          // I/Q phase error
  float imbalance;       // Ratio of I power to Q power
  double calibration;    // TCXO Offset (0 = on frequency)

  // portaudio parameters
  PaStream *Pa_Stream;       // Portaudio handle
  char sdr_name[50];         // name of associated audio device for A/D
  int overrun;               // A/D overrun count
  int overflows;
  uint32_t command_tag;
};

// constants, some of which you might want to tweak
float const AGC_upper = -15;
float const AGC_lower = -50;
int const ADC_samprate = 192000;
float const SCALE16 = 1./SHRT_MAX;
float const DC_alpha = 1.0e-6;  // high pass filter coefficient for DC offset estimates, per sample
float const Power_alpha = 1.0; // time constant (seconds) for smoothing power and I/Q imbalance estimates
int const Bufsize = 16384;
char const *Rundir = "/run/funcube"; // Where 'status' and 'pid' are created

// Empirical: noticeable aliasing beyond this noticed on strong 40m SSB signals
float const LowerEdge = -75000;
float const UpperEdge = +75000;

// Variables set by command line options
int Hold_open; // if set, close control between commands
// A larger blocksize makes more efficient use of each frame, but the receiver generally runs on
// frames that match the Opus codec: 2.5, 5, 10, 20, 40, 60, 180, 100, 120 ms
// So to minimize latency, make this a common denominator:
// 240 samples @ 16 bit stereo = 960 bytes/packet; at 192 kHz, this is 1.25 ms (800 pkt/sec)
int Blocksize = 240;
int Blocksize_TTL0 = 3840; // Default blocksize when IP TTL=0 (20 ms @ 192 kHz)
bool Blocksize_set = false;
int Device = 0;
const char *App_path;
int Verbose;
char const *Locale = "en_US.UTF-8";
bool Daemonize;
int RTP_ttl = 0; // By default, don't leave machine
int Status_ttl = 1; // Don't send fast IQ streams beyond the local network by default
int IP_tos = 48; // AF12 left shifted 2 bits
char const *Name;
char const *Metadata_dest;
char const *Data_dest;
dictionary *Dictionary;
char const *Conf_file = "/etc/radio/funcubed.conf";

struct option const Options[] =
  {
    {"conf", required_argument, NULL, 'f'},
    {"name", required_argument, NULL, 'N'},
    {"verbose", no_argument, NULL, 'v'},
    {"list-audio",no_argument,NULL,'L'},
    {NULL, 0, NULL, 0},
  };
char const Optstring[] = "N:f:vL";

// Global variables
struct rtp_state Rtp;
int Rtp_sock;     // Socket handle for sending real time stream
int Nctl_sock;    // Socket handle for incoming commands
int Status_sock;  // Socket handle for outgoing status messages
struct sockaddr_storage Output_data_source_address;   // Our socket address for data multicast
struct sockaddr_storage Output_metadata_dest_address; // Multicast output socket
struct sockaddr_storage Output_data_dest_address; // Multicast output socket
uint64_t Output_metadata_packets;
char const *Description;

struct sdrstate FCD;
pthread_t Display_thread;
pthread_t Ncmd_thread;
FILE *Status;
uint64_t Commands;
FILE *Tunestate;


void decode_fcd_commands(struct sdrstate *, unsigned char const *,int);
void send_fcd_status(struct sdrstate const *,int);
void do_fcd_agc(struct sdrstate *);
void readback(struct sdrstate *);
double fcd_actual(unsigned int);
static int front_end_init(struct sdrstate *,int,int);
void *display(void *);
void *ncmd(void *);
static void closedown(int a);

int main(int argc,char *argv[]){
  App_path = argv[0];
  struct sdrstate * const sdr = &FCD;

  {
    char const *cp = getenv("LANG");
    if(cp != NULL)
      Locale = cp;
  }
  int c;
  int List_audio = 0;
  int retval = 1; // Default to an abnormal termination code

  while((c = getopt_long(argc,argv,Optstring,Options,NULL)) != -1){
    switch(c){
    case 'f':
      Conf_file = optarg;
      break;
    case 'v':
      Verbose++;
      break;
    case 'N':
      Name = optarg;
      break;
    case 'L':
      List_audio++;
      break;
    default:
    case '?':
      fprintf(stdout,"Unknown argument %c\n",c);
      goto terminate;
      break;
    }
  }
  setlocale(LC_ALL,Locale);

  if(List_audio){
    // Just list audio devices and quit
    // On stdout, not stderr, so we can toss ALSA's noisy error messages with, e.g. 2> /dev/null
    Pa_Initialize();
    int const numDevices = Pa_GetDeviceCount();
    printf("%d Audio devices:\n",numDevices);
    for(int inDevNum=0; inDevNum < numDevices; inDevNum++){
      const PaDeviceInfo *deviceInfo = Pa_GetDeviceInfo(inDevNum);
      printf("%d: %s\n",inDevNum,deviceInfo->name);
    }
    retval = 0;
    goto terminate;
  }
  if(Name == NULL)
    Name = argv[optind];

  if(Conf_file){
    if((Dictionary = iniparser_load(Conf_file)) == NULL){
      fprintf(stdout,"Can't load config file %s\n",Conf_file);
      exit(1);
    }
  } else {
    fprintf(stdout,"No config file specified\n");
    exit(1);
  }
  if(Name == NULL){
    // Name not specified; default to hostname-funcube
    char hostname[_POSIX_HOST_NAME_MAX];
    gethostname(hostname,sizeof(hostname));
    char *cp = strchr(hostname,'.');
    if(cp != NULL)
      *cp = '\0'; // Strip the domain name
    char *foo;
    int const ret = asprintf(&foo,"%s-funcube",hostname);
    if(ret == -1)
      exit(1);
    Name = foo;
    fprintf(stdout,"defaulting to constructed name %s\n",Name);
  }
  if(iniparser_find_entry(Dictionary,Name) != 1){
    fprintf(stdout,"No section %s found in %s\n",Name,Conf_file);
    iniparser_freedict(Dictionary);
    exit(1);
  }
  Device = config_getint(Dictionary,Name,"device",0);
  Default_mcast_iface = config_getstring(Dictionary,Name,"iface",NULL);
  RTP_ttl = config_getint(Dictionary,Name,"data-ttl",0); // Default to 0 for data
  Status_ttl = config_getint(Dictionary,Name,"status-ttl",1);
  {
    char const *cp = config_getstring(Dictionary,Name,"status",NULL);
    if(cp != NULL)
      Metadata_dest = cp;
    else {
      char *foo;
      asprintf(&foo,"%s-status.local",Name);
      Metadata_dest = foo;
    }
  }

  Hold_open = config_getboolean(Dictionary,Name,"hold-open",true);
  IP_tos = config_getint(Dictionary,Name,"tos",48);
  Rtp.ssrc = config_getint(Dictionary,Name,"ssrc",0);
  Blocksize = config_getint(Dictionary,Name,"blocksize",RTP_ttl == 0 ? Blocksize_TTL0 : Blocksize);
  Description = config_getstring(Dictionary,Name,"description",NULL);
  {
    avahi_start(Name,"_ka9q-ctl._udp",DEFAULT_STAT_PORT,Metadata_dest,ElfHashString(Metadata_dest),NULL);
    char iface[IFNAMSIZ];
    resolve_mcast(Metadata_dest,&Output_metadata_dest_address,DEFAULT_STAT_PORT,iface,sizeof(iface));
    Status_sock = connect_mcast(&Output_metadata_dest_address,iface,Status_ttl,IP_tos);
    if(Status_sock <= 0){
      fprintf(stdout,"Can't create status socket %s: %s\n",Metadata_dest,strerror(errno));
      goto terminate;
    }
    // Set up new control socket on port DEFAULT_STAT_PORT
    Nctl_sock = listen_mcast(&Output_metadata_dest_address,iface);
    if(Nctl_sock <= 0){
      fprintf(stdout,"Can't create control socket %s: %s\n",Metadata_dest,strerror(errno));
      goto terminate;
    }
  }
  {
    char const *cp = config_getstring(Dictionary,Name,"data",NULL);
    if(cp != NULL)
      Data_dest = cp;
    else {
      char *foo;
      asprintf(&foo,"%s-data.local",Name);
      Data_dest = foo;
    }
  }

  {
    avahi_start(Name,"_rtp._udp",DEFAULT_RTP_PORT,Data_dest,ElfHashString(Data_dest),NULL);
    char iface[IFNAMSIZ];
    resolve_mcast(Data_dest,&Output_data_dest_address,DEFAULT_RTP_PORT,iface,sizeof(iface));
    Rtp_sock = connect_mcast(&Output_data_dest_address,iface,RTP_ttl,IP_tos);

    if(Rtp_sock == -1){
      fprintf(stdout,"Can't create data socket %s: %s\n",Data_dest,strerror(errno));
      goto terminate;
    }
  }
  socklen_t len = sizeof(Output_data_source_address);
  getsockname(Rtp_sock,(struct sockaddr *)&Output_data_source_address,&len);

  // Catch signals so portaudio can be shut down
  signal(SIGALRM,closedown);
  signal(SIGVTALRM,closedown);
  signal(SIGPIPE,SIG_IGN);
  signal(SIGINT,closedown);
  signal(SIGKILL,closedown);
  signal(SIGQUIT,closedown);
  signal(SIGTERM,closedown);        
  signal(SIGBUS,closedown);
  signal(SIGSEGV,closedown);

  // Load/save calibration file
  {
    char *calfilename = NULL;
    
    if(asprintf(&calfilename,"%s/cal-funcube-%d",VARDIR,Device) > 0){
      FILE *calfp = NULL;
      if(sdr->calibration == 0){
	if((calfp = fopen(calfilename,"r")) != NULL){
	  if(fscanf(calfp,"%lg",&sdr->calibration) < 1){
	    fprintf(stdout,"Can't read calibration from %s\n",calfilename);
	  }
	}
      } else {
	if((calfp = fopen(calfilename,"w")) != NULL){
	  fprintf(calfp,"%.6lg\n",sdr->calibration);
	}
      }
      if(calfp)
	fclose(calfp);
      free(calfilename);
    }
  }
  // Config file overrides state save file
  sdr->calibration = config_getdouble(Dictionary,Name,"calibration",sdr->calibration);
  sleep(1);
  Pa_Initialize();
  if(front_end_init(sdr,Device,Blocksize) < 0){
    fprintf(stdout,"front_end_init(%p,%d,%d) failed\n",
	   sdr,Device,Blocksize);
    goto terminate;
  }
  {
    char tmp[PATH_MAX];
    snprintf(tmp,sizeof(tmp),"%s/tune-funcube.%d",VARDIR,Device);
    Tunestate = fopen(tmp,"r+");
    if(!Tunestate){
      fprintf(stdout,"Can't open tuner state file %s: %s\n",tmp,strerror(errno));
    } else {
      // restore frequency from state file, if present
      int freq;
      rewind(Tunestate);
      if(fscanf(Tunestate,"%d",&freq) > 0){
	sdr->intfreq = freq;
	// LNA gain is frequency-dependent
	if(sdr->lna_gain){
	  if(sdr->intfreq >= 420e6)
	    sdr->lna_gain = 7;
	  else
	    sdr->lna_gain = 24;
	}
	if(sdr->phd == NULL && (sdr->phd = fcdOpen(sdr->sdr_name,sizeof(sdr->sdr_name),Device)) == NULL){
	  fprintf(stdout,"can't re-open control port: %s\n",strerror(errno));
	  return 1; // fatal error
	}
	fcdAppSetFreq(sdr->phd,sdr->intfreq);
	sdr->frequency = fcd_actual(sdr->intfreq) * (1 + sdr->calibration);
      }
      fclose(Tunestate);
      Tunestate = NULL;
    }
    // Recreate for writing
    Tunestate = fopen(tmp,"w+");
    if(Tunestate == NULL)
      fprintf(stdout,"Can't create tuner state file %s: %s\n",tmp,strerror(errno));
    else {
      fprintf(Tunestate,"%d\n",sdr->intfreq);
      fflush(Tunestate); // Leave open for further use
    }
  }
  uint8_t bias = config_getboolean(Dictionary,Name,"bias",false);
  fcdAppSetParam(sdr->phd,FCD_CMD_APP_SET_BIAS_TEE,&bias,sizeof(bias));

  pthread_create(&Ncmd_thread,NULL,ncmd,sdr);

  if(Status)
    pthread_create(&Display_thread,NULL,display,sdr);

  if(Rtp.ssrc == 0){
    Rtp.ssrc = gps_time_sec() & 0xffffffff; // low 32 bits of clock time
  }
  fprintf(stdout,"uid %d; device %d; cal %f ppm; bias tee %s; dest %s; blocksize %'d samples; RTP SSRC %u\n",
	  getuid(),
	  Device,
	  sdr->calibration * 1e6,
	  bias ? "On" : "Off",
	  Metadata_dest,
	  Blocksize,
	  Rtp.ssrc);
  // Gain and phase corrections. These will be updated every block
  float gain_q = 1;
  float gain_i = 1;
  float secphi = 1;
  float tanphi = 0;

  sdr->timestamp = gps_time_ns();
  float const rate_factor = Blocksize/(ADC_samprate * Power_alpha);

  int ConsecPaErrs = 0;
  int ConsecSendErrs = 0;

  while(1){
    struct rtp_header rtp;
    memset(&rtp,0,sizeof(rtp));
    rtp.version = RTP_VERS;
    rtp.type = PCM_STEREO_PT;
    rtp.ssrc = Rtp.ssrc;
    rtp.seq = Rtp.seq++;
    rtp.timestamp = Rtp.timestamp;

    // Space for the samples (stereo 16-bit samples) + RTP header
    unsigned char buffer[Blocksize * 2 * sizeof(int16_t) + 100]; // Pick a better value
    unsigned char *dp = buffer;

    dp = hton_rtp(dp,&rtp);
    int16_t *sampbuf = (int16_t *)dp;

    // Read block of I/Q samples from A/D converter
    // The timer is necessary because portaudio will go into a tight loop if the device is unplugged
    struct itimerval itime;
    memset(&itime,0,sizeof(itime));
    itime.it_value.tv_sec = 1; // 1 second should be more than enough
    if(setitimer(ITIMER_VIRTUAL,&itime,NULL) == -1){
      perror("setitimer start");
      goto terminate;
    }
    int const r = Pa_ReadStream(sdr->Pa_Stream,sampbuf,Blocksize);
    memset(&itime,0,sizeof(itime));
    if(setitimer(ITIMER_VIRTUAL,&itime,NULL) == -1){
      perror("setitimer stop");
      goto terminate;
    }
    if(r < 0){
      if(r == paInputOverflowed){
	sdr->overflows++; // Not fatal
	ConsecPaErrs = 0;
      } else if(++ConsecPaErrs < 10){
	fprintf(stdout,"Pa_ReadStream: %s\n",Pa_GetErrorText(r));
      } else {
	fprintf(stdout,"Pa_ReadStream: %s, exiting\n",Pa_GetErrorText(r));
	goto terminate;
      }
    } else
      ConsecPaErrs = 0;

    dp += Blocksize * 2 * sizeof(*sampbuf);

    float i_energy=0, q_energy=0;
    complex float samp_sum = 0;
    float dotprod = 0;
    
    for(int i=0; i<2*Blocksize; i += 2){
      complex float samp = CMPLXF(sampbuf[i],sampbuf[i+1]) * SCALE16;

      samp_sum += samp; // Accumulate average DC values
      samp -= sdr->DC;   // remove smoothed DC offset (which can be fractional)

      // Must correct gain and phase before frequency shift
      // accumulate I and Q energies before gain correction
      i_energy += crealf(samp) * crealf(samp);
      q_energy += cimagf(samp) * cimagf(samp);
    
      // Balance gains, keeping constant total energy
      __real__ samp *= gain_i;
      __imag__ samp *= gain_q;
    
      // Accumulate phase error
      dotprod += crealf(samp) * cimagf(samp);

      // Correct phase
      __imag__ samp = secphi * cimagf(samp) - tanphi * crealf(samp);
      
      // Cast is necessary since htons() is a macro!
      sampbuf[i] = htons(scaleclip(crealf(samp)));
      sampbuf[i+1] = htons(scaleclip(cimagf(samp)));
    }

    if(send(Rtp_sock,buffer,dp - buffer,0) == -1){
      if(errno == ENOBUFS || errno == EDESTADDRREQ || errno == ENOTCONN){
	ConsecSendErrs = 0;
	continue; // Probably transient, ignore
      }
      if(++ConsecSendErrs < 10)
	fprintf(stdout,"send: %s\n",strerror(errno));
      else {
	fprintf(stdout,"send: %s, exiting\n",strerror(errno));
	goto terminate;
      }
      // If we're sending to a unicast address without a listener, we'll get ECONNREFUSED
      // Should sleep to slow down the rate of these messages
    } else {
      ConsecSendErrs = 0;
      Rtp.packets++;
      Rtp.bytes += Blocksize;
    }
    Rtp.timestamp += Blocksize; // Increment timestamp even if there's an error

#if 1
    // Get status timestamp from UNIX TOD clock -- but this might skew because of inexact sample rate
    sdr->timestamp = gps_time_ns();
#else
    // Simply increment by number of samples
    // But what if we lose some? Then the clock will always be off
    sdr->timestamp += 1.e9 * Blocksize / ADC_samprate;
#endif

    // Update every block
    // estimates of DC offset, signal powers and phase error
    sdr->DC += DC_alpha * (samp_sum - Blocksize*sdr->DC);
    float const block_energy = i_energy + q_energy; // Normalize for complex pairs
    if(block_energy > 0){ // Avoid divisions by 0, etc
      sdr->in_power = block_energy/Blocksize; // Average A/D output power per channel  
      sdr->imbalance += rate_factor * ((i_energy / q_energy) - sdr->imbalance);
      float const dpn = 2 * dotprod / block_energy;
      sdr->sinphi += rate_factor * (dpn - sdr->sinphi);
      gain_q = sqrtf(0.5 * (1 + sdr->imbalance));
      gain_i = sqrtf(0.5 * (1 + 1./sdr->imbalance));
      secphi = 1/sqrtf(1 - sdr->sinphi * sdr->sinphi); // sec(phi) = 1/cos(phi)
      tanphi = sdr->sinphi * secphi;      // tan(phi) = sin(phi) * sec(phi) = sin(phi)/cos(phi)
    }
  }
 terminate:
  Pa_Terminate();
  if(Status_sock > 2)
    close(Status_sock);
  if(Nctl_sock > 2)
    close(Nctl_sock);

  if(Rtp_sock > 2)
    close(Rtp_sock);

  return retval;
}

// Thread to send metadata and process commands
void *ncmd(void *arg){
  pthread_setname("funcube-cmd");
  assert(arg != NULL);
  struct sdrstate * const sdr = arg;

  // Set up status socket on port DEFAULT_STAT_PORT
  struct timeval tv;
  tv.tv_sec = 0;
  tv.tv_usec = 100000; // 100 ms

  if(setsockopt(Nctl_sock,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv))){
    perror("ncmd setsockopt");
    return NULL;
  }
  int counter = 0;
  while(1){
    unsigned char buffer[Bufsize];
    int const length = recv(Nctl_sock,buffer,sizeof(buffer),0); // Waits up to 100 ms for command
    if(sdr->phd == NULL && (sdr->phd = fcdOpen(sdr->sdr_name,sizeof(sdr->sdr_name),Device)) == NULL){
      fprintf(stdout,"can't re-open control port: %s\n",strerror(errno));
      return NULL;
    }
    if(length > 0){
      if(buffer[0] == 0)
	continue;
      // Parse entries
      Commands++;
      decode_fcd_commands(sdr,buffer+1,length-1);
      counter = 0; // Respond with full status
    }
    readback(sdr);
    Output_metadata_packets++;
    send_fcd_status(sdr,counter == 0);
    if(!Hold_open){
      do_fcd_agc(sdr);
    } else if(sdr->phd != NULL){
      fcdClose(sdr->phd);
      sdr->phd = NULL;
    }
    if(counter-- <= 0)
      counter = 10;
  }
}

// Status display thread
void *display(void *arg){
  pthread_setname("funcube-disp");

  long messages = 0;
  struct sdrstate const *sdr = (struct sdrstate *)arg;

  fprintf(Status,"funcube daemon pid %d device %d\n",getpid(),Device);
  fprintf(Status,"               |---Gains dB---|      |----Levels dB --|   |---------Errors---------|           Overflows                messages\n");
  fprintf(Status,"Frequency      LNA  mixer bband          RF   A/D   Out     DC-I   DC-Q  phase  gain                        TCXO\n");
  fprintf(Status,"Hz                                           dBFS  dBFS                    deg    dB                         ppm\n");   

  off_t const stat_point = ftello(Status); // Current offset if file, -1 if terminal

  // End lines with return when writing to terminal, newlines when writing to status file
  char const eol = stat_point == -1 ? '\r' : '\n';

  while(1){
    //    float powerdB = 10*log10f(sdr->in_power) - 90.308734;
    float powerdB = power2dB(sdr->in_power);

    if(stat_point != -1)
      fseeko(Status,stat_point,SEEK_SET);
    fprintf(Status,"%'-15.0lf%3d%7d%6d%'12.1f%'6.1f%'6.1f%9.4f%7.4f%7.2f%6.2f%'16d    %8.4lf%'10ld%c",
	    sdr->frequency,
	    sdr->lna_gain,	    
	    sdr->mixer_gain,
	    sdr->if_gain,
	    powerdB - (sdr->lna_gain + sdr->mixer_gain + sdr->if_gain),
	    powerdB,
	    powerdB,
	    crealf(sdr->DC),
	    cimagf(sdr->DC),
	    (180/M_PI) * asin(sdr->sinphi),
	    power2dB(sdr->imbalance),
	    sdr->overflows,
	    sdr->calibration * 1e6,
	    messages,
	    eol
	    );
    messages++;
    fflush(Status);
    usleep(100000);
  }    
  return NULL;
}


void decode_fcd_commands(struct sdrstate *sdr, unsigned char const *buffer,int length){
  unsigned char const *cp = buffer;

  while(cp - buffer < length){
    enum status_type const type = *cp++; // increment cp to length field
    
    if(type == EOL)
      break; // End of list
    
    unsigned int const optlen = *cp++;
    if(cp - buffer + optlen >= length)
      break; // Invalid length

    switch(type){
    case EOL: // Shouldn't get here
      break;
    case CALIBRATE:
      sdr->calibration = decode_double(cp,optlen);
      break;
    case RADIO_FREQUENCY:
      sdr->frequency = decode_double(cp,optlen);
      sdr->intfreq = round(sdr->frequency/ (1 + sdr->calibration));
      if(Tunestate){
	rewind(Tunestate);
	// Don't let stuff pile up in file when the frequency field shortens
	if(ftruncate(fileno(Tunestate),0) != 0)
	  perror("ftruncate");

	fprintf(Tunestate,"%d\n",sdr->intfreq);
	fflush(Tunestate);
      }
      // LNA gain is frequency-dependent
      if(sdr->lna_gain){
	if(sdr->intfreq >= 420e6)
	  sdr->lna_gain = 7;
	else
	  sdr->lna_gain = 24;
      }
      fcdAppSetFreq(sdr->phd,sdr->intfreq);
      sdr->frequency = fcd_actual(sdr->intfreq) * (1 + sdr->calibration);
      break;
    case LNA_GAIN:
      sdr->lna_gain = decode_int(cp,optlen);
      {
	unsigned char val = sdr->lna_gain ? 1 : 0;
	fcdAppSetParam(sdr->phd,FCD_CMD_APP_SET_LNA_GAIN,&val,sizeof(val));
      }
      break;
    case MIXER_GAIN:
      sdr->mixer_gain = decode_int(cp,optlen);
      {
	unsigned char val = sdr->mixer_gain ? 1 : 0;
	fcdAppSetParam(sdr->phd,FCD_CMD_APP_SET_MIXER_GAIN,&val,sizeof(val));
      }
      break;
    case IF_GAIN:
      sdr->if_gain = decode_int(cp,optlen);
      fcdAppSetParam(sdr->phd,FCD_CMD_APP_SET_IF_GAIN1,&sdr->if_gain,sizeof(sdr->if_gain));
      break;
    case COMMAND_TAG:
      sdr->command_tag = decode_int(cp,optlen);
      break;
    default: // Ignore all others
      break;
    }
    cp += optlen;
  }
}

void send_fcd_status(struct sdrstate const *sdr,int full){
  unsigned char packet[2048];
  memset(packet,0,sizeof(packet));
  unsigned char *bp = packet;
  
  *bp++ = 0; // command/response = response
  encode_int32(&bp,COMMAND_TAG,sdr->command_tag);
  encode_int64(&bp,CMD_CNT,Commands);
  
  encode_int64(&bp,GPS_TIME,gps_time_ns());

  if(Description)
    encode_string(&bp,DESCRIPTION,Description,strlen(Description));
  
  encode_socket(&bp,OUTPUT_DATA_SOURCE_SOCKET,&Output_data_source_address);
  encode_socket(&bp,OUTPUT_DATA_DEST_SOCKET,&Output_data_dest_address);
  encode_int32(&bp,OUTPUT_SSRC,Rtp.ssrc);
  encode_byte(&bp,OUTPUT_TTL,RTP_ttl);
  encode_int32(&bp,INPUT_SAMPRATE,ADC_samprate);   // Both sample rates are the same
  encode_int32(&bp,OUTPUT_SAMPRATE,ADC_samprate);
  encode_int64(&bp,OUTPUT_DATA_PACKETS,Rtp.packets);
  encode_int64(&bp,OUTPUT_METADATA_PACKETS,Output_metadata_packets);
  
  // Front end
  //  encode_float(&bp,AD_LEVEL,power2dB(sdr->in_power));
  encode_double(&bp,CALIBRATE,sdr->calibration);
  encode_byte(&bp,LNA_GAIN,sdr->lna_gain);
  encode_byte(&bp,MIXER_GAIN,sdr->mixer_gain);
  encode_byte(&bp,IF_GAIN,sdr->if_gain);
  encode_float(&bp,DC_I_OFFSET,crealf(sdr->DC));
  encode_float(&bp,DC_Q_OFFSET,cimagf(sdr->DC));
  encode_float(&bp,IQ_IMBALANCE,power2dB(sdr->imbalance));
  encode_float(&bp,IQ_PHASE,sdr->sinphi);
  encode_byte(&bp,DIRECT_CONVERSION,1);
  encode_int32(&bp,OUTPUT_BITS_PER_SAMPLE,16); // Always
  
  // Tuning
  encode_double(&bp,RADIO_FREQUENCY,sdr->frequency);
  
  // Filtering
  encode_float(&bp,LOW_EDGE,LowerEdge);
  encode_float(&bp,HIGH_EDGE,UpperEdge);
  
  encode_float(&bp,OUTPUT_LEVEL,power2dB(sdr->in_power));

  float analog_gain = sdr->mixer_gain + sdr->if_gain + sdr->lna_gain;
  encode_float(&bp,GAIN,analog_gain); // Overall gain (no digital gain)
  encode_byte(&bp,DEMOD_TYPE,0); // Actually LINEAR_MODE
  encode_int32(&bp,OUTPUT_CHANNELS,2);

  encode_eol(&bp);
  int const len = bp - packet;
  
  assert(len < sizeof(packet));
  send(Status_sock,packet,len,0);
}

void readback(struct sdrstate *sdr){
  // Read back FCD state every iteration, whether or not we processed a command, just in case it was set by another program
  unsigned char val;
  fcdAppGetParam(sdr->phd,FCD_CMD_APP_GET_LNA_GAIN,&val,sizeof(val));
  if(val){
    if(sdr->intfreq >= 420000000)
      sdr->lna_gain = 7;
    else
      sdr->lna_gain = 24;
  } else
    sdr->lna_gain = 0;
  
  fcdAppGetParam(sdr->phd,FCD_CMD_APP_GET_MIXER_GAIN,&val,sizeof(val));
  sdr->mixer_gain = val ? 19 : 0;
  
  fcdAppGetParam(sdr->phd,FCD_CMD_APP_GET_IF_GAIN1,&val,sizeof(val));
  sdr->if_gain = val;
  
  fcdAppGetParam(sdr->phd,FCD_CMD_APP_GET_FREQ_HZ,(unsigned char *)&sdr->intfreq,sizeof(sdr->intfreq));
  sdr->frequency = fcd_actual(sdr->intfreq) * (1 + sdr->calibration);
}
static int front_end_init(struct sdrstate *sdr,int device,int L){
  if((sdr->phd = fcdOpen(sdr->sdr_name,sizeof(sdr->sdr_name),device)) == NULL){
    fprintf(stdout,"fcdOpen(%s): %s\n",sdr->sdr_name,strerror(errno));
    return -1;
  }
  int r;
  if((r = fcdGetMode(sdr->phd)) == FCD_MODE_APP){
    char caps_str[100];
    fcdGetCapsStr(sdr->phd,caps_str);
    fprintf(stdout,"audio device name '%s', caps '%s'\n",sdr->sdr_name,caps_str);
  } else if(r == FCD_MODE_NONE){
    fprintf(stdout," No FCD detected!\n");
    r = -1;
    goto done;
  } else if (r == FCD_MODE_BL){
    fprintf(stdout," is in bootloader mode\n");
    r = -1;
    goto done;
  }
  // Set up sample stream through portaudio subsystem
  // Search audio devices
  int const numDevices = Pa_GetDeviceCount();
  int inDevNum = paNoDevice;
  for(int i = 0; i < numDevices; i++){
    PaDeviceInfo const *deviceInfo = Pa_GetDeviceInfo(i);
    if(strstr(deviceInfo->name,sdr->sdr_name) != NULL){
      inDevNum = i;
      fprintf(stdout,"portaudio name: %s\n",deviceInfo->name);
      break;
    }
  }
  if(inDevNum == paNoDevice){
    fprintf(stdout,"Can't find portaudio name\n");
    r = -1;
    goto done;
  }
  PaStreamParameters inputParameters;
  memset(&inputParameters,0,sizeof(inputParameters));
  inputParameters.channelCount = 2;
  inputParameters.device = inDevNum;
  inputParameters.sampleFormat = paInt16;
  inputParameters.suggestedLatency = 0.020;
  r = Pa_OpenStream(&sdr->Pa_Stream,&inputParameters,NULL,ADC_samprate,
		    paFramesPerBufferUnspecified, 0, NULL, NULL);

  if(r < 0){
    fprintf(stdout,"Pa_OpenStream error: %s\n",Pa_GetErrorText(r));
    goto done;
  }

  r = Pa_StartStream(sdr->Pa_Stream);
  if(r < 0)
    fprintf(stdout,"Pa_StartStream error: %s\n",Pa_GetErrorText(r));

 done:; // Also the abort target: close handle before returning
  if(!Hold_open && sdr->phd != NULL){
    fcdClose(sdr->phd);
    sdr->phd = NULL;
  }
  return r;
}
// Crude analog AGC just to keep signal roughly within A/D range
// Executed only if -o option isn't specified; this allows manual control with, e.g., the fcdpp command
void do_fcd_agc(struct sdrstate *sdr){

  float const powerdB = power2dB(sdr->in_power);
  
  if(powerdB > AGC_upper){
    if(sdr->if_gain > 0){
      // Decrease gain in 10 dB steps, down to 0
      unsigned char val = sdr->if_gain = max(0,sdr->if_gain - 10);
      fcdAppSetParam(sdr->phd,FCD_CMD_APP_SET_IF_GAIN1,&val,sizeof(val));
    } else if(sdr->mixer_gain){
      unsigned char val = sdr->mixer_gain = 0;
      fcdAppSetParam(sdr->phd,FCD_CMD_APP_SET_MIXER_GAIN,&val,sizeof(val));
    } else if(sdr->lna_gain){
      unsigned char val = sdr->lna_gain = 0;
      fcdAppSetParam(sdr->phd,FCD_CMD_APP_SET_LNA_GAIN,&val,sizeof(val));
    }
  } else if(powerdB < AGC_lower){
    if(sdr->lna_gain == 0){
      sdr->lna_gain = 24;
      unsigned char val = 1;
      fcdAppSetParam(sdr->phd,FCD_CMD_APP_SET_LNA_GAIN,&val,sizeof(val));
    } else if(sdr->mixer_gain == 0){
      sdr->mixer_gain = 19;
      unsigned char val = 1;
      fcdAppSetParam(sdr->phd,FCD_CMD_APP_SET_MIXER_GAIN,&val,sizeof(val));
    } else if(sdr->if_gain < 20){ // Limit to 20 dB - seems enough to keep A/D going even on noise
      unsigned char val = sdr->if_gain = min(20,sdr->if_gain + 10);
      fcdAppSetParam(sdr->phd,FCD_CMD_APP_SET_IF_GAIN1,&val,sizeof(val));
    }
  }
}

// The funcube device uses the Mirics MSi001 tuner. It has a fractional N synthesizer that can't actually do integer frequency steps.
// This formula is hacked down from code from Howard Long; it's what he uses in the firmware so I can figure out
// the *actual* frequency. Of course, we still have to correct it for the TCXO offset.

// This needs to be generalized since other tuners will be completely different!
double fcd_actual(unsigned int u32Freq){
  typedef uint32_t UINT32;
  typedef uint64_t UINT64;

  UINT32 const u32Thresh = 3250U;
  UINT32 const u32FRef = 26000000U;

  
  struct
  {
    UINT32 u32Freq;
    UINT32 u32FreqOff;
    UINT32 u32LODiv;
  } *pts,ats[]=
      {
	{4000000U,130000000U,16U},
	{8000000U,130000000U,16U},
	{16000000U,130000000U,16U},
	{32000000U,130000000U,16U},
	{75000000U,130000000U,16U},
	{125000000U,0U,32U},
	{142000000U,0U,16U},
	{148000000U,0U,16U},
	{300000000U,0U,16U},
	{430000000U,0U,4U},
	{440000000U,0U,4U},
	{875000000U,0U,4U},
	{UINT32_MAX,0U,2U},
	{0U,0U,0U}
      };
  for(pts = ats; u32Freq >= pts->u32Freq; pts++)
    ;

  if (pts->u32Freq == 0)
    pts--;
      
  // Frequency of synthesizer before divider - can possibly exceed 32 bits, so it's stored in 64
  UINT64 const u64FSynth = ((UINT64)u32Freq + pts->u32FreqOff) * pts->u32LODiv;

  // Integer part of divisor ("INT")
  UINT32 const u32Int = u64FSynth / (u32FRef*4);

  // Subtract integer part to get fractional and AFC parts of divisor ("FRAC" and "AFC")
  UINT32 const u32Frac4096 =  (u64FSynth<<12) * u32Thresh/(u32FRef*4) - (u32Int<<12) * u32Thresh;

  // FRAC is higher 12 bits
  UINT32 const u32Frac = u32Frac4096>>12;

  // AFC is lower 12 bits
  UINT32 const u32AFC = u32Frac4096 - (u32Frac<<12);
      
  // Actual tuner frequency, in floating point, given specified parameters
  double const f64FAct = (4.0 * u32FRef / (double)pts->u32LODiv) * (u32Int + ((u32Frac * 4096.0 + u32AFC) / (u32Thresh * 4096.))) - pts->u32FreqOff;
  
  // double f64step = ( (4.0 * u32FRef) / (pts->u32LODiv * (double)u32Thresh) ) / 4096.0;
  //      printf("f64step = %'lf, u32LODiv = %'u, u32Frac = %'d, u32AFC = %'d, u32Int = %'d, u32Thresh = %'d, u32FreqOff = %'d, f64FAct = %'lf err = %'lf\n",
  //	     f64step, pts->u32LODiv, u32Frac, u32AFC, u32Int, u32Thresh, pts->u32FreqOff,f64FAct,f64FAct - u32Freq);
  return f64FAct;
}

// If we don't stop the A/D, it'll take several seconds to overflow and stop by itself,
// and during that time we can't restart
static void closedown(int a){
  fprintf(stdout,"funcube: caught signal %d: %s\n",a,strsignal(a));
  Pa_Terminate();
  if(a == SIGTERM) // sent by systemd when shutting down. Return success
    exit(0);

  exit(1);
}


