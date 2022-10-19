// $Id: decode_status.c,v 1.20 2022/08/05 06:35:10 karn Exp $
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include <pthread.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/time.h>
#include <errno.h>
#include "misc.h"
#include "radio.h"
#include "status.h"


// Process status messages from the front end
void *sdr_status(void *arg){
  // Solicit immediate full status
  pthread_setname("sdrstat");

  struct frontend * const frontend = (struct frontend *)arg;
  assert(frontend != NULL);

  long long const random_interval = 50000000; // 50 ms
  // Pick soon but still random times for the first polls
  long long next_fe_poll = random_time(0,random_interval);
  long long const fe_poll_interval = 975000000;

  while(1){
    if(gps_time_ns() > next_fe_poll){
      // Poll front end
      if(frontend->input.ctl_fd > 2)
	send_poll(frontend->input.ctl_fd,0);
      next_fe_poll = random_time(fe_poll_interval,random_interval);
    }
    fd_set fdset;
    FD_ZERO(&fdset);
    if(frontend->input.status_fd > 2)
      FD_SET(frontend->input.status_fd,&fdset);

    long long timeout = next_fe_poll - gps_time_ns();
    if(timeout < 0)
      timeout = 0;
    int n = frontend->input.status_fd + 1;
    {
      struct timespec ts;
      ns2ts(&ts,timeout);
      n = pselect(n,&fdset,NULL,NULL,&ts,NULL);
    }
    if(n <= 0){
      if(n < 0)
	fprintf(stdout,"sdr_status pselect: %s\n",strerror(errno));
      continue;
    }
    if(FD_ISSET(frontend->input.status_fd,&fdset)){
      // Status Update from SDR
      unsigned char buffer[8192];
      socklen_t socklen = sizeof(frontend->input.metadata_source_address);
      int const len = recvfrom(frontend->input.status_fd,buffer,sizeof(buffer),0,(struct sockaddr *)&frontend->input.metadata_source_address,&socklen);
      if(len <= 0){
	perror("sdrstat recvfrom");
	usleep(100000);
	continue;
      }
      // Parse entries
      int const cr = buffer[0]; // command-response byte
      
      if(cr == 1)
	continue; // Ignore commands

      frontend->input.metadata_packets++;
      // Protect Frontend structure
      pthread_mutex_lock(&frontend->sdr.status_mutex);
      decode_fe_status(frontend,buffer+1,len-1);
      pthread_cond_broadcast(&frontend->sdr.status_cond);
      pthread_mutex_unlock(&frontend->sdr.status_mutex);
      next_fe_poll = random_time(fe_poll_interval,random_interval);
    }    
  }
}


// Decode status messages from front end
// Used by both radio and control
int decode_fe_status(struct frontend *frontend,unsigned char const *buffer,int length){
  unsigned char const *cp = buffer;
  while(cp - buffer < length){
    enum status_type const type = *cp++; // increment cp to length field

    if(type == EOL)
      break; // end of list

    unsigned int const optlen = *cp++;
    if(cp - buffer + optlen >= length)
      break; // invalid length; we can't continue to scan

    switch(type){
    case EOL: // Shouldn't get here
      goto done;
    case DESCRIPTION:
      decode_string(cp,optlen,frontend->sdr.description,sizeof(frontend->sdr.description));
      break;
    case COMMAND_TAG:
      frontend->sdr.command_tag = decode_int(cp,optlen);
      break;
    case CMD_CNT:
      frontend->sdr.commands = decode_int(cp,optlen);
      break;
    case GPS_TIME:
      frontend->sdr.timestamp = decode_int(cp,optlen);
      break;
    case INPUT_DATA_SOURCE_SOCKET:
      decode_socket(&frontend->input.data_source_address,cp,optlen);
      break;
    case INPUT_DATA_DEST_SOCKET:
      decode_socket(&frontend->input.data_dest_address,cp,optlen);
      break;
    case INPUT_METADATA_SOURCE_SOCKET:
      decode_socket(&frontend->input.metadata_source_address,cp,optlen);
      break;
    case INPUT_METADATA_DEST_SOCKET:
      decode_socket(&frontend->input.metadata_dest_address,cp,optlen);
      break;
    case INPUT_DATA_PACKETS:
      frontend->input.rtp.packets = decode_int(cp,optlen);
      break;
    case INPUT_METADATA_PACKETS:
      frontend->input.metadata_packets = decode_int(cp,optlen);
      break;
    case INPUT_SAMPLES:
      frontend->input.samples = decode_int(cp,optlen);
      break;
    case INPUT_DROPS:
      frontend->input.rtp.drops = decode_int(cp,optlen);
      break;
    case INPUT_DUPES:
      frontend->input.rtp.dupes = decode_int(cp,optlen);
      break;
    case OUTPUT_DATA_DEST_SOCKET:
      decode_socket(&frontend->input.data_dest_address,cp,optlen);
      break;
    case OUTPUT_SSRC:
      frontend->input.rtp.ssrc = decode_int(cp,optlen);
      break;
    case OUTPUT_SAMPRATE:
      frontend->sdr.samprate = decode_int(cp,optlen);
      break;
    case RADIO_FREQUENCY:
      frontend->sdr.frequency = decode_double(cp,optlen);
      break;
#if 0 // Deprecated
    case OUTPUT_LEVEL:
      frontend->sdr.output_level = dB2power(decode_double(cp,optlen));
      break;
#endif
    case LOCK:
      frontend->sdr.lock = decode_int(cp,optlen);
      break;
    case FIRST_LO_FREQUENCY:
      frontend->sdr.frequency = decode_double(cp,optlen);
      break;
    case LNA_GAIN:
      frontend->sdr.lna_gain = decode_int(cp,optlen);
      break;
    case MIXER_GAIN:
      frontend->sdr.mixer_gain = decode_int(cp,optlen);
      break;
    case IF_GAIN:
      frontend->sdr.if_gain = decode_int(cp,optlen);
      break;
    case GAIN:
      frontend->sdr.gain = dB2voltage(decode_float(cp,optlen));
      break;
    case LOW_EDGE:
      frontend->sdr.min_IF = decode_float(cp,optlen);
      break;
    case HIGH_EDGE:
      frontend->sdr.max_IF = decode_float(cp,optlen);
      break;
    case FILTER_BLOCKSIZE:
      if(frontend->in != NULL)
	frontend->in->ilen = decode_int(cp,optlen);
      break;
    case FILTER_FIR_LENGTH:
      if(frontend->in != NULL)
	frontend->in->impulse_length = decode_int(cp,optlen);
      break;
    case OUTPUT_CHANNELS:
      {
	int const i = decode_int(cp,optlen);
	if(i == 1)
	  frontend->sdr.isreal = 1;
	else
	  frontend->sdr.isreal = 0;
      }
      break;
    case OUTPUT_BITS_PER_SAMPLE:
      frontend->sdr.bitspersample = decode_int(cp,optlen);
      break;
    case DIRECT_CONVERSION:
      frontend->sdr.direct_conversion = decode_int(cp,optlen);
      break;
    default:
      break;
    }
    cp += optlen;
  }
  done:;

  if(frontend->sdr.samprate != 0 && frontend->sdr.min_IF == 0 && frontend->sdr.max_IF == 0){
    // Not initialized; avoid assertion fails by defaulting to +/- Fs/2
    frontend->sdr.min_IF = -frontend->sdr.samprate/2;
    frontend->sdr.max_IF = +frontend->sdr.samprate/2;
  }
  return 0;
}
