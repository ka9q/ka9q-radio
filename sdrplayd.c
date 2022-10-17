// Read from SDRplay SDR using SDRplay API version 3.x
// Accept control commands from UDP socket
// Written by K4VZ July 2020, adapted from existing KA9Q SDR handler programs
#define _GNU_SOURCE 1
#include <assert.h>
#include <pthread.h>
#include <string.h>
#if defined(linux)
#include <bsd/string.h>
#endif
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
#include <signal.h>
#include <locale.h>
#include <sys/time.h>
#include <sdrplay_api.h>
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

// Configurable parameters
// decibel limits for power
float const AGC_upper = -20;
float const AGC_lower = -50;

int const Bufsize = 65536; // should pick more deterministically
#define BUFFERSIZE  (1<<21) // Upcalls seem to be 256KB; don't make too big or we may blow out of the cache

// Global variables set by config file options
char const *Iface;
char *Locale;
int RTP_ttl;
int Status_ttl;
int IP_tos;
const char *App_path;
int Verbose;
static int Terminate;

// SDRplay device status
enum sdrplay_status {
  NOT_INITIALIZED = 0,
  SDRPLAY_API_OPEN = 1,
  DEVICE_API_LOCKED = 2,
  DEVICE_SELECTED = 4,
  DEVICE_STREAMING = 8
};

struct sdrstate {
  sdrplay_api_DeviceT device;
  sdrplay_api_DeviceParamsT *device_params;
  sdrplay_api_RxChannelParamsT *rx_channel_params;

  enum sdrplay_status device_status;
  char const *description;

  // Tuning
  int frequency_lock;
  char *frequency_file; // Local file to store frequency in case we restart

  // Sample statistics
  long long sample_count;
  long long event_count;
  int clips;  // Sample clips since last reset (???)
  float power;   // Running estimate of A/D signal power (???)

  unsigned int next_sample_num;

  int blocksize;// Number of samples per packet

  complex short *samples;   // samples buffer

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
    {"config",required_argument,NULL,'f'},
    {NULL, 0, NULL, 0},
  };
static char const Optstring[] = "f:v";


// SDRplay specific constants, data structures, and functions
static const sdrplay_api_DbgLvl_t dbgLvl = sdrplay_api_DbgLvl_Disable;
//static const sdrplay_api_DbgLvl_t dbgLvl = sdrplay_api_DbgLvl_Verbose;

static const double MIN_SAMPLE_RATE = 2e6;
static const double MAX_SAMPLE_RATE = 10.66e6;
static const int MAX_DECIMATION = 32;

// Taken from SDRplay API Specification Guide (Gain Reduction Tables)
uint8_t rsp1_0_420_lna_states[] = { 0, 24, 19, 43 };
uint8_t rsp1_420_1000_lna_states[] = { 0, 7, 19, 26 };
uint8_t rsp1_1000_2000_lna_states[] = { 0, 5, 19, 24 };

uint8_t rsp1a_0_60_lna_states[] = { 0, 6, 12, 18, 37, 42, 61 };
uint8_t rsp1a_60_420_lna_states[] = { 0, 6, 12, 18, 20, 26, 32, 38, 57, 62 };
uint8_t rsp1a_420_1000_lna_states[] = { 0, 7, 13, 19, 20, 27, 33, 39, 45, 64 };
uint8_t rsp1a_1000_2000_lna_states[] = { 0, 6, 12, 20, 26, 32, 38, 43, 62 };

uint8_t rsp2_0_420_lna_states[] = { 0, 10, 15, 21, 24, 34, 39, 45, 64 };
uint8_t rsp2_420_1000_lna_states[] = { 0, 7, 10, 17, 22, 41 };
uint8_t rsp2_1000_2000_lna_states[] = { 0, 5, 21, 15, 15, 34 };
uint8_t rsp2_0_60_hiz_lna_states[] = { 0, 6, 12, 18, 37 };

uint8_t rspduo_0_60_lna_states[] = { 0, 6, 12, 18, 37, 42, 61 };
uint8_t rspduo_60_420_lna_states[] = { 0, 6, 12, 18, 20, 26, 32, 38, 57, 62 };
uint8_t rspduo_420_1000_lna_states[] = { 0, 7, 13, 19, 20, 27, 33, 39, 45, 64 };
uint8_t rspduo_1000_2000_lna_states[] = { 0, 6, 12, 20, 26, 32, 38, 43, 62 };
uint8_t rspduo_0_60_hiz_lna_states[] = { 0, 6, 12, 18, 37 };

uint8_t rspdx_0_2_hdr_lna_states[] = { 0, 3, 6, 9, 12, 15, 18, 21, 24, 25, 27, 30, 33, 36, 39, 42, 45, 48, 51, 54, 57, 60 };
uint8_t rspdx_0_12_lna_states[] = { 0, 3, 6, 9, 12, 15, 24, 27, 30, 33, 36, 39, 42, 45, 48, 51, 54, 57, 60 };
uint8_t rspdx_12_60_lna_states[] = { 0, 3, 6, 9, 12, 15, 18, 24, 27, 30, 33, 36, 39, 42, 45, 48, 51, 54, 57, 60 };
uint8_t rspdx_60_250_lna_states[] = { 0, 3, 6, 9, 12, 15, 24, 27, 30, 33, 36, 39, 42, 45, 48, 51, 54, 57, 60, 63, 66, 69, 72, 75, 78, 81, 84 };
uint8_t rspdx_250_420_lna_states[] = { 0, 3, 6, 9, 12, 15, 18, 24, 27, 30, 33, 36, 39, 42, 45, 48, 51, 54, 57, 60, 63, 66, 69, 72, 75, 78, 81, 84 };
uint8_t rspdx_420_1000_lna_states[] = { 0, 7, 10, 13, 16, 19, 22, 25, 31, 34, 37, 40, 43, 46, 49, 52, 55, 58, 61, 64, 67 };
uint8_t rspdx_1000_2000_lna_states[] = { 0, 5, 8, 11, 14, 17, 20, 32, 35, 38, 41, 44, 47, 50, 53, 56, 59, 62, 65 };

// SDRplay specific functions
static int init_api(struct sdrstate *sdr);
static int find_rsp(struct sdrstate *sdr,char const *sn);
static int set_rspduo_mode(struct sdrstate *sdr,char const *mode,char const *antenna);
static int select_device(struct sdrstate *sdr);
static int set_center_freq(struct sdrstate *sdr,double const frequency);
static int set_ifreq(struct sdrstate *sdr,int const ifreq);
static int set_bandwidth(struct sdrstate *sdr,int const bandwidth,double const samprate);
static int set_samplerate(struct sdrstate *sdr,double const samprate);
static double get_samplerate(struct sdrstate *sdr);
static int set_antenna(struct sdrstate *sdr,char const *antenna);
static int set_rf_gain(struct sdrstate *sdr,int const lna_state,int const rf_att,int const rf_gr,double const frequency);
static int set_if_gain(struct sdrstate *sdr,int const if_att,int const if_gr,int const if_agc,int const if_agc_rate,int const if_agc_setPoint_dBfs,int const if_agc_attack_ms,int const if_agc_decay_ms,int const if_agc_decay_delay_ms,int const if_agc_decay_threshold_dB);
static int set_dc_offset_iq_imbalance_correction(struct sdrstate *sdr,int const dc_offset_corr,int const iq_imbalance_corr);
static int set_bulk_transfer_mode(struct sdrstate *sdr,int const transfer_mode_bulk);
static int set_notch_filters(struct sdrstate *sdr,int const rf_notch,int const dab_notch,int const am_notch);
static int set_biasT(struct sdrstate *sdr,int const biasT);
static int start_streaming(struct sdrstate *sdr);
static void rx_callback(short *xi,short *xq,sdrplay_api_StreamCbParamsT *params,unsigned int numSamples,unsigned int reset,void *cbContext);
static void event_callback(sdrplay_api_EventT eventId,sdrplay_api_TunerSelectT tuner,sdrplay_api_EventParamsT *params,void *cbContext);
static void show_device_params(struct sdrstate *sdr);
static void close_and_exit(struct sdrstate *sdr,int exit_code);
static void set_terminate(int a);

// handle commands, display status, and ka9q-radio related functions
static void decode_sdrplay_commands(struct sdrstate *,unsigned char *,int);
static void send_sdrplay_status(struct sdrstate *,int);
static void *display(void *);
static void *ncmd(void *);

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

  struct sdrstate * const sdr = (struct sdrstate *)calloc(1,sizeof(struct sdrstate));
  sdr->device_status = NOT_INITIALIZED;
  sdr->samples = NULL;
  sdr->rtp_type = IQ_PT;

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
      fprintf(stdout,"Unknown argument %c\n",c);
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
  } else if((Dictionary = iniparser_load("/etc/radio/sdrplayd.conf")) != NULL){
    if(iniparser_find_entry(Dictionary,Name) == 1){
      printf("Using config file /etc/radio/sdrplayd.conf\n");
    } else {
      iniparser_freedict(Dictionary);
      Dictionary = NULL;
    }
  }
  if(Dictionary == NULL){
    // Search everything under /etc/radio/sdrplayd.conf.d
    char const *subdir = "/etc/radio/sdrplayd.conf.d";
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

  if(init_api(sdr) == -1)
    close_and_exit(sdr,1);
  char const *sn = config_getstring(Dictionary,Name,"serial",NULL);
  if(sn == NULL){
    fprintf(stdout,"'serial' not defined in section %s\n",Name);
    close_and_exit(sdr,1);
  }
  // Serial number specified, find that one
  if(find_rsp(sdr,sn) == -1)
    close_and_exit(sdr,1);
  if(sdr->device.hwVer == SDRPLAY_RSPduo_ID){
    char const *mode = config_getstring(Dictionary,Name,"rspduo-mode",NULL);
    char const *antenna = config_getstring(Dictionary,Name,"antenna",NULL);
    if(set_rspduo_mode(sdr,mode,antenna) == -1)
      close_and_exit(sdr,1);
  }
  if(select_device(sdr) == -1)
    close_and_exit(sdr,1);

  int const ifreq = config_getint(Dictionary,Name,"ifreq",-1);
  if(set_ifreq(sdr,ifreq) == -1)
    close_and_exit(sdr,1);

  // Default sample rate to 2Msps
  int const bandwidth = config_getint(Dictionary,Name,"bandwidth",-1);
  double const samprate = config_getdouble(Dictionary,Name,"samprate",MIN_SAMPLE_RATE);
  if(set_bandwidth(sdr,bandwidth,samprate) == -1)
    close_and_exit(sdr,1);

  fprintf(stdout,"Set sample rate %'f Hz\n",samprate);
  if(set_samplerate(sdr,samprate) == -1)
    close_and_exit(sdr,1);

  {
    // Multicast output interface for both data and status
    Iface = config_getstring(Dictionary,Name,"iface",NULL);

    sdr->data_dest = config_getstring(Dictionary,Name,"data",NULL);
    // Set up output sockets
    if(sdr->data_dest == NULL){
      // Construct from serial number
      // Technically creates a memory leak since we never free it, but it's only once per run
      char *cp;
      int ret = asprintf(&cp,"sdrplay-%s-pcm.local",sdr->device.SerNo);
      if(ret == -1)
	close_and_exit(sdr,1);
      sdr->data_dest = cp;
    }
    sdr->metadata_dest = config_getstring(Dictionary,Name,"status",NULL);
    if(sdr->metadata_dest == NULL){
      // Construct from serial number
      // Technically creates a memory leak since we never free it, but it's only once per run
      char *cp;
      int ret = asprintf(&cp,"sdrplay-%s-status.local",sdr->device.SerNo);
      if(ret == -1)
	close_and_exit(sdr,1);
      sdr->metadata_dest = cp;
    }
  }

  // Need to know the initial frequency beforehand because of RF att/LNA state
  init_frequency = config_getdouble(Dictionary,Name,"frequency",0);
  if(init_frequency != 0)
    sdr->frequency_lock = 1;

  if(asprintf(&sdr->frequency_file,"%s/tune-sdrplay.%s",VARDIR,sdr->device.SerNo) == -1)
    close_and_exit(sdr,1);
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
    init_frequency = 149e6; // Fallback default
    fprintf(stderr,"Fallback default frequency %'.3lf Hz\n",init_frequency);
  }

  // Hardware device settings
  {
    char const *antenna = config_getstring(Dictionary,Name,"antenna",NULL);
    if(set_antenna(sdr,antenna) == -1)
      close_and_exit(sdr,1);

    int const lna_state = config_getint(Dictionary,Name,"lna-state",-1);
    int const rf_att = config_getint(Dictionary,Name,"rf-att",-1);
    int const rf_gr = config_getint(Dictionary,Name,"rf-gr",-1);
    if(set_rf_gain(sdr,lna_state,rf_att,rf_gr,init_frequency) == -1)
      close_and_exit(sdr,1);

    int const if_att = config_getint(Dictionary,Name,"if-att",-1);
    int const if_gr = config_getint(Dictionary,Name,"if-gr",-1);
    int const if_agc = config_getboolean(Dictionary,Name,"if-agc",0); // default off
    int const if_agc_rate = config_getint(Dictionary,Name,"if-agc-rate",-1);
    int const if_agc_setPoint_dBfs = config_getint(Dictionary,Name,"if-agc-setpoint-dbfs",-60);
    int const if_agc_attack_ms = config_getint(Dictionary,Name,"if-agc-attack-ms",0);
    int const if_agc_decay_ms = config_getint(Dictionary,Name,"if-agc-decay-ms",0);
    int const if_agc_decay_delay_ms = config_getint(Dictionary,Name,"if-agc-decay-delay-ms",0);
    int const if_agc_decay_threshold_dB = config_getint(Dictionary,Name,"if-agc-decay-threshold-db",0);
    if(set_if_gain(sdr,if_att,if_gr,if_agc,if_agc_rate,if_agc_setPoint_dBfs,if_agc_attack_ms,if_agc_decay_ms,if_agc_decay_delay_ms,if_agc_decay_threshold_dB) == -1)
      close_and_exit(sdr,1);

    fprintf(stdout,"RF LNA state %d, IF att %d, IF AGC %d, IF AGC setPoint %d\n",
            (int)(sdr->rx_channel_params->tunerParams.gain.LNAstate),
            sdr->rx_channel_params->tunerParams.gain.gRdB,
            sdr->rx_channel_params->ctrlParams.agc.enable,
            sdr->rx_channel_params->ctrlParams.agc.setPoint_dBfs);

    int const dc_offset_corr = config_getboolean(Dictionary,Name,"dc-offset-corr",1); // default on
    int const iq_imbalance_corr = config_getboolean(Dictionary,Name,"iq-imbalance-corr",1); // default on
    if(set_dc_offset_iq_imbalance_correction(sdr,dc_offset_corr,iq_imbalance_corr) == -1)
      close_and_exit(sdr,1);

    int const transfer_mode_bulk = config_getboolean(Dictionary,Name,"bulk-transfer-mode",0); // default isochronous
    if(set_bulk_transfer_mode(sdr,transfer_mode_bulk) == -1)
      close_and_exit(sdr,1);

    int const rf_notch = config_getboolean(Dictionary,Name,"rf-notch",0);
    int const dab_notch = config_getboolean(Dictionary,Name,"dab-notch",0);
    int const am_notch = config_getboolean(Dictionary,Name,"am-notch",0);
    if(set_notch_filters(sdr,rf_notch,dab_notch,am_notch) == -1)
      close_and_exit(sdr,1);

    int const biasT = config_getboolean(Dictionary,Name,"bias-t",0);
    if(set_biasT(sdr,biasT) == -1)
      close_and_exit(sdr,1);
  }

  // When the IP TTL is 0, we're not limited by the Ethernet hardware MTU so select a much larger packet size
  // unless one has been set explicitly
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
    char service_name[1024];
    snprintf(service_name,sizeof(service_name),"%s (%s)",sdr->description,sdr->metadata_dest);
    avahi_start(service_name,"_ka9q-ctl._udp",5006,sdr->metadata_dest,ElfHashString(sdr->metadata_dest),sdr->description);
    snprintf(service_name,sizeof(service_name),"%s (%s)",sdr->description,sdr->data_dest);
    avahi_start(service_name,"_rtp._udp",5004,sdr->data_dest,ElfHashString(sdr->data_dest),sdr->description);
  }
  {
    char iface[1024];
    resolve_mcast(sdr->data_dest,&sdr->output_data_dest_address,DEFAULT_RTP_PORT,iface,sizeof(iface));
    if(strlen(iface) == 0 && Iface != NULL)
      strlcpy(iface,Iface,sizeof(iface));
    sdr->data_sock = connect_mcast(&sdr->output_data_dest_address,iface,RTP_ttl,IP_tos);
    if(sdr->data_sock == -1){
      fprintf(stderr,"Can't create multicast socket to %s: %s\n",sdr->data_dest,strerror(errno));
      exit(1);
    }
    socklen_t len = sizeof(sdr->output_data_source_address);
    getsockname(sdr->data_sock,(struct sockaddr *)&sdr->output_data_source_address,&len);

    resolve_mcast(sdr->metadata_dest,&sdr->output_metadata_dest_address,DEFAULT_STAT_PORT,iface,sizeof(iface));
    if(strlen(iface) == 0 && Iface != NULL)
      strlcpy(iface,Iface,sizeof(iface));
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

  fprintf(stderr,"Setting initial frequency %'.3lf Hz, %s\n",init_frequency,sdr->frequency_lock ? "locked" : "not locked");
  set_center_freq(sdr,init_frequency);

  signal(SIGPIPE,SIG_IGN);
  signal(SIGINT,set_terminate);
  signal(SIGKILL,set_terminate);
  signal(SIGQUIT,set_terminate);
  signal(SIGTERM,set_terminate);

  if(sdr->status)
    pthread_create(&sdr->display_thread,NULL,display,sdr);

  pthread_create(&sdr->ncmd_thread,NULL,ncmd,sdr);

  sdr->samples = malloc(sdr->blocksize * sizeof(complex short));
  if(start_streaming(sdr) == -1)
    close_and_exit(sdr,1);

  send_sdrplay_status(sdr,1); // Tell the world we're alive

  // Periodically poll status to ensure device hasn't reset
  long long prev_sample_count = 0;
  while(1){
    sleep(1);
    if(Terminate){
      fprintf(stderr,"Terminating as requsted by user\n");
      close_and_exit(sdr,Terminate-1);
    }
    long long curr_sample_count = sdr->sample_count;
    if(!(curr_sample_count > prev_sample_count))
      break; // Device seems to have bombed. Exit and let systemd restart us
    prev_sample_count = curr_sample_count;
  }

  fprintf(stderr,"Device is no longer streaming, exiting\n");
  close(sdr->data_sock);
  close_and_exit(sdr,0);
}


// Thread to send metadata and process commands
void *ncmd(void *arg){

  // Send status, process commands
  pthread_setname("sdrplay-cmd");
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
      decode_sdrplay_commands(sdr,buffer+1,length-1);
      send_sdrplay_status(sdr,1);
    }
  }
}

// Status display thread
void *display(void *arg){
  assert(arg != NULL);
  struct sdrstate *sdr = (struct sdrstate *)arg;

  pthread_setname("sdrplay-disp");

  fprintf(sdr->status,"Frequency     Output     clips\n");

  off_t stat_point = ftello(sdr->status);
  // End lines with return when writing to terminal, newlines when writing to status file
  char const  eol = stat_point == -1 ? '\r' : '\n';
  while(1){
    float powerdB = power2dB(sdr->power);

    if(stat_point != -1)
      fseeko(sdr->status,stat_point,SEEK_SET);

    fprintf(sdr->status,"%'-14.0lf%'7.1f%'10d    %c",
	    sdr->rx_channel_params->tunerParams.rfFreq.rfHz,
	    powerdB,
	    sdr->clips,
	    eol);
    fflush(sdr->status);
    usleep(100000); // 10 Hz
  }
  return NULL;
}
#if 0
// Status display thread
void *display(void *arg){
  assert(arg != NULL);
  struct sdrstate *sdr = (struct sdrstate *)arg;

  pthread_setname("sdrplay-disp");

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
#endif

void decode_sdrplay_commands(struct sdrstate *sdr,unsigned char *buffer,int length){
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
    case RADIO_FREQUENCY:
      if(!sdr->frequency_lock){
	double const f = decode_double(cp,optlen);
        set_center_freq(sdr,f);
      }
      break;
    case LNA_GAIN:
      {
	int lna_gain = decode_int(cp,optlen);
	if(lna_gain >= 0){   // LNA gain >= 0 -> LNA state
	  set_rf_gain(sdr,lna_gain,-1,-1,sdr->rx_channel_params->tunerParams.rfFreq.rfHz);
	} else {             // LNA gain < 0 -> RF attenuation
	  set_rf_gain(sdr,-1,-lna_gain,-1,sdr->rx_channel_params->tunerParams.rfFreq.rfHz);
	}
      }
      break;
    case IF_GAIN:
      {
	int if_gain = decode_int(cp,optlen);
	if(if_gain == 0){
	  // IF gain == 0 -> enable AGC
	  set_if_gain(sdr,-1,-1,1,-1,-60,0,0,0,0);
	} else if(-if_gain >= sdrplay_api_NORMAL_MIN_GR && -if_gain <= MAX_BB_GR){
	  // IF gain in [-20,-59] -> set IF GR
	  set_if_gain(sdr,-if_gain,-1,0,0,-60,0,0,0,0);
	  
	}
      }
      break;
    default: // Ignore all others
      break;
    }
    cp += optlen;
  }
}

void send_sdrplay_status(struct sdrstate *sdr,int full){
  unsigned char packet[2048];
  unsigned char *bp = packet;

  sdr->output_metadata_packets++;

  *bp++ = 0;   // Command/response = response

  encode_int32(&bp,COMMAND_TAG,sdr->command_tag);
  encode_int64(&bp,CMD_CNT,sdr->commands);
  encode_int64(&bp,GPS_TIME,gps_time_ns());

  if(sdr->description)
    encode_string(&bp,DESCRIPTION,sdr->description,strlen(sdr->description));

  double samprate = get_samplerate(sdr);

  // Source address we're using to send data
  encode_socket(&bp,OUTPUT_DATA_SOURCE_SOCKET,&sdr->output_data_source_address);
  // Where we're sending output
  encode_socket(&bp,OUTPUT_DATA_DEST_SOCKET,&sdr->output_data_dest_address);
  encode_int32(&bp,OUTPUT_SSRC,sdr->rtp.ssrc);
  encode_byte(&bp,OUTPUT_TTL,RTP_ttl);
  encode_int32(&bp,INPUT_SAMPRATE,(int)samprate);
  encode_int64(&bp,OUTPUT_DATA_PACKETS,sdr->rtp.packets);
  encode_int64(&bp,OUTPUT_METADATA_PACKETS,sdr->output_metadata_packets);

  // Front end
  encode_byte(&bp,LNA_GAIN,sdr->rx_channel_params->tunerParams.gain.LNAstate);
  encode_int32(&bp,IF_GAIN,sdr->rx_channel_params->tunerParams.gain.gRdB);
  encode_double(&bp,GAIN,sdr->rx_channel_params->tunerParams.gain.gainVals.curr);

  // Tuning
  encode_double(&bp,RADIO_FREQUENCY,sdr->rx_channel_params->tunerParams.rfFreq.rfHz);
  encode_int32(&bp,LOCK,sdr->frequency_lock);

  encode_byte(&bp,DEMOD_TYPE,0); // actually LINEAR_MODE
  encode_int32(&bp,OUTPUT_SAMPRATE,(int)samprate);
  encode_int32(&bp,OUTPUT_CHANNELS,2);
  double bandwidth = min(1000.0 * sdr->rx_channel_params->tunerParams.bwType, samprate);
  encode_float(&bp,HIGH_EDGE,+0.43 * bandwidth);     // empirical for sdrplay
  encode_float(&bp,LOW_EDGE,-0.43 * bandwidth);     // Should look at the actual filter curves

  encode_eol(&bp);
  int const len = bp - packet;
  assert(len < sizeof(packet));
  send(sdr->status_sock,packet,len,0);
}

// SDRplay specific functions
static int init_api(struct sdrstate *sdr){
  sdrplay_api_ErrT err;
  err = sdrplay_api_Open();
  if(err != sdrplay_api_Success){
    fprintf(stdout,"sdrplay_api_Open() failed: %s\n",sdrplay_api_GetErrorString(err));
    return -1;
  }
  sdr->device_status |= SDRPLAY_API_OPEN;
  float ver;
  err = sdrplay_api_ApiVersion(&ver);
  if(err != sdrplay_api_Success){
    fprintf(stdout,"sdrplay_api_ApiVersion() failed: %s\n",sdrplay_api_GetErrorString(err));
    return -1;
  }
  if(ver != SDRPLAY_API_VERSION){
    fprintf(stdout,"SDRplay API version mismatch: found %.2f, expecting %.2f\n",ver,SDRPLAY_API_VERSION);
    return -1;
  }
  err = sdrplay_api_DebugEnable(NULL,dbgLvl);
  if(err != sdrplay_api_Success){
    fprintf(stdout,"sdrplay_api_DebugEnable() failed: %s\n",sdrplay_api_GetErrorString(err));
    return -1;
  }
  return 0;
}

static int find_rsp(struct sdrstate *sdr,char const *sn){
  sdrplay_api_ErrT err;
  err = sdrplay_api_LockDeviceApi();
  if(err != sdrplay_api_Success){
    fprintf(stdout,"sdrplay_api_LockDeviceApi() failed: %s\n",sdrplay_api_GetErrorString(err));
    return -1;
  }
  sdr->device_status |= DEVICE_API_LOCKED;
  unsigned int ndevices = SDRPLAY_MAX_DEVICES;
  sdrplay_api_DeviceT devices[SDRPLAY_MAX_DEVICES];
  err = sdrplay_api_GetDevices(devices,&ndevices,ndevices);
  if(err != sdrplay_api_Success){
    fprintf(stdout,"sdrplay_api_GetDevices() failed: %s\n",sdrplay_api_GetErrorString(err));
    return -1;
  }

  int found = 0;
  for(int i = 0; i < ndevices; i++){
    if(strcmp(devices[i].SerNo, sn) == 0){
      sdr->device = devices[i];
      found = 1;
      break;
    }
  }
  if(!found){
    fprintf(stdout,"sdrplay device %s not found or unavailable\n",sn);
    return -1;
  }
  return 0;
}

static int set_rspduo_mode(struct sdrstate *sdr,char const *mode,char const *antenna){
  // RSPduo mode
  int valid_mode = 1;
  if(mode == NULL){
    if(sdr->device.rspDuoMode & sdrplay_api_RspDuoMode_Single_Tuner)
      sdr->device.rspDuoMode = sdrplay_api_RspDuoMode_Single_Tuner;
  } else if(strcmp(mode,"single-tuner") == 0 || strcmp(mode,"Single Tuner") == 0){
    if(sdr->device.rspDuoMode & sdrplay_api_RspDuoMode_Single_Tuner)
      sdr->device.rspDuoMode = sdrplay_api_RspDuoMode_Single_Tuner;
    else
      valid_mode = 0;
  } else if(strcmp(mode,"dual-tuner") == 0 || strcmp(mode,"Dual Tuner") == 0){
    if(sdr->device.rspDuoMode & sdrplay_api_RspDuoMode_Dual_Tuner){
      sdr->device.rspDuoMode = sdrplay_api_RspDuoMode_Dual_Tuner;
      sdr->device.rspDuoSampleFreq = 6e6;
    } else
      valid_mode = 0;
  } else if(strcmp(mode,"master") == 0 || strcmp(mode,"Master") == 0){
    if(sdr->device.rspDuoMode & sdrplay_api_RspDuoMode_Master){
      sdr->device.rspDuoMode = sdrplay_api_RspDuoMode_Master;
      sdr->device.rspDuoSampleFreq = 6e6;
    } else
      valid_mode = 0;
  } else if(strcmp(mode,"master-8msps") == 0 || strcmp(mode,"Master (SR=8MHz)") == 0){
    if(sdr->device.rspDuoMode & sdrplay_api_RspDuoMode_Master){
      sdr->device.rspDuoMode = sdrplay_api_RspDuoMode_Master;
      sdr->device.rspDuoSampleFreq = 8e6;
    } else
      valid_mode = 0;
  } else if(strcmp(mode,"slave") == 0 || strcmp(mode,"Slave") == 0){
    if(!(sdr->device.rspDuoMode == sdrplay_api_RspDuoMode_Slave))
      valid_mode = 0;
  } else
    valid_mode = 0;
  if(!valid_mode){
    fprintf(stdout,"sdrplay - RSPduo mode %s is invalid or not available\n",mode);
    return -1;
  }

  // RSPduo tuner
  int valid_tuner = 1;
  if(antenna == NULL){
    if(sdr->device.rspDuoMode == sdrplay_api_RspDuoMode_Single_Tuner ||
       sdr->device.rspDuoMode == sdrplay_api_RspDuoMode_Master)
      sdr->device.tuner = sdrplay_api_Tuner_A;
  } else if(strcmp(antenna,"tuner1-50ohm") == 0 || strcmp(antenna,"Tuner 1 50ohm") == 0 || strcmp(antenna,"high-z") == 0 || strcmp(antenna,"High Z") == 0){
    if(sdr->device.rspDuoMode != sdrplay_api_RspDuoMode_Dual_Tuner && sdr->device.tuner & sdrplay_api_Tuner_A)
      sdr->device.tuner = sdrplay_api_Tuner_A;
    else
      valid_tuner = 0;
  } else if(strcmp(antenna,"tuner2-50ohm") == 0 || strcmp(antenna,"Tuner 2 50ohm") == 0){
    if(sdr->device.rspDuoMode != sdrplay_api_RspDuoMode_Dual_Tuner && sdr->device.tuner & sdrplay_api_Tuner_B)
      sdr->device.tuner = sdrplay_api_Tuner_B;
    else
      valid_tuner = 0;
  } else
    valid_tuner = 0;
  if(!valid_tuner){
    fprintf(stdout,"sdrplay - antenna %s is invalid or not available\n",antenna);
    return -1;
  }

  return 0;
}

static int select_device(struct sdrstate *sdr){
  sdrplay_api_ErrT err;
  err = sdrplay_api_SelectDevice(&sdr->device);
  if(err != sdrplay_api_Success){
    fprintf(stdout,"sdrplay_api_SelectDevice() failed: %s\n",sdrplay_api_GetErrorString(err));
    return -1;
  }
  sdr->device_status |= DEVICE_SELECTED;
  err = sdrplay_api_UnlockDeviceApi();
  sdr->device_status &= ~DEVICE_API_LOCKED;
  if(err != sdrplay_api_Success){
    fprintf(stdout,"sdrplay_api_UnlockDeviceApi() failed: %s\n",sdrplay_api_GetErrorString(err));
    return -1;
  }
  err = sdrplay_api_DebugEnable(sdr->device.dev,dbgLvl);
  if(err != sdrplay_api_Success){
    fprintf(stdout,"sdrplay_api_DebugEnable() failed: %s\n",sdrplay_api_GetErrorString(err));
    return -1;
  }
  err = sdrplay_api_GetDeviceParams(sdr->device.dev,&sdr->device_params);
  if(err != sdrplay_api_Success){
    fprintf(stdout,"sdrplay_api_GetDeviceParams() failed: %s\n",sdrplay_api_GetErrorString(err));
    return -1;
  }
  if(sdr->device.tuner == sdrplay_api_Tuner_A){
    sdr->rx_channel_params = sdr->device_params->rxChannelA;
  } else if(sdr->device.tuner == sdrplay_api_Tuner_B){
    sdr->rx_channel_params = sdr->device_params->rxChannelB;
  } else {
    fprintf(stdout,"sdrplay - invalid tuner: %d\n",sdr->device.tuner);
    return -1;
  }
  return 0;
}

static int set_center_freq(struct sdrstate *sdr,double const frequency){
  sdr->rx_channel_params->tunerParams.rfFreq.rfHz = frequency;
  if(sdr->device_status & DEVICE_STREAMING){
    sdrplay_api_ErrT err;
    err = sdrplay_api_Update(sdr->device.dev,sdr->device.tuner,sdrplay_api_Update_Tuner_Frf,sdrplay_api_Update_Ext1_None);
    if(err != sdrplay_api_Success){
      fprintf(stdout,"sdrplay_api_Update(Tuner_Frf) failed: %s\n",sdrplay_api_GetErrorString(err));
      return -1;
    }
  }
  return 0;
}

static int set_ifreq(struct sdrstate *sdr,int const ifreq){
  int valid_if = 1;
  if(sdr->device.hwVer == SDRPLAY_RSPduo_ID && (sdr->device.rspDuoMode == sdrplay_api_RspDuoMode_Dual_Tuner || sdr->device.rspDuoMode == sdrplay_api_RspDuoMode_Master || sdr->device.rspDuoMode == sdrplay_api_RspDuoMode_Slave)){
    if(sdr->device.rspDuoSampleFreq == 6e6 && (ifreq == -1 || ifreq == 1620))
      sdr->rx_channel_params->tunerParams.ifType = sdrplay_api_IF_1_620;
    else if(sdr->device.rspDuoSampleFreq == 8e6 && (ifreq == -1 || ifreq == 2048))
      sdr->rx_channel_params->tunerParams.ifType = sdrplay_api_IF_2_048;
    else
      valid_if = 0;
  } else {
    if(ifreq == -1 || ifreq == 0){
      sdr->rx_channel_params->tunerParams.ifType = sdrplay_api_IF_Zero;
    } else if(ifreq == 450){
      sdr->rx_channel_params->tunerParams.ifType = sdrplay_api_IF_0_450;
    } else if(ifreq == 1620){
      sdr->rx_channel_params->tunerParams.ifType = sdrplay_api_IF_1_620;
    } else if(ifreq == 2048){
      sdr->rx_channel_params->tunerParams.ifType = sdrplay_api_IF_2_048;
    } else
      valid_if = 0;
  }
  if(!valid_if){
    fprintf(stdout,"sdrplay - IF=%d is invalid\n",ifreq);
    return -1;
  }
  return 0;
}

static int set_bandwidth(struct sdrstate *sdr,int const bandwidth,double const samprate){
  double samprate_kHz = samprate / 1000.0;
  int valid_bandwidth = 1;
  if(bandwidth == sdrplay_api_BW_0_200 || (bandwidth == -1 && samprate_kHz < sdrplay_api_BW_0_300)){
    sdr->rx_channel_params->tunerParams.bwType = sdrplay_api_BW_0_200;
  } else if(bandwidth == sdrplay_api_BW_0_300 || (bandwidth == -1 && samprate_kHz < sdrplay_api_BW_0_600)){
    sdr->rx_channel_params->tunerParams.bwType = sdrplay_api_BW_0_300;
  } else if(bandwidth == sdrplay_api_BW_0_600 || (bandwidth == -1 && samprate_kHz < sdrplay_api_BW_1_536)){
    sdr->rx_channel_params->tunerParams.bwType = sdrplay_api_BW_0_600;
  } else if(bandwidth == sdrplay_api_BW_1_536 || (bandwidth == -1 && samprate_kHz < sdrplay_api_BW_5_000)){
    sdr->rx_channel_params->tunerParams.bwType = sdrplay_api_BW_1_536;
  } else if(bandwidth == sdrplay_api_BW_5_000 || (bandwidth == -1 && samprate_kHz < sdrplay_api_BW_6_000)){
    sdr->rx_channel_params->tunerParams.bwType = sdrplay_api_BW_5_000;
  } else if(bandwidth == sdrplay_api_BW_6_000 || (bandwidth == -1 && samprate_kHz < sdrplay_api_BW_7_000)){
    sdr->rx_channel_params->tunerParams.bwType = sdrplay_api_BW_6_000;
  } else if(bandwidth == sdrplay_api_BW_7_000 || (bandwidth == -1 && samprate_kHz < sdrplay_api_BW_8_000)){
    sdr->rx_channel_params->tunerParams.bwType = sdrplay_api_BW_7_000;
  } else if(bandwidth == sdrplay_api_BW_8_000 || (bandwidth == -1 && samprate_kHz >= sdrplay_api_BW_8_000)){
    sdr->rx_channel_params->tunerParams.bwType = sdrplay_api_BW_8_000;
  } else
    valid_bandwidth = 0;
  if(!valid_bandwidth){
    fprintf(stdout,"sdrplay - Bandwidth=%d is invalid\n",bandwidth);
    return -1;
  }
  return 0;
}

static int set_samplerate(struct sdrstate *sdr,double const samprate){
  // get actual sample rate and decimation
  double actual_sample_rate;
  int decimation;
  for(decimation = 1; decimation <= MAX_DECIMATION; decimation *= 2){
    actual_sample_rate = samprate * decimation;
    if(actual_sample_rate >= MIN_SAMPLE_RATE)
      break;
  }
  if(!(actual_sample_rate >= MIN_SAMPLE_RATE && actual_sample_rate <= MAX_SAMPLE_RATE)){
    fprintf(stdout,"sdrplay - sample_rate=%f is invalid\n",samprate);
    return -1;
  }
  if(sdr->device.hwVer == SDRPLAY_RSPduo_ID && (sdr->device.rspDuoMode == sdrplay_api_RspDuoMode_Dual_Tuner || sdr->device.rspDuoMode == sdrplay_api_RspDuoMode_Master || sdr->device.rspDuoMode == sdrplay_api_RspDuoMode_Slave)){
    if(actual_sample_rate == MIN_SAMPLE_RATE){
      if(sdr->device_params->devParams)
        sdr->device_params->devParams->fsFreq.fsHz = sdr->device.rspDuoSampleFreq;
    } else {
      fprintf(stdout,"sdrplay - sample_rate=%f is invalid\n",samprate);
      return -1;
    }
  } else {
    sdr->device_params->devParams->fsFreq.fsHz = actual_sample_rate;
  }
  if(decimation > 1){
    sdr->rx_channel_params->ctrlParams.decimation.enable = 1;
    sdr->rx_channel_params->ctrlParams.decimation.decimationFactor = decimation;
  } else {
    sdr->rx_channel_params->ctrlParams.decimation.enable = 0;
    sdr->rx_channel_params->ctrlParams.decimation.decimationFactor = 1;
  }

  return 0;
}

static double get_samplerate(struct sdrstate *sdr){
  double samprate = 0.0;
  if(sdr->rx_channel_params->tunerParams.ifType == sdrplay_api_IF_Zero){
    samprate = sdr->device_params->devParams->fsFreq.fsHz;
  } else if(sdr->rx_channel_params->tunerParams.ifType == sdrplay_api_IF_1_620){
    samprate = 2e6;
  } else if(sdr->rx_channel_params->tunerParams.ifType == sdrplay_api_IF_2_048){
    samprate = 2e6;
  }
  if(sdr->rx_channel_params->ctrlParams.decimation.enable)
    samprate /= sdr->rx_channel_params->ctrlParams.decimation.decimationFactor;
  return samprate;
}

static int set_antenna(struct sdrstate *sdr,char const *antenna){
  int valid_antenna = 1;
  if(sdr->device.hwVer == SDRPLAY_RSP2_ID){
    if(antenna == NULL || strcmp(antenna, "antenna-a") == 0 || strcmp(antenna, "Antenna A") == 0){
      sdr->rx_channel_params->rsp2TunerParams.antennaSel = sdrplay_api_Rsp2_ANTENNA_A;
      sdr->rx_channel_params->rsp2TunerParams.amPortSel = sdrplay_api_Rsp2_AMPORT_2;
    } else if(strcmp(antenna, "antenna-b") == 0 || strcmp(antenna, "Antenna B") == 0){
      sdr->rx_channel_params->rsp2TunerParams.antennaSel = sdrplay_api_Rsp2_ANTENNA_B;
      sdr->rx_channel_params->rsp2TunerParams.amPortSel = sdrplay_api_Rsp2_AMPORT_2;
    } else if(strcmp(antenna, "hi-z") == 0 || strcmp(antenna, "Hi-Z") == 0){
      sdr->rx_channel_params->rsp2TunerParams.antennaSel = sdrplay_api_Rsp2_ANTENNA_A;
      sdr->rx_channel_params->rsp2TunerParams.amPortSel = sdrplay_api_Rsp2_AMPORT_1;
    } else
      valid_antenna = 0;
  } else if(sdr->device.hwVer == SDRPLAY_RSPduo_ID){
    if(antenna == NULL){
      sdr->rx_channel_params->rspDuoTunerParams.tuner1AmPortSel = sdrplay_api_RspDuo_AMPORT_2;
    } else if(strcmp(antenna,"tuner1-50ohm") == 0 || strcmp(antenna,"Tuner 1 50ohm") == 0){
      if(sdr->device.tuner & sdrplay_api_Tuner_A)
        sdr->rx_channel_params->rspDuoTunerParams.tuner1AmPortSel = sdrplay_api_RspDuo_AMPORT_2;
      else
        valid_antenna = 0;
    } else if(strcmp(antenna,"tuner2-50ohm") == 0 || strcmp(antenna,"Tuner 2 50ohm") == 0){
      if(sdr->device.tuner & sdrplay_api_Tuner_B)
        sdr->rx_channel_params->rspDuoTunerParams.tuner1AmPortSel = sdrplay_api_RspDuo_AMPORT_2;
      else
        valid_antenna = 0;
    } else if(strcmp(antenna,"high-z") == 0 || strcmp(antenna,"High Z") == 0){
      if(sdr->device.tuner & sdrplay_api_Tuner_A)
        sdr->rx_channel_params->rspDuoTunerParams.tuner1AmPortSel = sdrplay_api_RspDuo_AMPORT_1;
      else
        valid_antenna = 0;
    } else
      valid_antenna = 0;
  } else if(sdr->device.hwVer == SDRPLAY_RSPdx_ID){
    if(antenna == NULL || strcmp(antenna, "antenna-a") == 0 || strcmp(antenna, "Antenna A") == 0){
      sdr->device_params->devParams->rspDxParams.antennaSel = sdrplay_api_RspDx_ANTENNA_A;
    } else if(strcmp(antenna, "antenna-b") == 0 || strcmp(antenna, "Antenna B") == 0){
      sdr->device_params->devParams->rspDxParams.antennaSel = sdrplay_api_RspDx_ANTENNA_B;
    } else if(strcmp(antenna, "antenna-c") == 0 || strcmp(antenna, "Antenna C") == 0){
      sdr->device_params->devParams->rspDxParams.antennaSel = sdrplay_api_RspDx_ANTENNA_C;
    } else
      valid_antenna = 0;
  } else {
    if(antenna != NULL)
      valid_antenna = 0;
  }
  if(!valid_antenna){
    fprintf(stdout,"sdrplay - Antenna=%s is invalid (or not available)\n",antenna);
    return -1;
  }
  return 0;
}

static int set_rf_gain(struct sdrstate *sdr,int const lna_state,int const rf_att,int const rf_gr,double const frequency){
  uint8_t const *lna_states = NULL;
  int lna_state_count = 0;
  if(sdr->device.hwVer == SDRPLAY_RSP1_ID){
    if(frequency < 420e6){
      lna_states = rsp1_0_420_lna_states;
      lna_state_count = sizeof(rsp1_0_420_lna_states) / sizeof(uint8_t);
    } else if(frequency < 1000e6){
      lna_states = rsp1_420_1000_lna_states;
      lna_state_count = sizeof(rsp1_420_1000_lna_states) / sizeof(uint8_t);
    } else {
      lna_states = rsp1_1000_2000_lna_states;
      lna_state_count = sizeof(rsp1_1000_2000_lna_states) / sizeof(uint8_t);
    }
  } else if(sdr->device.hwVer == SDRPLAY_RSP1A_ID){
    if(frequency < 60e6){
      lna_states = rsp1a_0_60_lna_states;
      lna_state_count = sizeof(rsp1a_0_60_lna_states) / sizeof(uint8_t);
    } else if(frequency < 420e6){
      lna_states = rsp1a_60_420_lna_states;
      lna_state_count = sizeof(rsp1a_60_420_lna_states) / sizeof(uint8_t);
    } else if(frequency < 1000e6){
      lna_states = rsp1a_420_1000_lna_states;
      lna_state_count = sizeof(rsp1a_420_1000_lna_states) / sizeof(uint8_t);
    } else {
      lna_states = rsp1a_1000_2000_lna_states;
      lna_state_count = sizeof(rsp1a_1000_2000_lna_states) / sizeof(uint8_t);
    }
  } else if(sdr->device.hwVer == SDRPLAY_RSP2_ID){
    if(frequency < 60e6 && sdr->rx_channel_params->rsp2TunerParams.amPortSel == sdrplay_api_Rsp2_AMPORT_1){
      lna_states = rsp2_0_60_hiz_lna_states;
      lna_state_count = sizeof(rsp2_0_60_hiz_lna_states) / sizeof(uint8_t);
    } else if(frequency < 420e6){
      lna_states = rsp2_0_420_lna_states;
      lna_state_count = sizeof(rsp2_0_420_lna_states) / sizeof(uint8_t);
    } else if(frequency < 1000e6){
      lna_states = rsp2_420_1000_lna_states;
      lna_state_count = sizeof(rsp2_420_1000_lna_states) / sizeof(uint8_t);
    } else {
      lna_states = rsp2_1000_2000_lna_states;
      lna_state_count = sizeof(rsp2_1000_2000_lna_states) / sizeof(uint8_t);
    }
  } else if(sdr->device.hwVer == SDRPLAY_RSPduo_ID){
    if(frequency < 60e6 && sdr->rx_channel_params->rspDuoTunerParams.tuner1AmPortSel == sdrplay_api_RspDuo_AMPORT_1){
      lna_states = rspduo_0_60_hiz_lna_states;
      lna_state_count = sizeof(rspduo_0_60_hiz_lna_states) / sizeof(uint8_t);
    } else if(frequency < 60e6){
      lna_states = rspduo_0_60_lna_states;
      lna_state_count = sizeof(rspduo_0_60_lna_states) / sizeof(uint8_t);
    } else if(frequency < 420e6){
      lna_states = rspduo_60_420_lna_states;
      lna_state_count = sizeof(rspduo_60_420_lna_states) / sizeof(uint8_t);
    } else if(frequency < 1000e6){
      lna_states = rspduo_420_1000_lna_states;
      lna_state_count = sizeof(rspduo_420_1000_lna_states) / sizeof(uint8_t);
    } else {
      lna_states = rspduo_1000_2000_lna_states;
      lna_state_count = sizeof(rspduo_1000_2000_lna_states) / sizeof(uint8_t);
    }
  } else if(sdr->device.hwVer == SDRPLAY_RSPdx_ID){
    if(frequency < 2e6 && sdr->device_params->devParams->rspDxParams.hdrEnable == 1){
      lna_states = rspdx_0_2_hdr_lna_states;
      lna_state_count = sizeof(rspdx_0_2_hdr_lna_states) / sizeof(uint8_t);
    } else if(frequency < 12e6){
      lna_states = rspdx_0_12_lna_states;
      lna_state_count = sizeof(rspdx_0_12_lna_states) / sizeof(uint8_t);
    } else if(frequency < 60e6){
      lna_states = rspdx_12_60_lna_states;
      lna_state_count = sizeof(rspdx_12_60_lna_states) / sizeof(uint8_t);
    } else if(frequency < 250e6){
      lna_states = rspdx_60_250_lna_states;
      lna_state_count = sizeof(rspdx_60_250_lna_states) / sizeof(uint8_t);
    } else if(frequency < 420e6){
      lna_states = rspdx_250_420_lna_states;
      lna_state_count = sizeof(rspdx_250_420_lna_states) / sizeof(uint8_t);
    } else if(frequency < 1000e6){
      lna_states = rspdx_420_1000_lna_states;
      lna_state_count = sizeof(rspdx_420_1000_lna_states) / sizeof(uint8_t);
    } else {
      lna_states = rspdx_1000_2000_lna_states;
      lna_state_count = sizeof(rspdx_1000_2000_lna_states) / sizeof(uint8_t);
    }
  }
  int valid_rf_gain = 1;
  if(lna_state != -1) {
    if(rf_att != -1 || rf_gr != -1){
      fprintf(stdout,"sdrplay - only one of lna-state, rf-att, or rf-gr is allowed\n");
      return -1;
    }
    if(lna_state >= 0 && lna_state < lna_state_count)
      sdr->rx_channel_params->tunerParams.gain.LNAstate = lna_state;
    else
      valid_rf_gain = 0;
  } else {
    if(rf_att != -1 && rf_gr != -1){
      fprintf(stdout,"sdrplay - only one of lna-state, rf-att, or rf-gr is allowed\n");
      return -1;
    }
    int rf_gRdB = rf_att;
    if(rf_gRdB == -1)
      rf_gRdB = rf_gr;
    if(rf_gRdB == -1)
      return 0;
    // find the closest LNA state
    int lna_state_min = -1;
    int delta_min = 1000;
    for(int i = 0; i < lna_state_count; i++){
      int delta = abs(lna_states[i] - rf_gRdB);
      if(delta < delta_min){
        lna_state_min = i;
        delta_min = delta;
      }
    }
    sdr->rx_channel_params->tunerParams.gain.LNAstate = lna_state_min;
  }
  if(!valid_rf_gain){
    fprintf(stdout,"sdrplay - RF gain reduction is invalid - lna_state=%d rf_att=%d rf_gr=%d\n",lna_state,rf_att,rf_gr);
    return -1;
  }

  if(sdr->device_status & DEVICE_STREAMING){
    sdrplay_api_ErrT err;
    err = sdrplay_api_Update(sdr->device.dev,sdr->device.tuner,sdrplay_api_Update_Tuner_Gr,sdrplay_api_Update_Ext1_None);
    if(err != sdrplay_api_Success){
      fprintf(stdout,"sdrplay_api_Update(Tuner_Gr) failed: %s\n",sdrplay_api_GetErrorString(err));
      return -1;
    }
  }
  return 0;
}

static int set_if_gain(struct sdrstate *sdr,int const if_att,int const if_gr,int const if_agc,int const if_agc_rate,int const if_agc_setPoint_dBfs,int const if_agc_attack_ms,int const if_agc_decay_ms,int const if_agc_decay_delay_ms,int const if_agc_decay_threshold_dB){
  if(!if_agc){
    int if_gRdB = if_att;
    if(if_gRdB == -1)
      if_gRdB = if_gr;
    if(if_gRdB != -1){
      if(if_gRdB >= sdrplay_api_NORMAL_MIN_GR && if_gRdB <= MAX_BB_GR){
        sdr->rx_channel_params->ctrlParams.agc.enable = sdrplay_api_AGC_DISABLE;
        sdr->rx_channel_params->tunerParams.gain.gRdB = if_gRdB;
      } else {
        fprintf(stdout,"sdrplay - IF gain reduction is out of range - if_att/if_gr=%d\n",if_gRdB);
        return -1;
      }
    }

    if(sdr->device_status & DEVICE_STREAMING){
      sdrplay_api_ErrT err;
      err = sdrplay_api_Update(sdr->device.dev,sdr->device.tuner,sdrplay_api_Update_Ctrl_Agc | sdrplay_api_Update_Tuner_Gr,sdrplay_api_Update_Ext1_None);
      if(err != sdrplay_api_Success){
        fprintf(stdout,"sdrplay_api_Update(Ctrl_Agc | Tuner_Gr) failed: %s\n",sdrplay_api_GetErrorString(err));
        return -1;
      }
    }
    return 0;
  }

  // AGC
  if(if_gr != -1){
    fprintf(stdout,"sdrplay - cannot select both IF gain reduction (if-gr) and AGC (if-agc)\n");
    return -1;
  }

  if(if_agc_rate == -1){   // default: sdrplay_api_AGC_50HZ
    sdr->rx_channel_params->ctrlParams.agc.enable = sdrplay_api_AGC_50HZ;
  } else if(if_agc_rate == 5){
    sdr->rx_channel_params->ctrlParams.agc.enable = sdrplay_api_AGC_5HZ;
  } else if(if_agc_rate == 50){
    sdr->rx_channel_params->ctrlParams.agc.enable = sdrplay_api_AGC_50HZ;
  } else if(if_agc_rate == 100){
    sdr->rx_channel_params->ctrlParams.agc.enable = sdrplay_api_AGC_100HZ;
  } else if(if_agc_rate == 0){  // use AGC scheme
    sdr->rx_channel_params->ctrlParams.agc.enable = sdrplay_api_AGC_CTRL_EN;
    sdr->rx_channel_params->ctrlParams.agc.setPoint_dBfs = if_agc_setPoint_dBfs;
    sdr->rx_channel_params->ctrlParams.agc.attack_ms = if_agc_attack_ms;
    sdr->rx_channel_params->ctrlParams.agc.decay_ms = if_agc_decay_ms;
    sdr->rx_channel_params->ctrlParams.agc.decay_delay_ms = if_agc_decay_delay_ms;
    sdr->rx_channel_params->ctrlParams.agc.decay_threshold_dB = if_agc_decay_threshold_dB;
  }

  if(sdr->device_status & DEVICE_STREAMING){
    sdrplay_api_ErrT err;
    err = sdrplay_api_Update(sdr->device.dev,sdr->device.tuner,sdrplay_api_Update_Ctrl_Agc | sdrplay_api_Update_Tuner_Gr,sdrplay_api_Update_Ext1_None);
    if(err != sdrplay_api_Success){
      fprintf(stdout,"sdrplay_api_Update(Ctrl_Agc | Tuner_Gr) failed: %s\n",sdrplay_api_GetErrorString(err));
      return -1;
    }
  }

  return 0;
}

static int set_dc_offset_iq_imbalance_correction(struct sdrstate *sdr,int const dc_offset_corr,int const iq_imbalance_corr){
  if(dc_offset_corr){
    sdr->rx_channel_params->ctrlParams.dcOffset.DCenable = 1;
  } else {
    sdr->rx_channel_params->ctrlParams.dcOffset.DCenable = 0;
  }
  if(iq_imbalance_corr){
    sdr->rx_channel_params->ctrlParams.dcOffset.DCenable = 1;
    sdr->rx_channel_params->ctrlParams.dcOffset.IQenable = 1;
  } else {
    sdr->rx_channel_params->ctrlParams.dcOffset.IQenable = 0;
  }

  return 0;
}

static int set_bulk_transfer_mode(struct sdrstate *sdr,int const transfer_mode_bulk){
  if(transfer_mode_bulk)
    sdr->device_params->devParams->mode = sdrplay_api_BULK;
  else
    sdr->device_params->devParams->mode = sdrplay_api_ISOCH;
  return 0;
}

static int set_notch_filters(struct sdrstate *sdr,int const rf_notch,int const dab_notch,int const am_notch){
  if(sdr->device.hwVer == SDRPLAY_RSP1A_ID){
    sdr->device_params->devParams->rsp1aParams.rfNotchEnable = rf_notch ? 1 : 0;
    sdr->device_params->devParams->rsp1aParams.rfDabNotchEnable = dab_notch ? 1 : 0;
  } else if(sdr->device.hwVer == SDRPLAY_RSP2_ID){
    sdr->rx_channel_params->rsp2TunerParams.rfNotchEnable = rf_notch ? 1 : 0;
  } else if(sdr->device.hwVer == SDRPLAY_RSPduo_ID){
    sdr->rx_channel_params->rspDuoTunerParams.rfNotchEnable = rf_notch ? 1 : 0;
    sdr->rx_channel_params->rspDuoTunerParams.rfDabNotchEnable = dab_notch ? 1 : 0;
    if(sdr->device.tuner == sdrplay_api_Tuner_A)
      sdr->rx_channel_params->rspDuoTunerParams.tuner1AmNotchEnable = am_notch ? 1 : 0;
  } else if(sdr->device.hwVer == SDRPLAY_RSPdx_ID){
    sdr->device_params->devParams->rspDxParams.rfNotchEnable = rf_notch ? 1 : 0;
    sdr->device_params->devParams->rspDxParams.rfDabNotchEnable = dab_notch ? 1 : 0;
  }
  return 0;
}

static int set_biasT(struct sdrstate *sdr,int const biasT){
  if(sdr->device.hwVer == SDRPLAY_RSP1A_ID){
    sdr->rx_channel_params->rsp1aTunerParams.biasTEnable = biasT ? 1 : 0;
  } else if(sdr->device.hwVer == SDRPLAY_RSP2_ID){
    sdr->rx_channel_params->rsp2TunerParams.biasTEnable = biasT ? 1 : 0;
  } else if(sdr->device.hwVer == SDRPLAY_RSPduo_ID){
    sdr->rx_channel_params->rspDuoTunerParams.biasTEnable = biasT ? 1 : 0;
  } else if(sdr->device.hwVer == SDRPLAY_RSPdx_ID){
    sdr->device_params->devParams->rspDxParams.biasTEnable = biasT ? 1 : 0;
  }
  return 0;
}

static int start_streaming(struct sdrstate *sdr){
  sdrplay_api_ErrT err;
  sdrplay_api_CallbackFnsT callbacks;
  callbacks.StreamACbFn = rx_callback;
  callbacks.StreamBCbFn = NULL;
  callbacks.EventCbFn = event_callback;
  sdr->sample_count = 0L;
  sdr->event_count = 0L;
  if(Verbose)
    show_device_params(sdr);
  err = sdrplay_api_Init(sdr->device.dev,&callbacks,sdr);
  if(err != sdrplay_api_Success){
    fprintf(stdout,"sdrplay_api_Init() failed: %s\n",sdrplay_api_GetErrorString(err));
    return -1;
  }
  sdr->device_status |= DEVICE_STREAMING;
  return 0;
}


// Callback called with incoming receiver data from A/D
static void rx_callback(short *xi,short *xq,sdrplay_api_StreamCbParamsT *params,unsigned int numSamples,unsigned int reset,void *cbContext){
  static int ThreadnameSet;
  if(!ThreadnameSet){
    pthread_setname("sdrplay-cb");
    ThreadnameSet = 1;
  }
  struct sdrstate *sdr = (struct sdrstate *)cbContext;
  sdr->sample_count += numSamples;

  if(sdr->next_sample_num && params->firstSampleNum != sdr->next_sample_num){
    unsigned int dropped_samples;
    if(sdr->next_sample_num < params->firstSampleNum)
      dropped_samples = params->firstSampleNum - sdr->next_sample_num;
    else
      dropped_samples = UINT_MAX - (params->firstSampleNum - sdr->next_sample_num) + 1;
    sdr->next_sample_num = params->firstSampleNum + numSamples;
    fprintf(stdout,"dropped %'d\n",dropped_samples);
    sdr->rtp.timestamp += dropped_samples; // Let the radio program know
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

  int sample_count = numSamples;
  complex short *idp = sdr->samples;
  int idx;
  for(idx = 0; idx < sample_count; idx += 8){
    __real__ idp[idx+0] = xi[idx+0];
    __real__ idp[idx+1] = xi[idx+1];
    __real__ idp[idx+2] = xi[idx+2];
    __real__ idp[idx+3] = xi[idx+3];
    __real__ idp[idx+4] = xi[idx+4];
    __real__ idp[idx+5] = xi[idx+5];
    __real__ idp[idx+6] = xi[idx+6];
    __real__ idp[idx+7] = xi[idx+7];
    __imag__ idp[idx+0] = xq[idx+0];
    __imag__ idp[idx+1] = xq[idx+1];
    __imag__ idp[idx+2] = xq[idx+2];
    __imag__ idp[idx+3] = xq[idx+3];
    __imag__ idp[idx+4] = xq[idx+4];
    __imag__ idp[idx+5] = xq[idx+5];
    __imag__ idp[idx+6] = xq[idx+6];
    __imag__ idp[idx+7] = xq[idx+7];
  }
  if(idx + 4 < sample_count){
    __real__ idp[idx+0] = xi[idx+0];
    __real__ idp[idx+1] = xi[idx+1];
    __real__ idp[idx+2] = xi[idx+2];
    __real__ idp[idx+3] = xi[idx+3];
    __imag__ idp[idx+0] = xq[idx+0];
    __imag__ idp[idx+1] = xq[idx+1];
    __imag__ idp[idx+2] = xq[idx+2];
    __imag__ idp[idx+3] = xq[idx+3];
    idx += 4;
  }
  if(idx + 2 < sample_count){
    __real__ idp[idx+0] = xi[idx+0];
    __real__ idp[idx+1] = xi[idx+1];
    __imag__ idp[idx+0] = xq[idx+0];
    __imag__ idp[idx+1] = xq[idx+1];
    idx += 2;
  }
  if(idx + 1 < sample_count){
    __real__ idp[idx+0] = xi[idx+0];
    __imag__ idp[idx+0] = xq[idx+0];
    idx += 1;
  }

  while(sample_count > 0){
    int chunk = min(sample_count,sdr->blocksize);

    rtp.seq = sdr->rtp.seq++;
    rtp.timestamp = sdr->rtp.timestamp;
    uint8_t * const dp = hton_rtp(buffer,&rtp);

    iov[0].iov_len = dp - buffer; // length of RTP header
    iov[1].iov_base = idp;
    iov[1].iov_len = chunk * sizeof(complex short);

    idp += chunk;

    if(sendmsg(sdr->data_sock,&msg,0) == -1){
      fprintf(stderr,"send: %s\n",strerror(errno));
      //      usleep(100000); // inject a delay to avoid filling the log
    } else {
      sdr->rtp.packets++;
      sdr->rtp.bytes += iov[0].iov_len + iov[1].iov_len;
    }
    sdr->rtp.timestamp += chunk; // real-only samples
    sample_count -= chunk;
  }
  return;
}

static void event_callback(sdrplay_api_EventT eventId,sdrplay_api_TunerSelectT tuner,sdrplay_api_EventParamsT *params,void *cbContext){
  struct sdrstate *sdr = (struct sdrstate *)cbContext;
  // nothing for now
  sdr->event_count++;
  return;
}

static void show_device_params(struct sdrstate *sdr){
  fprintf(stdout,"\n");
  fprintf(stdout,"# Device parameters:\n");
  sdrplay_api_RxChannelParamsT *rx_channels[] = {sdr->device_params->rxChannelA,sdr->device_params->rxChannelB};
  for(int i = 0; i < 2; i++){
    sdrplay_api_RxChannelParamsT* rx_channel = rx_channels[i];
    fprintf(stdout,"RX channel=%s\n",rx_channel == sdr->device_params->rxChannelA ? "A" : (rx_channel == sdr->device_params->rxChannelB ? "B" : "?"));
    if(rx_channel == NULL)
      continue;
    sdrplay_api_TunerParamsT *tunerParams = &rx_channel->tunerParams;
    fprintf(stdout,"    rfHz=%lf\n",tunerParams->rfFreq.rfHz);
    fprintf(stdout,"    bwType=%d\n",tunerParams->bwType);
    fprintf(stdout,"    ifType=%d\n",tunerParams->ifType);
    sdrplay_api_DecimationT *decimation = &rx_channel->ctrlParams.decimation;
    fprintf(stdout,"    decimationFactor=%d\n",(int)(decimation->decimationFactor));
    fprintf(stdout,"    decimation.enable=%d\n",(int)(decimation->enable));
    fprintf(stdout,"    gain.gRdB=%d\n",tunerParams->gain.gRdB);
    fprintf(stdout,"    gain.LNAstate=%d\n",(int)(tunerParams->gain.LNAstate));
    sdrplay_api_AgcT *agc = &rx_channel->ctrlParams.agc;
    fprintf(stdout,"    agc.enable=%d\n",agc->enable);
    fprintf(stdout,"    agc.setPoint_dBfs=%d\n",agc->setPoint_dBfs);
    fprintf(stdout,"    agc.attack_ms=%hd\n",agc->attack_ms);
    fprintf(stdout,"    agc.decay_ms=%hd\n",agc->decay_ms);
    fprintf(stdout,"    agc.decay_delay_ms=%hd\n",agc->decay_delay_ms);
    fprintf(stdout,"    agc.decay_threashold_dB=%hd\n",agc->decay_threshold_dB);
    fprintf(stdout,"    agc.syncUpdate=%d\n",agc->syncUpdate);
    fprintf(stdout,"    dcOffset.DCenable=%d\n",(int)(rx_channel->ctrlParams.dcOffset.DCenable));
    fprintf(stdout,"    dcOffsetTuner.dcCale=%d\n",(int)(tunerParams->dcOffsetTuner.dcCal));
    fprintf(stdout,"    dcOffsetTuner.speedUp=%d\n",(int)(tunerParams->dcOffsetTuner.speedUp));
    fprintf(stdout,"    dcOffsetTuner.trackTime=%d\n",(int)(tunerParams->dcOffsetTuner.trackTime));
    fprintf(stdout,"    dcOffset.IQenable=%d\n",(int)(rx_channel->ctrlParams.dcOffset.IQenable));
  }
  fprintf(stdout,"\n");
  if (sdr->device_params->devParams) {
    fprintf(stdout,"fsHz=%lf\n",sdr->device_params->devParams->fsFreq.fsHz);
    fprintf(stdout,"ppm=%lf\n",sdr->device_params->devParams->ppm);
  }
  fprintf(stdout,"\n");
  if(sdr->device.hwVer == SDRPLAY_RSP2_ID){
    fprintf(stdout,"antennaSel=%d\n",sdr->rx_channel_params->rsp2TunerParams.antennaSel);
    fprintf(stdout,"amPortSel=%d\n",sdr->rx_channel_params->rsp2TunerParams.amPortSel);
    fprintf(stdout,"\n");
  } else if(sdr->device.hwVer == SDRPLAY_RSPduo_ID){
    fprintf(stdout,"tuner=%d\n",sdr->device.tuner);
    fprintf(stdout,"tuner1AmPortSel=%d\n",sdr->rx_channel_params->rspDuoTunerParams.tuner1AmPortSel);
    fprintf(stdout,"\n");
  } else if(sdr->device.hwVer == SDRPLAY_RSPdx_ID){
    fprintf(stdout,"antennaSel=%d\n",sdr->device_params->devParams->rspDxParams.antennaSel);
    fprintf(stdout,"\n");
  }
  fprintf(stdout,"transferMode=%d\n",sdr->device_params->devParams->mode);
  fprintf(stdout,"\n");
  return;
}

static void close_and_exit(struct sdrstate *sdr,int exit_code){
  sdrplay_api_ErrT err;
  if(sdr->device_status & DEVICE_STREAMING){
    err = sdrplay_api_Uninit(sdr->device.dev);
    if(err != sdrplay_api_Success){
      fprintf(stdout,"sdrplay_api_Uninit() failed: %s\n",sdrplay_api_GetErrorString(err));
      if(exit_code == 0)
        exit_code = 1;
    }
    sdr->device_status &= ~DEVICE_STREAMING;
    fprintf(stdout,"sdrplay done streaming - samples=%lld - events=%lld\n",sdr->sample_count,sdr->event_count);
    free(sdr->samples);
    sdr->samples = NULL;
  }
  if(sdr->device_status & DEVICE_SELECTED){
    sdrplay_api_LockDeviceApi();
    err = sdrplay_api_ReleaseDevice(&sdr->device);
    if(err != sdrplay_api_Success){
      fprintf(stdout,"sdrplay_api_ReleaseDevice() failed: %s\n",sdrplay_api_GetErrorString(err));
      if(exit_code == 0)
        exit_code = 1;
    }
    sdrplay_api_UnlockDeviceApi();
    sdr->device_status &= ~DEVICE_SELECTED;
  }
  if(sdr->device_status & DEVICE_API_LOCKED){
    err = sdrplay_api_UnlockDeviceApi();
    if(err != sdrplay_api_Success){
      fprintf(stdout,"sdrplay_api_UnlockDeviceApi() failed: %s\n",sdrplay_api_GetErrorString(err));
      if(exit_code == 0)
        exit_code = 1;
    }
    sdr->device_status &= ~DEVICE_API_LOCKED;
  }
  if(sdr->device_status & SDRPLAY_API_OPEN){
    err = sdrplay_api_Close();
    if(err != sdrplay_api_Success){
      fprintf(stdout,"sdrplay_api_Close() failed: %s\n",sdrplay_api_GetErrorString(err));
      if(exit_code == 0)
        exit_code = 1;
    }
    sdr->device_status &= ~SDRPLAY_API_OPEN;
  }
  // iniparser_freedict() causes a core dump - fv
  //iniparser_freedict(Dictionary);
  exit(exit_code);
}

static void set_terminate(int a){
  fprintf(stderr,"caught signal %d: %s\n",a,strsignal(a));
  int exit_code = 1;
  if(a == SIGTERM) // sent by systemd when shutting down. Return success
    exit_code = 0;
  Terminate = exit_code + 1;
}
