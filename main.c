// Core of KA9Q radiod
// downconvert, filter, demodulate, multicast output
// Copyright 2017-2023, Phil Karn, KA9Q, karn@ka9q.net
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
#include <stdint.h>
#include <stdbool.h>
#include <locale.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <signal.h>
#include <getopt.h>
#include <ctype.h>
#include <arpa/inet.h>
#include <iniparser/iniparser.h>
#include <net/if.h>
#include <sched.h>
#include <sysexits.h>

#include "misc.h"
#include "multicast.h"
#include "radio.h"
#include "filter.h"
#include "status.h"
#include "config.h"

// Configuration constants & defaults
static char const DEFAULT_PRESET[] = "am";
static int const DEFAULT_FFT_THREADS = 2;
static int const DEFAULT_IP_TOS = 48; // AF12 left shifted 2 bits
static int const DEFAULT_MCAST_TTL = 0; // Don't blast LANs with cheap Wifi!
static float const DEFAULT_BLOCKTIME = 20.0;
static int const DEFAULT_OVERLAP = 5;

char const *Iface;
char const *Data;
char const *Preset = DEFAULT_PRESET;
char Preset_file[PATH_MAX];

int IP_tos = DEFAULT_IP_TOS;
int Mcast_ttl = DEFAULT_MCAST_TTL;
float Blocktime = DEFAULT_BLOCKTIME;
int Overlap = DEFAULT_OVERLAP;
int RTCP_enable = false;
int SAP_enable = false;
struct sockaddr_storage Data_dest_address;
int Data_fd = -1;
struct channel *Template;

char const *Name;
extern int N_worker_threads; // owned by filter.c
extern double Fftw_plan_timelimit; // defined in filter.c

// Command line and environ params
const char *App_path;
int Verbose;
static char const *Locale = "en_US.UTF-8";
dictionary *Configtable; // Configtable file descriptor for iniparser for main radiod config file
dictionary *Preset_table;   // Table of presets, usually in /usr/local/share/ka9q-radio/modes.conf or presets.conf
volatile bool Stop_transfers = false; // Request to stop data transfers; how should this get set?

static int64_t Starttime;      // System clock at timestamp 0, for RTCP
pthread_t Status_thread;
pthread_t chan_reaper_thread;
struct sockaddr_storage Metadata_source_address;   // Source of SDR metadata
struct sockaddr_storage Metadata_dest_address;      // Dest of metadata (typically multicast)
char const *Metadata_dest_string; // DNS name of default multicast group for ostatus/commands
uint64_t Metadata_packets;

static void closedown(int);
static void verbosity(int);
static int loadconfig(char const *file);
static int setup_hardware(char const *sname);
static void *rtcp_send(void *);

// In sdrplay.c (maybe someday)
int sdrplay_setup(struct frontend *,dictionary *,char const *);
int sdrplay_startup(struct frontend *);
double sdrplay_tune(struct frontend *,double);

// In rx888.c
int rx888_setup(struct frontend *,dictionary *,char const *);
int rx888_startup(struct frontend *);
double rx888_tune(struct frontend *,double);

// In airspy.c
int airspy_setup(struct frontend *,dictionary *,char const *);
int airspy_startup(struct frontend *);
double airspy_tune(struct frontend *,double);

// In airspyhf.c
int airspyhf_setup(struct frontend *,dictionary *,char const *);
int airspyhf_startup(struct frontend *);
double airspyhf_tune(struct frontend *,double);

// In funcube.c
int funcube_setup(struct frontend *,dictionary *,char const *);
int funcube_startup(struct frontend *);
double funcube_tune(struct frontend *,double);

// In rtlsdr.c:
int rtlsdr_setup(struct frontend *,dictionary *,char const *);
int rtlsdr_startup(struct frontend *);
double rtlsdr_tune(struct frontend *,double);

// In sig_gen.c:
int sig_gen_setup(struct frontend *,dictionary *,char const *);
int sig_gen_startup(struct frontend *);
double sig_gen_tune(struct frontend *,double);



// The main program sets up the demodulator parameter defaults,
// overwrites them with command-line arguments and/or state file settings,
// initializes the various local oscillators, pthread mutexes and conditions
// sets up multicast I/Q input and PCM audio output
// Sets up the input half of the pre-detection filter
// starts the RTP input and downconverter/filter threads
// sets the initial demodulation mode, which starts the demodulator thread
// catches signals and eventually becomes the user interface/display loop
int main(int argc,char *argv[]){
  App_path = argv[0];

  VERSION();
#ifndef NDEBUG
  fprintf(stdout,"Assertion checking enabled, execution will be slower\n");
#endif

  setlinebuf(stdout);
  Starttime = gps_time_ns();

  struct timespec start_realtime;
  clock_gettime(CLOCK_MONOTONIC,&start_realtime);


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

  int c;
  while((c = getopt(argc,argv,"N:hvp:IV")) != -1){
    switch(c){
    case 'V': // Already shown above
      exit(EX_OK); 
    case 'p':
      Fftw_plan_timelimit = strtod(optarg,NULL);
      break;
    case 'v':
      Verbose++;
      break;
    case 'N':
      Name = optarg;
      break;
    case 'I':
      dump_interfaces();
      break;
    default:
      fprintf(stdout,"Unknown command line option %c\n",c);
    case 'h':
      fprintf(stderr,"Usage: %s [-I] [-N name] [-h] [-p fftw_plan_time_limit] [-v [-v] ...] <CONFIG_FILE>\n", argv[0]);
      exit(EX_USAGE);
    }
  }
  
  // Graceful signal catch
  signal(SIGPIPE,closedown);
  signal(SIGINT,closedown);
  signal(SIGKILL,closedown);
  signal(SIGQUIT,closedown);
  signal(SIGTERM,closedown);        
  signal(SIGPIPE,SIG_IGN);
  signal(SIGUSR1,verbosity);
  signal(SIGUSR2,verbosity);
  
  
  char const *configfile;
  if(argc <= optind){
    fprintf(stdout,"Configtable file missing\n");
    exit(EX_NOINPUT);
  }
  configfile = argv[optind];
  if(Name == NULL){
    // Extract name from config file pathname
    Name = argv[optind]; // Ah, just use whole thing
  }
  fprintf(stdout,"Loading config file %s...\n",configfile);
  int const n = loadconfig(argv[optind]);
  if(n < 0){
    fprintf(stdout,"Can't load config file %s\n",argv[optind]);
    exit(EX_NOINPUT);
  }
  fprintf(stdout,"%d total demodulators started\n",n);

  // Measure CPU usage
  struct timespec last_realtime = start_realtime;
  struct timespec last_cputime = {0};
  int sleep_period = 60;
  while(1){
    sleep(sleep_period);
    struct timespec new_realtime;
    clock_gettime(CLOCK_MONOTONIC,&new_realtime);
    double total_real = new_realtime.tv_sec - start_realtime.tv_sec
      + 1e-9 * (new_realtime.tv_nsec - start_realtime.tv_nsec);

    struct timespec new_cputime;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID,&new_cputime);
    double total_cpu = new_cputime.tv_sec + 1e-9 * (new_cputime.tv_nsec);

    double total_percent = 100. * total_cpu / total_real;

    double period_real = new_realtime.tv_sec - last_realtime.tv_sec
      + 1e-9 * (new_realtime.tv_nsec - last_realtime.tv_nsec);

    double period_cpu =  new_cputime.tv_sec - last_cputime.tv_sec
      + 1e-9 * (new_cputime.tv_nsec - last_cputime.tv_nsec);

    double period_percent = 100. * period_cpu / period_real;

    last_realtime = new_realtime;
    last_cputime = new_cputime;

    if(Verbose)
      fprintf(stdout,"CPU usage: %.1lf%% since start, %.1lf%% in last %.1lf sec\n",
	      total_percent, period_percent,period_real);
  }
  exit(EX_OK); // Can't happen
}

// Load the radiod config file, e.g., /etc/radio/radiod@rx888-ka9q-hf.conf
static int loadconfig(char const * const file){
  if(file == NULL || strlen(file) == 0)
    return -1;

  Configtable = iniparser_load(file);
  if(Configtable == NULL)
    return -1;

  // Process [global] section applying to all demodulator blocks
  char const * const global = "global";
  Verbose = config_getint(Configtable,global,"verbose",Verbose);
  Fftw_plan_timelimit = config_getdouble(Configtable,global,"fft-time-limit",Fftw_plan_timelimit);


  // Default multicast interface
  {
    // The area pointed to by returns from config_getstring() is freed and overwritten when the config dictionary is closed
    char const *p = config_getstring(Configtable,global,"iface",Iface);
    if(p != NULL){
      Iface = strdup(p);
      Default_mcast_iface = Iface;    
    }
  }
  // Overrides in [global] of compiled-in defaults 
  Data = config_getstring(Configtable,global,"data",NULL);
  IP_tos = config_getint(Configtable,global,"tos",IP_tos);
  Mcast_ttl = config_getint(Configtable,global,"ttl",Mcast_ttl);
  if(Data != NULL){
    // Set up default output stream file descriptor and socket
    // There can be multiple senders to an output stream, so let avahi suppress the duplicate addresses
    {
      char ttlmsg[100];
      snprintf(ttlmsg,sizeof(ttlmsg),"TTL=%d",Mcast_ttl);

      int slen = sizeof(Data_dest_address);
      avahi_start(Name,"_rtp._udp",DEFAULT_RTP_PORT,Data,ElfHashString(Data),ttlmsg,&Data_dest_address,&slen);
    }
    Data_fd = connect_mcast(&Data_dest_address,Iface,Mcast_ttl,IP_tos);
    if(Data_fd < 0){
      fprintf(stdout,"connect_mcast(%s = %s) failed: %s\n",
	      Data,formatsock(&Data_dest_address),strerror(errno));
      exit(EX_NOHOST); // let systemd restart us, or have the user remove data =
    }
  }
  Blocktime = fabs(config_getdouble(Configtable,global,"blocktime",Blocktime));
  Overlap = abs(config_getint(Configtable,global,"overlap",Overlap));
  N_worker_threads = config_getint(Configtable,global,"fft-threads",DEFAULT_FFT_THREADS); // variable owned by filter.c
  RTCP_enable = config_getboolean(Configtable,global,"rtcp",RTCP_enable);
  SAP_enable = config_getboolean(Configtable,global,"sap",SAP_enable);
  {

    // Accept either keyword; "preset" is more descriptive than the old (but still accepted) "mode"
    char const *p = config_getstring(Configtable,global,"mode-file","presets.conf");
    p = config_getstring(Configtable,global,"presets-file",p);
    dist_path(Preset_file,sizeof(Preset_file),p);
    Preset_table = iniparser_load(Preset_file); // Kept open for duration of program
    if(Preset_table == NULL){
      fprintf(stdout,"Can't load preset file %s\n",Preset_file);
      exit(EX_UNAVAILABLE); // Can't really continue without fixing
    }
  }

  {
    char const *p = config_getstring(Configtable,global,"wisdom-file",NULL);
    if(p != NULL)
      Wisdom_file = strdup(p);
  }
  const char *hardware = config_getstring(Configtable,global,"hardware",NULL);
  if(hardware == NULL){
    // 'hardware =' now required, no default
    fprintf(stdout,"'hardware = [sectionname]' now required to specify front end configuration\n");
    exit(EX_USAGE);
  }
  // Look for specified hardware section
  {
    int const nsect = iniparser_getnsec(Configtable);
    int sect;
    for(sect = 0; sect < nsect; sect++){
      char const * const sname = iniparser_getsecname(Configtable,sect);
      if(strcasecmp(sname,hardware) == 0){
	if(setup_hardware(sname) != 0)
	  exit(EX_NOINPUT);
	
	break;
      }
    }
    if(sect == nsect){
      fprintf(stdout,"no hardware section [%s] found, please create it\n",hardware);
      exit(EX_USAGE);
    }
  }
  {
    // Set up status/command stream, global for all receiver channels
    // Should probably generate one randomly if not specified
    char const * const status = config_getstring(Configtable,global,"status",NULL); // Status/command target for all demodulators
    if(status == NULL){
      fprintf(stdout,"status=<mcast group> missing in [global], e.g, status=hf.local\n");
      exit(EX_USAGE);
    }
    Metadata_dest_string = strdup(status);
    {
      char ttlmsg[100];
      snprintf(ttlmsg,sizeof(ttlmsg),"TTL=%d",Mcast_ttl);
      int slen = sizeof(Metadata_dest_address);
      avahi_start(Name,"_ka9q-ctl._udp",DEFAULT_STAT_PORT,Metadata_dest_string,ElfHashString(Metadata_dest_string),ttlmsg,&Metadata_dest_address,&slen);
    }
    // avahi_start has resolved the target DNS name into Metadata_dest_address and inserted the port number
    Status_fd = connect_mcast(&Metadata_dest_address,Iface,Mcast_ttl,IP_tos);
    if(Status_fd < 0){
      fprintf(stdout,"Can't send status to %s: %s\n",Metadata_dest_string,strerror(errno));
      exit(EX_NOHOST); // Let systemd retry us
    } else {
      socklen_t len = sizeof(Metadata_source_address);
      getsockname(Status_fd,(struct sockaddr *)&Metadata_source_address,&len);  
      // Same remote socket as status
      Ctl_fd = listen_mcast(&Metadata_dest_address,Iface);
      if(Ctl_fd < 0){
	fprintf(stdout,"can't listen for commands from %s: %s\n",Metadata_dest_string,strerror(errno));
	exit(EX_NOHOST);
      }
    }
  }
  // Set up template for dynamically created demods
  if(Data != NULL && Data_fd != -1){
    // Preset/mode must be specified to create a dynamic channel
    // (Trying to switch from term "mode" to term "preset" as more descriptive)
    char const * p = config_getstring(Configtable,global,"preset",NULL);
    char const * preset = config_getstring(Configtable,global,"mode",p); // Must be specified to create a dynamic channel
    if(preset != NULL){
      Template = create_chan(0);
      if(Template == NULL){
	fprintf(stdout,"can't create dynamic channel template??\n");
      } else {
	set_defaults(Template);
	if(loadpreset(Template,Preset_table,preset) != 0)
	  fprintf(stdout,"warning: loadpreset(%s,%s) in [global]\n",Preset_file,preset);
	strlcpy(Template->preset,preset,sizeof(Template->preset));
	
	loadpreset(Template,Configtable,global); // Overwrite with other entries from this section, without overwriting those
	memcpy(&Template->output.data_dest_address,&Data_dest_address,sizeof(Template->output.data_dest_address));
	strlcpy(Template->output.data_dest_string,Data,sizeof(Template->output.data_dest_string));
	Template->output.data_fd = Data_fd;
      }
    }
  }
  // Process individual demodulator sections
  int const nsect = iniparser_getnsec(Configtable);
  int nchans = 0;
  for(int sect = 0; sect < nsect; sect++){
    char const * const sname = iniparser_getsecname(Configtable,sect);
    if(strcasecmp(sname,global) == 0)
      continue; // Already processed above
    if(config_getstring(Configtable,sname,"device",NULL) != NULL)
      continue; // It's a front end configuration, ignore

    if(config_getboolean(Configtable,sname,"disable",false))
	continue; // section is disabled

    fprintf(stdout,"Processing [%s]\n",sname); // log only if not disabled
    // fall back to setting in [global] if parameter not specified in individual section
    // Set parameters even when unused for the current demodulator in case the demod is changed later
    char const * preset = config2_getstring(Configtable,Configtable,global,sname,"mode",NULL);
    preset = config2_getstring(Configtable,Configtable,global,sname,"preset",preset);
    if(preset == NULL || strlen(preset) == 0)
      fprintf(stdout,"warning: preset/mode not specified in [%s] or [global], all parameters must be explicitly set\n",sname);

    // Override [global] settings with section settings
    char const *data = config_getstring(Configtable,sname,"data",NULL);
    if(data == NULL && Data == NULL){
      fprintf(stdout,"'data =' missing and not set in [%s]\n",global);
      continue;
    }
    // Override global defaults
    int const mcast_ttl = config_getint(Configtable,sname,"ttl",Mcast_ttl);
    int const ip_tos = config_getint(Configtable,sname,"tos",IP_tos);
    char const *iface = config_getstring(Configtable,sname,"iface",Iface);
    
    struct sockaddr_storage data_dest_address;
    int data_fd = -1;
    
    if(data != NULL){
      // 'data =' is specified in this section
      memcpy(&data_dest_address,data,sizeof(data_dest_address));
      // There can be multiple senders to an output stream, so let avahi suppress the duplicate addresses
      {
	char ttlmsg[100];
	snprintf(ttlmsg,sizeof(ttlmsg),"TTL=%d",Mcast_ttl);

	int slen = sizeof(data_dest_address);
	avahi_start(sname,"_rtp._udp",DEFAULT_RTP_PORT,data,ElfHashString(data),ttlmsg,&data_dest_address,&slen);
      }
      data_fd = connect_mcast(&data_dest_address,iface,mcast_ttl,ip_tos);
    } else {
      // Data cannot be null or we wouldn't have gotten here
      memcpy(&data_dest_address,&Data_dest_address,sizeof(data_dest_address));
      data_fd = Data_fd;
      data = Data;
    }
    // Process frequency/frequencies
    // We need to do this first to ensure the resulting SSRCs are unique
    // To work around iniparser's limited line length, we look for multiple keywords
    // "freq", "freq0", "freq1", etc, up to "freq9"
    int nfreq = 0;

    for(int ff = -1; ff < 10; ff++){
      char fname[10];
      if(ff == -1)
	snprintf(fname,sizeof(fname),"freq");
      else
	snprintf(fname,sizeof(fname),"freq%d",ff);

      char const * const frequencies = config_getstring(Configtable,sname,fname,NULL);
      if(frequencies == NULL)
	break; // no more

      // Parse the frequency list(s)
      char *freq_list = strdup(frequencies); // Need writeable copy for strtok
      char *saveptr = NULL;
      for(char const *tok = strtok_r(freq_list," \t",&saveptr);
	  tok != NULL;
	  tok = strtok_r(NULL," \t",&saveptr)){
	
	double const f = parse_frequency(tok,true);
	if(f < 0){
	  fprintf(stdout,"can't parse frequency %s\n",tok);
	  continue;
	}
	uint32_t ssrc = 0;
	// Generate default ssrc from frequency string
	for(char const *cp = tok ; cp != NULL && *cp != '\0' ; cp++){
	  if(isdigit(*cp)){
	    ssrc *= 10;
	    ssrc += *cp - '0';
	  }
	}
	ssrc = config_getint(Configtable,sname,"ssrc",ssrc); // Explicitly set?
	if(ssrc == 0)
	  continue; // Reserved ssrc

	struct channel *chan = NULL;
	// Try to create it, incrementing in case of collision
	int const max_collisions = 100;
	for(int i=0; i < max_collisions; i++){
	  chan = create_chan(ssrc+i);
	  if(chan != NULL){
	    ssrc += i;
	    break;
	  }
	}
	if(chan == NULL){
	  fprintf(stdout,"Can't allocate requested ssrc %u-%u\n",ssrc,ssrc + max_collisions);
	  continue;
	}
	// Set reasonable compiled-in defaults just to keep things from blowing up
	set_defaults(chan);
	if(preset != NULL && loadpreset(chan,Preset_table,preset) != 0)
	  fprintf(stdout,"warning: in [%s], loadpreset(%s,%s) failed; compiled-in defaults and local settings used\n",sname,Preset_file,preset);
	
	strlcpy(chan->preset,preset,sizeof(chan->preset));
	loadpreset(chan,Configtable,sname); // Overwrite with other entries from this section, without overwriting those
	
	memcpy(&chan->output.data_dest_address,&data_dest_address,sizeof(chan->output.data_dest_address));
	strlcpy(chan->output.data_dest_string,data,sizeof(chan->output.data_dest_string));
	chan->output.data_fd = data_fd;
	
	if(chan->output.data_fd <= 0){
	  fprintf(stdout,"can't set up PCM output to %s: %s\n",data,strerror(errno));
	  exit(EX_IOERR); // Let systemd retry us
	}
	// Get the local socket for the output stream
	struct sockaddr_storage data_source_address;
	{
	  socklen_t len = sizeof(data_source_address);
	  getsockname(chan->output.data_fd,(struct sockaddr *)&chan->output.data_source_address,&len);
	}
	// Time to start it -- ssrc is stashed by create_chan()
	set_freq(chan,f);
	start_demod(chan);
	nfreq++;
	nchans++;
	
	if(SAP_enable){
	  // Highly experimental, off by default
	  char sap_dest[] = "224.2.127.254:9875"; // sap.mcast.net
	  chan->output.sap_fd = setup_mcast(sap_dest,NULL,1,mcast_ttl,ip_tos,0);
	  if(chan->output.sap_fd < 3)
	    fprintf(stdout,"Can't set up SAP output to %s\n",sap_dest); // not fatal
	  else
	    pthread_create(&chan->sap_thread,NULL,sap_send,chan);
	}
	// RTCP Real Time Control Protocol daemon is optional
	if(RTCP_enable){
	  chan->output.rtcp_fd = setup_mcast(chan->output.data_dest_string,NULL,1,mcast_ttl,ip_tos,1); // RTP port number + 1
	  if(chan->output.rtcp_fd < 3)
	    fprintf(stdout,"can't set up RTCP output to %s\n",chan->output.data_dest_string); // not fatal
	  else
	    pthread_create(&chan->rtcp_thread,NULL,rtcp_send,chan);
	}
      }
      // Done processing frequency list(s) and creating chans
      FREE(freq_list);
      fprintf(stdout,"%d channels started\n",nfreq);
    }
  }
  // Start the status thread after all the receivers have been created so it doesn't contend for the chan list lock
  if(Ctl_fd >= 3 && Status_fd >= 3)
    pthread_create(&Status_thread,NULL,radio_status,NULL);

  pthread_create(&chan_reaper_thread,NULL,chan_reaper,NULL);
  iniparser_freedict(Configtable);
  Configtable = NULL;
  return nchans;
}

// Set up a local front end device
static int setup_hardware(char const *sname){
  char const *device = config_getstring(Configtable,sname,"device",NULL);
  if(device == NULL){
    fprintf(stdout,"No device= entry in [%s]\n",sname);
    return -1;
  }
  // Do we support it?
  // This should go into a table somewhere
  if(strcasecmp(device,"rx888") == 0){
    Frontend.setup = rx888_setup;
    Frontend.start = rx888_startup;
    Frontend.tune = NULL; // Only direct sampling for now
  } else if(strcasecmp(device,"airspy") == 0){
    Frontend.setup = airspy_setup;
    Frontend.start = airspy_startup;
    Frontend.tune = airspy_tune;
  } else if(strcasecmp(device,"airspyhf") == 0){
    Frontend.setup = airspyhf_setup;
    Frontend.start = airspyhf_startup;
    Frontend.tune = airspyhf_tune;
  } else if(strcasecmp(device,"funcube") == 0){
    Frontend.setup = funcube_setup;
    Frontend.start = funcube_startup;
    Frontend.tune = funcube_tune;
  } else if(strcasecmp(device,"rtlsdr") == 0){
    Frontend.setup = rtlsdr_setup;
    Frontend.start = rtlsdr_startup;
    Frontend.tune = rtlsdr_tune;
  } else if(strcasecmp(device,"sig_gen") == 0){
    Frontend.setup = sig_gen_setup;
    Frontend.start = sig_gen_startup;
    Frontend.tune = sig_gen_tune;
#if 0
    // The sdrplay library is still proprietary and object-only, so I can't bundle it in ka9q-radio
    // Everything else either has a standard Debian package or I have information to program them directly.
    // To hell with vendors who deliberately make their products hard to use when they have plenty of competition.
  } else if(strcasecmp(device,"sdrplay") == 0){
    Frontend.setup = sdrplay_setup;
    Frontend.start = sdrplay_startup;
    Frontend.tune = sdrplay_tune;
#endif
  } else {
    fprintf(stdout,"device %s unrecognized\n",device);
    return -1;
  }

  int r = (*Frontend.setup)(&Frontend,Configtable,sname); 
  if(r != 0){
    fprintf(stdout,"device setup returned %d\n",r);
    return r;
  }

  // Create input filter now that we know the parameters
  // FFT and filter sizes computed from specified block duration and sample rate
  // L = input data block size
  // M = filter impulse response duration
  // N = FFT size = L + M - 1
  // Note: no checking that N is an efficient FFT blocksize; choose your parameters wisely
  assert(Frontend.samprate != 0);
  double const eL = Frontend.samprate * Blocktime / 1000.0; // Blocktime is in milliseconds
  Frontend.L = lround(eL);
  if(Frontend.L != eL)
    fprintf(stdout,"Warning: non-integral samples in %.3f ms block at sample rate %d Hz: remainder %g\n",
	    Blocktime,Frontend.samprate,eL-Frontend.L);

  Frontend.M = Frontend.L / (Overlap - 1) + 1;
  assert(Frontend.M != 0);
  assert(Frontend.L != 0);
  Frontend.in = create_filter_input(Frontend.L,Frontend.M, Frontend.isreal ? REAL : COMPLEX);
  if(Frontend.in == NULL){
    fprintf(stdout,"Input filter setup failed\n");
    return -1;
  }
  pthread_mutex_init(&Frontend.status_mutex,NULL);
  pthread_cond_init(&Frontend.status_cond,NULL);
  if(Frontend.start){
    int r = (*Frontend.start)(&Frontend); 
    if(r != 0)
      fprintf(stdout,"Front end start returned %d\n",r);

    return r;
  } else {
    fprintf(stdout,"No front end start routine?\n");
    return -1;
  }
}


// RTP control protocol sender task
static void *rtcp_send(void *arg){
  struct channel const *chan = (struct channel *)arg;
  if(chan == NULL)
    pthread_exit(NULL);

  {
    char name[100];
    snprintf(name,sizeof(name),"rtcp %u",chan->output.rtp.ssrc);
    pthread_setname(name);
  }

  while(1){

    if(chan->output.rtp.ssrc == 0) // Wait until it's set by output RTP subsystem
      goto done;
    uint8_t buffer[4096]; // much larger than necessary
    memset(buffer,0,sizeof(buffer));
    
    // Construct sender report
    struct rtcp_sr sr;
    memset(&sr,0,sizeof(sr));
    sr.ssrc = chan->output.rtp.ssrc;

    // Construct NTP timestamp (NTP uses UTC, ignores leap seconds)
    {
      struct timespec now;
      clock_gettime(CLOCK_REALTIME,&now);
      sr.ntp_timestamp = ((int64_t)now.tv_sec + NTP_EPOCH) << 32;
      sr.ntp_timestamp += ((int64_t)now.tv_nsec << 32) / BILLION; // NTP timestamps are units of 2^-32 sec
    }
    // The zero is to remind me that I start timestamps at zero, but they could start anywhere
    sr.rtp_timestamp = (0 + gps_time_ns() - Starttime) / BILLION;
    sr.packet_count = chan->output.rtp.seq;
    sr.byte_count = chan->output.rtp.bytes;
    
    uint8_t *dp = gen_sr(buffer,sizeof(buffer),&sr,NULL,0);

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
    FREE(string);

    sdes[1].type = NAME;
    strlcpy(sdes[1].message,"KA9Q Radio Program",sizeof(sdes[1].message));
    sdes[1].mlen = strlen(sdes[1].message);
    
    sdes[2].type = EMAIL;
    strlcpy(sdes[2].message,"karn@ka9q.net",sizeof(sdes[2].message));
    sdes[2].mlen = strlen(sdes[2].message);

    sdes[3].type = TOOL;
    strlcpy(sdes[3].message,"KA9Q Radio Program",sizeof(sdes[3].message));
    sdes[3].mlen = strlen(sdes[3].message);
    
    dp = gen_sdes(dp,sizeof(buffer) - (dp-buffer),chan->output.rtp.ssrc,sdes,4);


    send(chan->output.rtcp_fd,buffer,dp-buffer,0);
  done:;
    sleep(1);
  }
}
static void closedown(int a){
  fprintf(stdout,"Received signal %d, exiting\n",a);
  Stop_transfers = true;
  sleep(1); // pause for threads to see it

  if(a == SIGTERM)
    exit(EX_OK); // Return success when terminated by systemd
  else
    exit(EX_SOFTWARE);
}

// Increase or decrease logging level (thanks AI6VN for idea)
static void verbosity(int a){
  if(a == SIGUSR1)
    Verbose++;
  else if(a == SIGUSR2)
    Verbose = (Verbose <= 0) ? 0 : Verbose - 1;
  else
    return;

  fprintf(stdout,"Verbose = %d\n",Verbose);
}
