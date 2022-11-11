// $Id: airspyhfd.c,v 1.4 2022/09/24 15:00:46 karn Exp $
// Read from Airspy SDR
// Accept control commands from UDP socket
#define _GNU_SOURCE 1
#include <assert.h>
#include <pthread.h>
#include <string.h>
#include <complex.h>
#include <math.h>
#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/types.h>
#include <dirent.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <net/if.h>
#include <signal.h>
#include <locale.h>
#include <sys/time.h>
#include <libairspyhf/airspyhf.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <errno.h>
#include <syslog.h>
#include <sys/stat.h>
#include <getopt.h>
#include <iniparser/iniparser.h>

#include "conf.h"
#include "misc.h"
#include "multicast.h"
#include "decimate.h"
#include "status.h"
#include "config.h"

#define N_serials 20
uint64_t Serials[N_serials];

// Configurable parameters
// decibel limits for power
float const AGC_upper = -20;
float const AGC_lower = -50;

int const Bufsize = 65536; // should pick more deterministically
#define BUFFERSIZE  (1<<21) // Upcalls seem to be 256KB; don't make too big or we may blow out of the cache

// Global variables set by command line options
char const *Iface;
char *Locale;
int RTP_ttl;
int Status_ttl;
int IP_tos;
const char *App_path;
int Verbose;
struct airspyhf_device *Device; // Set for benefit of closedown()

struct sdrstate {
  struct airspyhf_device *device;    // Opaque pointer

  uint32_t sample_rates[20];
  uint64_t SN; // Serial number
  char const *description;
  int samprate; // True sample rate of single A/D converter
  int iq; // Working in complex sample mode

  // Tuning
  double frequency;
  double calibration; // Frequency error
  int frequency_lock;
  char *frequency_file; // Local file to store frequency in case we restart

  // AGC
  int agc; // Use firmware agc when set
  int linearity; // Use linearity gain tables; default is sensitivity
  uint8_t lna_gain;
  uint8_t mixer_gain;
  uint8_t if_gain;

  // Sample statistics
  int clips;  // Sample clips since last reset
  float power;   // Running estimate of A/D signal power
  float DC;      // DC offset for real samples

  int blocksize;// Number of real samples per packet or twice the number of complex samples per packet

  FILE *status;    // Real-time display in /run (currently unused)

  // Multicast I/O
  char const *metadata_dest;
  struct sockaddr_storage output_metadata_dest_address;
  uint64_t output_metadata_packets;
  int status_sock;  // Socket handle for outgoing status messages
  int nctl_sock;    // Socket handle for incoming commands (same socket as status)

  uint64_t commands; // Command counter
  uint32_t command_tag; // Last received command tag
  
  char const *data_dest;
  struct sockaddr_storage output_data_source_address; // Multicast output socket
  struct sockaddr_storage output_data_dest_address; // Multicast output socket
  int data_sock;     // Socket handle for sending real time stream
  struct rtp_state rtp; // Real time protocol (RTP) state
  int rtp_type;      // RTP type to indicate sample rate, etc (should be rethought)

  pthread_t display_thread;
  pthread_t ncmd_thread;
};

// Global state - only one device at the moment


static struct option Options[] =
  {
    {"verbose", no_argument, NULL, 'v'},
    {"config",required_argument,NULL,'f'},
    {NULL, 0, NULL, 0},
  };
static char Optstring[] = "f:v";

double set_correct_freq(struct sdrstate *sdr,double freq);
void decode_airspyhf_commands(struct sdrstate *,unsigned char *,int);
void send_airspyhf_status(struct sdrstate *,int);
int rx_callback(airspyhf_transfer_t *);
void *display(void *);
void *ncmd(void *arg);
double true_freq(uint64_t freq);
static void closedown(int a);

dictionary *Dictionary;
char const *Name;
char const *Conf_file;

int main(int argc,char *argv[]){
  App_path = argv[0];
  umask(02);
#if 0
  // Dump environment variables
  extern char **environ;
  for(int i=0;;i++){
    if(environ[i])
      fprintf(stderr,"environ[%d]: %s\n",i,environ[i]);
    else
      break;
  }
#endif  

  struct sdrstate * const sdr = (struct sdrstate *)calloc(1,sizeof(struct sdrstate));
  // Defaults
  sdr->agc = 1;
  // 2400 packets/sec @ 768 ks/s, 48 packets per 20 ms, 24 pkts/10 ms
  sdr->blocksize = 160;
  sdr->rtp_type = IQ_FLOAT;

  Locale = getenv("LANG");
  if(Locale == NULL || strlen(Locale) == 0)
    Locale = "en_US.UTF-8";
  setlocale(LC_ALL,Locale);

  setlinebuf(stdout);

  int c;
  double init_frequency = 0;
  while((c = getopt_long(argc,argv,Optstring,Options,NULL)) != -1){
    switch(c){
    case 'f':
      Conf_file = optarg;
      break;
    case 'v':
      Verbose++;
      break;
    default:
    case '?':
      fprintf(stderr,"Unknown argument %c\n",c);
      break;
    }
  }
  if(optind >= argc){
    fprintf(stdout,"Name missing\n");
    fprintf(stdout,"Usage: %s [-v] [-f config_file] instance_name\n",argv[0]);
    exit(1);
  }
  Name = argv[optind];
  // Process config files
  if(Conf_file){
    // If this fails, don't fall back to any of the defaults
    if((Dictionary = iniparser_load(Conf_file)) == NULL){
      fprintf(stdout,"Can't load config file %s\n",Conf_file);
      exit(1);
    }
    if(iniparser_find_entry(Dictionary,Name) != 1){
      fprintf(stdout,"No section %s found in %s\n",Name,Conf_file);
      iniparser_freedict(Dictionary);
      exit(1);
    }
  } else if((Dictionary = iniparser_load("/etc/radio/airspyd.conf")) != NULL){
    if(iniparser_find_entry(Dictionary,Name) == 1){
      printf("Using config file /etc/radio/airspyd.conf\n");
    } else {
      iniparser_freedict(Dictionary);
      Dictionary = NULL;
    }
  }
  if(Dictionary == NULL){
    // Search everything under /etc/radio/airspyd.conf.d
    char const *subdir = "/etc/radio/airspyd.conf.d";
    DIR *dir = opendir(subdir);
    if(dir != NULL){
      struct dirent const *dp;
      while((dp = readdir(dir)) != NULL){
	if(dp->d_type != DT_REG)
	  continue;
	int len = strlen(dp->d_name);
	if(len < 5)
	  continue;
	if(strcmp(&dp->d_name[len-5],".conf") != 0)
	  continue; // Name doesn't end in .conf
	char path[PATH_MAX];
	// Checking the return value suppresses a (bogus) gcc warning
	// about possibly truncating the target. We know it can't
	// beecause dp->d_name is 256 chars
	int ret = snprintf(path,sizeof(path),"%s/%s",subdir,dp->d_name);
	if(ret > sizeof(path))
	  continue; // bogus entry
	if((Dictionary = iniparser_load(path)) != NULL){
	  if(iniparser_find_entry(Dictionary,Name) == 1){
	    printf("Using config file %s section %s\n",path,Name);
	    break;
	  } else {
	    iniparser_freedict(Dictionary);
	    Dictionary = NULL;
	  }
	}
      }
      closedir(dir);
      dir = NULL;
    }
  }
  if(Dictionary == NULL){
    fprintf(stdout,"section %s not found in any config file\n",Name);
    exit(1);
  }

  int ret;
  {
    char const *sn = config_getstring(Dictionary,Name,"serial",NULL);
    if(sn == NULL){
      fprintf(stdout,"'serial' not defined in section %s\n",Name);
      iniparser_freedict(Dictionary);
      exit(1);
    }
    char *endptr = NULL;
    sdr->SN = strtoull(sn,&endptr,16);
    if(endptr == NULL || *endptr != '\0'){
      fprintf(stdout,"Invalid serial number %s in section %s\n",sn,Name);
      iniparser_freedict(Dictionary);
      exit(1);
    }
  }
  // Serial number specified, open that one
  if((ret = airspyhf_open_sn(&sdr->device,sdr->SN)) != AIRSPYHF_SUCCESS){
    fprintf(stderr,"airspyhf_open(%llx) failed: %d\n",(long long unsigned)sdr->SN,ret);
    exit(1);
  }
  {
    airspyhf_lib_version_t version;
    airspyhf_lib_version(&version);

    char hw_version[MAX_VERSION_STRING_SIZE];
    airspyhf_version_string_read(sdr->device,hw_version,sizeof(hw_version));

    fprintf(stdout,"AirspyHF serial %llx, hw version %s, library version %d.%d.%d\n",
	    (long long unsigned)sdr->SN,
	    hw_version,
	    version.major_version,version.minor_version,version.revision);
  }

  Device = sdr->device;
  {
    uint32_t num_samprates;
    int ret = airspyhf_get_samplerates(sdr->device,&num_samprates,0);
    assert(ret == AIRSPYHF_SUCCESS);
    
    uint32_t samprates[num_samprates];
    ret = airspyhf_get_samplerates(sdr->device,samprates,num_samprates);
    assert(ret == AIRSPYHF_SUCCESS);

    fprintf(stdout,"Sample rates:");
    for(int i=0;i<num_samprates;i++){
      fprintf(stdout," %'d",samprates[i]);
    }
    fprintf(stdout,"\n");

    sdr->samprate = config_getint(Dictionary,Name,"samprate",samprates[0]); // First (fastest) is default
    ret = airspyhf_set_samplerate(sdr->device,(uint32_t)sdr->samprate);
    if(ret != AIRSPYHF_SUCCESS){
      fprintf(stdout,"invalid sample rate %d\n",sdr->samprate);
      exit(1);
    }
    fprintf(stdout,"Set sample rate %'u Hz\n",sdr->samprate);
  }

  {
    // Multicast output interface for both data and status
    Iface = config_getstring(Dictionary,Name,"iface",NULL);

    sdr->data_dest = config_getstring(Dictionary,Name,"data",NULL);
    // Set up output sockets
    if(sdr->data_dest == NULL){
      // Construct from serial number
      // Technically creates a memory leak since we never free it, but it's only once per run
      char *cp;
      int ret = asprintf(&cp,"airspy-%016llx-pcm.local",(long long unsigned)sdr->SN);
      if(ret == -1)
	exit(1);
      sdr->data_dest = cp;
    }
    sdr->metadata_dest = config_getstring(Dictionary,Name,"status",NULL);
    if(sdr->metadata_dest == NULL){
      // Construct from serial number
      // Technically creates a memory leak since we never free it, but it's only once per run
      char *cp;
      int ret = asprintf(&cp,"airspy-%016llx-status.local",(long long unsigned)sdr->SN);
      if(ret == -1)
	exit(1);
      sdr->metadata_dest = cp;
    }
  }
  sdr->calibration = 0;
  // Hardware device settings
  {
    int const hf_agc = config_getboolean(Dictionary,Name,"hf-agc",0); // default off
    airspyhf_set_hf_agc(sdr->device,hf_agc);

    int const agc_thresh = config_getboolean(Dictionary,Name,"agc-thresh",0); // default off
    airspyhf_set_hf_agc_threshold(sdr->device,agc_thresh);

    int const hf_att = config_getboolean(Dictionary,Name,"hf-att",0); // default off
    airspyhf_set_hf_att(sdr->device,hf_att);

    int const hf_lna = config_getboolean(Dictionary,Name,"hf-lna",0); // default off
    airspyhf_set_hf_lna(sdr->device,hf_lna);

    int const lib_dsp = config_getboolean(Dictionary,Name,"lib-dsp",1); // default on
    airspyhf_set_lib_dsp(sdr->device,lib_dsp);

    fprintf(stdout,"HF AGC %d, AGC thresh %d, hf att %d, hf-lna %d, lib-dsp %d\n",
	    hf_agc,agc_thresh,hf_att,hf_lna,lib_dsp);
  }
  // When the IP TTL is 0, we're not limited by the Ethernet hardware MTU so select a much larger packet size
  // unless one has been set explicitly
  // 32768 is 1/3 of the buffer returned by the airspy library callback function
  // IPv4 packets are limited to 64KB, IPv6 can go larger with the Jumbo Payload option

  RTP_ttl = config_getint(Dictionary,Name,"data-ttl",0); // Default to TTL=0
  Status_ttl = config_getint(Dictionary,Name,"status-ttl",1); // Default 1 for status; much lower bandwidth
  {
    int x = config_getint(Dictionary,Name,"blocksize",-1);
    if(x != -1){
      sdr->blocksize = x;
    } else if(RTP_ttl == 0)
      sdr->blocksize = 2048; 
    else
      sdr->blocksize = 960;
  }
  sdr->description = config_getstring(Dictionary,Name,"description",NULL);
  {
    time_t tt;
    time(&tt);
    sdr->rtp.ssrc = config_getint(Dictionary,Name,"ssrc",tt);
  }
  // Default is AF12 left shifted 2 bits
  IP_tos = config_getint(Dictionary,Name,"tos",48);

  fprintf(stdout,"Status TTL %d, Data TTL %d, blocksize %'d samples, %'lu bytes\n",
	  Status_ttl,RTP_ttl,sdr->blocksize,(long unsigned)(sdr->blocksize * sizeof(complex float)));

  {
    // Start Avahi client that will maintain our mDNS registrations
    // Service name, if present, must be unique
    // Description, if present becomes TXT record if present
    avahi_start(sdr->description,"_ka9q-ctl._udp",DEFAULT_STAT_PORT,sdr->metadata_dest,ElfHashString(sdr->metadata_dest),sdr->description);
    avahi_start(sdr->description,"_rtp._udp",DEFAULT_RTP_PORT,sdr->data_dest,ElfHashString(sdr->data_dest),sdr->description);
  }
  {
    char iface[IFNAMSIZ];
    resolve_mcast(sdr->data_dest,&sdr->output_data_dest_address,DEFAULT_RTP_PORT,iface,sizeof(iface));
    sdr->data_sock = connect_mcast(&sdr->output_data_dest_address,iface,RTP_ttl,IP_tos);
    if(sdr->data_sock == -1){
      fprintf(stderr,"Can't create multicast socket to %s: %s\n",sdr->data_dest,strerror(errno));
      exit(1);
    }
    socklen_t len = sizeof(sdr->output_data_source_address);
    getsockname(sdr->data_sock,(struct sockaddr *)&sdr->output_data_source_address,&len);
    
    resolve_mcast(sdr->metadata_dest,&sdr->output_metadata_dest_address,DEFAULT_STAT_PORT,iface,sizeof(iface));
    sdr->status_sock = connect_mcast(&sdr->output_metadata_dest_address,iface,Status_ttl,IP_tos);
    if(sdr->status_sock <= 0){
      fprintf(stderr,"Can't create multicast status socket to %s: %s\n",sdr->metadata_dest,strerror(errno));
      exit(1);
    }
    
    // Set up new control socket on port 5006
    sdr->nctl_sock = listen_mcast(&sdr->output_metadata_dest_address,iface);
    if(sdr->nctl_sock <= 0){
      fprintf(stderr,"Can't create multicast command socket from %s: %s\n",sdr->metadata_dest,strerror(errno));
      exit(1);
    }
  }
  
  init_frequency = config_getdouble(Dictionary,Name,"frequency",0);
  if(init_frequency != 0)
    sdr->frequency_lock = 1;

  ret = asprintf(&sdr->frequency_file,"%s/tune-airspy.%llx",VARDIR,(unsigned long long)sdr->SN);
  if(ret == -1)
    exit(1);
  if(init_frequency == 0){
    // If not set on command line, load saved frequency
    FILE *fp = fopen(sdr->frequency_file,"r+");
    if(fp == NULL)
      fprintf(stderr,"Can't open tuner state file %s: %s\n",sdr->frequency_file,strerror(errno));
    else {
      fprintf(stderr,"Using tuner state file %s\n",sdr->frequency_file);
      int r;
      if((r = fscanf(fp,"%lf",&init_frequency)) < 0)
	fprintf(stderr,"Can't read stored freq. r = %'d: %s\n",r,strerror(errno));
      fclose(fp);
    }
  }
  if(init_frequency == 0){
    // Not set on command line, and not read from file. Use fallback to cover 2m
    init_frequency = 10e6; // Fallback default
    fprintf(stderr,"Fallback default frequency %'.3lf Hz\n",init_frequency);
  }
  fprintf(stderr,"Setting initial frequency %'.3lf Hz, %s\n",init_frequency,sdr->frequency_lock ? "locked" : "not locked");
  set_correct_freq(sdr,init_frequency);

  signal(SIGPIPE,SIG_IGN);
  signal(SIGINT,closedown);
  signal(SIGKILL,closedown);
  signal(SIGQUIT,closedown);
  signal(SIGTERM,closedown);        
  
  if(sdr->status)
    pthread_create(&sdr->display_thread,NULL,display,sdr);

  pthread_create(&sdr->ncmd_thread,NULL,ncmd,sdr);
  ret = airspyhf_start(sdr->device,rx_callback,sdr);
  assert(ret == AIRSPYHF_SUCCESS);

  send_airspyhf_status(sdr,1); // Tell the world we're alive
  
  // Periodically poll status to ensure device hasn't reset
  while(1){
    sleep(1);
    if(!airspyhf_is_streaming(sdr->device))
      break; // Device seems to have bombed. Exit and let systemd restart us
  }
  fprintf(stderr,"Device is no longer streaming, exiting\n");
  exit(1); // Closedown has been hanging, so just exit
#if 0
  close(sdr->data_sock);
  closedown(0);
#endif
}

// Thread to send metadata and process commands
void *ncmd(void *arg){

  // Send status, process commands
  pthread_setname("aspyhf-cmd");
  assert(arg != NULL);
  struct sdrstate * const sdr = arg;
  if(sdr->status_sock == -1 || sdr->nctl_sock == -1) 
    return NULL; // Nothing to do

  while(1){
    unsigned char buffer[Bufsize];
    int const length = recv(sdr->nctl_sock,buffer,sizeof(buffer),0);
    if(length > 0){
      // Parse entries
      if(buffer[0] == 0)
	continue; // Ignore our own status messages

      sdr->commands++;
      decode_airspyhf_commands(sdr,buffer+1,length-1);
      send_airspyhf_status(sdr,1);
    }
  }
}

// Status display thread
void *display(void *arg){
  assert(arg != NULL);
  struct sdrstate *sdr = (struct sdrstate *)arg;

  pthread_setname("aspyhf-disp");

  fprintf(sdr->status,"Frequency     Output     clips\n");

  off_t stat_point = ftello(sdr->status);
  // End lines with return when writing to terminal, newlines when writing to status file
  char const  eol = stat_point == -1 ? '\r' : '\n';
  while(1){
    float powerdB = power2dB(sdr->power);

    if(stat_point != -1)
      fseeko(sdr->status,stat_point,SEEK_SET);
    
    fprintf(sdr->status,"%'-14.0lf%'7.1f%'10d    %c",
	    sdr->frequency,
	    powerdB,
	    sdr->clips,
	    eol);
    fflush(sdr->status);
    usleep(100000); // 10 Hz
  }
  return NULL;
}

void decode_airspyhf_commands(struct sdrstate *sdr,unsigned char *buffer,int length){
  unsigned char *cp = buffer;

  while(cp - buffer < length){
    int ret __attribute__((unused)); // Won't be used when asserts are disabled
    enum status_type const type = *cp++; // increment cp to length field
    
    if(type == EOL)
      break; // End of list
    
    unsigned int const optlen = *cp++;
    if(cp - buffer + optlen >= length)
      break; // Invalid length
    
    switch(type){
    case EOL: // Shouldn't get here
      break;
    case COMMAND_TAG:
      sdr->command_tag = decode_int(cp,optlen);
      break;
    case CALIBRATE:
      sdr->calibration = decode_double(cp,optlen);
      break;
    case RADIO_FREQUENCY:
      if(!sdr->frequency_lock){
	double const f = decode_double(cp,optlen);
	set_correct_freq(sdr,f);
      }
      break;
    default: // Ignore all others
      break;
    }
    cp += optlen;
  }    
}  

void send_airspyhf_status(struct sdrstate *sdr,int full){
  unsigned char packet[2048];
  unsigned char *bp = packet;
  
  sdr->output_metadata_packets++;

  *bp++ = 0;   // Command/response = response
  
  encode_int32(&bp,COMMAND_TAG,sdr->command_tag);
  encode_int64(&bp,CMD_CNT,sdr->commands);
  
  long long timestamp = gps_time_ns();
  encode_int64(&bp,GPS_TIME,timestamp);

  if(sdr->description)
    encode_string(&bp,DESCRIPTION,sdr->description,strlen(sdr->description));

  // Source address we're using to send data
  encode_socket(&bp,OUTPUT_DATA_SOURCE_SOCKET,&sdr->output_data_source_address);
  // Where we're sending output
  encode_socket(&bp,OUTPUT_DATA_DEST_SOCKET,&sdr->output_data_dest_address);
  encode_int32(&bp,OUTPUT_SSRC,sdr->rtp.ssrc);
  encode_byte(&bp,OUTPUT_TTL,RTP_ttl);
  encode_int32(&bp,INPUT_SAMPRATE,sdr->samprate);
  encode_int64(&bp,OUTPUT_DATA_PACKETS,sdr->rtp.packets);
  encode_int64(&bp,OUTPUT_METADATA_PACKETS,sdr->output_metadata_packets);

  // Front end
  encode_double(&bp,CALIBRATE,sdr->calibration);
  encode_byte(&bp,GAIN,0);
  
  // Tuning
  encode_double(&bp,RADIO_FREQUENCY,sdr->frequency);
  encode_int32(&bp,LOCK,sdr->frequency_lock);

  encode_byte(&bp,DEMOD_TYPE,0); // actually LINEAR_MODE
  encode_int32(&bp,OUTPUT_SAMPRATE,sdr->samprate);
  encode_int32(&bp,OUTPUT_CHANNELS,2);
  encode_float(&bp,HIGH_EDGE,+0.43 * sdr->samprate); // empirical for airspyhf
  encode_float(&bp,LOW_EDGE,-0.43 * sdr->samprate); // Should look at the actual filter curves

  encode_eol(&bp);
  int const len = bp - packet;
  assert(len < sizeof(packet));
  send(sdr->status_sock,packet,len,0);
}


// Callback called with incoming receiver data from A/D
int ThreadnameSet;
int rx_callback(airspyhf_transfer_t *transfer){
  if(!ThreadnameSet){
    pthread_setname("aspyhf-cb");
    ThreadnameSet = 1;
  }
  struct sdrstate * const sdr = (struct sdrstate *)transfer->ctx;
  if(transfer->dropped_samples){
    fprintf(stdout,"dropped %'lld\n",(long long)transfer->dropped_samples);
    sdr->rtp.timestamp += transfer->dropped_samples; // Let the radio program know
  }
  struct rtp_header rtp;
  memset(&rtp,0,sizeof(rtp));
  rtp.version = RTP_VERS;
  rtp.type = sdr->rtp_type;
  rtp.ssrc = sdr->rtp.ssrc;

  uint8_t buffer[128]; // larger than biggest possible RTP header

  // Use scatter-gather to avoid copying data
  // Unchanging fields are set here to move them out of the loop
  struct iovec iov[2];
  iov[0].iov_base = buffer;

  struct msghdr msg;
  memset(&msg,0,sizeof(msg));
  msg.msg_iov = iov;
  msg.msg_iovlen = 2;

  int samples = transfer->sample_count;
  complex float *idp = (complex float *)transfer->samples;

  while(samples > 0){
    int chunk = min(samples,sdr->blocksize);

    rtp.seq = sdr->rtp.seq++;
    rtp.timestamp = sdr->rtp.timestamp;
    uint8_t * const dp = hton_rtp(buffer,&rtp);
    
    iov[0].iov_len = dp - buffer; // length of RTP header
    iov[1].iov_base = idp;
    iov[1].iov_len = chunk * sizeof(complex float);

    idp += chunk;

    if(sendmsg(sdr->data_sock,&msg,0) == -1){
      fprintf(stderr,"send: %s\n",strerror(errno));
      //      usleep(100000); // inject a delay to avoid filling the log
    } else {
      sdr->rtp.packets++;
      sdr->rtp.bytes += iov[0].iov_len + iov[1].iov_len;
    }  
    sdr->rtp.timestamp += chunk; // real-only samples
    samples -= chunk;
  }
  return 0;
}

double true_freq(uint64_t freq_hz){
  return (double)freq_hz; // dummy for now

}

// set the airspy tuner to the requested frequency applying calibration offset,
// 5 MHz offset and true frequency correction model for 820T synthesizer
// the calibration offset is a holdover from the Funcube dongle and doesn't
// really fit the Airspy with its internal factory calibration
// All this really works correctly only with a gpsdo
// Remember, airspy firmware always adds 5 MHz to frequency we give it.

double set_correct_freq(struct sdrstate *sdr,double freq){
  int64_t const intfreq = round(freq / (1 + sdr->calibration));
  int ret __attribute__((unused)) = AIRSPYHF_SUCCESS; // Won't be used when asserts are disabled
  ret = airspyhf_set_freq(sdr->device,intfreq);
  assert(ret == AIRSPYHF_SUCCESS);
  double const tf = true_freq(intfreq);
  sdr->frequency = tf * (1 + sdr->calibration);
  FILE *fp = fopen(sdr->frequency_file,"w");
  if(fp){
    if(fprintf(fp,"%lf\n",sdr->frequency) < 0)
      fprintf(stderr,"Can't write to tuner state file %s: %s\n",sdr->frequency_file,strerror(errno));
    fclose(fp);
    fp = NULL;
  }
  return sdr->frequency;
}

static void closedown(int a){
  fprintf(stderr,"caught signal %d: %s\n",a,strsignal(a));
  airspyhf_close(Device);

  if(a == SIGTERM) // sent by systemd when shutting down. Return success
    exit(0);
  exit(1);
}

