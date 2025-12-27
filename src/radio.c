// This is the core of radiod:
// read and process global config settings
// set up and start hardware and convolver
// Also provides entry points for channel creation and control for commands processed in radio_status.c
// Copyright 2018-2025, Phil Karn, KA9Q
#define _GNU_SOURCE 1
#include <assert.h>
#include <complex.h>
#include <errno.h>
#include <fftw3.h>
#undef I
#include <iniparser/iniparser.h>
#include <limits.h>
#include <math.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(linux)
#include <bsd/string.h>
#endif

// For SAP/SDP
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <dirent.h>
#include <pwd.h>
#include <uuid/uuid.h>
#include <fcntl.h>
#include <sysexits.h>
#include <ctype.h>
#include "misc.h"
#include "osc.h"
#include "radio.h"
#include "filter.h"
#include "status.h"
#include "avahi.h"

#define DEFAULT_PRESET "am"
#define GLOBAL "global"


static int Total_channels;
static bool Global_use_dns;
static void *Dl_handle;
static struct frontend Frontend;
static int const DEFAULT_IP_TOS = 46 << 2; // Expedited Forwarding
static double const DEFAULT_BLOCKTIME = .02; // 20 ms
static char *Metadata_dest_string; // DNS name of default multicast group for status/commands
static pthread_t Status_thread;
static dictionary *Configtable; // Configtable file descriptor for iniparser for main radiod config file
static int SAP_enable = false;
static int RTCP_enable = false;
static int const DEFAULT_UPDATE = 25; // 2 Hz for 20 ms blocktime (50 Hz frame rate)
static int Update = DEFAULT_UPDATE;
static int const DEFAULT_FFTW_THREADS = 1;
static int const DEFAULT_FFTW_INTERNAL_THREADS = 1;
static int const DEFAULT_LIFETIME = 20; // 20 sec for idle sessions tuned to 0 Hz
static int const DEFAULT_OVERLAP = 5;
static double const Power_alpha = 0.10; // Noise estimation time smoothing factor, per block. Use double to reduce risk of slow denormals
static double const NQ = 0.10; // look for energy in 10th quartile, hopefully contains only noise
static double const N_cutoff = 1.5; // Average (all noise, hopefully) bins up to 1.5x the energy in the 10th quartile
// Minimum to get reasonable noise level statistics; 1000 * 40 Hz = 40 kHz which seems reasonable
static int const Min_noise_bins = 1000;
static char const *Iface;
static char *Data;
static int IP_tos = DEFAULT_IP_TOS;
static char Preset_file[PATH_MAX];
static char Hostname[256]; // can't use sysconf(_SC_HOST_NAME_MAX) at file scope
static struct channel Template; // Template for dynamically created channels
static pthread_mutex_t Channel_list_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t Freq_mutex = PTHREAD_MUTEX_INITIALIZER;
static int Active_channel_count; // Active channels

// List of valid config keys in [global] section, for error checking
static char const *Global_keys[] = {
  "affinity",
  "blocktime",
  "data",
  "dc-cut",
  "description",
  "dns",
  "fft-plan-level",
  "fft-internal-threads",
  "fft-threads",
  "hardware",
  "iface",
  "mode-file",
  "mode",
  "overlap",
  "preset",
  "presets-file",
  "prio",
  "rtcp",
  "sap",
  "static",
  "status",
  "tos",
  "ttl",
  "update",
  "verbose",
  "wisdom-file",
  NULL
};

// Remaining global variables are linked mostly from radio_status.c
// Try to eliminate as many as possible
struct channel Channel_list[Nchannels];
double Blocktime = 0;      // Actual blocktime to give integral blocksize at input sample rate. Starts uninitialized
double User_blocktime = DEFAULT_BLOCKTIME; // User's requested blocktime
char const *Description; // Set either in [global] or [hardware]
int Overlap = DEFAULT_OVERLAP;
dictionary *Preset_table;   // Table of presets, usually in /usr/local/share/ka9q-radio/presets.conf

int Output_fd = -1; // Unconnected socket used for output when ttl > 0
int Output_fd0 = -1; // Unconnected socket used for local loopback when ttl = 0
int Ctl_fd = -1;     // File descriptor for receiving user commands

// If a channel is tuned to 0 Hz and then not polled for this many seconds, destroy it
// Must be computed at run time because it depends on the block time
int Channel_idle_timeout;  //  = DEFAULT_LIFETIME / Blocktime;

extern char const *Name;     // owned by main.c

static double estimate_noise(struct channel *chan,int shift);// Noise estimator tuning
static int setup_hardware(char const *sname);
static void *process_section(void *p);
static void *sap_send(void *p);
static void *rtcp_send(void *p);
// Table of frequencies to start
struct ftab {
  double f;
  bool valid; // Will be false if mentioned in "except" list
  double tone; // PL/CTCSS tone, if any
};
static double get_tone(char const *sname,int i);

static int fcompare(void const *ap, void const *bp); // Compare frequencies in table entries
static int tcompare(void const *ap,void const *bp); // Lookup frequency in sorted table

// Load the radiod config file, e.g., /etc/radio/radiod@rx888-ka9q-hf.conf
// Called from main(), concatenates sections of config file (if in a directory)
// Processes the [global] section
// calls setup_hardware to process the hardware section,(e.g., [rx888]
// Sets up the input filter with the big FFT
// Calls process_section() to process each receiver channel section
// Returns count of receiver channels to main()
int loadconfig(char const *file){
  if(file == NULL || strlen(file) == 0)
    return -1;

  DIR *dirp = NULL;
  struct stat statbuf = {0};
  char dname[PATH_MAX] = {0};
  if(stat(file,&statbuf) == 0){
    switch(statbuf.st_mode & S_IFMT){
    case S_IFREG:
      // primary config file radiod@foo.conf exists and is a regular file; just read it
      fprintf(stderr,"Loading config file %s\n",file);
      Configtable = iniparser_load(file); // Just try to read the primary
      if(Configtable == NULL)
	return -1;
      break;
    case S_IFDIR:
      // It's a directory, read its contents
      fprintf(stderr,"Loading config directory %s\n",file);
      dirp = opendir(file);
      if(dirp == NULL)
	return -1; // give up
      break;
    default:
      fprintf(stderr,"Config file %s exists but is not a regular file or directory\n",file);
      return -1;
    }
  } else {
    // Otherwise append ".d" and see if that's a directory
    snprintf(dname,sizeof(dname),"%s.d",file);
    if(stat(dname,&statbuf) == 0 && (statbuf.st_mode & S_IFMT) == S_IFDIR){
      fprintf(stderr,"Loading config directory %s\n",dname);
      dirp = opendir(dname);
    }
  }
  if(Configtable == NULL){
    if(dirp == NULL){
      fprintf(stderr,"%s Not a valid config file/directory\n",file);
      return -1; // give up
    }
    // Read and sort list of foo.d/*.conf files, merge into temp file
    int dfd = dirfd(dirp); // this gets used for openat() and fstatat() so don't close dirp right way
    struct dirent *dp;
#define N_SUBFILES (100)
    char *subfiles[N_SUBFILES]; // List of subfiles
    int sf = 0;
    while ((dp = readdir(dirp)) != NULL && sf < N_SUBFILES) {
      // only consider regular files ending in .conf
      if(strcmp(".conf",dp->d_name + strlen(dp->d_name) - 5) == 0
	 && fstatat(dfd,dp->d_name,&statbuf,0) == 0
	 && (statbuf.st_mode & S_IFMT) == S_IFREG)
	subfiles[sf++] = strdup(dp->d_name);
    }
    if(sf == 0){
      fprintf(stderr,"%s: empty config directory\n",strlen(dname) > 0 ? dname : file);
      closedir(dirp);
      return -1;
    }
    // Don't close dirp just yet, would invalidate dfd
    // Config sections can actually be in any order, but just in case one is split across multiple files...
    qsort(subfiles,sf,sizeof(subfiles[0]),(int (*)(void const *,void const *))strcmp);

    // Concatenate sorted files into temporary copy
    char tempfilename[PATH_MAX];
    strlcpy(tempfilename,"/tmp/radiod-configXXXXXXXX",sizeof(tempfilename));
    int tfd = mkstemp(tempfilename);
    if(tfd == -1){
      fprintf(stderr,"mkstemp(%s) failed: %s\n",tempfilename,strerror(errno));
      closedir(dirp);
      return -1;
    }
    FILE *tfp = fdopen(tfd,"rw+");
    if(tfp == NULL){
      fprintf(stderr,"Can't create temporary file %s: %s\n",tempfilename,strerror(errno));
      close(tfd);
      (void)closedir(dirp);
      return -1;
    }
    // Concatenate the sub config files in order
    for(int i=0; i < sf; i++){
      int fd = openat(dfd,subfiles[i],O_RDONLY|O_CLOEXEC);
      // There's no "fopenat()"
      if(fd == -1){
	fprintf(stderr,"Can't read config component %s: %s\n",subfiles[i],strerror(errno));
	continue;
      }
      FILE *fp = fdopen(fd,"r");
      if(fp == NULL){
	fprintf(stderr,"fdopen(%d,r) of %s failed: %s\n",fd,subfiles[i],strerror(errno));
	close(fd);
	continue;
      }
      fprintf(tfp,"# %s\n",subfiles[i]); // for debugging
      char *linep = NULL;
      size_t linecap = 0;
      while(getline(&linep,&linecap,fp) >= 0){
	if(fputs(linep,tfp) == EOF){
	  fprintf(stderr,"fputs(%s,%s): %s\n",linep,tempfilename,strerror(errno));
	  break;
	}
      }
      FREE(linep);
      fclose(fp);     fp = NULL;
    }
    // Done with file names and directory
    for(int i=0; i < sf; i++)
      FREE(subfiles[i]); // Allocated by strdup()

    (void)closedir(dirp); dirp = NULL;
    fclose(tfp); tfp = NULL; tfd = -1; // Also does close(tfd)

    Configtable = iniparser_load(tempfilename);
    unlink(tempfilename); // Done with temp file
  }
  if(Configtable == NULL){
    return -1;
  }
  config_validate_section(stderr,Configtable,GLOBAL,Global_keys,Channel_keys);

  // Process [global] section applying to all demodulator blocks
  Description = config_getstring(Configtable,GLOBAL,"description",NULL);
  Verbose = config_getint(Configtable,GLOBAL,"verbose",Verbose);
  User_blocktime = fabs(config_getdouble(Configtable,GLOBAL,"blocktime",User_blocktime)); // Input value is in ms, internally in sec
  Channel_idle_timeout = (int)round(20 / User_blocktime);
  Overlap = abs(config_getint(Configtable,GLOBAL,"overlap",Overlap));
  N_worker_threads = config_getint(Configtable,GLOBAL,"fft-threads",DEFAULT_FFTW_THREADS); // variable owned by filter.c
  N_internal_threads = config_getint(Configtable,GLOBAL,"fft-internal-threads",DEFAULT_FFTW_INTERNAL_THREADS); // owned by filter.c
  RTCP_enable = config_getboolean(Configtable,GLOBAL,"rtcp",RTCP_enable);
  SAP_enable = config_getboolean(Configtable,GLOBAL,"sap",SAP_enable);
  {
    char const *cp = config_getstring(Configtable,GLOBAL,"fft-plan-level","patient");
    if(strcasecmp(cp,"estimate") == 0){
      FFTW_planning_level = FFTW_ESTIMATE;
    } else if(strcasecmp(cp,"measure") == 0){
      FFTW_planning_level = FFTW_MEASURE;
    } else if(strcasecmp(cp,"patient") == 0){
      FFTW_planning_level = FFTW_PATIENT;
    } else if(strcasecmp(cp,"exhaustive") == 0){
      FFTW_planning_level = FFTW_EXHAUSTIVE;
    } else if(strcasecmp(cp,"wisdom-only") == 0){
      FFTW_planning_level = FFTW_WISDOM_ONLY;
    }
  }
  Update = config_getint(Configtable,GLOBAL,"update",Update);
  IP_tos = config_getint(Configtable,GLOBAL,"tos",IP_tos);
  Global_use_dns = config_getboolean(Configtable,GLOBAL,"dns",false);
  Static_avahi = config_getboolean(Configtable,GLOBAL,"static",false);
  Affinity = config_getboolean(Configtable,GLOBAL,"affinity",false);
  {
    char const *p = config_getstring(Configtable,GLOBAL,"wisdom-file",NULL);
    if(p != NULL)
      Wisdom_file = strdup(p);

    // Accept either keyword; "preset" is more descriptive than the old (but still accepted) "mode"
    p = config_getstring(Configtable,GLOBAL,"mode-file","presets.conf");
    p = config_getstring(Configtable,GLOBAL,"presets-file",p);
    dist_path(Preset_file,sizeof(Preset_file),p);
    fprintf(stderr,"Loading presets file %s\n",Preset_file);
    Preset_table = iniparser_load(Preset_file); // Kept open for duration of program
    config_validate(stderr,Preset_table,Channel_keys,NULL);
    if(Preset_table == NULL){
      fprintf(stderr,"Can't load preset file %s\n",Preset_file);
      exit(EX_UNAVAILABLE); // Can't really continue without fixing
    }
  }

  // Form default status dns name

  gethostname(Hostname,sizeof(Hostname));
  // Edit off .domain, .local, etc
  {
    char *cp = strchr(Hostname,'.');
    if(cp != NULL)
      *cp = '\0';
  }
  char default_status[strlen(Hostname) + strlen(Name) + 20]; // Enough room for snprintf
  snprintf(default_status,sizeof(default_status),"%s-%s.local",Hostname,Name);
  {
   char const *cp = config_getstring(Configtable,GLOBAL,"status",default_status); // Status/command target for all demodulators
    // Add .local if not present
   Metadata_dest_string = ensure_suffix(cp,".local");
  }
  // Set up the hardware early, in case it fails
  const char *hardware = config_getstring(Configtable,GLOBAL,"hardware",NULL);
  if(hardware == NULL){
    // 'hardware =' now required, no default
    fprintf(stderr,"'hardware = [sectionname]' now required to specify front end configuration\n");
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
      fprintf(stderr,"no hardware section [%s] found, please create it\n",hardware);
      exit(EX_USAGE);
    }
  }
  if(strlen(Frontend.description) == 0)
    strlcpy(Frontend.description,Name,sizeof(Frontend.description)); // Set default description

  // Default multicast interface
  {
    // The area pointed to by returns from config_getstring() is freed and overwritten when the config dictionary is closed
    char const *p = config_getstring(Configtable,GLOBAL,"iface",Iface);
    if(p != NULL){
      Iface = strdup(p);
      Default_mcast_iface = Iface;
    }
  }
  // Overrides in [global] of compiled-in defaults
  {
    char data_default[256];
    snprintf(data_default,sizeof(data_default),"%s-pcm.local",Name);
    char const *cp = config_getstring(Configtable,GLOBAL,"data",data_default);
    // Add .local if not present
    Data = ensure_suffix(cp,".local");
  }
  // Set up template for all new channels
  set_defaults(&Template);
  Template.frontend = &Frontend;
  assert(Blocktime != 0);
  Template.lifetime = (int)round(DEFAULT_LIFETIME / Blocktime); // If freq == 0, goes away 20 sec after last command

  // Set up default output stream file descriptor and socket
  // There can be multiple senders to an output stream, so let avahi suppress the duplicate addresses
  strlcpy(Template.output.dest_string,Data,sizeof(Template.output.dest_string));

  // Preset/mode must be specified to create a dynamic channel
  // (Trying to switch from term "mode" to term "preset" as more descriptive)
  char const * p = config_getstring(Configtable,GLOBAL,"preset","am"); // Hopefully "am" is defined in presets.conf
  char const * preset = config_getstring(Configtable,GLOBAL,"mode",p); // Must be specified to create a dynamic channel
  if(preset != NULL){
    if(loadpreset(&Template,Preset_table,preset) != 0)
      fprintf(stderr,"warning: loadpreset(%s,%s) in [global]\n",Preset_file,preset);
    strlcpy(Template.preset,preset,sizeof(Template.preset));

    loadpreset(&Template,Configtable,GLOBAL); // Overwrite with other entries from this section, without overwriting those
  } else {
    fprintf(stderr,"No default mode for template\n");
  }

  /* The ttl in the [global] section is used for any dynamic
     data channels. It is the default for static data channels unless
     overridden in each section. Note that when a section specifies a
     non-zero TTL, the global setting is actually used so the section TTLs could as well be booleans.
     It's too tedious and not very useful to manage a whole bunch of sockets with arbitrary
     TTLs. 0 and 1 are most useful.
     At the moment, elicited status messages are always sent with TTL > 0 on the status group
  */

  // Look quickly (2 tries max) to see if it's already in the DNS
  {
    uint32_t addr = 0;
    if(!Global_use_dns || resolve_mcast(Data,&Template.output.dest_socket,DEFAULT_RTP_PORT,NULL,0,2) != 0)
      addr = make_maddr(Data);

    char ttlmsg[128];
    snprintf(ttlmsg,sizeof(ttlmsg),"TTL=%d",Template.output.ttl);
    size_t slen = sizeof(Template.output.dest_socket);
    // Advertise dynamic service(s)
    avahi_start(Frontend.description,
	      "_rtp._udp",
	      DEFAULT_RTP_PORT,
	      Data,
	      addr,
	      ttlmsg,
	      addr != 0 ? &Template.output.dest_socket : NULL,
	      addr != 0 ? &slen : NULL);

    // Status sent to same group, different port
    Template.status.dest_socket = Template.output.dest_socket;
    setport(&Template.status.dest_socket,DEFAULT_STAT_PORT);
   }
  {
    // Non-zero TTL streams use the global ttl if it is nonzero, 1 otherwise
    int const ttl = Template.output.ttl > 1 ? Template.output.ttl : 1;

    Output_fd = output_mcast(&Template.output.dest_socket,Iface,ttl,IP_tos);
    if(Output_fd < 0){
      fprintf(stderr,"can't create output socket for TTL=%d: %s\n",ttl,strerror(errno));
      exit(EX_NOHOST); // let systemd restart us
    }
  }
  join_group(Output_fd,NULL,&Template.output.dest_socket,Iface); // Work around snooping switch problem
  // Secondary output socket with ttl = 0
  Output_fd0 = output_mcast(&Template.output.dest_socket,Iface,0,IP_tos);
  if(Output_fd0 < 0){
    fprintf(stderr,"can't create output socket for TTL=0: %s\n",strerror(errno));
    exit(EX_NOHOST); // let systemd restart us
  }

  // Set up status/command stream, global for all receiver channels
  if(0 == strcmp(Metadata_dest_string,Data)){
    fprintf(stderr,"Duplicate status/data stream names: data=%s, status=%s\n",Data,Metadata_dest_string);
    exit(EX_USAGE);
  }
  // Look quickly (2 tries max) to see if it's already in the DNS
  {
    uint32_t addr = 0;
    if(!Global_use_dns || resolve_mcast(Metadata_dest_string,&Frontend.metadata_dest_socket,DEFAULT_STAT_PORT,NULL,0,2) != 0)
      addr = make_maddr(Metadata_dest_string);

    // If dns name already exists in the DNS, advertise the service record but not an address record
    // Advertise control/status channel with a ttl of at least 1
    char ttlmsg[128];
    snprintf(ttlmsg,sizeof ttlmsg,"TTL=%d",Template.output.ttl > 0? Template.output.ttl : 1);
    size_t slen = sizeof(Frontend.metadata_dest_socket);
    avahi_start(Frontend.description,"_ka9q-ctl._udp",DEFAULT_STAT_PORT,
		Metadata_dest_string,addr,ttlmsg,
		addr != 0 ? &Frontend.metadata_dest_socket : NULL,
		addr != 0 ? &slen : NULL);
  }
  // either resolve_mcast() or avahi_start() has resolved the target DNS name into Frontend.metadata_dest_socket and inserted the port number
  join_group(Output_fd,NULL,&Frontend.metadata_dest_socket,Iface);
  // Same remote socket as status
  Ctl_fd = listen_mcast(NULL,&Frontend.metadata_dest_socket,Iface);
  if(Ctl_fd < 0){
    fprintf(stderr,"can't listen for commands from %s: %s; no control channel is set\n",Metadata_dest_string,strerror(errno));
  } else {
    if(Ctl_fd >= 3)
      pthread_create(&Status_thread,NULL,radio_status,NULL);
  }

  // Process individual demodulator sections in parallel for speed
  int const nsect = iniparser_getnsec(Configtable);
  pthread_t startup_threads[nsect];
  int nthreads = 0;
  for(int sect = 0; sect < nsect; sect++){
    char const * const sname = iniparser_getsecname(Configtable,sect);

    if(strcasecmp(sname,GLOBAL) == 0)
      continue; // Already processed above
    if(strcasecmp(sname,hardware) == 0)
      continue; // Already processed as a hardware section (possibly without device=)
    if(config_getstring(Configtable,sname,"device",NULL) != NULL)
      continue; // It's a front end configuration, ignore
    if(config_getboolean(Configtable,sname,"disable",false))
      continue; // section is disabled

    pthread_create(&startup_threads[nthreads++],NULL,process_section,(void *)sname);
  }
  // Wait for them all to finish
  for(int sect = 0; sect < nthreads; sect++){
    pthread_join(startup_threads[sect],NULL);
#if 0
    printf("startup thread %s joined\n",iniparser_getsecname(Configtable,sect));
#endif
  }
  iniparser_freedict(Configtable);
  Configtable = NULL;
  if(Ctl_fd == -1 && Total_channels == 0)
    fprintf(stderr,"Warning: no control channel and no static demodulators, radiod won't do anything\n");

  return Total_channels;
}

// Set up the SDR front end hardware
// Process the hardware config section, load driver, have it set up the hardware,
// set up the input half (time -> frequency) half of the fast convolver, and start the front end A/D
// Set up the experimental coherent spur notch filters
static int setup_hardware(char const *sname){
  if(sname == NULL)
   return -1; // Possible?
  char const *device = config_getstring(Configtable,sname,"device",sname);
  {
    // Try to find it dynamically
    char defname[PATH_MAX];
    snprintf(defname,sizeof(defname),"%s/%s.so",SODIR,device);
    char const *dlname = config_getstring(Configtable,device,"library",defname);
    if(dlname == NULL){
      fprintf(stderr,"No dynamic library specified for device %s\n",device);
      return -1;
    }
    fprintf(stderr,"Dynamically loading %s hardware driver from %s\n",device,dlname);
    char *error;
    Dl_handle = dlopen(dlname,RTLD_GLOBAL|RTLD_NOW);
    if(Dl_handle == NULL){
      error = dlerror();
      fprintf(stderr,"Error loading %s to handle device %s: %s\n",dlname,device,error);
      return -1;
    }
    char symname[128];
    snprintf(symname,sizeof(symname),"%s_setup",device);
    Frontend.setup = dlsym(Dl_handle,symname);
    if((error = dlerror()) != NULL){
      fprintf(stderr,"error: symbol %s not found in %s for %s: %s\n",symname,dlname,device,error);
      dlclose(Dl_handle);
      return -1;
    }
    snprintf(symname,sizeof(symname),"%s_startup",device);
    Frontend.start = dlsym(Dl_handle,symname);
    if((error = dlerror()) != NULL){
      fprintf(stderr,"error: symbol %s not found in %s for %s: %s\n",symname,dlname,device,error);
      dlclose(Dl_handle);
      return -1;
    }
    snprintf(symname,sizeof(symname),"%s_tune",device);
    Frontend.tune = dlsym(Dl_handle,symname);
    if((error = dlerror()) != NULL){
      // Not fatal, but no tuning possible
      fprintf(stderr,"warning: symbol %s not found in %s for %s: %s\n",symname,dlname,device,error);
    }
    // No error checking on these, they're optional
    snprintf(symname,sizeof(symname),"%s_gain",device);
    Frontend.gain = dlsym(Dl_handle,symname);
    snprintf(symname,sizeof(symname),"%s_atten",device);
    Frontend.atten = dlsym(Dl_handle,symname);
  }

  int r = (*Frontend.setup)(&Frontend,Configtable,sname);
  if(r != 0){
    fprintf(stderr,"device setup returned %d\n",r);
    return r;
  }

  // Create input filter now that we know the parameters
  // FFT and filter sizes computed from specified block duration and sample rate
  // L = input data block size
  // M = filter impulse response duration
  // N = FFT size = L + M - 1
  // Note: no checking that N is an efficient FFT blocksize; choose your parameters wisely
  assert(Frontend.samprate != 0);
  Frontend.L = (int)round(Frontend.samprate * User_blocktime); // Blocktime is in seconds
  Frontend.M = Frontend.L / (Overlap - 1) + 1;
  assert(Frontend.M != 0);
  assert(Frontend.L != 0);
  int const N = Frontend.M + Frontend.L - 1;
  Blocktime = Frontend.L / Frontend.samprate; // True value, must be set early, many things depend on it
  if(fabs(Blocktime - User_blocktime) > 1e-6)
    fprintf(stderr,"Warning: requested block time %lf changed to %lf for integral block size %d at sample rate %lf Hz\n",
	    User_blocktime,Blocktime,Frontend.L,Frontend.samprate);

  fprintf(stderr,"Block time %.3lf ms, overlap %d, forward FFT size %'u %s\n",
	  1000.*Blocktime, Overlap, N,Frontend.isreal ? "real" : "complex");
  create_filter_input(&Frontend.in,Frontend.L,Frontend.M, Frontend.isreal ? REAL : COMPLEX);
  // Create list of frequency spurs in filter input (experimental)
  Frontend.in.notches = calloc(100,sizeof (struct notch_state));
  struct notch_state *notch = Frontend.in.notches;


  // Initialize spur list. MUST leave last entry zeroed as sentinel; also doubles as 0 Hz (DC) suppression
  for(int i = 0; i < NSPURS; i++){
    int shift;
    double remainder; // Offset from bin center, Hz, e.g, -20 to +20. Or is it -25 to +25?
    int r = compute_tuning(N,Frontend.M,Frontend.samprate,&shift,&remainder,Frontend.spurs[i]);
    if(r != 0)
      break;
    notch->state = 0;
    notch->bin = abs(shift);
    notch->alpha = .01; //  About 10 sec. Arbitrary, make adaptive.
    if(shift == 0) // DC is implicitly last
      break;
    notch++;
  }
  pthread_mutex_init(&Frontend.status_mutex,NULL);
  pthread_cond_init(&Frontend.status_cond,NULL);
  if(Frontend.start){
    int r = (*Frontend.start)(&Frontend);
    if(r != 0)
      fprintf(stderr,"Front end start returned %d\n",r);

    return r;
  } else {
    fprintf(stderr,"No front end start routine?\n");
    return -1;
  }
}

// called by loadconfig() to process one receiver section of a config file
static void *process_section(void *p){
  char const *sname = (char *)p;
  if(sname == NULL)
    return NULL;

  config_validate_section(stderr,Configtable,sname,Channel_keys,NULL);

  // fall back to setting in [global] if parameter not specified in individual section
  // Set parameters even when unused for the current demodulator in case the demod is changed later
  char const * preset = config2_getstring(Configtable,Configtable,GLOBAL,sname,"mode",NULL);
  preset = config2_getstring(Configtable,Configtable,GLOBAL,sname,"preset",preset);
  if(preset == NULL || strlen(preset) == 0)
    fprintf(stderr,"[%s] preset/mode not specified, all parameters must be explicitly set\n",sname);

  // Override [global] settings with section settings
  char const *data = NULL;
  {
    char const *cp = config_getstring(Configtable,sname,"data",Data);
    // Add .local if not present
    data = ensure_suffix(cp,".local");
  }

  // Set up a template for all channels defined in this section
  // Parameter priority, from high to low:
  // 1. this section
  // 2. the preset database entry, if specified
  // 3. the [global] section
  // 4. compiled-in defaults to keep things from blowing up
  struct channel chan_template = {
    .frontend = &Frontend
  };
  set_defaults(&chan_template); // compiled-in defaults (#4)
  loadpreset(&chan_template,Configtable,GLOBAL); // [global] section (#3)

  if(loadpreset(&chan_template,Preset_table,preset) != 0) // preset database entry (#2)
    fprintf(stderr,"[%s] loadpreset(%s,%s) failed; compiled-in defaults and local settings used\n",sname,Preset_file,preset);

  strlcpy(chan_template.preset,preset,sizeof chan_template.preset);
  loadpreset(&chan_template,Configtable,sname); // this section's config (#1)

  if(chan_template.output.ttl != 0 && Template.output.ttl != 0)
    chan_template.output.ttl = Template.output.ttl; // use global ttl when both are non-zero

  // There can be multiple senders to an output stream, so let avahi suppress the duplicate addresses
  // Look quickly (2 tries max) to see if it's already in the DNS. Otherwise make a multicast address.
  uint32_t addr = 0;
  bool const use_dns = config_getboolean(Configtable,sname,"dns",Global_use_dns);

  if(!use_dns || resolve_mcast(data,&chan_template.output.dest_socket,DEFAULT_RTP_PORT,NULL,0,2) != 0)
    // If we're not using the DNS, or if resolution fails, hash name string to make IP multicast address in 239.x.x.x range
    addr = make_maddr(data);

  {
    size_t slen = sizeof(chan_template.output.dest_socket);
    // there may be several hosts with the same section names
    // prepend the host name to the service name
    char service_name[512] = {0};
    snprintf(service_name, sizeof service_name, "%s %s", Hostname, sname);
    char ttlmsg[128];
    snprintf(ttlmsg,sizeof ttlmsg,"TTL=%d",chan_template.output.ttl);
    char const *cp = config2_getstring(Configtable,Configtable,GLOBAL,sname,"encoding","s16be");
    bool const is_opus = strcasecmp(cp,"opus") == 0 ? true : false;
    avahi_start(service_name,
		is_opus ? "_opus._udp" : "_rtp._udp",
		DEFAULT_RTP_PORT,
		data,addr,ttlmsg,
		addr != 0 ? &chan_template.output.dest_socket : NULL,
		addr != 0 ? &slen : NULL);
  }

  // Set up output stream (data + status)
  // data stream is shared by all channels in this section
  // Now also used for per-channel status/control, with different port number
  chan_template.status.dest_socket = chan_template.output.dest_socket;
  setport(&chan_template.status.dest_socket,DEFAULT_STAT_PORT);
  strlcpy(chan_template.output.dest_string,data,sizeof chan_template.output.dest_string);
  chan_template.output.rtp.type = pt_from_info(chan_template.output.samprate,chan_template.output.channels,chan_template.output.encoding);

  char const *iface = NULL;
  if(chan_template.output.ttl != 0){
    // Override global defaults
    iface = config_getstring(Configtable,sname,"iface",Iface);
    join_group(Output_fd,NULL,&chan_template.output.dest_socket,iface);
  }
  // No need to also join group for status socket, since the IP addresses are the same

  int section_chans = 0; // Count demodulators started in this section
  int nchan = 0; // Count of entries in section table, including excluded ones
  struct ftab freq_table[Nchannels] = {0}; // List of frequencies to be started

  // Process "raster = start stop step" directive
  // create channels of common type from starting to ending frequency with fixed spacing
  // To work around iniparser's limited line length, we look for multiple keywords
  // "raster", "raster0", "raster1", etc, up to "raster9"
  for(int i = -1; i < 10; i++){
    char fname[10];
    if(i == -1)
      snprintf(fname,sizeof(fname),"raster");
    else
      snprintf(fname,sizeof(fname),"raster%d",i);

    char const * const flist = config_getstring(Configtable,sname,fname,NULL);
    if(flist == NULL)
      continue; // none with this prefix; look for more

    char *flist_copy = strdup(flist);
    char *next = NULL;
    char const *p = strtok_r(flist_copy," \t",&next);
    if(p == NULL)
      goto raster_error;
    double start = parse_frequency(p,true);
    if(start <= 0)
      goto raster_error;

    p = strtok_r(NULL," \t",&next);
    if(p == NULL)
      goto raster_error;
    double stop = parse_frequency(p,true);
    if(stop <= 0)
      goto raster_error;

    p = strtok_r(NULL," \t",&next);
    if(p == NULL)
      goto raster_error;
    double const step = parse_frequency(p,true);
    if(step <= 0)
      goto raster_error;

    // Ensure stop >= start
    if(start > stop){
      double tmp = start;
      start = stop;
      stop = tmp;
    }
    double tone = get_tone(sname,i);
    for(double f = start; f < stop && nchan < Nchannels; f += step){
      freq_table[nchan].valid = true;
      freq_table[nchan].tone = tone;
      freq_table[nchan++].f = f;
    }
    FREE(flist_copy);
    continue;
  raster_error:
    fprintf(stderr,"[%s]: can't parse raster command %s\n",sname,flist);
    FREE(flist_copy);
  }
  // Process freq = and freq[0-9] = directives
  // To work around iniparser's limited line length, we look for multiple keywords
  // "freq", "freq0", "freq1", etc, up to "freq9"
  for(int i = -1; i < 10; i++){
    char fname[10];
    if(i == -1)
      snprintf(fname,sizeof(fname),"freq");
    else
      snprintf(fname,sizeof(fname),"freq%d",i);

    char const * const flist = config_getstring(Configtable,sname,fname,NULL);
    if(flist == NULL)
      continue; // none with this prefix; look for more

    // Parse the frequency list(s)
    char *flist_copy = strdup(flist); // Need writeable copy for strtok
    char *next = NULL;
    for(char const *tok = strtok_r(flist_copy," \t",&next);
	tok != NULL;
	tok = strtok_r(NULL," \t",&next)){

      double const f = parse_frequency(tok,true);
      if(f < 0){
	fprintf(stderr,"[%s] can't parse frequency %s\n",sname,tok);
	continue;
      }
      double tone = get_tone(sname,i);
      if(nchan < Nchannels){
	freq_table[nchan].f = f;
	freq_table[nchan].tone = tone;
	freq_table[nchan++].valid = true;
      }
    }
    FREE(flist_copy);
  }
  // Process "except" directive(s) by flagging entries invalid
  // Useful for knocking out specific elements of rasters, e.g., containing spurs
  qsort(freq_table,nchan,sizeof freq_table[0],fcompare); // Sort for binary searching
  for(int i = -1; i < 10; i++){
    char ename[10];
    if(i == -1)
      snprintf(ename,sizeof(ename),"except");
    else
      snprintf(ename,sizeof(ename),"except%d",i);

    char const * const elist = config_getstring(Configtable,sname,ename,NULL);
    if(elist == NULL)
      continue; // none with this prefix; look for more

    // Parse the frequency list(s)
    char *elist_copy = strdup(elist); // Need writeable copy for strtok
    char *next = NULL;
    for(char const *tok = strtok_r(elist_copy," \t",&next);
	tok != NULL;
	tok = strtok_r(NULL," \t",&next)){

      double const f = parse_frequency(tok,true);
      if(f < 0){
	fprintf(stderr,"[%s] can't parse frequency %s\n",sname,tok);
	continue;
      }
      struct ftab *ftp = bsearch(&f,freq_table,nchan,sizeof freq_table[0],tcompare);
      if(ftp != NULL)
	ftp->valid = false;
    }
    FREE(elist_copy);
  }
  // Finally spawn the demods from the list
  // No manual ssrcs for now, maybe add back in later?
  for(int i = 0; i < nchan; i++){
    // Generate default ssrc from frequency
    if(!freq_table[i].valid)
      continue;

    uint32_t ssrc = (uint32_t)round(freq_table[i].f / 1000.0); // Kilohertz

    struct channel *chan = NULL;
    // Try to create it, incrementing in case of collision
    int const max_collisions = 100;
    for(int i=0; i < max_collisions; i++,ssrc++){
      chan = create_chan(ssrc);
      if(chan != NULL)
	break;
    }
    if(chan == NULL){
      fprintf(stderr,"Can't allocate requested ssrc in range %u-%u\n",ssrc-max_collisions,ssrc);
      continue;
    }
    // Initialize from template, set frequency and start
    // Be careful with shallow copies like this; although the pointers in the channel structure are still NULL
    // the ssrc and inuse fields are active and must be cleaned up. Are there any others...?
    *chan = chan_template;
    chan->output.rtp.ssrc = ssrc; // restore after template copy
    chan->fm.tone_freq = freq_table[i].tone;
    set_freq(chan,freq_table[i].f);
    start_demod(chan);
    Total_channels++;
    section_chans++;
    if(SAP_enable){
      // Highly experimental, off by default
      char sap_dest[] = "224.2.127.254:9875"; // sap.mcast.net
      resolve_mcast(sap_dest,&chan->sap.dest_socket,0,NULL,0,0);
      if(chan_template.output.ttl != 0)
	join_group(Output_fd,NULL,&chan->sap.dest_socket,iface);
      pthread_create(&chan->sap.thread,NULL,sap_send,chan);
    }
    // RTCP Real Time Control Protocol daemon is optional
    if(RTCP_enable){
      // Set the dest socket to the RTCP port on the output group
      // What messy code just to overwrite a structure field, eh?
      chan->rtcp.dest_socket = chan->output.dest_socket;
      setport(&chan->rtcp.dest_socket,DEFAULT_RTCP_PORT);
      pthread_create(&chan->rtcp.thread,NULL,rtcp_send,chan);
    }
  }
  fprintf(stderr,"[%s] %d channels started\n",sname,section_chans);
  return NULL;
}
// Find chan by ssrc
struct channel *lookup_chan(uint32_t ssrc){
  struct channel *chan = NULL;
  pthread_mutex_lock(&Channel_list_mutex);
  for(int i=0; i < Nchannels; i++){
    if(Channel_list[i].inuse && Channel_list[i].output.rtp.ssrc == ssrc){
      chan = &Channel_list[i];
      break;
    }
  }
  pthread_mutex_unlock(&Channel_list_mutex);
  return chan;
}


// Atomically create chan only if the ssrc doesn't already exist
struct channel *create_chan(uint32_t ssrc){
  if(ssrc == 0xffffffff)
    return NULL; // reserved
  pthread_mutex_lock(&Channel_list_mutex);
  for(int i=0; i < Nchannels; i++){
    if(Channel_list[i].inuse && Channel_list[i].output.rtp.ssrc == ssrc){
      pthread_mutex_unlock(&Channel_list_mutex);
      return NULL; // sorry, already taken
    }
  }
  struct channel *chan = NULL;
  for(int i=0; i < Nchannels; i++){
    if(!Channel_list[i].inuse){
      chan = &Channel_list[i];
      break;
    }
  }
  if(chan == NULL){
    fprintf(stderr,"Warning: out of chan table space (%'d)\n",Active_channel_count);
    // Abort here? Or keep going?
  } else {
    // Because the memcpy clobbers the ssrc, we must keep the lock held on Channel_list_mutex
    *chan = Template; // Template.inuse is already set
    chan->frontend = &Frontend; // Should be already set in template, but just be sure
    chan->output.rtp.ssrc = ssrc; // Stash it
    Active_channel_count++;
    assert(Blocktime != 0);
    chan->lifetime = (int)round(20. / Blocktime); // If freq == 0, goes away 20 sec after last command
  }
  pthread_mutex_unlock(&Channel_list_mutex);
  return chan;
}



static void *demod_thread(void *p){
  assert(p != NULL);
  struct channel *chan = (struct channel *)p;
  if(chan == NULL)
    return NULL;

  pthread_detach(pthread_self());

  // Repeatedly invoke appropriate demodulator
  // When a demod exits, the appropriate one is restarted,
  // which can be the same one if demod_type hasn't changed
  // A demod can terminate completely by setting an invalid demod_type and exiting
  int status = 0;
  while(status == 0){ // A demod returns non-zero to signal a fatal error, don't restart
    switch(chan->demod_type){
    case LINEAR_DEMOD:
      status = demod_linear(p);
      break;
    case FM_DEMOD:
      status = demod_fm(p);
      break;
    case WFM_DEMOD:
      status = demod_wfm(p);
      break;
    case SPECT_DEMOD:
      status = demod_spectrum(p);
      break;
    default:
      status = -1; // Unknown demod, quit
      break;
    }
  }
  close_chan(chan);
  return NULL;
}

// start demod thread on already-initialized chan structure
int start_demod(struct channel * chan){
  assert(chan != NULL);
  if(chan == NULL)
    return -1;

  if(Verbose){
    fprintf(stderr,"start_demod: ssrc %'u, output %s, demod %d, freq %'.3lf, preset %s, filter (%'+.0f,%'+.0f)\n",
	    chan->output.rtp.ssrc, chan->output.dest_string, chan->demod_type, chan->tune.freq, chan->preset, chan->filter.min_IF, chan->filter.max_IF);
  }
  pthread_create(&chan->demod_thread,NULL,demod_thread,chan);
  return 0;
}

// Called by a demodulator to clean up its own resources
int close_chan(struct channel *chan){
  if(chan == NULL)
    return -1;

  pthread_t nullthread = {0};

  if(chan->rtcp.thread != nullthread){
    pthread_cancel(chan->rtcp.thread);
    pthread_join(chan->rtcp.thread,NULL);
  }
  if(chan->sap.thread != nullthread){
    pthread_cancel(chan->sap.thread);
    pthread_join(chan->sap.thread,NULL);
  }

  pthread_mutex_lock(&chan->status.lock);
  FREE(chan->status.command);
  FREE(chan->spectrum.bin_data);
  delete_filter_output(&chan->filter.out);
  if(chan->opus.encoder != NULL){
    opus_encoder_destroy(chan->opus.encoder);
    chan->opus.encoder = NULL;
  }
  pthread_mutex_unlock(&chan->status.lock);
  pthread_mutex_lock(&Channel_list_mutex);
  if(chan->inuse){
    // Should be set, but check just in case to avoid messing up Active_channel_count
    chan->inuse = false;
    Active_channel_count--;
  }
  pthread_mutex_unlock(&Channel_list_mutex);
  return 0;
}

// Set receiver frequency
// The new IF is computed here only to determine if the front end needs retuning
// The second LO frequency is actually set when the new front end frequency is
// received back from the front end metadata
double set_freq(struct channel * const chan,double const f){
  assert(chan != NULL);
  if(chan == NULL)
    return NAN;

  assert(!isnan(f));
  chan->tune.freq = f;

  // Tuning to 0 Hz is a special case, don't move front end
  // Essentially disables a chan
  if(f == 0)
    return f;

  pthread_mutex_lock(&Freq_mutex); // Protect front end tuner
  // Determine new IF
  double new_if = f - Frontend.frequency;

  // Flip sign to convert LO2 frequency to IF carrier frequency
  // Tune an extra kHz to account for front end roundoff
  // Ideally the front end would just round in a preferred direction
  // but it doesn't know where our IF will be so it can't make the right choice
  // Retuning the front end will cause all the other channels to recalculate their own IFs
  // What if the IF is wider than the receiver can supply?
  double const fudge = 1000;
  if(new_if > Frontend.max_IF - chan->filter.max_IF){
    // Retune LO1 as little as possible
    new_if = Frontend.max_IF - chan->filter.max_IF - fudge;
    set_first_LO(chan,f - new_if);
  } else if(new_if < Frontend.min_IF - chan->filter.min_IF){
    // Also retune LO1 as little as possible
    new_if = Frontend.min_IF - chan->filter.min_IF + fudge;
    set_first_LO(chan,f - new_if);
  }
  pthread_mutex_unlock(&Freq_mutex);
  return f;
}

// Set first (front end tuner) oscillator
// Note: single precision floating point is not accurate enough at VHF and above
// chan->first_LO is NOT updated here!
// It is set by incoming status frames so this will take time
double set_first_LO(struct channel const * const chan,double const first_LO){
  assert(chan != NULL);
  if(chan == NULL)
    return NAN;

  double const current_lo1 = Frontend.frequency;

  // Just return actual frequency without changing anything
  if(first_LO == current_lo1 || first_LO <= 0)
    return first_LO;

  // Direct tuning through local module if available
  if(Frontend.tune != NULL)
    return (*Frontend.tune)(&Frontend,first_LO);

  return first_LO;
}

/* Compute FFT bin shift and time-domain fine tuning offset for specified LO frequency
 N = input fft length
 M = input buffer overlap
 freq = frequency to mix by (double). If negative, shift will be negative
 samprate = input sample rate
 remainder = fine LO frequency (double)

 This version tunes to arbitrary FFT bin rotations and computes the necessary
 block phase correction factor described in equation (12) of
 "Analysis and Design of Efficient and Flexible Fast-Convolution Based Multirate Filter Banks"
 by Renfors, Yli-Kaakinen & Harris, IEEE Trans on Signal Processing, Aug 2014

 Essentially just a modulo function; divide frequency by the width of each bin (eg 40 Hz), returning
 an integer quotient and a double remainder, e.g, +/- 20 Hz
*/
int compute_tuning(int N, int M, double samprate,int *shift,double *remainder, double freq){
  double const hzperbin = samprate / N;

#if 0
  // Round to multiples of V (not needed anymore)
  int const V = N / (M-1);
  int const r = (int)(V * round((freq/hzperbin) / V));
#else
  (void)M;
  int const r = (int)round(freq/hzperbin);
#endif

  if(shift)
    *shift = r;

  if(remainder)
    *remainder = freq - (r * hzperbin);

  // Check if there's no overlap in the range we want
  // Intentionally allow real input to go both ways, for front ends with high and low side injection
  // Even though only one works, this lets us manually check for images
  // No point in tuning to aliases, though
  if(abs(r) >= N/2)
    return -1; // Chan thread will wait for the front end status to change
  return 0;
}

// RTP control protocol sender task
static void *rtcp_send(void *arg){
  struct channel *chan = (struct channel *)arg;
  if(chan == NULL)
    pthread_exit(NULL);

  {
    char name[100];
    snprintf(name,sizeof(name),"rtcp %u",chan->output.rtp.ssrc);
    pthread_setname(name);
  }

  int64_t Starttime = gps_time_ns();      // System clock at timestamp 0, for RTCP
  while(true){

    if(chan->output.rtp.ssrc == 0) // Wait until it's set by output RTP subsystem
      goto done;
    uint8_t buffer[PKTSIZE]; // much larger than necessary
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
    sr.rtp_timestamp = (unsigned)((0 + gps_time_ns() - Starttime) / BILLION);
    sr.packet_count = chan->output.rtp.seq;
    sr.byte_count = (unsigned)chan->output.rtp.bytes;

    uint8_t *dp = gen_sr(buffer,sizeof(buffer),&sr,NULL,0);

    // Construct SDES
    struct rtcp_sdes sdes[4];

    // CNAME
    char *string = NULL;
    int sl = asprintf(&string,"radio@%s",Hostname);
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

    if(sendto(Output_fd,buffer,dp-buffer,0,&chan->rtcp.dest_socket,sizeof(chan->rtcp.dest_socket)) < 0)
      chan->output.errors++;
  done:;
    sleep(1);
  }
}

/* Session announcement protocol - highly experimental, off by default
   The whole point was to make it easy to use VLC and similar tools, but they either don't actually implement SAP (e.g. in iOS)
   or implement some vague subset that you have to guess how to use
   Will probably work better with Opus streams from the opus transcoder, since they're always 48000 Hz stereo; no switching midstream
*/
static void *sap_send(void *p){
  struct channel *chan = (struct channel *)p;
  assert(chan != NULL);
  if(chan == NULL)
    return NULL;

  int64_t start_time = utc_time_sec() + NTP_EPOCH; // NTP uses UTC, not GPS

  // These should change when a change is made elsewhere
  uint16_t const id = (uint16_t)random(); // Should be a hash, but it changes every time anyway
  int const sess_version = 1;

  for(;;){
    char message[PKTSIZE],*wp;
    int space = sizeof(message);
    wp = message;

    *wp++ = 0x20; // SAP version 1, ipv4 address, announce, not encrypted, not compressed
    *wp++ = 0; // No authentication
    *wp++ = id >> 8;
    *wp++ = id & 0xff;
    space -= 4;

    // our sending ipv4 address
    struct sockaddr_in const *sin = (struct sockaddr_in *)&chan->output.source_socket;
    uint32_t *src = (uint32_t *)wp;
    *src = sin->sin_addr.s_addr; // network byte order
    wp += 4;
    space -= 4;

    int len = snprintf(wp,space,"application/sdp");
    wp += len + 1; // allow space for the trailing null
    space -= (len + 1);

    // End of SAP header, beginning of SDP

    // Version v=0 (always)
    len = snprintf(wp,space,"v=0\r\n");
    wp += len;
    space -= len;

    {
      // Originator o=
      char hostname[sysconf(_SC_HOST_NAME_MAX)];
      gethostname(hostname,sizeof(hostname));

      struct passwd pwd,*result = NULL;
      char buf[1024];

      getpwuid_r(getuid(),&pwd,buf,sizeof(buf),&result);
      len = snprintf(wp,space,"o=%s %lld %d IN IP4 %s\r\n",
		     result ? result->pw_name : "-",
		     (long long)start_time,sess_version,hostname);

      wp += len;
      space -= len;
    }

    // s= (session name)
    len = snprintf(wp,space,"s=radio %s\r\n",Frontend.description);
    wp += len;
    space -= len;

    // i= (human-readable session information)
    len = snprintf(wp,space,"i=PCM output stream from ka9q-radio on %s\r\n",Frontend.description);
    wp += len;
    space -= len;

    {
      char *mcast = strdup(formatsock(&chan->output.dest_socket,false));
      assert(mcast != NULL);
      // Remove :port field, confuses the vlc listener
      char *cp = strchr(mcast,':');
      if(cp)
	*cp = '\0';
      len = snprintf(wp,space,"c=IN IP4 %s/%d\r\n",mcast,chan->output.ttl);
      wp += len;
      space -= len;
      FREE(mcast);
    }


#if 0 // not currently used
    int64_t current_time = utc_time_sec() + NTP_EPOCH;
#endif

    // t= (time description)
    len = snprintf(wp,space,"t=%lld %lld\r\n",(long long)start_time,0LL); // unbounded
    wp += len;
    space -= len;

    // m = media description
    // set from current state. This will require changing the session version and IDs, and
    // it's not clear that clients like VLC will do the right thing anyway
    len = snprintf(wp,space,"m=audio 5004/1 RTP/AVP %d\r\n",chan->output.rtp.type);
    wp += len;
    space -= len;

    len = snprintf(wp,space,"a=rtpmap:%d %s/%d/%d\r\n",
		   chan->output.rtp.type,
		   encoding_string(chan->output.encoding),
		   chan->output.samprate,
		   chan->output.channels);
    wp += len;
    space -= len;

    int const outsock = chan->output.ttl != 0 ? Output_fd : Output_fd0;
      if(sendto(outsock,message,wp - message,0,&chan->sap.dest_socket,sizeof(chan->sap.dest_socket)) < 0)
      chan->output.errors++;
    sleep(5);
  }
}

// Run top-of-loop stuff common to all demod types
// 1. If dynamic and sufficiently idle, terminate
// 2. Process any commands from the common command/status channel
// 3. Send any requested delayed status to the common status channel
// 4. Send any status to the output channel
//    if the processed command requires a restart, return +1
// 5. Block until front end is in range
// 6. compute FFT bin shift & fine tuning remainder
// 7. Set fine tuning oscillator frequency & phase
// 8. Run output half (IFFT) of filter
// 9. Update noise estimate
// 10. Run fine tuning, compute average power

// Baseband samples placed in chan->filter.out->output.c
int downconvert(struct channel *chan){
  assert(chan != NULL);
  if(chan == NULL)
    return -1;

  assert(Blocktime != 0);

  int shift = 0;
  double remainder = 0;

  while(true){
    // Should we die?
    // Will be slower if 0 Hz is outside front end coverage because of slow timed wait below
    // But at least it will eventually go away
    if(chan->tune.freq == 0 && chan->lifetime > 0 && --chan->lifetime <= 0){
      chan->demod_type = -1;  // No demodulator
      if(Verbose > 1)
	fprintf(stderr,"chan %u terminate needed\n",chan->output.rtp.ssrc);
      return -1; // terminate needed
    }
    // Process any commands and return status
    bool restart_needed = false;
    pthread_mutex_lock(&chan->status.lock);

    if(chan->status.output_interval != 0 && chan->status.output_timer == 0 && !chan->output.silent)
      chan->status.output_timer = 1; // channel has become active, send update on this pass

    // Look on the single-entry command queue and grab it atomically
    if(chan->status.command != NULL){
      restart_needed = decode_radio_commands(chan,chan->status.command,chan->status.length);
      FREE(chan->status.command);
      // When a spectrum restart is needed, blow away old bin data so it won't get sent with this status response
      if(chan->demod_type == SPECT_DEMOD && restart_needed)
	FREE(chan->spectrum.bin_data);

      send_radio_status(&Frontend.metadata_dest_socket,&Frontend,chan); // Send status in response
      chan->status.global_timer = 0; // Just sent one
      // Also send to output stream
      if(chan->demod_type != SPECT_DEMOD){
	// Only send spectrum on status channel, and only in response to poll
	// Spectrum channel output socket isn't set anyway
	send_radio_status(&chan->status.dest_socket,&Frontend,chan);
      }
      chan->status.output_timer = chan->status.output_interval; // Reload
      reset_radio_status(chan); // After both are sent
    } else if(chan->status.global_timer != 0 && --chan->status.global_timer <= 0){
      // Delayed status request, used mainly by all-channel polls to avoid big bursts
      send_radio_status(&Frontend.metadata_dest_socket,&Frontend,chan); // Send status in response
      chan->status.global_timer = 0; // to make sure
      reset_radio_status(chan);
    } else if(chan->status.output_interval != 0 && chan->status.output_timer > 0){
      // Timer is running for status on output stream
      if(--chan->status.output_timer == 0){
	// Timer has expired; send status on output channel
	send_radio_status(&chan->status.dest_socket,&Frontend,chan);
	reset_radio_status(chan);
	if(!chan->output.silent)
	  chan->status.output_timer = chan->status.output_interval; // Restart timer only if channel is active
      }
    }

    pthread_mutex_unlock(&chan->status.lock);
    if(restart_needed){
      if(Verbose > 1)
	fprintf(stderr,"chan %u restart needed\n",chan->output.rtp.ssrc);
      return +1; // Restart needed
    }
    // To save CPU time when the front end is completely tuned away from us, block (with timeout) until the front
    // end status changes rather than process zeroes. We must still poll the terminate flag.
    pthread_mutex_lock(&Frontend.status_mutex);

    // Sign conventions are reversed and simplified from before
    // When RF > LO, tune.second_LO is still negative but shift is now positive
    // When RF < LO, tune.second_LO is still positive but shift is now negative
    chan->tune.second_LO = Frontend.frequency - chan->tune.freq;
    double const freq = -(chan->tune.doppler + chan->tune.second_LO); // Total logical oscillator frequency
    if(compute_tuning(Frontend.in.ilen + Frontend.in.impulse_length - 1,
		      Frontend.in.impulse_length,
		      Frontend.samprate,
		      &shift,&remainder,freq) != 0){
      // No front end coverage of our carrier; wait one block time for it to retune
      chan->sig.bb_power = 0;
      chan->output.power = 0;
      struct timespec timeout; // Needed to avoid deadlock if no front end is available
      clock_gettime(CLOCK_REALTIME,&timeout);
      timeout.tv_nsec += (long)(Blocktime * BILLION); // seconds to nanoseconds
      if(timeout.tv_nsec > BILLION){
	timeout.tv_sec += 1; // 1 sec in the future
	timeout.tv_nsec -= BILLION;
      }
      pthread_cond_timedwait(&Frontend.status_cond,&Frontend.status_mutex,&timeout);
      pthread_mutex_unlock(&Frontend.status_mutex);
      continue;
    }
    pthread_mutex_unlock(&Frontend.status_mutex);
    chan->tp1 = shift;
    chan->tp2 = remainder;

    execute_filter_output(&chan->filter.out,shift); // block until new data frame
    chan->status.blocks_since_poll++;

    if(chan->filter.out.output.c == NULL){
      chan->filter.bin_shift = shift; // Needed by spectrum.c in wideband mode
      return 0; // Probably in spectrum mode, nothing more to do
    }
    // Compute and exponentially smooth noise estimate
    if(isnan(chan->sig.n0))
      chan->sig.n0 = estimate_noise(chan,shift);
    else {
      // Use double to minimize risk of denormalization in the smoother
      double diff = estimate_noise(chan,shift) - chan->sig.n0;
      chan->sig.n0 += Power_alpha * diff;
    }

    // set fine tuning frequency & phase
    // avoid them both being 0 at startup; init chan->filter.remainder as NAN
    if(shift != chan->filter.bin_shift || remainder != chan->filter.remainder){ // Detect startup
      assert(isfinite(chan->tune.doppler_rate));
      set_osc(&chan->fine,-remainder/chan->output.samprate,chan->tune.doppler_rate/(chan->output.samprate * chan->output.samprate));
      chan->filter.remainder = remainder;
    }
    /* Block phase adjustment (folded into the fine tuning osc) in two parts:
       (a) phase_adjust is applied on each block when FFT bin shifts aren't divisible by V; otherwise it's unity
       (b) second term keeps the phase continuous when shift changes; found empirically, dunno yet why it works!
       Be sure to Initialize chan->filter.bin_shift at startup to something bizarre to force this inequality on first call
       See "Analysis and Design of Efficient and Flexible Fast-Convolution Based Multirate Filter Banks" by Renfors, Yli-Kaakinen and harris,
       equation (12).
    */
    if(shift != chan->filter.bin_shift){
      const int V = 1 + (Frontend.in.ilen / (Frontend.in.impulse_length - 1)); // Overlap factor
      chan->filter.phase_adjust = cispi(2.0*(shift % V)/(double)V); // Amount to rotate on each block for shifts not divisible by V
      chan->fine.phasor *= cispi((shift - chan->filter.bin_shift) / (-2.0 * (V-1))); // One time adjust for shift change
      chan->filter.bin_shift = shift;
    }
    chan->fine.phasor *= chan->filter.phase_adjust;

    // Make fine tuning correction before secondary filtering
    for(int n=0; n < chan->filter.out.olen; n++)
      chan->filter.out.output.c[n] *= step_osc(&chan->fine);

    if(chan->filter2.blocking == 0){
      // No secondary filtering, done
      chan->baseband = chan->filter.out.output.c;
      chan->sampcount = chan->filter.out.olen;
    } else {
      int r = write_cfilter(&chan->filter2.in,chan->filter.out.output.c,chan->filter.out.olen); // Will trigger execution of input side if buffer is full, returning 1
      if(r == 0)
	continue; // Filter 2 not finishd, wait for another block
      execute_filter_output(&chan->filter2.out,0); // No frequency shifting
      chan->baseband = chan->filter2.out.output.c;
      chan->sampcount = chan->filter2.out.olen;
    }
    double energy = 0;
    for(int n=0; n < chan->sampcount; n++)
      energy += cnrmf(chan->baseband[n]);
    chan->sig.bb_power = energy / chan->sampcount;
    return 0;
  }
  return 0; // Should not actually be reached
}
void response(struct channel *chan,bool response_needed){
  if(chan == NULL)
    return;

  pthread_mutex_lock(&chan->status.lock);
  if(chan->status.output_interval != 0 && chan->status.output_timer == 0 && !chan->output.silent)
    chan->status.output_timer = 1; // channel has become active, send update on this pass
  struct frontend const *frontend = chan->frontend;

  if(response_needed){
    send_radio_status(&frontend->metadata_dest_socket,frontend,chan); // Send status in response
    chan->status.global_timer = 0; // Just sent one
    // Also send to output stream
    // Only send spectrum on status channel, and only in response to poll
    send_radio_status(&chan->status.dest_socket,frontend,chan);
    chan->status.output_timer = chan->status.output_interval; // Reload
    reset_radio_status(chan); // After both are sent
  } else if(chan->status.global_timer != 0 && --chan->status.global_timer <= 0){
    // Delayed status request, used mainly by all-channel polls to avoid big bursts
    send_radio_status(&frontend->metadata_dest_socket,frontend,chan); // Send status in response
    chan->status.global_timer = 0; // to make sure
    reset_radio_status(chan);
  } else if(chan->status.output_interval != 0 && chan->status.output_timer > 0 && --chan->status.output_timer == 0){
    // Output stream status timer has expired; send status on output channel
    send_radio_status(&chan->status.dest_socket,frontend,chan);
    reset_radio_status(chan);
    if(!chan->output.silent)
      chan->status.output_timer = chan->status.output_interval; // Restart timer only if channel is active
  }
  pthread_mutex_unlock(&chan->status.lock);
}



int set_channel_filter(struct channel *chan){
  // Limit to Nyquist rate
  double lower = max(chan->filter.min_IF, -(double)chan->output.samprate/2);
  double upper = min(chan->filter.max_IF, (double)chan->output.samprate/2);

  if(Verbose > 1)
    fprintf(stderr,"new filter for chan %'u: IF=[%'.0f,%'.0f], samprate %'d, kaiser beta %.1f\n",
	    chan->output.rtp.ssrc, lower, upper,
	    chan->output.samprate, chan->filter.kaiser_beta);

  delete_filter_output(&chan->filter2.out);
  delete_filter_input(&chan->filter2.in);
  if(chan->filter2.blocking > 0){
    assert(Blocktime != 0);
    int const blocksize = (int)round(chan->filter2.blocking * chan->output.samprate * Blocktime);
    double const binsize = (1.0 / Blocktime) * ((double)(Overlap - 1) / Overlap);
    double const margin = 4 * binsize; // 4 bins should be enough even for large Kaiser betas

    int n = round2(2 * blocksize); // Overlap >= 50%
    int order = n - blocksize;
    if(Verbose > 1)
       fprintf(stderr,"filter2 create: L = %d, M = %d, N = %d\n",blocksize,order+1,n);
    // Secondary filter running at 1:1 sample rate with order = filter2.blocking * inblock
    create_filter_input(&chan->filter2.in,blocksize,order+1,COMPLEX);
    chan->filter2.in.perform_inline = true;
    create_filter_output(&chan->filter2.out,&chan->filter2.in,NULL,blocksize, COMPLEX);
    chan->filter2.low = lower;
    chan->filter2.high = upper;
    if(chan->filter2.kaiser_beta < 0 || !isfinite(chan->filter2.kaiser_beta))
      chan->filter2.kaiser_beta = chan->filter.kaiser_beta;
    set_filter(&chan->filter2.out,
	       lower/chan->output.samprate,
	       upper/chan->output.samprate,
	       chan->filter2.kaiser_beta);
    // Widen the main filter a little to keep its broad skirts from cutting into filter2's response
    // I.e., the main filter becomes a roofing filter
    // Again limit to Nyquist rate
    lower -= margin;
    lower = max(lower, -(double)chan->output.samprate/2);
    upper += margin;
    upper = min(upper, (double)chan->output.samprate/2);
  }
  // Set main filter
  set_filter(&chan->filter.out,
	     lower/chan->output.samprate,
	     upper/chan->output.samprate,
	     chan->filter.kaiser_beta);

  chan->filter.remainder = NAN; // Force re-init of fine oscillator
  return 0;
}

// scale A/D output power to full scale for monitoring overloads
double scale_ADpower2FS(struct frontend const *frontend){
  assert(frontend != NULL);
  if(frontend == NULL)
    return NAN;

  assert(frontend->bitspersample > 0);
  double scale = 1.0 / (1 << (frontend->bitspersample - 1)); // Important to force the numerator to double, otherwise the divide produces zero!
  scale *= scale;
  // Scale real signals up 3 dB so a rail-to-rail sine will be 0 dBFS, not -3 dBFS
  // Complex signals carry twice as much power, divided between I and Q
  if(frontend->isreal)
    scale *= 2;
  return scale;
}
// Returns multiplicative factor for converting raw samples to doubles with analog gain correction
double scale_AD(struct frontend const *frontend){
  assert(frontend != NULL);
  if(frontend == NULL)
    return NAN;

  assert(frontend->bitspersample > 0);
  double scale = (1 << (frontend->bitspersample - 1));

  // net analog gain, dBm to dBFS, that we correct for to maintain unity gain, i.e., 0 dBm -> 0 dBFS

  double analog_gain = frontend->rf_gain - frontend->rf_atten;
  if(isfinite(frontend->rf_level_cal))
    analog_gain -= frontend->rf_level_cal; // new sign convention
  if(frontend->isreal)
    analog_gain -= 3.0;
  // Will first get called before the filter input is created
  return dB2voltage(-analog_gain) / scale; // Front end gain as amplitude ratio
}

/*
==============================================================================
 Real-Time Noise Floor Estimation Specification
==============================================================================

Overview:
---------
This method provides fast, robust, and mathematically sound estimation of
the background noise floor (N0) from FFT bin powers in real-time SDR
applications. It works on short timescales without long-term averaging,
yet remains unbiased and resilient to signal contamination.

Algorithm:
----------
1. Calculate power in FFT bins from rectangular windowed FFT.
2. Select quantile (q) of bin powers (e.g. 10% quantile).
3. Determine threshold (T) as multiplier of quantile (e.g. T = 1.5).
4. Select bins where power < T * quantile.
5. Compute the average power of selected bins.
6. Apply correction factor C to obtain unbiased N0 estimate:

    z = T * (-ln(1 - q))
    C = 1 / [1 - (z * exp(-z)) / (1 - exp(-z))]
    N0_estimate = mean(selected_bins) * C

7. Exponentially smooth the N0 estimate in linear power domain:

    N0_smoothed += alpha * (N0_estimate - N0_smoothed);

Recommended Parameters:
-----------------------
- Quantile (q):      0.10 (10%)
- Threshold (T):     1.5
- Smoothing alpha:   0.1 (at 50 Hz block update rate)

This provides:
- Low bias
- Low variance
- Fast response (approx 0.6–1 second adaptation)
- Robustness against signal contamination

SNR Calculation:
----------------
True S/N (excluding noise):

    SNR_linear = max(0, (S_measured - B * N0_smoothed) / (B * N0_smoothed))
    SNR_dB = 10 * log10(SNR_linear)

Notes:
- log(0) -> -inf, which is correct
- Negative S -> clamped to zero before log to avoid NaN

Advantages over Older Min-based Methods:
----------------------------------------
- No bias from minimum selection
- Exact correction factor for unbiased estimation
- Fast response without long-term smoothing
- Tunable tradeoffs between purity and smoothness

Recommended Application:
------------------------
- SDR receive channels
- AGC thresholding
- SNR reporting and squelch
- Fast-changing noise environments (HF, FT8, QRM)
*/

// Written by ChatGPT to analyze noise stats
// Swap two doubles
static void swap(double *a, double *b) {
    double tmp = *a;
    *a = *b;
    *b = tmp;
}

// Partition step for quickselect
static int partition(double *arr, int left, int right, int pivot_index) {
    double pivot_value = arr[pivot_index];
    swap(&arr[pivot_index], &arr[right]); // Move pivot to end
    int store_index = left;

    for (int i = left; i < right; i++) {
        if (arr[i] < pivot_value) {
            swap(&arr[store_index], &arr[i]);
            store_index++;
        }
    }

    swap(&arr[right], &arr[store_index]); // Move pivot to final place
    return store_index;
}

// Quickselect: find the k-th smallest element (0-based index)
static double quickselect(double *arr, int left, int right, int k) {
    while (left < right) {
        int pivot_index = left + (right - left) / 2;
        int pivot_new = partition(arr, left, right, pivot_index);
        if (pivot_new == k)
            return arr[k];
        else if (k < pivot_new)
            right = pivot_new - 1;
        else
            left = pivot_new + 1;
    }
    return arr[left];
}

// Compute the p-quantile (0 <= p <= 1) of array[0..n-1]
static double quantile(double *array, int n, double p) {
    if (n == 0) return NAN;

    double pos = p * (n - 1);
    int i = (int)floor(pos);
    double frac = pos - i;

    double q1 = quickselect(array, 0, n - 1, i);

    if (frac == 0.0)
        return q1;
    else {
        double q2 = quickselect(array, 0, n - 1, i + 1);
        return q1 + frac * (q2 - q1);  // Linear interpolation
    }
}

// Complex Gaussian noise has a Rayleigh amplitude distribution. The square of the amplitudes,
// ie the energies, has an exponential distribution. The mean of an exponential distribution
// is the mean of the samples, and the standard deviation is equal to the mean.
// However, the distribution is skewed, so you have to compensate for this when computing means from partial averages
// ChatGPT helped me work out the math; its reasoning is summarized in docs/noise.md
// I'm using its method 3 (average of bins below a threshold)
static double estimate_noise(struct channel *chan,int shift){
  assert(chan != NULL);
  if(chan == NULL)
    return NAN;
  struct filter_out *slave = &chan->filter.out;
  assert(slave != NULL);
  if(slave == NULL)
    return NAN;
  if(slave->bins <= 0)
    return 0;

  int nbins = slave->bins;
  if(nbins < Min_noise_bins)
    nbins = Min_noise_bins;

  double energies[nbins];
  struct filter_in const * const master = slave->master;
  // slave->next_jobnum already incremented by execute_filter_output
  float complex const * const fdomain = master->fdomain[(slave->next_jobnum - 1) % ND];

  if(master->in_type == REAL){
    // Only half as many bins as with complex input, all positive or all negative
    // if shift < 0, the spectrum is inverted and we'll look at it in reverse order but that's OK
    // Look between -Fs/2 and +Fs/2. If this bounces too much we *could* look wider to hopefully find more noise bins to average

    // New algorithm (thanks to ChatGPT) responds instantly to changes because it averages noise across frequency rather than time
    // but is a little more wobbly than the old minimum-of-time-smoothed-energies because it doesn't average as much
    // Higher sample rates will give more stable results because more noise-only bins will be averaged
    //
    int mbin = abs(shift) - nbins/2; // lower edge
    if(mbin < 0)
      mbin = 0; // Don't let the window go below DC
    else if (mbin + nbins > master->bins)
      mbin = master->bins - nbins; // or above nyquist

    for(int i=0; i < nbins; i++,mbin++)
      energies[i] = cnrmf(fdomain[mbin]);
  } else { // Complex input
    int mbin = shift - nbins/2; // Start at lower channel edge (upper for inverted real)
    // Complex input that often straddles DC
    if(mbin < 0)
      mbin += master->bins; // starting in negative frequencies
    else if(mbin >= master->bins)
      mbin -= master->bins;
    if(mbin < 0 || mbin >= master->bins)
      return 0; // wraparound gives me a headache. Just give up

    for(int i=0; i < nbins; i++){
      energies[i] = cnrmf(fdomain[mbin]);
      if(++mbin == master->bins)
	mbin = 0; // wrap around from neg freq to pos freq
      if(mbin == master->bins/2)
	break; // fallen off the right edge
    }
  }
  // Not sure if this could be numerically unstable, but use double anyway especially since it's only executed once
  static double correction = 0;
  if(correction == 0){
    // Compute correction only once
    double z = N_cutoff * (-log(1-NQ));
    correction = 1 / (1 - z*exp(-z)/(1-exp(-z)));
  }

  double en = N_cutoff * quantile(energies,nbins,NQ); // energy in the 10th quantile bin
  // average the noise-only bins, excluding signal bins above 1.5 * q
  double energy = 0;
  int noisebins = 0;
  for(int i=0; i < nbins; i++){
    if(energies[i] <= en){
      energy += energies[i];
      noisebins++;
    }
  }
  if(noisebins == 0)
    return 0; // No noise bins?

  energy /= noisebins;
  // Scale for distribution
  double noise_bin_energy = energy * correction;

  // correct for FFT scaling and normalize to 1 Hz
  // With an unnormalized FFT, the noise energy in each bin scales proportionately with the number of points in the FFT
  return noise_bin_energy / ((double)master->bins * Frontend.samprate);
}
static int fcompare(void const *ap, void const *bp){
  struct ftab *a = (struct ftab *)ap;
  struct ftab *b = (struct ftab *)bp;
  return (a->f > b->f) ? +1 : (a->f < b->f) ? -1 : 0;
}
static int tcompare(void const *ap,void const *bp){
  double a = *(double *)ap;
  struct ftab *t = (struct ftab *)bp;
  return (a > t->f) ? +1 : (a < t->f) ? -1 : 0;
}
static double get_tone(char const *sname,int i){
  // Any matching PL tones?
  // "tone", "pl" and "ctcss" are synonyms
  double tone = 0;
  char tmp[20];
  if(i == -1)
    snprintf(tmp,sizeof tmp, "tone");
  else
    snprintf(tmp,sizeof tmp, "tone%d",i);
  tone = config_getdouble(Configtable,sname,tmp,tone);

  if(i == -1)
    snprintf(tmp,sizeof tmp, "pl");
  else
    snprintf(tmp,sizeof tmp, "pl%d",i);
  tone = config_getdouble(Configtable,sname,tmp,tone);

  if(i == -1)
    snprintf(tmp,sizeof tmp, "ctcss");
  else
    snprintf(tmp,sizeof tmp, "ctcss%d",i);
  tone = config_getdouble(Configtable,sname,tmp,tone);

  tone = fabs(tone);
  if(tone > 3000){
    fprintf(stderr,"PL/CTCSS tone %.1lf out of range\n",tone);
    tone = 0;
  }
  return tone;
}
