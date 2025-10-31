// read FFT bin energies from spectrum pseudo-demod and format similar to rtl_power - out of date
// Copyright 2023 Phil Karn, KA9Q

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#if defined(linux)
#include <bsd/string.h>
#endif
#include <assert.h>
#include <getopt.h>
#include <sysexits.h>
#include <fcntl.h>

#include "misc.h"
#include "status.h"
#include "multicast.h"
#include "radio.h"

struct sockaddr Metadata_dest_socket;      // Dest of metadata (typically multicast)
struct sockaddr Metadata_source_socket;      // Source of metadata
int IP_tos;
int Mcast_ttl = 1;
const char *App_path;
const char *Target;
int Verbose;
uint32_t Ssrc;
char Iface[1024]; // Multicast interface to talk to front end
int Status_fd = -1, Ctl_fd = -1;
int64_t Timeout = BILLION; // Retransmission timeout
bool details;   // Output bin, frequency, power, newline
char const *Source;
struct sockaddr_storage *Source_socket;

static char const Optstring[] = "b:c:C:df:hi:o:s:t:T:vw:V";
static struct  option Options[] = {
  {"bins", required_argument, NULL, 'b'},
  {"count", required_argument, NULL, 'c'},
  {"details", no_argument, NULL, 'd'},
  {"frequency", required_argument, NULL, 'f'},
  {"help", no_argument, NULL, 'h'},
  {"interval", required_argument, NULL, 'i'},
  {"ssrc", required_argument, NULL, 's'},
  {"timeout", required_argument, NULL, 'T'},
  {"verbose", no_argument, NULL, 'v'},
  {"version", no_argument, NULL, 'V'},
  {"bin-width", required_argument, NULL, 'w'},
  {"crossover", required_argument, NULL, 'C'},
  {"source", required_argument, NULL, 'o'},
  {NULL, 0, NULL, 0},
};


int extract_powers(float *power,int npower,uint64_t *time,double *freq,double *bin_bw,int32_t const ssrc,uint8_t const * const buffer,int length);

void help(){
  fprintf(stderr,"Usage: %s [-v|--verbose] [-V|--version] [-f|--frequency freq] [-w|--bin-width bin_bw] [-b|--bins bins] [-c|--count count] [-i|--interval interval] [-T|--timeout timeout] [-d|--details] -s|--ssrc ssrc mcast_addr [-o|--source <source name-or-address>\n",App_path);
  exit(1);
}

int main(int argc,char *argv[]){
  App_path = argv[0];
  int count = 1;     // Number of updates. -1 means infinite
  float interval = 5; // Period between updates, sec
  float frequency = -1;
  int bins = 0;
  float bin_bw = 0;
  float crossover = -1;
  {
    int c;
    while((c = getopt_long(argc,argv,Optstring,Options,NULL)) != -1){
      switch(c){
      case 'b':
	bins = strtol(optarg,NULL,0);
	break;
      case 'c':
	count = strtol(optarg,NULL,0);
	break;
      case 'C':
	crossover = strtod(optarg,NULL);
	break;
      case 'd':
	details = true;
	break;
      case 'f':
	frequency = parse_frequency(optarg,true);
	break;
      case 'h':
	help();
	break;
      case 'i':
	interval = strtof(optarg,NULL);
	break;
      case 's':
	Ssrc = strtol(optarg,NULL,0); // Send to specific SSRC
	break;
      case 'T':
	Timeout = (int64_t)(BILLION * strtof(optarg,NULL)); // Retransmission timeout
	break;
      case 'v':
	Verbose++;
	break;
      case 'w':
	bin_bw = strtof(optarg,NULL);
	break;
      case 'V':
	VERSION();
	exit(EX_OK);
      case 'o':
	Source = optarg;
	break;
      default:
	fprintf(stdout,"Unknown option %c\n",c);
	help();
	break;
      }
    }
  }
  if(argc <= optind)
    help();

  Target = argv[optind];
  resolve_mcast(Target,&Metadata_dest_socket,DEFAULT_STAT_PORT,Iface,sizeof(Iface),0);
  if(Verbose)
    fprintf(stderr,"Resolved %s -> %s\n",Target,formatsock(&Metadata_dest_socket,false));

  if(Source != NULL){
    Source_socket = calloc(1,sizeof(struct sockaddr_storage));
    if(Verbose)
      fprintf(stdout,"Resolving source %s\n",Source);
    resolve_mcast(Source,Source_socket,0,NULL,0,0);
  }

  Status_fd = listen_mcast(Source_socket,&Metadata_dest_socket,Iface);
  if(Status_fd == -1){
    fprintf(stderr,"Can't listen to mcast status %s\n",Target);
    exit(1);
  }
  int Ctl_fd = output_mcast(&Metadata_dest_socket,Iface,Mcast_ttl,IP_tos);
  if(Ctl_fd == -1){
    fprintf(stderr,"connect to mcast control failed: %s\n",strerror(errno));
    exit(1);
  }

  // Send command to set up the channel?? Or do in a separate command? We'd like to reuse the same demod & ssrc,
  // which is hard to do in one command, as we'd have to stash the ssrc somewhere.
  while(true){
    uint8_t buffer[PKTSIZE];
    uint8_t *bp = buffer;
    *bp++ = 1; // Command

    encode_int(&bp,OUTPUT_SSRC,Ssrc);
    uint32_t tag = random();
    encode_int(&bp,COMMAND_TAG,tag);
    encode_int(&bp,DEMOD_TYPE,SPECT_DEMOD);
    if(frequency >= 0)
      encode_float(&bp,RADIO_FREQUENCY,frequency); // 0 frequency means terminate
    if(bins > 0)
      encode_int(&bp,BIN_COUNT,bins);
    if(bin_bw > 0)
      encode_float(&bp,NONCOHERENT_BIN_BW,bin_bw);
    if(crossover >= 0)
      encode_float(&bp,CROSSOVER,crossover);
    encode_eol(&bp);
    int const command_len = bp - buffer;
    if(Verbose > 1){
      fprintf(stderr,"Sent:");
      dump_metadata(stderr,buffer+1,command_len-1,details ? true : false);
    }
    if(sendto(Ctl_fd, buffer, command_len, 0, &Metadata_dest_socket, sizeof Metadata_dest_socket) != command_len){
      perror("command send");
      usleep(10000); // 10 millisec
      goto again;
    }
    // The deadline starts at 1 sec after a command
    int64_t deadline = gps_time_ns() + Timeout;
    int length = 0;
    do {
      // Wait for a reply to our query
      // ignore all packets on group without changing deadline
      fd_set fdset;
      FD_ZERO(&fdset);
      FD_SET(Status_fd,&fdset);
      int n = Status_fd + 1;
      int64_t timeout = deadline - gps_time_ns();
      // Immediate poll if timeout is negative
      if(timeout < 0)
	timeout = 0;
      struct timespec ts;
      ns2ts(&ts,timeout);
      n = pselect(n,&fdset,NULL,NULL,&ts,NULL);
      if(n <= 0 && timeout == 0){
	usleep(10000); // rate limit, just in case
	goto again;
      }
      if(!FD_ISSET(Status_fd,&fdset))
	continue;
      // Read message on the multicast group
      socklen_t ssize = sizeof(Metadata_source_socket);
      length = recvfrom(Status_fd,buffer,sizeof(buffer),0,(struct sockaddr *)&Metadata_source_socket,&ssize);

      // Ignore invalid packets, non-status packets, packets re other SSRCs and packets not in response to our polls
      // Should we insist on the same command tag, or accept any "recent" status packet, e.g., triggered by the control program?
      // This is needed because an initial delay in joining multicast groups produces a burst of buffered responses; investigate this
    } while(length < 2 || (enum pkt_type)buffer[0] != STATUS || Ssrc != get_ssrc(buffer+1,length-1) || tag != get_tag(buffer+1,length-1));

    if(Verbose > 1){
      fprintf(stderr,"Received:");
      dump_metadata(stderr,buffer+1,length-1,details ? true : false);
    }
    float powers[PKTSIZE / sizeof(float)]; // floats in a max size IP packet
    uint64_t time;
    double r_freq;
    double r_bin_bw;

    int npower = extract_powers(powers,sizeof(powers) / sizeof (powers[0]), &time,&r_freq,&r_bin_bw,Ssrc,buffer+1,length-1);
    if(npower <= 0){
      fprintf(stderr,"Invalid response, length %d\n",npower);
      usleep(10000); // 10 millisec
      continue; // Invalid for some reason; retry
    }
    // Note from VK5QI:
    // the output format from that utility matches that produced by rtl_power, which is:
    //2022-04-02, 16:24:55, 400050181, 401524819, 450.13, 296, -52.95, -53.27, -53.26, -53.24, -53.40, <many more points here>
    // date, time, start_frequency, stop_frequency, bin_size_hz, number_bins, data0, data1, data2

    // **************Process here ***************
    char gps[1024];
    printf("%s,",format_gpstime_iso8601(gps,sizeof(gps),time));

    // Frequencies below center; note integer round-up, e.g, 65 -> 33; 64 -> 32
    // npower odd: emit N/2+1....N-1 0....N/2 (division truncating to integer)
    // npower even: emit N/2....N-1 0....N/2-1
    int const first_neg_bin = (npower + 1)/2; // round up, e.g., 64->32, 65 -> 33, 66 -> 33
    float base = r_freq - r_bin_bw * (npower/2); // integer truncation (round down), e.g., 64-> 32, 65 -> 32
    printf(" %.0f, %.0f, %.0f, %d",
	   base, base + r_bin_bw * (npower-1), r_bin_bw, npower);

    // Find lowest non-zero entry, use the same for zero power to avoid -infinity dB
    // Zero power in any bin is unlikely unless they're all zero, but handle it anyway
    float lowest = INFINITY;
    for(int i=0; i < npower; i++){
      if(powers[i] < 0){
	fprintf(stderr,"Invalid power %g in response\n",powers[i]);
	usleep(10000); // 10 millisec
	goto again; // negative powers are invalid
      }
      if(powers[i] > 0 && powers[i] < lowest)
	lowest = powers[i];
    }
    float const min_db = lowest != INFINITY ? power2dB(lowest) : 0;

    if (details){
      // Frequencies below center
      printf("\n");
      for(int i=first_neg_bin ; i < npower; i++){
        printf("%d %f %.2f\n",i,base,(powers[i] == 0) ? min_db : power2dB(powers[i]));
        base += r_bin_bw;
      }
      // Frequencies above center
      for(int i=0; i < first_neg_bin; i++){
        printf("%d %f %.2f\n",i,base,(powers[i] == 0) ? min_db : power2dB(powers[i]));
        base += r_bin_bw;
      }
    } else {
      for(int i= first_neg_bin; i < npower; i++)
        printf(", %.2f",(powers[i] == 0) ? min_db : power2dB(powers[i]));
      // Frequencies above center
      for(int i=0; i < first_neg_bin; i++)
        printf(", %.2f",(powers[i] == 0) ? min_db : power2dB(powers[i]));
    }
    printf("\n");
    if(--count == 0)
      break;

    // need to add randomized wait and avoidance of poll if response elicited by other poller (eg., control) comes in first
    // And if we decide to use those responses (currently blocked by command tag check)
    usleep((useconds_t)(interval * 1e6));
  again:;
  }
  exit(0);
}

// Decode only those status fields relevant to spectrum measurement
// Return number of bins
int extract_powers(float *power,int npower,uint64_t *time,double *freq,double *bin_bw,int32_t const ssrc,uint8_t const * const buffer,int length){
#if 0  // use later
  double l_lo1 = 0,l_lo2 = 0;
#endif
  int l_ccount = 0;
  uint8_t const *cp = buffer;
  int l_count = 0;

  while(cp - buffer < length){
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
    if(cp + optlen >= buffer + length)
      break; // Invalid length
    switch(type){
    case EOL: // Shouldn't get here
      goto done;
    case GPS_TIME:
      *time = decode_int64(cp,optlen);
      break;
    case OUTPUT_SSRC: // Don't really need this, it's already been checked
      if((int32_t)decode_int32(cp,optlen) != ssrc)
	return -1; // Not what we want
      break;
    case DEMOD_TYPE:
      {
	const int i = decode_int(cp,optlen);
	if(i != SPECT_DEMOD)
	  return -1; // Not what we want
      }
      break;
    case RADIO_FREQUENCY:
      *freq = decode_double(cp,optlen);
      break;
#if 0  // Use this to fine-tweak freq later
    case FIRST_LO_FREQUENCY:
      l_lo1 = decode_double(cp,optlen);
      break;
    case SECOND_LO_FREQUENCY: // ditto
      l_lo2 = decode_double(cp,optlen);
      break;
#endif
    case BIN_DATA:
      l_count = optlen/sizeof(float);
      if(l_count > npower)
	return -2; // Not enough room in caller's array
      // Note these are still in FFT order
      for(int i=0; i < l_count; i++){
	power[i] = decode_float(cp,sizeof(float));
	cp += sizeof(float);
      }
      break;
    case NONCOHERENT_BIN_BW:
      *bin_bw = decode_float(cp,optlen);
      break;
    case BIN_COUNT: // Do we check that this equals the length of the BIN_DATA tlv?
      l_ccount = decode_int(cp,optlen);
      break;
    default:
      break;
    }
    cp += optlen;
  }
 done:
  ;
  if(l_ccount == 0 || l_count != l_ccount)
    return 0;
  return l_count;
}
