// $Id: airspyd.c,v 1.7 2022/08/07 20:48:01 karn Exp $
// Read from Airspy SDR
// Accept control commands from UDP socket
#undef DEBUG_AGC
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
#include <libairspy/airspy.h>
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

int const Bufsize = 16384;

// Global variables set by config file options
char const *Iface;
char *Locale;
int RTP_ttl;
int Status_ttl;
int IP_tos;
const char *App_path;
int Verbose;
int Software_agc = 1; // default
float Low_threshold;
float High_threshold;

struct airspy_device *Device; // Set for benefit of closedown()

struct sdrstate {
  struct airspy_device *device;    // Opaque pointer

  uint32_t sample_rates[20];
  uint64_t SN; // Serial number
  char const *description;
  int samprate; // True sample rate of single A/D converter

  int antenna_bias; // Bias tee on/off

  // Tuning
  double frequency;
  double converter;   // Upconverter base frequency (usually 120 MHz)
  double calibration; // Frequency error
  int frequency_lock;
  int offset; // 1/4 of sample rate in real mode; 0 in complex mode
  char *frequency_file; // Local file to store frequency in case we restart

  // AGC
  int holdoff;  // Holdoff counter after automatic change to allow settling
  int linearity; // Use linearity gain tables; default is sensitivity
  int gainstep; // Airspy gain table steps (0-21), higher numbers == higher gain
  uint8_t lna_gain;
  uint8_t mixer_gain;
  uint8_t if_gain;

  // Sample statistics
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

static struct option Options[] =
  {
    {"verbose", no_argument, NULL, 'v'},
    {NULL, 0, NULL, 0},
  };
static char const Optstring[] = "v";

// Taken from Airspy library driver source
#define GAIN_COUNT (22)
uint8_t airspy_linearity_vga_gains[GAIN_COUNT] = {     13, 12, 11, 11, 11, 11, 11, 10, 10, 10, 10, 10, 10, 10, 10, 10, 9, 8, 7, 6, 5, 4 };
uint8_t airspy_linearity_mixer_gains[GAIN_COUNT] = {   12, 12, 11,  9,  8,  7,  6,  6,  5,  0,  0,  1,  0,  0,  2,  2, 1, 1, 1, 1, 0, 0 };
uint8_t airspy_linearity_lna_gains[GAIN_COUNT] = {     14, 14, 14, 13, 12, 10,  9,  9,  8,  9,  8,  6,  5,  3,  1,  0, 0, 0, 0, 0, 0, 0 };
uint8_t airspy_sensitivity_vga_gains[GAIN_COUNT] = {   13, 12, 11, 10,  9,  8,  7,  6,  5,  5,  5,  5,  5,  4,  4,  4, 4, 4, 4, 4, 4, 4 };
uint8_t airspy_sensitivity_mixer_gains[GAIN_COUNT] = { 12, 12, 12, 12, 11, 10, 10,  9,  9,  8,  7,  4,  4,  4,  3,  2, 2, 1, 0, 0, 0, 0 };
uint8_t airspy_sensitivity_lna_gains[GAIN_COUNT] = {   14, 14, 14, 14, 14, 14, 14, 14, 14, 13, 12, 12,  9,  9,  8,  7, 6, 5, 3, 2, 1, 0 };


double set_correct_freq(struct sdrstate *sdr,double freq);
void decode_airspy_commands(struct sdrstate *,unsigned char *,int);
void send_airspy_status(struct sdrstate *,int);
int rx_callback(airspy_transfer *transfer);
void *display(void *);
void *ncmd(void *);
double true_freq(uint64_t freq);
static void closedown(int a);
static void set_gain(struct sdrstate *sdr,int gainstep);

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
      printf("environ[%'d]: %s\n",i,environ[i]);
    else
      break;
  }
#endif  


  Locale = getenv("LANG");
  if(Locale == NULL || strlen(Locale) == 0)
    Locale = "en_US.UTF-8";
  setlocale(LC_ALL,Locale);

  setlinebuf(stdout);

  struct sdrstate * const sdr = (struct sdrstate *)calloc(1,sizeof(struct sdrstate));
  sdr->rtp_type = AIRSPY_PACKED;
  double init_frequency = 0;

  int c;
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
      fprintf(stdout,"Unknown argument %c\n",c);
      break;
    }
  }
  if(optind >= argc){
    fprintf(stdout,"Name missing\n");
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
	  continue; // bogus entry?
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
  if((ret = airspy_init()) != AIRSPY_SUCCESS){
    fprintf(stdout,"airspy_init() failed: %s\n",airspy_error_name(ret));
    iniparser_freedict(Dictionary);
    exit(1);
  }
  {
    char const *sn = config_getstring(Dictionary,Name,"serial",NULL);
    if(sn == NULL){
      fprintf(stdout,"'serial' not defined in section %s\n",Name);
      iniparser_freedict(Dictionary);
      exit(1);
    }
    char *endptr = NULL;
    sdr->SN = 0;
    sdr->SN = strtoull(sn,&endptr,16);
    if(endptr == NULL || *endptr != '\0'){
      fprintf(stdout,"Invalid serial number %s in section %s\n",sn,Name);
      iniparser_freedict(Dictionary);
      exit(1);
    }
  }
  // Serial number specified, open that one
  if((ret = airspy_open_sn(&sdr->device,sdr->SN)) != AIRSPY_SUCCESS){
    fprintf(stdout,"airspy_open(%llx) failed: %s\n",(long long)sdr->SN,airspy_error_name(ret));
    iniparser_freedict(Dictionary);
    exit(1);
  }
  {
    airspy_lib_version_t version;
    airspy_lib_version(&version);

    const int VERSION_LOCAL_SIZE = 128; // Library doesn't define, but says should be >= 128
    char hw_version[VERSION_LOCAL_SIZE];
    airspy_version_string_read(sdr->device,hw_version,sizeof(hw_version));

    fprintf(stdout,"Airspy serial %llx, hw version %s, library version %d.%d.%d\n",
	    (long long unsigned)sdr->SN,
	    hw_version,
	    version.major_version,version.minor_version,version.revision);
  }

  // Initialize hardware first
  Device = sdr->device;
  ret = airspy_set_packing(sdr->device,1);
  assert(ret == AIRSPY_SUCCESS);

  // Set this now, as it affects the list of supported sample rates
  ret = airspy_set_sample_type(sdr->device,AIRSPY_SAMPLE_RAW);
  assert(ret == AIRSPY_SUCCESS);

  // Get and list sample rates
  ret = airspy_get_samplerates(sdr->device,sdr->sample_rates,0);
  assert(ret == AIRSPY_SUCCESS);
  int const number_sample_rates = sdr->sample_rates[0];
  fprintf(stdout,"%'d sample rates:",number_sample_rates);
  if(number_sample_rates == 0){
    fprintf(stdout,"error, no valid sample rates!\n");
    exit(1);
  }

  ret = airspy_get_samplerates(sdr->device,sdr->sample_rates,number_sample_rates);
  assert(ret == AIRSPY_SUCCESS);
  for(int n = 0; n < number_sample_rates; n++){
    fprintf(stdout," %'d",sdr->sample_rates[n]);
    if(sdr->sample_rates[n] < 1)
      break;
  }
  fprintf(stdout,"\n");

  // Default to first (highest) sample rate on list
  sdr->samprate = config_getint(Dictionary,Name,"samprate",sdr->sample_rates[0]);
  sdr->offset = sdr->samprate/4;
  sdr->converter = config_getfloat(Dictionary,Name,"converter",0);

  fprintf(stdout,"Set sample rate %'u Hz, offset %'d Hz\n",sdr->samprate,sdr->offset);
  ret = airspy_set_samplerate(sdr->device,(uint32_t)sdr->samprate);
  assert(ret == AIRSPY_SUCCESS);

  sdr->calibration = 0;
  sdr->gainstep = -1; // Force update first time

  // Hardware device settings
  int const lna_agc = config_getboolean(Dictionary,Name,"lna-agc",0); // default off
  airspy_set_lna_agc(sdr->device,lna_agc);
  if(lna_agc)
    Software_agc = 0;

  int const mixer_agc = config_getboolean(Dictionary,Name,"mixer-agc",0); // default off
  airspy_set_mixer_agc(sdr->device,mixer_agc);
  if(mixer_agc)
    Software_agc = 0;
  
  int const lna_gain = config_getint(Dictionary,Name,"lna-gain",-1);
  if(lna_gain != -1){
    sdr->lna_gain = lna_gain;
    airspy_set_lna_gain(sdr->device,lna_gain);
    Software_agc = 0;
  }      
  int const mixer_gain = config_getint(Dictionary,Name,"mixer-gain",-1);
  if(mixer_gain != -1){
    sdr->mixer_gain = mixer_gain;
    airspy_set_mixer_gain(sdr->device,mixer_gain);
    Software_agc = 0;
  }
  int const vga_gain = config_getint(Dictionary,Name,"vga-gain",-1);
  if(vga_gain != -1){
    sdr->if_gain = vga_gain;
    airspy_set_vga_gain(sdr->device,vga_gain);
    Software_agc = 0;
  }
  int gainstep = config_getint(Dictionary,Name,"gainstep",-1);
  if(gainstep >= 0){
    if(gainstep > GAIN_COUNT-1)
      gainstep = GAIN_COUNT-1;
    set_gain(sdr,gainstep); // Start AGC with max gain step
  } else if(Software_agc){
    gainstep = GAIN_COUNT-1;
    set_gain(sdr,gainstep); // Start AGC with max gain step
  }
  sdr->antenna_bias = config_getboolean(Dictionary,Name,"bias",0);
  ret = airspy_set_rf_bias(sdr->device,sdr->antenna_bias);
  assert(ret == AIRSPY_SUCCESS);
  
  fprintf(stdout,"Software AGC %d; LNA AGC %d, Mix AGC %d, LNA gain %d, Mix gain %d, VGA gain %d, gainstep %d, bias tee %d\n",
	  Software_agc,lna_agc,mixer_agc,sdr->lna_gain,sdr->mixer_gain,sdr->if_gain,gainstep,sdr->antenna_bias);

  // When the IP TTL is 0, we're not limited by the Ethernet hardware MTU so select a much larger packet size
  // unless one has been set explicitly
  // 32768 is 1/3 of the buffer returned by the airspy library callback function
  // IPv4 packets are limited to 64KB, IPv6 can go larger with the Jumbo Payload option
  
  RTP_ttl = config_getint(Dictionary,Name,"data-ttl",0); // Default to TTL=0
  Status_ttl = config_getint(Dictionary,Name,"status-ttl",1); // Default 1 for status; much lower bandwidth
  {
    int const x = config_getint(Dictionary,Name,"blocksize",-1);
    if(x != -1){
      sdr->blocksize = x;
    } else if(RTP_ttl == 0)
      sdr->blocksize = 32768; 
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
  sdr->linearity = config_getboolean(Dictionary,Name,"linearity",0);
  {
    float const dh = config_getdouble(Dictionary,Name,"agc-high-threshold",-10.0);
    High_threshold = dB2power(-fabs(dh));
    float const dl = config_getdouble(Dictionary,Name,"agc-low-threshold",-40.0);
    Low_threshold = dB2power(-fabs(dl));
  }
  fprintf(stdout,"Status TTL %d, Data TTL %d, blocksize %'d samples, %'d bytes\n",Status_ttl,RTP_ttl,sdr->blocksize,3 * sdr->blocksize/2);
  sdr->data_dest = config_getstring(Dictionary,Name,"data",NULL);
  // Set up output sockets
  if(sdr->data_dest == NULL){
    // Construct from serial number
    // Technically creates a memory leak since we never free it, but it's only once per run
    char *cp;
    int const ret = asprintf(&cp,"airspy-%016llx-pcm.local",(long long unsigned)sdr->SN);
    if(ret == -1)
      exit(1);
    sdr->data_dest = cp;
  }
  sdr->metadata_dest = config_getstring(Dictionary,Name,"status",NULL);
  if(sdr->metadata_dest == NULL){
    // Construct from serial number
    // Technically creates a memory leak since we never free it, but it's only once per run
    char *cp;
    int const ret = asprintf(&cp,"airspy-%016llx-status.local",(long long unsigned)sdr->SN);
    if(ret == -1)
      exit(1);
    sdr->metadata_dest = cp;
  }
  // Multicast output interface for both data and status
  Iface = config_getstring(Dictionary,Name,"iface",NULL);
  
  {
    // Start Avahi client that will maintain our mDNS registrations
    // Service name, if present, must be unique
    // Description, if present becomes TXT record if present
    avahi_start(sdr->description,"_ka9q-ctl._udp",DEFAULT_STAT_PORT,sdr->metadata_dest,ElfHashString(sdr->metadata_dest),sdr->description);
    avahi_start(sdr->description,"_rtp._udp",DEFAULT_RTP_PORT,sdr->data_dest,ElfHashString(sdr->data_dest),sdr->description);
  }
  {
    // Note: iface as part of address is ignored, and it breaks avahi_start anyway
    char iface[IFNAMSIZ];
    resolve_mcast(sdr->data_dest,&sdr->output_data_dest_address,DEFAULT_RTP_PORT,iface,sizeof(iface));
    sdr->data_sock = connect_mcast(&sdr->output_data_dest_address,Iface,RTP_ttl,IP_tos); // note use of global default Iface
    if(sdr->data_sock == -1){
      fprintf(stdout,"Can't create multicast socket to %s: %s\n",sdr->data_dest,strerror(errno));
      exit(1);
    }
    socklen_t len = sizeof(sdr->output_data_source_address);
    getsockname(sdr->data_sock,(struct sockaddr *)&sdr->output_data_source_address,&len);
    resolve_mcast(sdr->metadata_dest,&sdr->output_metadata_dest_address,DEFAULT_STAT_PORT,iface,sizeof(iface));
    sdr->status_sock = connect_mcast(&sdr->output_metadata_dest_address,Iface,Status_ttl,IP_tos); // note use of global default Iface
    if(sdr->status_sock <= 0){
      fprintf(stdout,"Can't create multicast status socket to %s: %s\n",sdr->metadata_dest,strerror(errno));
      exit(1);
    }
    // Set up new control socket on port 5006
    sdr->nctl_sock = listen_mcast(&sdr->output_metadata_dest_address,Iface); // Note use of global default Iface
    if(sdr->nctl_sock <= 0){
      fprintf(stdout,"Can't create multicast command socket from %s: %s\n",sdr->metadata_dest,strerror(errno));
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
      fprintf(stdout,"Can't open tuner state file %s: %s\n",sdr->frequency_file,strerror(errno));
    else {
      fprintf(stdout,"Using tuner state file %s\n",sdr->frequency_file);
      int r;
      if((r = fscanf(fp,"%lf",&init_frequency)) < 0)
	fprintf(stdout,"Can't read stored freq. r = %'d: %s\n",r,strerror(errno));
      fclose(fp);
    }
  }
  if(init_frequency == 0){
    // Not set in config file or from cache file. Use fallback to cover 2m
    init_frequency = 149e6; // Fallback default
    fprintf(stdout,"Fallback default frequency %'.3lf Hz\n",init_frequency);
  }
  fprintf(stdout,"Setting initial frequency %'.3lf Hz, %s\n",init_frequency,sdr->frequency_lock ? "locked" : "not locked");
  set_correct_freq(sdr,init_frequency);

  signal(SIGPIPE,SIG_IGN);
  signal(SIGINT,closedown);
  signal(SIGKILL,closedown);
  signal(SIGQUIT,closedown);
  signal(SIGTERM,closedown);        
  
  if(sdr->status)
    pthread_create(&sdr->display_thread,NULL,display,sdr);

  pthread_create(&sdr->ncmd_thread,NULL,ncmd,sdr);
  ret = airspy_start_rx(sdr->device,rx_callback,sdr);
  assert(ret == AIRSPY_SUCCESS);
  send_airspy_status(sdr,1); // Tell the world we're alive

  // Periodically poll status to ensure device hasn't reset
  while(1){
    sleep(1);
    if(!airspy_is_streaming(sdr->device))
      break; // Device seems to have bombed. Exit and let systemd restart us
  }
  fprintf(stdout,"Device is no longer streaming, exiting\n");
  close(sdr->data_sock);
  exit(1);
}

// Thread to send metadata and process commands
void *ncmd(void *arg){
  // Send status, process commands
  pthread_setname("airspy-cmd");
  assert(arg != NULL);
  struct sdrstate * const sdr = (struct sdrstate *)arg;
  if(sdr->status_sock == -1 || sdr->nctl_sock == -1) 
    return NULL; // Nothing to do

  while(1){
    unsigned char buffer[Bufsize];
    int const length = recv(sdr->nctl_sock,buffer,sizeof(buffer),0);
    if(length > 0){
      // Parse entries
      if(buffer[0] == 0)
	continue;  // Ignore our own status messages

      sdr->commands++;
      decode_airspy_commands(sdr,buffer+1,length-1);
      send_airspy_status(sdr,1);
    }
  }
}

// Status display thread
void *display(void *arg){
  assert(arg != NULL);
  struct sdrstate *sdr = (struct sdrstate *)arg;

  pthread_setname("airspy-disp");

  fprintf(sdr->status,"               |-----Gains dB-- ---|      |----Levels dB --|           clips\n");
  fprintf(sdr->status,"Frequency      step LNA  mixer bband          RF   A/D   Out\n");
  fprintf(sdr->status,"Hz                                           dBFS  dBFS\n");

  off_t stat_point = ftello(sdr->status);
  // End lines with return when writing to terminal, newlines when writing to status file
  char   eol = stat_point == -1 ? '\r' : '\n';
  while(1){

    if(stat_point != -1)
      fseeko(sdr->status,stat_point,SEEK_SET);
    
    fprintf(sdr->status,"%'-15.0lf%4d%4d%7d%6d%c",
	    sdr->frequency,
	    sdr->gainstep,
	    sdr->lna_gain,	    
	    sdr->mixer_gain,
	    sdr->if_gain,
	    eol);
    fflush(sdr->status);
    usleep(100000); // 10 Hz
  }
  return NULL;
}

void decode_airspy_commands(struct sdrstate *sdr,unsigned char *buffer,int length){
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
    case LNA_GAIN:
      sdr->lna_gain = decode_int(cp,optlen);
      airspy_set_lna_gain(sdr->device,sdr->lna_gain);
      break;
    case MIXER_GAIN:
      sdr->mixer_gain = decode_int(cp,optlen);
      airspy_set_mixer_gain(sdr->device,sdr->mixer_gain);
      break;
    case IF_GAIN:
      sdr->if_gain = decode_int(cp,optlen);
      airspy_set_vga_gain(sdr->device,sdr->if_gain);
      break;
    case GAINSTEP:
      sdr->gainstep = decode_int(cp,optlen);
      break;
    default: // Ignore all others
      break;
    }
    cp += optlen;
  }    
}  

void send_airspy_status(struct sdrstate *sdr,int full){
  sdr->output_metadata_packets++;

  unsigned char packet[2048],*bp;
  bp = packet;
  
  *bp++ = 0;   // Command/response = response
  
  encode_int32(&bp,COMMAND_TAG,sdr->command_tag);
  encode_int64(&bp,CMD_CNT,sdr->commands);
  
  encode_int64(&bp,GPS_TIME,gps_time_ns());

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
  encode_byte(&bp,LNA_GAIN,sdr->lna_gain);
  encode_byte(&bp,MIXER_GAIN,sdr->mixer_gain);
  encode_byte(&bp,IF_GAIN,sdr->if_gain);
  if(sdr->gainstep >= 0)
    encode_byte(&bp,GAINSTEP,sdr->gainstep);
  encode_double(&bp,GAIN,(double)(sdr->lna_gain + sdr->mixer_gain + sdr->if_gain));

  // Tuning
  encode_double(&bp,RADIO_FREQUENCY,sdr->frequency);
  encode_int32(&bp,LOCK,sdr->frequency_lock);

  encode_byte(&bp,DEMOD_TYPE,0); // actually LINEAR_MODE
  encode_int32(&bp,OUTPUT_SAMPRATE,sdr->samprate);
  encode_int32(&bp,OUTPUT_CHANNELS,1);
  encode_int32(&bp,DIRECT_CONVERSION,1);
  // Receiver inverts spectrum, use lower side
  encode_float(&bp,HIGH_EDGE,-600000);
  encode_float(&bp,LOW_EDGE,-0.47 * sdr->samprate); // Should look at the actual filter curves
  encode_int32(&bp,OUTPUT_BITS_PER_SAMPLE,12); // Always

  if(sdr->converter != 0)
    encode_float(&bp,CONVERTER_OFFSET,sdr->converter);

  encode_eol(&bp);
  int len = bp - packet;
  assert(len < sizeof(packet));
  send(sdr->status_sock,packet,len,0);
}


int ThreadnameSet;
// Callback called with incoming receiver data from A/D
int rx_callback(airspy_transfer *transfer){
  if(!ThreadnameSet){
    pthread_setname("aspy-cb");
    ThreadnameSet = 1;
  }
  struct sdrstate * const sdr = (struct sdrstate *)transfer->ctx;
  if(transfer->dropped_samples){
    fprintf(stdout,"dropped %'lld\n",(long long)transfer->dropped_samples);
    sdr->rtp.timestamp += transfer->dropped_samples; // Let 'radio' know to maintain timing
  }
  assert(transfer->sample_type == AIRSPY_SAMPLE_RAW);
  if(Software_agc){
    uint32_t const *up = (uint32_t *)transfer->samples;
    uint64_t in_energy = 0;
    for(int i=0; i<transfer->sample_count; i+= 8){ // assumes multiple of 8
      int s[8];
      s[0] =  *up >> 20;
      s[1] =  *up >> 8;
      s[2] =  *up++ << 4;
      s[2] |= *up >> 28;
      s[3] =  *up >> 16;
      s[4] =  *up >> 4;
      s[5] =  *up++ << 8;
      s[5] |= *up >> 24;
      s[6] =  *up >> 12;
      s[7] =  *up++;
      for(int j=0; j < 8; j++){
	int const x = (s[j] & 0xfff) - 2048; // not actually necessary for s[0]
	in_energy += x * x;
      }
    }
    // Scale by 2 / 2048^2 = 2^-21 for 0 dBFS = full scale sine wave
    float const power = (float)(in_energy >> 21) / transfer->sample_count;
    if(power < Low_threshold && sdr->holdoff == 0){
      if(Verbose)
	printf("Power %.1f dB\n",power2dB(power));
      set_gain(sdr,sdr->gainstep + 1);
      sdr->holdoff = 2; // seems to settle down in 2 blocks
    } else if(power > High_threshold && sdr->holdoff == 0){
      if(Verbose)
	printf("Power %.1f dB\n",power2dB(power));
      set_gain(sdr,sdr->gainstep - 1);
      sdr->holdoff = 2;
    } else if(sdr->holdoff > 0){
      sdr->holdoff--;
      if(Verbose > 1)
	printf("Power %.1f dB\n",power2dB(power));
    }
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
  uint8_t *idp = transfer->samples;

  while(samples > 0){
    int const chunk = min(samples,sdr->blocksize);

    rtp.seq = sdr->rtp.seq++;
    rtp.timestamp = sdr->rtp.timestamp;
    uint8_t * const dp = hton_rtp(buffer,&rtp);
    
    iov[0].iov_len = dp - buffer;
    iov[1].iov_base = idp;
    iov[1].iov_len = (chunk * 3) / 2;

    idp += iov[1].iov_len;
    
    if(sendmsg(sdr->data_sock,&msg,0) == -1){
      fprintf(stdout,"send: %s\n",strerror(errno));
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

// For a requested frequency, give the actual tuning frequency
// Many thanks to Youssef Touil <youssef@airspy.com> who gave me most of it
// This "mostly" works except that the calibration correction inside the unit
// shifts the tuning steps, so the result here can be off one
// Not easy to fix without knowing the calibration parameters.
// Best workaround is a GPSDO, which disables the correction
double true_freq(uint64_t freq_hz){
  uint32_t const VCO_MIN=1770000000u; // 1.77 GHz
  uint32_t const VCO_MAX=(VCO_MIN << 1); // 3.54 GHz
  int const MAX_DIV = 5;

  // Clock divider set to 2 for the best resolution
  uint32_t const pll_ref = 25000000u / 2; // 12.5 MHz
  
  // Find divider to put VCO = f*2^(d+1) in range VCO_MIN to VCO_MAX
  //          MHz             step, Hz
  // 0: 885.0     1770.0      190.735
  // 1: 442.50     885.00      95.367
  // 2: 221.25     442.50      47.684
  // 3: 110.625    221.25      23.842
  // 4:  55.3125   110.625     11.921
  // 5:  27.65625   55.312      5.960
  int8_t div_num;
  for (div_num = 0; div_num <= MAX_DIV; div_num++){
    uint32_t const vco = freq_hz << (div_num + 1);
    if (VCO_MIN <= vco && vco <= VCO_MAX)
      break;
  }
  if(div_num > MAX_DIV)
    return 0; // Frequency out of range
  
  // r = PLL programming bits: Nint in upper 16 bits, Nfract in lower 16 bits
  // Freq steps are pll_ref / 2^(16 + div_num) Hz
  // Note the '+ (pll_ref >> 1)' term simply rounds the division to the nearest integer
  uint32_t const r = ((freq_hz << (div_num + 16)) + (pll_ref >> 1)) / pll_ref;
  
  // This is a puzzle; is it related to spur suppression?
  double const offset = 0.25;
  // Compute true frequency
  return (((double)r + offset) * pll_ref) / (double)(1 << (div_num + 16));
}

// set the airspy tuner to the requested frequency, applying:
// Spyverter converter offset (120 MHz, or 0 if not in use)
// TCXO calibration offset
// Fs/4 = 5 MHz offset (firmware assumes library real->complex conversion, which we don't use)
// Apply 820T synthesizer tuning step model

// the TCXO calibration offset is a holdover from the Funcube dongle and doesn't
// really fit the Airspy with its internal factory calibration
// All this really works correctly only with a gpsdo, forcing the calibration offset to 0

double set_correct_freq(struct sdrstate *sdr,double freq){
  int64_t intfreq = round((freq + sdr->converter)/ (1 + sdr->calibration));
  int ret __attribute__((unused)) = AIRSPY_SUCCESS; // Won't be used when asserts are disabled
  ret = airspy_set_freq(sdr->device,intfreq - sdr->offset);
  assert(ret == AIRSPY_SUCCESS);
  double const tf = true_freq(intfreq);
  sdr->frequency = tf * (1 + sdr->calibration) - sdr->converter;
  FILE *fp = fopen(sdr->frequency_file,"w");
  if(fp){
    if(fprintf(fp,"%lf\n",sdr->frequency) < 0)
      fprintf(stdout,"Can't write to tuner state file %s: %s\n",sdr->frequency_file,strerror(errno));
    fclose(fp);
    fp = NULL;
  }
  return sdr->frequency;
}

static void closedown(int a){
  fprintf(stdout,"caught signal %'d: %s\n",a,strsignal(a));
  airspy_close(Device);
  airspy_exit();

  if(a == SIGTERM) // sent by systemd when shutting down. Return success
    exit(0);
  exit(1);
}

static void set_gain(struct sdrstate *sdr,int gainstep){
  if(gainstep < 0)
    gainstep = 0;
  else if(gainstep >= GAIN_COUNT)
    gainstep = GAIN_COUNT-1;

  if(gainstep != sdr->gainstep){
    sdr->gainstep = gainstep;
    int const tab = GAIN_COUNT - 1 - sdr->gainstep;
    if(sdr->linearity){
      int ret __attribute__((unused)) = AIRSPY_SUCCESS; // Won't be used when asserts are disabled
      ret = airspy_set_linearity_gain(sdr->device,sdr->gainstep);
      assert(ret == AIRSPY_SUCCESS);
      sdr->if_gain = airspy_linearity_vga_gains[tab];
      sdr->mixer_gain = airspy_linearity_mixer_gains[tab];
      sdr->lna_gain = airspy_linearity_lna_gains[tab];
    } else {
      int ret __attribute__((unused)) = AIRSPY_SUCCESS; // Won't be used when asserts are disabled
      ret = airspy_set_sensitivity_gain(sdr->device,sdr->gainstep);
      assert(ret == AIRSPY_SUCCESS);
      sdr->if_gain = airspy_sensitivity_vga_gains[tab];
      sdr->mixer_gain = airspy_sensitivity_mixer_gains[tab];
      sdr->lna_gain = airspy_sensitivity_lna_gains[tab];
    }
    send_airspy_status(sdr,1);
    if(Verbose)
      printf("New gainstep %d: LNA = %d, mixer = %d, vga = %d\n",gainstep,
	     sdr->lna_gain,sdr->mixer_gain,sdr->if_gain);
  }
}


