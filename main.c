// $Id: main.c,v 1.238 2022/04/11 07:30:16 karn Exp $
// Read samples from multicast stream
// downconvert, filter, demodulate, multicast output
// Copyright 2017-2022, Phil Karn, KA9Q, karn@ka9q.net
#define _GNU_SOURCE 1
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <string.h>
#if defined(linux)
#include <bsd/string.h>
#endif
#include <math.h>
#include <complex.h>
#undef I
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <locale.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <signal.h>
#include <getopt.h>
#include <ctype.h>
#include <arpa/inet.h>
#include <iniparser/iniparser.h>

#include "conf.h"
#include "misc.h"
#include "multicast.h"
#include "radio.h"
#include "filter.h"
#include "status.h"
#include "config.h"

// Config constants & defaults
static char const *Wisdom_file = VARDIR "/wisdom";
char const *Libdir = LIBDIR;

static int const DEFAULT_IP_TOS = 48;
static int const DEFAULT_MCAST_TTL = 1;
static float const DEFAULT_BLOCKTIME = 20.0;
static int const DEFAULT_OVERLAP = 5;
static int const DEFAULT_FFT_THREADS = 1;
static int const DEFAULT_SAMPRATE = 48000;
char const *Modefile = "/usr/local/share/ka9q-radio/modes.conf";

// Command line and environ params
int Verbose;
static char const *Locale = "en_US.UTF-8";
dictionary *Dictionary; // Config file descriptor for iniparser

struct demod *Dynamic_demod; // Prototype for dynamically created demods

// Global parameters, can be changed in init file
struct {
  int samprate;
  char const *data;
  char const *mode;
  float gain;
} Default;

int Mcast_ttl;
int IP_tos; // AF12 left shifted 2 bits
int RTCP_enable;
int SAP_enable;
static int Overlap;
char const *Name;

static struct timeval Starttime;      // System clock at timestamp 0, for RTCP
pthread_t Status_thread;
pthread_t Demod_reaper_thread;
struct sockaddr_storage Metadata_source_address;   // Source of SDR metadata
struct sockaddr_storage Metadata_dest_address;      // Dest of metadata (typically multicast)b
char Metadata_dest_string[_POSIX_HOST_NAME_MAX+20]; // Allow room for :portnum
uint64_t Metadata_packets;
uint32_t Command_tag;
uint64_t Commands;

static void closedown(int);
static int setup_frontend(char const *arg);
static int loadconfig(char const *file);

// The main program sets up the demodulator parameter defaults,
// overwrites them with command-line arguments and/or state file settings,
// initializes the various local oscillators, pthread mutexes and conditions
// sets up multicast I/Q input and PCM audio output
// Sets up the input half of the pre-detection filter
// starts the RTP input and downconverter/filter threads
// sets the initial demodulation mode, which starts the demodulator thread
// catches signals and eventually becomes the user interface/display loop
int main(int argc,char *argv[]){
  // if we have root, up our priority and drop privileges
  int prio = getpriority(PRIO_PROCESS,0);
  prio = setpriority(PRIO_PROCESS,0,prio - 10);

  // Quickly drop root if we have it
  // The sooner we do this, the fewer options there are for abuse
  if(seteuid(getuid()) != 0)
    perror("seteuid");

  setlinebuf(stdout);
  gettimeofday(&Starttime,NULL);

  // Set up program defaults
  // Some can be overridden by command line args
  {
    // The display thread assumes en_US.UTF-8, or anything with a thousands grouping character
    // Otherwise the cursor movements will be wrong
    char const * const cp = getenv("LANG");
    if(cp != NULL)
      Locale = cp;
  }
  setlocale(LC_ALL,Locale); // Set either the hardwired default or the value of $LANG if it exists
  fprintf(stdout,"KA9Q Multichannel SDR\n");
  fprintf(stdout,"Copyright 2018-2022 by Phil Karn, KA9Q; may be used under the terms of the GNU General Public License\n");
#ifndef NDEBUG
  fprintf(stdout,"Assertion checking enabled\n");
#endif

  int c;
  while((c = getopt(argc,argv,"W:N:L:v")) != -1){
    switch(c){
    case 'v':
      Verbose++;
      break;
    case 'W':
      Wisdom_file = optarg;
      break;
    case 'N':
      Name = optarg;
      break;
    case 'L':
      Libdir = optarg;
      break;
    default:
      fprintf(stdout,"Unknown command line option %c\n",c);
      break;
    }
  }

  
  // Graceful signal catch
  signal(SIGPIPE,closedown);
  signal(SIGINT,closedown);
  signal(SIGKILL,closedown);
  signal(SIGQUIT,closedown);
  signal(SIGTERM,closedown);        
  signal(SIGPIPE,SIG_IGN);
  
  char const *configfile;
  if(argc <= optind){
    fprintf(stdout,"Config file missing\n");
    exit(1);
  }
  configfile = argv[optind];
  if(Name == NULL)
    Name = argv[optind];
  
  fprintf(stdout,"Loading config file %s...\n",configfile);
  fflush(stdout);
  int n = loadconfig(argv[optind]);
  fprintf(stdout,"%d total demodulators started\n",n);
  fflush(stdout);

  // all done, but we have to stay alive
  while(1)
    sleep(100);

  exit(0);
}

static int Frontend_started;

static int setup_frontend(char const *arg){
  if(Frontend_started)
    return 0;  // Only do this once
  Frontend.sdr.gain = 1; // In case it's never sent by front end

  fftwf_init_threads();
  fftwf_make_planner_thread_safe();
  int r = fftwf_import_system_wisdom();
  fprintf(stdout,"fftwf_import_system_wisdom() %s\n",r == 1 ? "succeeded" : "failed");
  r = fftwf_import_wisdom_from_filename(Wisdom_file);
  fprintf(stdout,"fftwf_import_wisdom_from_filename(%s) %s\n",Wisdom_file,r == 1 ? "succeeded" : "failed");

  pthread_mutex_init(&Frontend.sdr.status_mutex,NULL);
  pthread_cond_init(&Frontend.sdr.status_cond,NULL);

  Frontend.input.status_fd = -1;

  strlcpy(Frontend.input.metadata_dest_string,arg,sizeof(Frontend.input.metadata_dest_string));
  {
    char iface[1024];
    resolve_mcast(Frontend.input.metadata_dest_string,&Frontend.input.metadata_dest_address,DEFAULT_STAT_PORT,iface,sizeof(iface));
    Frontend.input.status_fd = listen_mcast(&Frontend.input.metadata_dest_address,iface);

    if(Frontend.input.status_fd < 3){
      fprintf(stdout,"%s: Can't set up SDR status socket\n",Frontend.input.metadata_dest_string);
      return -1;
    }
    Frontend.input.ctl_fd = connect_mcast(&Frontend.input.metadata_dest_address,iface,Mcast_ttl,IP_tos);
  }

  if(Frontend.input.ctl_fd < 3){
    fprintf(stdout,"%s: Can't set up SDR control socket\n",Frontend.input.metadata_dest_string);
    return -1;
  }
  {
    char addrtmp[256];
    addrtmp[0] = 0;
    switch(Frontend.input.metadata_dest_address.ss_family){
    case AF_INET:
      {
	struct sockaddr_in *sin = (struct sockaddr_in *)&Frontend.input.metadata_dest_address;
	inet_ntop(AF_INET,&sin->sin_addr,addrtmp,sizeof(addrtmp));
      }
      break;
    case AF_INET6:
      {
	struct sockaddr_in6 *sin = (struct sockaddr_in6 *)&Frontend.input.metadata_dest_address;
	inet_ntop(AF_INET6,&sin->sin6_addr,addrtmp,sizeof(addrtmp));
      }
      break;
    }
    fprintf(stdout,"Front end control stream %s (%s)\n",Frontend.input.metadata_dest_string,addrtmp);
  }    
  // Start status thread - will also listen for SDR commands
  if(Verbose)
    fprintf(stdout,"Starting front end status thread\n");
  pthread_create(&Frontend.status_thread,NULL,sdr_status,&Frontend);

  // We must acquire a status stream before we can proceed further
  pthread_mutex_lock(&Frontend.sdr.status_mutex);
  while(Frontend.sdr.samprate == 0 || Frontend.input.data_dest_address.ss_family == 0)
    pthread_cond_wait(&Frontend.sdr.status_cond,&Frontend.sdr.status_mutex);
  pthread_mutex_unlock(&Frontend.sdr.status_mutex);

  {
    char addrtmp[256];
    addrtmp[0] = 0;
    switch(Frontend.input.data_dest_address.ss_family){
    case AF_INET:
      {
	struct sockaddr_in *sin = (struct sockaddr_in *)&Frontend.input.data_dest_address;
	inet_ntop(AF_INET,&sin->sin_addr,addrtmp,sizeof(addrtmp));
      }
      break;
    case AF_INET6:
      {
	struct sockaddr_in6 *sin = (struct sockaddr_in6 *)&Frontend.input.data_dest_address;
	inet_ntop(AF_INET6,&sin->sin6_addr,addrtmp,sizeof(addrtmp));
      }
      break;
    }
    fprintf(stdout,"Front end data stream %s\n",addrtmp);
  }  
  fprintf(stdout,"Input sample rate %'d Hz, %s; block time %.1f ms, %.1f Hz\n",
	  Frontend.sdr.samprate,Frontend.sdr.isreal ? "real" : "complex",Blocktime,1000./Blocktime);
  fflush(stdout);

  // Input socket for I/Q data from SDR, set from OUTPUT_DEST_SOCKET in SDR metadata
  Frontend.input.data_fd = listen_mcast(&Frontend.input.data_dest_address,NULL);
  if(Frontend.input.data_fd < 3){
    fprintf(stdout,"Can't set up IF input\n");
    return -1;
  }
  // Create input filter now that we know the parameters
  // FFT and filter sizes now computed from specified block duration and sample rate
  // L = input data block size
  // M = filter impulse response duration
  // N = FFT size = L + M - 1
  // Note: no checking that N is an efficient FFT blocksize; choose your parameters wisely
  int L = (long long)llroundf(Frontend.sdr.samprate * Blocktime / 1000); // Blocktime is in milliseconds
  int M = L / (Overlap - 1) + 1;
  Frontend.in = create_filter_input(L,M, Frontend.sdr.isreal ? REAL : COMPLEX);
  if(Frontend.in == NULL){
    fprintf(stdout,"Input filter setup failed\n");
    return -1;
  }

  // Launch procsamp to process incoming samples and execute the forward FFT
  pthread_t procsamp_thread;
  pthread_create(&procsamp_thread,NULL,proc_samples,NULL);

  // Launch thread to estimate noise spectral density N0
  // Is this always necessary? It's not always used
  pthread_t n0_thread;
  pthread_create(&n0_thread,NULL,estimate_n0,NULL);

  Frontend_started++; // Only do this once!!
  return 0;
}

static int loadconfig(char const * const file){
  if(file == NULL || strlen(file) == 0)
    return -1;

  int ndemods = 0;
  int base_address = 0;
  for(int i=0;i<3;i++){
    base_address <<= 8;
    base_address += Name[i];
  }
  Dictionary = iniparser_load(file);
  if(Dictionary == NULL){
    fprintf(stdout,"Can't load config file %s\n",file);
    exit(1);
  }
  char const * const global = "global";
  {
    IP_tos = config_getint(Dictionary,global,"tos",DEFAULT_IP_TOS);
    Mcast_ttl = config_getint(Dictionary,global,"ttl",DEFAULT_MCAST_TTL);
    Blocktime = fabs(config_getdouble(Dictionary,global,"blocktime",DEFAULT_BLOCKTIME));
    Overlap = abs(config_getint(Dictionary,global,"overlap",DEFAULT_OVERLAP));
    Nthreads = config_getint(Dictionary,global,"fft-threads",DEFAULT_FFT_THREADS);
    RTCP_enable = config_getboolean(Dictionary,global,"rtcp",0);
    SAP_enable = config_getboolean(Dictionary,global,"sap",0);
    Default.samprate = config_getint(Dictionary,global,"samprate",DEFAULT_SAMPRATE);
    Modefile = config_getstring(Dictionary,global,"mode-file",Modefile);
    Default.data = config_getstring(Dictionary,global,"data",NULL);
    Default.mode = config_getstring(Dictionary,global,"mode",NULL);
    char const * const input = config_getstring(Dictionary,global,"input",NULL);
    if(input == NULL){
      // Mandatory
      fprintf(stdout,"input not specified in [%s]\n",global);
      exit(1);
    }
    if(setup_frontend(input) == -1){
      fprintf(stdout,"Front end setup of %s failed\n",input);
      exit(1);
    }
    char const * const status = config_getstring(Dictionary,global,"status",NULL); // Status/command thread for all demodulators
    if(status != NULL){
      // Target for status/control stream. Optional.
      strlcpy(Metadata_dest_string,status,sizeof(Metadata_dest_string));
      char service_name[1024];
      snprintf(service_name,sizeof(service_name),"%s radio (%s)",Name,status);
      char description[1024];
      snprintf(description,sizeof(description),"input=%s",input);
      avahi_start(service_name,"_ka9q-ctl._udp",DEFAULT_STAT_PORT,Metadata_dest_string,ElfHashString(Metadata_dest_string),description);
      base_address += 16;
      char iface[1024];
      resolve_mcast(Metadata_dest_string,&Metadata_dest_address,DEFAULT_STAT_PORT,iface,sizeof(iface));
      Status_fd = connect_mcast(&Metadata_dest_address,iface,Mcast_ttl,IP_tos);
      if(Status_fd < 3){
	fprintf(stdout,"Can't send status to %s\n",Metadata_dest_string);
      } else {
	socklen_t len = sizeof(Metadata_source_address);
	getsockname(Status_fd,(struct sockaddr *)&Metadata_source_address,&len);  
	// Same remote socket as status
	Ctl_fd = setup_mcast(NULL,(struct sockaddr *)&Metadata_dest_address,0,Mcast_ttl,IP_tos,2);
	if(Ctl_fd < 3)
	  fprintf(stdout,"can't listen for commands from %s\n",Metadata_dest_string);
      }
    }
  }

  // Process sections other than global
  int const nsect = iniparser_getnsec(Dictionary);

  for(int sect = 0; sect < nsect; sect++){
    char const * const sname = iniparser_getsecname(Dictionary,sect);
    if(strcmp(sname,global) == 0)
      continue; // Already processed above

    fprintf(stdout,"Processing [%s]\n",sname);
    {
      int const d = config_getboolean(Dictionary,sname,"disable",0);
      if(d)
	continue; // section is disabled
    }

    // Structure is created and initialized before being put on list
    struct demod *demod = alloc_demod();
    // Set nonzero defaults
    demod->tp1 = demod->tp2 = NAN;
    demod->output.samprate = Default.samprate;
    
    // load presets from mode table, overwriting/merging with defaults
    char const * const mode = config_getstring(Dictionary,sname,"mode",Default.mode);
    if(mode == NULL || strlen(mode) == 0){
      fprintf(stdout,"'mode =' missing and not set in [%s]\n",global);
      free_demod(&demod);
      continue;
    }
    if(preset_mode(demod,mode) == -1){
      fprintf(stdout,"'mode = %s' invalid\n",mode);
      free_demod(&demod);
      continue;
    }
    
    // Override any defaults
    {
      char const *cp = config_getstring(Dictionary,sname,"samprate",NULL);
      if(cp)
	demod->output.samprate = labs(strtol(cp,NULL,0));
    }
    demod->output.channels = abs(config_getint(Dictionary,sname,"channels",demod->output.channels)); // value loaded from mode table
    if(demod->output.channels < 1 || demod->output.channels > 2){
      fprintf(stdout,"Invalid channel count: %d\n",demod->output.channels);
      free_demod(&demod);
      continue;
    }
    {
      char const *cp = config_getstring(Dictionary,sname,"headroom",NULL);
      if(cp)
	demod->output.headroom = dB2voltage(-fabs(strtof(cp,NULL)));
    }
    demod->tune.shift = config_getdouble(Dictionary,sname,"shift",demod->tune.shift); // value loaded from mode table
    {
      char const *cp = config_getstring(Dictionary,sname,"squelch-open",NULL);
      if(cp)
	demod->squelch_open = dB2power(strtof(cp,NULL));
      
      cp = config_getstring(Dictionary,sname,"squelch-close",NULL);
      if(cp)
	demod->squelch_close = dB2power(strtof(cp,NULL));
      else
	demod->squelch_close = demod->squelch_open * 0.794; // - 1 dB
    }
    {
      char const * const status = config_getstring(Dictionary,sname,"status",NULL);
      if(status){
	fprintf(stdout,"note: 'status =' now set in [global] section only\n");
      }
    }

    char const * const data = config_getstring(Dictionary,sname,"data",Default.data);
    if(data == NULL){
      fprintf(stdout,"'data =' missing and not set in [%s]\n",global);
      free_demod(&demod);
      continue;
    }
    strlcpy(demod->output.data_dest_string,data,sizeof(demod->output.data_dest_string));
    // There can be multiple senders to an output stream, so let avahi suppress the duplicate addresses
    char service_name[1024];
    snprintf(service_name,sizeof(service_name),"%s radio (%s)",sname,data);
    char description[1024];
    snprintf(description,sizeof(description),"pcm-source=%s",formatsock(&Frontend.input.data_dest_address));
    avahi_start(service_name,"_rtp._udp",5004,demod->output.data_dest_string,ElfHashString(demod->output.data_dest_string),description);
    base_address += 16;
    char iface[1024];
    resolve_mcast(demod->output.data_dest_string,&demod->output.data_dest_address,DEFAULT_RTP_PORT,iface,sizeof(iface));
    demod->output.data_fd = connect_mcast(&demod->output.data_dest_address,iface,Mcast_ttl,IP_tos);
    if(demod->output.data_fd < 3){
      fprintf(stdout,"can't set up PCM output to %s\n",demod->output.data_dest_string);
      continue;
    } else {
      socklen_t len = sizeof(demod->output.data_source_address);
      getsockname(demod->output.data_fd,(struct sockaddr *)&demod->output.data_source_address,&len);
    }
    
    if(SAP_enable){
      // Highly experimental, off by default
      char sap_dest[] = "224.2.127.254:9875"; // sap.mcast.net
      demod->output.sap_fd = setup_mcast(sap_dest,NULL,1,Mcast_ttl,IP_tos,0);
      if(demod->output.sap_fd < 3)
	fprintf(stdout,"Can't set up SAP output to %s\n",sap_dest);
      else
	pthread_create(&demod->sap_thread,NULL,sap_send,demod);
    }
     // RTCP Real Time Control Protocol daemon is optional
    if(RTCP_enable){
      demod->output.rtcp_fd = setup_mcast(demod->output.data_dest_string,NULL,1,Mcast_ttl,IP_tos,1); // RTP port number + 1
      if(demod->output.rtcp_fd < 3)
	fprintf(stdout,"can't set up RTCP output to %s\n",demod->output.data_dest_string);
      else
	pthread_create(&demod->rtcp_thread,NULL,rtcp_send,demod);
    }
    {
      const char *cp = config_getstring(Dictionary,sname,"low",NULL);
      if(cp)
	demod->filter.min_IF = strtof(cp,NULL);
    }    
    {
      const char *cp = config_getstring(Dictionary,sname,"high",NULL);
      if(cp)
	demod->filter.max_IF = strtof(cp,NULL);
    }    
    {
      const char *cp = config_getstring(Dictionary,sname,"recover",NULL);
      if(cp)
	demod->linear.recovery_rate = dB2voltage(fabsf(strtof(cp,NULL) * .001f * Blocktime));
    }
    {
      const char *cp = config_getstring(Dictionary,sname,"hang-time",NULL);
      if(cp)
	demod->linear.hangtime = fabsf(strtof(cp,NULL)) / (.001f * Blocktime);  // time in seconds -> time in blocks
    }
    {
      const char *cp = config_getstring(Dictionary,sname,"threshold",NULL);
      if(cp)
	demod->linear.threshold = dB2voltage(-fabsf(strtof(cp,NULL)));
    }    
    {
      const char *cp = config_getstring(Dictionary,sname,"gain",NULL);
      if(cp)
	demod->output.gain = dB2voltage(-fabsf(strtof(cp,NULL)));
    }    
    demod->linear.env = config_getboolean(Dictionary,sname,"envelope",demod->linear.env);
    demod->output.rtp.ssrc = (uint32_t)config_getdouble(Dictionary,sname,"ssrc",0); // Default to 0 to trigger auto gen from freq
    demod->linear.loop_bw = fabs(config_getdouble(Dictionary,sname,"loop-bw",demod->linear.loop_bw));
    demod->linear.pll = config_getboolean(Dictionary,sname,"pll",demod->linear.pll);
    demod->linear.square = config_getboolean(Dictionary,sname,"square",demod->linear.square);
    if(demod->linear.square)
      demod->linear.pll = 1; // Square implies PLL on
    
    // Process frequency/frequencies
    // To work around iniparser's limited line length, we look for multiple keywords
    // "freq", "freq0", "freq1", etc, up to "freq9"
    int nfreq = 0;
    for(int ff = -1; ff < 10; ff++){
      char fname[10];
      if(ff == -1)
	snprintf(fname,sizeof(fname),"freq");
      else
	snprintf(fname,sizeof(fname),"freq%d",ff);

      char const * const frequencies = config_getstring(Dictionary,sname,fname,NULL);
      if(frequencies == NULL)
	break; // no more

      char *freq_list = strdup(frequencies); // Need writeable copy for strtok
      char *saveptr = NULL;
      for(char *tok = strtok_r(freq_list," \t",&saveptr);
	  tok != NULL;
	  tok = strtok_r(NULL," \t",&saveptr)){
	
	double const f = parse_frequency(tok);
	if(f < 0){
	  fprintf(stdout,"can't parse frequency %s\n",tok);
	  continue;
	}
	demod->tune.freq = f;

	// If not explicitly specified, generate SSRC in decimal using frequency in Hz
	if(demod->output.rtp.ssrc == 0){
	  if(f == 0){
	    if(Dynamic_demod){
	      free_demod(&Dynamic_demod);
	    }
	    // Template for dynamically created demods
	    Dynamic_demod = demod;
	    fprintf(stdout,"dynamic demod template created\n");
	  } else {
	    for(char const *cp = tok ; cp != NULL && *cp != '\0' ; cp++){
	      if(isdigit(*cp)){
		demod->output.rtp.ssrc *= 10;
		demod->output.rtp.ssrc += *cp - '0';
	      }
	    }
	  }
	}
	int const blocksize = demod->output.samprate * Blocktime / 1000;
	demod->filter.out = create_filter_output(Frontend.in,NULL,blocksize,COMPLEX);
	if(demod->filter.out == NULL){
	  fprintf(stdout,"unable to create filter for f = %'.3lf Hz\n",f);
	  free_demod(&demod);
	  break;
	}
	set_filter(demod->filter.out,
		   demod->filter.min_IF/demod->output.samprate,
		   demod->filter.max_IF/demod->output.samprate,
		   demod->filter.kaiser_beta);
	
	set_freq(demod,demod->tune.freq);
	
	// Initialization all done, start it up
	start_demod(demod);
	nfreq++;
	ndemods++;
	if(Verbose)
	  fprintf(stdout,"started %'.3lf Hz\n",demod->tune.freq);

	// Set up for next demod
	struct demod *ndemod = alloc_demod();
	if(ndemod == NULL){
	  fprintf(stdout,"alloc_demod() failed, quitting\n");
	  break;
	}
	// Copy everything to next demod except filter, demod thread ID, freq and ssrc
	memcpy(ndemod,demod,sizeof(*ndemod));
	ndemod->filter.out = NULL;
	ndemod->demod_thread = (pthread_t)0;
	ndemod->tune.freq = 0;
	ndemod->output.rtp.ssrc = 0;
	demod = ndemod;
	ndemod = NULL;
      }
      free(freq_list);
      freq_list = NULL;
    }
    free_demod(&demod); // last one wasn't needed
    fprintf(stdout,"%d demodulators started\n",nfreq);
  }
  // Start the status thread after all the receivers have been created so it doesn't contend for the demod list lock
  if(Ctl_fd >= 3 && Status_fd >= 3){
    pthread_create(&Status_thread,NULL,radio_status,NULL);
    pthread_create(&Demod_reaper_thread,NULL,demod_reaper,NULL);
  }
  iniparser_freedict(Dictionary);
  Dictionary = NULL;
  return ndemods;
}

// RTP control protocol sender task
void *rtcp_send(void *arg){
  struct demod const *demod = (struct demod *)arg;
  if(demod == NULL)
    pthread_exit(NULL);

  {
    char name[100];
    snprintf(name,sizeof(name),"rtcp %u",demod->output.rtp.ssrc);
    pthread_setname(name);
  }

  while(1){

    if(demod->output.rtp.ssrc == 0) // Wait until it's set by output RTP subsystem
      goto done;
    unsigned char buffer[4096]; // much larger than necessary
    memset(buffer,0,sizeof(buffer));
    
    // Construct sender report
    struct rtcp_sr sr;
    memset(&sr,0,sizeof(sr));
    sr.ssrc = demod->output.rtp.ssrc;

    // Construct NTP timestamp
    struct timeval tv;
    gettimeofday(&tv,NULL);
    double runtime = (tv.tv_sec - Starttime.tv_sec) + (tv.tv_usec - Starttime.tv_usec)/1000000.;

    long long now_time = ((long long)tv.tv_sec + NTP_EPOCH)<< 32;
    now_time += ((long long)tv.tv_usec << 32) / 1000000;

    sr.ntp_timestamp = now_time;
    // The zero is to remind me that I start timestamps at zero, but they could start anywhere
    sr.rtp_timestamp = 0 + runtime * demod->output.samprate;
    sr.packet_count = demod->output.rtp.seq;
    sr.byte_count = demod->output.rtp.bytes;
    
    unsigned char *dp = gen_sr(buffer,sizeof(buffer),&sr,NULL,0);

    // Construct SDES
    struct rtcp_sdes sdes[4];
    
    // CNAME
    char hostname[1024];
    gethostname(hostname,sizeof(hostname));
    char *string = NULL;
    int sl = asprintf(&string,"radio@%s",hostname);
    if(sl > 0 && sl <= 255){
      sdes[0].type = CNAME;
      strlcpy(sdes[0].message,string,sizeof(sdes[0].message));
      sdes[0].mlen = strlen(sdes[0].message);
    }
    if(string){
      free(string); string = NULL;
    }

    sdes[1].type = NAME;
    strlcpy(sdes[1].message,"KA9Q Radio Program",sizeof(sdes[1].message));
    sdes[1].mlen = strlen(sdes[1].message);
    
    sdes[2].type = EMAIL;
    strlcpy(sdes[2].message,"karn@ka9q.net",sizeof(sdes[2].message));
    sdes[2].mlen = strlen(sdes[2].message);

    sdes[3].type = TOOL;
    strlcpy(sdes[3].message,"KA9Q Radio Program",sizeof(sdes[3].message));
    sdes[3].mlen = strlen(sdes[3].message);
    
    dp = gen_sdes(dp,sizeof(buffer) - (dp-buffer),demod->output.rtp.ssrc,sdes,4);


    send(demod->output.rtcp_fd,buffer,dp-buffer,0);
  done:;
    usleep(1000000);
  }
}
static void closedown(int a){
  fprintf(stdout,"Received signal %d, exiting\n",a);
  int r = fftwf_export_wisdom_to_filename(Wisdom_file);
  fprintf(stdout,"fftwf_export_wisdom_to_filename(%s) %s\n",Wisdom_file,r == 1 ? "succeeded" : "failed");

  if(a == SIGTERM)
    exit(0); // Return success when terminated by systemd
  else
    exit(1);
}

