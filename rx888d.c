// Read from RX888 SDR
// Accept control commands from UDP socket
//
// Copyright (c)  2021 Ruslan Migirov <trapi78@gmail.com>
// Credit: https://github.com/rhgndf/rx888_stream
// Copyright (c)  2023 Franco Venturi
// Copyright (c)  2023 Phil Karn

#define _GNU_SOURCE 1
#include <assert.h>
#include <pthread.h>
#include <string.h>
#include <complex.h>
#include <libusb-1.0/libusb.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/types.h>
#include <dirent.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <net/if.h>
#include <signal.h>
#include <locale.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <errno.h>
#include <syslog.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <getopt.h>
#include <iniparser/iniparser.h>
#include <sched.h>

#include "conf.h"
#include "misc.h"
#include "multicast.h"
#include "decimate.h"
#include "status.h"
#include "config.h"
#include "rx888.h"
#include "ezusb.h"

int ezusb_verbose = 0;
static int const Bufsize = 16384;

// Global variables set by config file options
char const *Iface;
char *Locale;
int RTP_ttl;
int Status_ttl;
int IP_tos;
const char *App_path;
int Verbose;

volatile bool stop_transfers = false; // Request to stop data transfers
volatile bool sigterm_exit = false;

struct sdrstate {
  struct libusb_device_handle *dev_handle;
  int interface_number;
  struct libusb_config_descriptor *config;
  unsigned int pktsize;
  unsigned long success_count;  // Number of successful transfers
  unsigned long failure_count;  // Number of failed transfers
  unsigned int transfer_size;  // Size of data transfers performed so far
  unsigned int transfer_index; // Write index into the transfer_size array

  struct libusb_transfer **transfers; // List of transfer structures.
  unsigned char **databuffers;        // List of data buffers.
  int xfers_in_progress;

  char const *description;
  unsigned int samprate; // True sample rate of single A/D converter

  // Hardware
  bool randomizer;
  bool dither;
  float rf_atten;
  float rf_gain;
  int gainmode;

  // USB transfer
  unsigned int queuedepth; // Number of requests to queue
  unsigned int reqsize;    // Request size in number of packets

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


static void decode_rx888_commands(struct sdrstate *,uint8_t const *,int);
static void send_rx888_status(struct sdrstate *);
static void rx_callback(struct libusb_transfer *transfer);
static void *display(void *);
static void *ncmd(void *);
static void closedown(int a);
static int rx888_init(struct sdrstate *sdr,const char *firmware,unsigned int queuedepth,unsigned int reqsize);
static void rx888_set_dither_and_randomizer(struct sdrstate *sdr,bool dither,bool randomizer);
static void rx888_set_att(struct sdrstate *sdr,float att);
static void rx888_set_gain(struct sdrstate *sdr,float gain);
static void rx888_set_samprate(struct sdrstate *sdr,unsigned int samprate);
static int rx888_start_rx(struct sdrstate *sdr,libusb_transfer_cb_fn callback);
static void rx888_stop_rx(struct sdrstate *sdr);
static void rx888_close(struct sdrstate *sdr);
static void free_transfer_buffers(unsigned char **databuffers,struct libusb_transfer **transfers,unsigned int queuedepth);
static double val2gain(int g);
static int gain2val(int m, double gain);

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

#if !defined(NDEBUG)
  fprintf(stderr,"Debugging (asserts) enabled\n");
#endif

  struct sdrstate * const sdr = (struct sdrstate *)calloc(1,sizeof(struct sdrstate));

  int c;
  while((c = getopt_long(argc,argv,Optstring,Options,NULL)) != -1){
    switch(c){
    case 'f':
      Conf_file = optarg;
      break;
    case 'v':
      Verbose++;
      ezusb_verbose++;
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
  } else if((Dictionary = iniparser_load("/etc/radio/rx888d.conf")) != NULL){
    if(iniparser_find_entry(Dictionary,Name) == 1){
      printf("Using config file /etc/radio/rx888d.conf\n");
    } else {
      iniparser_freedict(Dictionary);
      Dictionary = NULL;
    }
  }
  if(Dictionary == NULL){
    // Search everything under /etc/radio/rx888d.conf.d
    char const *subdir = "/etc/radio/rx888d.conf.d";
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

  // Firmware file
  char const *firmware = config_getstring(Dictionary,Name,"firmware",NULL);
  if(firmware == NULL){
    fprintf(stdout,"'firmware' not defined in section %s\n",Name);
    iniparser_freedict(Dictionary);
    exit(1);
  }
  // Queue depth, default 16
  int const queuedepth = config_getint(Dictionary,Name,"queuedepth",16);
  if(queuedepth < 1 || queuedepth > 64) {
    fprintf(stdout,"Invalid queue depth %d\n",queuedepth);
    iniparser_freedict(Dictionary);
    exit(1);
  }
  // Packets per transfer request, default 8
  int const reqsize = config_getint(Dictionary,Name,"reqsize",8);
  if(reqsize < 1 || reqsize > 64) {
    fprintf(stdout,"Invalid request size %d\n",reqsize);
    iniparser_freedict(Dictionary);
    exit(1);
  }
  {
    char full_firmware_file[PATH_MAX];
    dist_path(full_firmware_file,sizeof(full_firmware_file),firmware);
    fprintf(stdout,"Loading firmware file %s\n",full_firmware_file);
    int ret;
    if((ret = rx888_init(sdr,full_firmware_file,queuedepth,reqsize)) != 0){
      fprintf(stdout,"rx888_init() failed\n");
      iniparser_freedict(Dictionary);
      exit(1);
    }
  }
  // Enable dithering
  bool const dither = config_getint(Dictionary,Name,"dither",0);
  // Enable output randomization
  bool const randomizer = config_getint(Dictionary,Name,"rand",0);
  rx888_set_dither_and_randomizer(sdr,dither,randomizer);

  // Attenuation, default 0
  {
    float att = fabsf(config_getfloat(Dictionary,Name,"att",0));
    if(att > 31.5)
      att = 31.5;
    rx888_set_att(sdr,att);

    // Gain Mode low/high, default high
    char const *gainmode = config_getstring(Dictionary,Name,"gainmode","high");
    if(strcmp(gainmode, "high") == 0)
      sdr->gainmode = 1;
    else if(strcmp(gainmode, "low") == 0)
      sdr->gainmode = 0;
    else {
      fprintf(stdout,"Invalid gain mode %s, default high\n",gainmode);
      sdr->gainmode = 1;
    }
    // Gain value, default +1.5 dB
    float gain = config_getfloat(Dictionary,Name,"gain",1.5);
    if(gain > 34.0)
      gain = 34.0;
    
    rx888_set_gain(sdr,gain);

    // Sample Rate, default 32000000
    unsigned int const samprate = config_getint(Dictionary,Name,"samprate",32000000);
    if(samprate < 1000000){
      fprintf(stdout,"Invalid sample rate %'d\n",samprate);
      iniparser_freedict(Dictionary);
      exit(1);
    }
    rx888_set_samprate(sdr,samprate);
  }
  
  fprintf(stdout,"Samprate %'d, Gain %.1f dB, Attenuation %.1f dB, Dithering %d, Randomizer %d, USB Queue depth %d, USB Request size %d, USB packet size %'d\n",
	  sdr->samprate,sdr->rf_gain,sdr->rf_atten,sdr->dither,sdr->randomizer,sdr->queuedepth,sdr->reqsize,sdr->reqsize * sdr->pktsize);

  // When the IP TTL is 0, we're not limited by the Ethernet hardware MTU so select a much larger packet size
  // unless one has been set explicitly
  // IPv4 packets are limited to 64KB, IPv6 can go larger with the Jumbo Payload option

  RTP_ttl = config_getint(Dictionary,Name,"data-ttl",0); // Default to TTL=0
  Status_ttl = config_getint(Dictionary,Name,"status-ttl",1); // Default 1 for status; much lower bandwidth
  {
    int const x = config_getint(Dictionary,Name,"blocksize",-1);
    if(x != -1){
      sdr->blocksize = x;
    } else if(RTP_ttl == 0)
      sdr->blocksize = 24576; // * 2 bytes/sample = 49152 bytes/packet
    else
      sdr->blocksize = 720;   // * 2 bytes/sample = 1440 bytes/packet
  }

  sdr->description = config_getstring(Dictionary,Name,"description",NULL);
  {
    time_t tt;
    time(&tt);
    sdr->rtp.ssrc = config_getint(Dictionary,Name,"ssrc",tt);
  }
  sdr->rtp_type = PCM_MONO_LE_PT; // 16 bits/sample, mono, little endian
  // Default is AF12 left shifted 2 bits
  IP_tos = config_getint(Dictionary,Name,"tos",48);
  sdr->data_dest = config_getstring(Dictionary,Name,"data","rx888-pcm.local");
  // Set up output sockets

  sdr->metadata_dest = config_getstring(Dictionary,Name,"status","rx888-status.local");

  // Multicast output interface for both data and status
  Iface = config_getstring(Dictionary,Name,"iface",NULL);

  {
    // Start Avahi client that will maintain our mDNS registrations
    // Service name, if present, must be unique
    // Description, if present becomes TXT record if present
    avahi_start(sdr->description,"_ka9q-ctl._udp",DEFAULT_STAT_PORT,sdr->metadata_dest,ElfHashString(sdr->metadata_dest),sdr->description);
    // don't register data service if using local UNIX socket
    if(sdr->data_dest[0] != '/') // Don't announce a UNIX local socket
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
  fprintf(stdout,"%s: iface %s; status -> %s, data -> %s (TTL %d, TOS %d, %d samples/packet)\n",
	  sdr->description,Iface,formatsock(&sdr->output_metadata_dest_address),formatsock(&sdr->output_data_dest_address),
	  RTP_ttl,IP_tos,sdr->blocksize);
	  

  signal(SIGPIPE,SIG_IGN);
  signal(SIGINT,closedown);
  signal(SIGKILL,closedown);
  signal(SIGQUIT,closedown);
  signal(SIGTERM,closedown);

  if(sdr->status)
    pthread_create(&sdr->display_thread,NULL,display,sdr);

  pthread_create(&sdr->ncmd_thread,NULL,ncmd,sdr);
  realtime();
  {
    int ret __attribute__ ((unused));
    ret = rx888_start_rx(sdr,rx_callback);
    assert(ret == 0);
  }
  send_rx888_status(sdr); // Tell the world we're alive

  do {
    libusb_handle_events(NULL);
  } while (!stop_transfers);

  fprintf(stderr, "RX888 streaming complete. Stopping transfers\n");

  rx888_stop_rx(sdr);
  rx888_close(sdr);
  fprintf(stdout,"Device is done streaming, exiting\n");
  close(sdr->data_sock);
  if(sigterm_exit) // sent by systemd when shutting down. Return success
    exit(0);
  exit(1);
}

// Thread to send metadata and process commands
static void *ncmd(void *arg){
  // Send status, process commands
  pthread_setname("rx888-cmd");
  assert(arg != NULL);
  struct sdrstate * const sdr = (struct sdrstate *)arg;
  if(sdr->status_sock == -1 || sdr->nctl_sock == -1)
    return NULL; // Nothing to do

  while(1){
    uint8_t buffer[Bufsize];
    int const length = recv(sdr->nctl_sock,buffer,sizeof(buffer),0);
    if(length > 0){
      // Parse entries
      if(buffer[0] == 0)
	continue;  // Ignore our own status messages

      sdr->commands++;
      decode_rx888_commands(sdr,buffer+1,length-1);
      send_rx888_status(sdr);
    }
  }
}

// Status display thread
static void *display(void *arg){
  assert(arg != NULL);
  struct sdrstate *sdr = (struct sdrstate *)arg;

  pthread_setname("rx888-disp");

  fprintf(sdr->status,"Gain   Att\n");

  off_t stat_point = ftello(sdr->status);
  // End lines with return when writing to terminal, newlines when writing to status file
  char   eol = stat_point == -1 ? '\r' : '\n';
  while(1){

    if(stat_point != -1)
      fseeko(sdr->status,stat_point,SEEK_SET);

    fprintf(sdr->status,"%4.1f%4.1f%c",
	    sdr->rf_gain,	
	    sdr->rf_atten,
	    eol);
    fflush(sdr->status);
    usleep(100000); // 10 Hz
  }
  return NULL;
}

static void decode_rx888_commands(struct sdrstate *sdr,uint8_t const *buffer,int length){
  uint8_t const *cp = buffer;


  while(cp - buffer < length){
    int ret __attribute__((unused)); // Won't be used when asserts are disabled
    enum status_type const type = *cp++; // increment cp to length field

    if(type == EOL)
      break; // End of list

    unsigned int optlen = *cp++;
    if(optlen & 0x80){
      // length is >= 128 bytes; fetch actual length from next N bytes, where N is low 7 bits of optlen
      int length_of_length = optlen & 0x7f;
      optlen = 0;
      while(length_of_length > 0){
	optlen <<= 8;
	optlen |= *cp++;
	length_of_length--;
      }
    }
    if(cp - buffer + optlen >= length)
      break; // Invalid length

    switch(type){
    case EOL: // Shouldn't get here
      break;
    case COMMAND_TAG:
      sdr->command_tag = decode_int(cp,optlen);
      break;
    case RF_GAIN:
      {
	float gain = decode_float(cp,optlen);
	rx888_set_gain(sdr,gain);
      }
      break;
    case RF_ATTEN:
      {
	float a = decode_float(cp,optlen);
	rx888_set_att(sdr,a);
      }
      break;
    default: // Ignore all others
      break;
    }
    cp += optlen;
  }
}

static void send_rx888_status(struct sdrstate *sdr){
  sdr->output_metadata_packets++;

  uint8_t packet[2048],*bp;
  bp = packet;

  *bp++ = 0;   // Command/response = response

  encode_int32(&bp,COMMAND_TAG,sdr->command_tag);
  encode_int64(&bp,CMD_CNT,sdr->commands);

  encode_int64(&bp,GPS_TIME,gps_time_ns());

  if(sdr->description)
    encode_string(&bp,DESCRIPTION,sdr->description,strlen(sdr->description));

  // Where we're sending output

  if(sdr->data_dest[0] != '/'){
    // Source address we're using to send data
    encode_socket(&bp,OUTPUT_DATA_SOURCE_SOCKET,&sdr->output_data_source_address);
    encode_socket(&bp,OUTPUT_DATA_DEST_SOCKET,&sdr->output_data_dest_address); // IPv4/v6 socket
  } else {
    // AF_LINUX local socket, send path name as string
    encode_string(&bp,OUTPUT_DATA_UNIX_SOCKET,sdr->data_dest,strlen(sdr->data_dest)); // AF_UNIX socket, send local pathname as string
  }
  encode_int32(&bp,OUTPUT_SSRC,sdr->rtp.ssrc);
  encode_byte(&bp,OUTPUT_TTL,RTP_ttl);
  encode_int32(&bp,INPUT_SAMPRATE,sdr->samprate);
  encode_int64(&bp,OUTPUT_DATA_PACKETS,sdr->rtp.packets);
  encode_int64(&bp,OUTPUT_METADATA_PACKETS,sdr->output_metadata_packets);

  // Front end
  encode_float(&bp,RF_ATTEN,sdr->rf_atten);
  encode_float(&bp,RF_GAIN,sdr->rf_gain);

  // Tuning
  encode_double(&bp,RADIO_FREQUENCY,0);

  encode_byte(&bp,DEMOD_TYPE,0); // actually LINEAR_MODE
  encode_int32(&bp,OUTPUT_SAMPRATE,sdr->samprate);
  encode_int32(&bp,OUTPUT_CHANNELS,1);
  encode_int32(&bp,DIRECT_CONVERSION,1);
  encode_float(&bp,LOW_EDGE,0);
  encode_float(&bp,HIGH_EDGE,0.47 * sdr->samprate); // Should look at the actual filter curves
  encode_int32(&bp,OUTPUT_BITS_PER_SAMPLE,16); // Always

  encode_eol(&bp);
  int const len = bp - packet;
  assert(len < sizeof(packet));
  send(sdr->status_sock,packet,len,0);
}


bool ThreadnameSet;
bool Marker_sent;
// Callback called with incoming receiver data from A/D
static void rx_callback(struct libusb_transfer *transfer){
  int size = 0;

  if(!ThreadnameSet){
    pthread_setname("rx888-cb");
    ThreadnameSet = true;
  }
  struct sdrstate * const sdr = (struct sdrstate *)transfer->user_data;

  sdr->xfers_in_progress--;

  if(transfer->status != LIBUSB_TRANSFER_COMPLETED) {
    sdr->failure_count++;
    if(Verbose > 1)
      fprintf(stdout,"Transfer %p callback status %s received %d bytes.\n",transfer,
	      libusb_error_name(transfer->status), transfer->actual_length);
    if(!stop_transfers) {
      if(libusb_submit_transfer(transfer) == 0)
        sdr->xfers_in_progress++;
    }
    return;
  }

  // successful USB transfer
  size = transfer->actual_length;
  sdr->success_count++;
  if(sdr->randomizer) {
    uint16_t *samples = (uint16_t *)transfer->buffer;
    for (int i = 0; i < size / 2; i++) {
      samples[i] ^= 0xfffe * (samples[i] & 1);
    }
  }
#if 0 // Now using PCM_MONO_LE_PT
  // Convert to big endian (this is wasteful)
  {
    uint16_t *samples = (uint16_t *)transfer->buffer;
    for(int i=0; i < size/2; i++)
      samples[i] = htons(samples[i]);
  }
#endif
  struct rtp_header rtp;
  memset(&rtp,0,sizeof(rtp));
  if(!Marker_sent){
    rtp.marker = true;
    Marker_sent = true;
  }

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

  int samples = size / 2;
  uint8_t *idp = transfer->buffer;

  while(samples > 0){
    int const chunk = min(samples,sdr->blocksize);

    rtp.seq = sdr->rtp.seq++;
    rtp.timestamp = sdr->rtp.timestamp;
    uint8_t * const dp = hton_rtp(buffer,&rtp);

    iov[0].iov_len = dp - buffer;
    iov[1].iov_base = idp;
    iov[1].iov_len = chunk * 2;   // 16 bits per sample -> 2 bytes

    idp += iov[1].iov_len;

    if(sendmsg(sdr->data_sock,&msg,0) == -1){
      if(Verbose)
	fprintf(stdout,"send: %s\n",strerror(errno));
      //      usleep(100000); // inject a delay to avoid filling the log
    } else {
      sdr->rtp.packets++;
      sdr->rtp.bytes += iov[0].iov_len + iov[1].iov_len;
    }
    sdr->rtp.timestamp += chunk; // real-only samples
    samples -= chunk;
  }
  if(!stop_transfers) {
    if(libusb_submit_transfer(transfer) == 0)
      sdr->xfers_in_progress++;
  }
}

static void closedown(int a){
  fprintf(stdout,"caught signal %'d: %s\n",a,strsignal(a));
  stop_transfers = true;

  if(a == SIGTERM) // sent by systemd when shutting down. Return success
    sigterm_exit = true;
}

static int rx888_init(struct sdrstate *sdr,const char *firmware,unsigned int queuedepth,unsigned int reqsize){
  {
    int ret = libusb_init(NULL);
    if(ret != 0){
      fprintf(stdout,"Error initializing libusb: %s\n",
	      libusb_error_name(ret));
      return 1;
    }
  }
  if(firmware != NULL){ // there is argument with image file
    // Temporary ID when device doesn't already have firmware
    uint16_t const vendor_id = 0x04b4;
    uint16_t const product_id = 0x00f3;
    // does it already have firmware?
    sdr->dev_handle =
      libusb_open_device_with_vid_pid(NULL,vendor_id,product_id);
    if(sdr->dev_handle){
      // No, doesn't have firmware
      struct libusb_device *dev = libusb_get_device(sdr->dev_handle);
      
      if(ezusb_load_ram(sdr->dev_handle,firmware,FX_TYPE_FX3,IMG_TYPE_IMG,1) == 0){
	fprintf(stdout,"Firmware updated\n");
      } else {
	fprintf(stdout,"Firmware upload failed for device %d.%d (logical).\n",
		libusb_get_bus_number(dev),libusb_get_device_address(dev));
	return 1;
      }
      sleep(2); // this seems rather long
    } else {
      fprintf(stdout,"Firmware already loaded\n");
    }      
  }
  // Device changes product_id when it has firmware
  uint16_t const vendor_id = 0x04b4;
  uint16_t const product_id = 0x00f1;
  sdr->dev_handle = libusb_open_device_with_vid_pid(NULL,vendor_id,product_id);
  if(!sdr->dev_handle){
    fprintf(stdout,"Error or device could not be found, try loading firmware\n");
    goto close;
  }
  {
    int ret = libusb_kernel_driver_active(sdr->dev_handle,0);
    if(ret != 0){
      fprintf(stdout,"Kernel driver active. Trying to detach kernel driver\n");
      ret = libusb_detach_kernel_driver(sdr->dev_handle,0);
      if(ret != 0){
	fprintf(stdout,"Could not detach kernel driver from an interface\n");
	goto close;
      }
    }
  }
  struct libusb_device *dev = libusb_get_device(sdr->dev_handle);
  libusb_get_config_descriptor(dev, 0, &sdr->config);
  {
    int const ret = libusb_claim_interface(sdr->dev_handle, sdr->interface_number);
    if(ret != 0){
      fprintf(stderr, "Error claiming interface\n");
      goto end;
    }
  }
  fprintf(stdout,"Successfully claimed interface\n");
  {
    // All this just to get sdr->pktsize?
    struct libusb_interface_descriptor const *interfaceDesc = &(sdr->config->interface[0].altsetting[0]);
    struct libusb_endpoint_descriptor const *endpointDesc = &interfaceDesc->endpoint[0];
    struct libusb_device_descriptor desc;
    libusb_get_device_descriptor(dev,&desc);
    struct libusb_ss_endpoint_companion_descriptor *ep_comp;
    libusb_get_ss_endpoint_companion_descriptor(NULL,endpointDesc,&ep_comp);
    sdr->pktsize = endpointDesc->wMaxPacketSize * (ep_comp->bMaxBurst + 1);
    libusb_free_ss_endpoint_companion_descriptor(ep_comp);
  }
  bool allocfail = false;
  sdr->databuffers = (u_char **)calloc(queuedepth,sizeof(u_char *));
  sdr->transfers = (struct libusb_transfer **)calloc(queuedepth,sizeof(struct libusb_transfer *));

  if((sdr->databuffers != NULL) && (sdr->transfers != NULL)){
    for(unsigned int i = 0; i < queuedepth; i++){
      sdr->databuffers[i] = (u_char *)malloc(reqsize * sdr->pktsize);
      sdr->transfers[i] = libusb_alloc_transfer(0);
      if((sdr->databuffers[i] == NULL) || (sdr->transfers[i] == NULL)) {
        allocfail = true;
        break;
      }
    }
  } else {
    allocfail = true;
  }

  if(allocfail) {
    fprintf(stdout,"Failed to allocate buffers and transfers\n");
    // Is it OK if one or both of these is already null?
    free_transfer_buffers(sdr->databuffers,sdr->transfers,sdr->queuedepth);
    sdr->databuffers = NULL;
    sdr->transfers = NULL;
  }
  sdr->queuedepth = queuedepth;
  sdr->reqsize = reqsize;
  return 0;

end:
  if(sdr->dev_handle)
    libusb_release_interface(sdr->dev_handle,sdr->interface_number);

  if(sdr->config)
    libusb_free_config_descriptor(sdr->config);
  sdr->config = NULL;

close:
  if(sdr->dev_handle)
    libusb_close(sdr->dev_handle);
  sdr->dev_handle = NULL;

  libusb_exit(NULL);
  return 1;
}

static void rx888_set_dither_and_randomizer(struct sdrstate *sdr,bool dither,bool randomizer){
  uint32_t gpio = 0;
  if(dither)
    gpio |= DITH;

  if(randomizer)
    gpio |= RANDO;

  usleep(5000);
  command_send(sdr->dev_handle,GPIOFX3,gpio);
  sdr->dither = dither;
  sdr->randomizer = randomizer;
}

static void rx888_set_att(struct sdrstate *sdr,float att){
  usleep(5000);
  sdr->rf_atten = att;
  int arg = (int)(att * 2);
  argument_send(sdr->dev_handle,DAT31_ATT,arg);
}

static void rx888_set_gain(struct sdrstate *sdr,float gain){
  usleep(5000);

  int arg = gain2val(sdr->gainmode,gain);
  argument_send(sdr->dev_handle,AD8340_VGA,arg);
  sdr->rf_gain = val2gain(arg); // Store actual nearest value
}

static void rx888_set_samprate(struct sdrstate *sdr,unsigned int samprate){
  usleep(5000);
  command_send(sdr->dev_handle,STARTADC,samprate);
  sdr->samprate = samprate;
}

static int rx888_start_rx(struct sdrstate *sdr,libusb_transfer_cb_fn callback){
  unsigned int ep = 1 | LIBUSB_ENDPOINT_IN;

  assert(callback != NULL);
  for(unsigned int i = 0; i < sdr->queuedepth; i++){
    assert(sdr->transfers[i] != NULL);
    assert(sdr->databuffers[i] != NULL);
    assert(sdr->dev_handle != NULL);

    libusb_fill_bulk_transfer(sdr->transfers[i],sdr->dev_handle,ep,sdr->databuffers[i],
                  sdr->reqsize * sdr->pktsize,callback,(void *)sdr,0);
    int const rStatus = libusb_submit_transfer(sdr->transfers[i]);
    assert(rStatus == 0);
    if(rStatus == 0)
      sdr->xfers_in_progress++;
  }

  usleep(5000);
  command_send(sdr->dev_handle,STARTFX3,0);
  usleep(5000);
  command_send(sdr->dev_handle,TUNERSTDBY,0);

  return 0;
}

static void rx888_stop_rx(struct sdrstate *sdr){
  while(sdr->xfers_in_progress != 0){
    if(Verbose)
      fprintf(stdout,"%d transfers are pending\n",sdr->xfers_in_progress);
    libusb_handle_events(NULL);
    usleep(100000);
  }

  fprintf(stdout,"Transfers completed\n");
  free_transfer_buffers(sdr->databuffers,sdr->transfers,sdr->queuedepth);
  sdr->databuffers = NULL;
  sdr->transfers = NULL;

  command_send(sdr->dev_handle,STOPFX3,0);
}

static void rx888_close(struct sdrstate *sdr){
  if(sdr->dev_handle)
    libusb_release_interface(sdr->dev_handle,sdr->interface_number);

  if(sdr->config)
    libusb_free_config_descriptor(sdr->config);
  sdr->config = NULL;

  if(sdr->dev_handle)
    libusb_close(sdr->dev_handle);

  sdr->dev_handle = NULL;
  libusb_exit(NULL);
}

// Function to free data buffers and transfer structures
static void free_transfer_buffers(unsigned char **databuffers,
                                  struct libusb_transfer **transfers,
                                  unsigned int queuedepth){
  // Free up any allocated data buffers
  if(databuffers != NULL){
    for(unsigned int i = 0; i < queuedepth; i++)
      FREE(databuffers[i]);

    free(databuffers); // caller will have to nail the pointer
  }

  // Free up any allocated transfer structures
  if(transfers != NULL){
    for(unsigned int i = 0; i < queuedepth; i++){
      if(transfers[i] != NULL)
        libusb_free_transfer(transfers[i]);

      transfers[i] = NULL;
    }
    free(transfers); // caller will have to nail the pointer
  }
}

// gain computation for AD8370 variable gain amplifier
static double const vernier = 0.055744;
static double const pregain = 7.079458;

static double val2gain(int g){
  int const msb = g & 128 ? true : false;
  int const gaincode = g & 127;
  double av = gaincode * vernier * (1 + (pregain - 1) * msb);
  return voltage2dB(av); // decibels
}

static int gain2val(int m, double gain){
  if(m < 0)
    m = 0;
  else if(m > 1)
    m = 1;
  int g = round(dB2voltage(gain) / (vernier * (1 + (pregain - 1)* m)));

  if(g > 127)
    g = 127;
  if(g < 0)
    g = 0;
  g |= (m << 7);
  return g;
}
