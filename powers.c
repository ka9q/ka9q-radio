// $Id$
// read FFT bin energies from spectrum pseudo-demod and format similar to rtl_power
// Copyright 2023 Phil Karn, KA9Q
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#if defined(linux)
#include <bsd/string.h>
#endif
#include <locale.h>
#include <assert.h>
#include "misc.h"
#include "status.h"
#include "multicast.h"
#include "radio.h"


struct sockaddr_storage Metadata_dest_address;      // Dest of metadata (typically multicast)
struct sockaddr_storage Metadata_source_address;      // Source of metadata
int IP_tos;
int Mcast_ttl = 1;



const char *App_path;
int Verbose;
uint32_t Ssrc;
char Locale[256] = "en_US.UTF-8";
char Iface[1024]; // Multicast interface to talk to front end
int Status_fd,Ctl_fd;

int dump_powers(float *power,int npower,uint64_t *time,double *freq,double *bin_bw,int32_t const ssrc,uint8_t const * const buffer,int length);

int main(int argc,char *argv[]){
  App_path = argv[0];
  int count = -1;     // Number of updates. -1 means infinite
  float interval = 5; // Period between updates, sec
  {
    int c;
    while((c = getopt(argc,argv,"c:i:vs:")) != -1){
      switch(c){
      case 'c':
	count = strtol(optarg,NULL,0);
	break;
      case 'i':
	interval = strtof(optarg,NULL);
	break;
      case 'v':
	Verbose++;
	break;
      case 's':
	Ssrc = strtol(optarg,NULL,0); // Send to specific SSRC
	break;
      default:
	fprintf(stdout,"Unknown option %c\n",c);
	break;
      }
    }
  }
  {
    // The display thread assumes en_US.UTF-8, or anything with a thousands grouping character
    // Otherwise the cursor movements will be wrong
    char const * const cp = getenv("LANG");
    if(cp != NULL){
      strlcpy(Locale,cp,sizeof(Locale));
    }
  }
  setlocale(LC_ALL,Locale); // Set either the hardwired default or the value of $LANG if it exists
  // Dummy filter

  if(argc <= optind){
    fprintf(stderr,"Usage: %s -s ssrc [-v] [-i interval] [-c count] mcast_addr\n",App_path);
    exit(1);

  }
  resolve_mcast(argv[optind],&Metadata_dest_address,DEFAULT_STAT_PORT,Iface,sizeof(Iface));
  Status_fd = listen_mcast(&Metadata_dest_address,Iface);
  if(Status_fd == -1){
    fprintf(stderr,"Can't listen to mcast status %s\n",argv[optind]);
    exit(1);
  }
  Ctl_fd = connect_mcast(&Metadata_dest_address,Iface,Mcast_ttl,IP_tos);
  if(Ctl_fd < 0){
    fprintf(stderr,"connect to mcast control failed\n");
    exit(1);
  }


  // Send command to set up the channel?? Or do in a separate command? We'd like to reuse the same demod & ssrc,
  // which is hard to do in one command, as we'd have to stash the ssrc somewhere.
  while(true){
    uint32_t tag = send_poll(Ctl_fd,Ssrc);
    // The deadline starts at 1 sec after a poll
    int64_t deadline = gps_time_ns() + BILLION;

    int length = 0;
    uint8_t buffer[8192];
    while(true){
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
      socklen_t ssize = sizeof(Metadata_source_address);
      length = recvfrom(Status_fd,buffer,sizeof(buffer),0,(struct sockaddr *)&Metadata_source_address,&ssize);
    
      // Ignore our own command packets
      if(length < 2 || buffer[0] != 0)
	continue; // Wait for another one
      if(Ssrc != get_ssrc(buffer+1,length-1))
	continue; // Not the droid we're looking for; wait for another one

      // Should we insist on the same command tag, or accept any "recent" status packet, e.g., triggered by the control program?
      // This is needed because initial delays in joining multicast groups produce bursts of buffered responses; investigate this
      if(tag != get_tag(buffer+1,length-1))
	 continue; // Also not the droid we're looking for.

      break;
    }    
    float powers[65536];
    uint64_t time;
    double freq;
    double bin_bw;
    
    int npower = dump_powers(powers,sizeof(powers) / sizeof (powers[0]), &time,&freq,&bin_bw,Ssrc,buffer+1,length-1);
    if(npower < 0){
      printf("Invalid response, length %d\n",npower);
      
      continue; // Invalid for some reason
    }
    // Note from VK5QI:
    // the output format from that utility matches that produced by rtl_power, which is: 
    //2022-04-02, 16:24:55, 400050181, 401524819, 450.13, 296, -52.95, -53.27, -53.26, -53.24, -53.40, <many more points here>
    // date, time, start_frequency, stop_frequency, bin_size_hz, number_bins, data0, data1, data2

    // **************Process here ***************
    char gps[1024];
    printf("%s,",format_gpstime(gps,sizeof(gps),time));

    if(npower & 1){
      // Odd: emit N/2+1....N-1 0....N/2 (division truncating to integer)
      printf(" %.0f, %.0f, %.0f, %d,",
	     freq - bin_bw * npower/2, freq + bin_bw * npower/2, bin_bw, npower);
 
      // Frequencies below center
      for(int i= npower/2; i < npower; i++)
	printf(" %.1f,",powers[i] == 0 ? -100.0 : 10*log10(powers[i]));
      // Frequencies above center
      for(int i= 0; i < npower/2; i++)
	printf(" %.1f,",powers[i] == 0 ? -100.0 : 10*log10(powers[i]));
    } else {
      // Even: emit N/2....N-1 0....N/2-1
      printf(" %.0f, %.0f, %.0f, %d,",
	     freq - bin_bw * npower/2, freq + bin_bw * npower/2, bin_bw, npower);
 
      // Frequencies below center
      for(int i= npower/2; i < npower; i++)
	printf(" %.1f,",powers[i] == 0 ? -100.0 : 10*log10(powers[i]));
      // Frequencies above center
      for(int i= 0; i < npower/2; i++)
	printf(" %.1f,",powers[i] == 0 ? -100.0 : 10*log10(powers[i]));
    }
    printf("\n");
    // need to add randomized wait and avoidance of poll if response elicited by other poller (eg., control) comes in first
    usleep((useconds_t)(interval * 1e6));

    if(count == -1)
      continue;
    count--;
    if(count == 0)
      break;
  again:;
  }
  exit(0);
}

// Decode only those status fields relevant to spectrum measurement
// Return number of bins
int dump_powers(float *power,int npower,uint64_t *time,double *freq,double *bin_bw,int32_t const ssrc,uint8_t const * const buffer,int length){
#if 0  // use later
  double l_lo1 = 0,l_lo2 = 0;
#endif
  int l_ccount = 0;
  uint8_t const *cp = buffer;
  int l_count;

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
    if(cp - buffer + optlen >= length)
      break; // Invalid length
    switch(type){
    case EOL: // Shouldn't get here
      goto done;
    case GPS_TIME:
      *time = (uint64_t)decode_int(cp,optlen);
      break;
    case OUTPUT_SSRC: // Don't really need this, it's already been checked
      if(decode_int(cp,optlen) != ssrc)
	return -1; // Not what we want
      break;
    case DEMOD_TYPE:
      {
	const int i = (int)decode_int(cp,optlen);
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
  assert(l_ccount == l_count);
  return l_ccount;
}
