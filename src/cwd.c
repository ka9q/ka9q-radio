// CW generator for ka9q-radio
// Runs as daemon, reads from a named pipe, sends audio to a specified multicast group + RTP SSRC
// Useful for IDs and other messages in repeater mode
// Copyright Phil Karn, KA9Q, July 31, 2022 - 2023

#include <stdio.h>
#include <wchar.h>
#include <wctype.h>
#include <unistd.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdint.h>
#include <locale.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <sysexits.h>
#include <time.h>

#include "misc.h"
#include "multicast.h"
#include "rtp.h"
#include "morse.h"

static int const Samprate = 12000;
double CW_speed = 18.0;
double CW_pitch = 500.0;
double CW_level = -29.0; // dB
static int const Default_ssrc = 100;

const char *App_path;
int Verbose = 0;

char const *Input = "/run/cw/input";
char const *Target = NULL;

#define PCM_BUFSIZE ((size_t)700)        // 16-bit sample count per packet; must fit in Ethernet MTU
int Dit_length;

// Redo for loopback?
int send_cw(int sock, struct rtp_state *rtp_state, wchar_t *msg){
  // Should be longer than any character
  float fsamples[60 * Dit_length];

  int const type = pt_from_info(Samprate,1, S16BE);
  if(type < 0)
    return 0; // Can't allocate!

  struct rtp_header rtp = {
    .type = (uint8_t)type,
    .version = RTP_VERS,
    .ssrc = rtp_state->ssrc,
  };
  struct iovec iovec[2] = {0};
  struct msghdr msghdr = {
    .msg_iov = iovec,
    .msg_iovlen = 2
  };
  int16_t *samples = malloc(60 * Dit_length * sizeof *samples);
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC,&now);

  uint64_t total_samples = 0;

  wchar_t c;
  while((c = *msg++) != 0){
    size_t const sample_count = encode_morse_char(fsamples, c);
    for(size_t i=0; i < sample_count;i++){
      float s = ldexpf(fsamples[i],15);
      int16_t is = s > 32767 ? 32767 : s < -32768 ? -32768 : (int16_t)s;
      samples[i] = htons(is);  // byte swap for network
    }
    int16_t *outp = samples;

    size_t samples_remaining = sample_count;
    while(samples_remaining > 0){
      size_t const chunk = min(PCM_BUFSIZE, samples_remaining);

      rtp.timestamp = rtp_state->timestamp;
      rtp_state->timestamp += chunk;
      rtp.seq = rtp_state->seq++;
      rtp_state->packets++;
      rtp_state->bytes += chunk * sizeof *samples;

      uint8_t encoded_rtp_header[128]; // longer than any possible RTP header?
      size_t const encoded_rtp_header_size = (uint8_t *)hton_rtp(encoded_rtp_header,&rtp) - encoded_rtp_header;

      // 0th element for RTP header, 1st element for sample data
      iovec[0].iov_base = encoded_rtp_header;
      iovec[0].iov_len = encoded_rtp_header_size;
      iovec[1].iov_base = outp;
      iovec[1].iov_len = chunk * sizeof *samples;
      if(Verbose > 1)
	fprintf(stdout,"iovec[0] = (%p,%lu) iovec[1] = (%p,%lu)\n",
		iovec[0].iov_base,(unsigned long)iovec[0].iov_len,
		iovec[1].iov_base,(unsigned long)iovec[1].iov_len);

      ssize_t const r = sendmsg(sock,&msghdr,0);
      if(r <= 0)
	fprintf(stdout,"sendmsg: (%d) %s\n",errno,strerror(errno));

      total_samples += chunk;
      samples_remaining -= chunk;
      outp += chunk;

      // Wait until this finishes sending
      // Note 48 kHz has a factor of 3, so it's not necessarily a whole number of ns
      uint64_t samptime_ns = 1000000000UL * total_samples / Samprate;

      int secs = samptime_ns / 1000000000UL;
      int nsecs = samptime_ns % 1000000000UL;
      struct timespec deadline;
      deadline.tv_sec = now.tv_sec + secs;
      deadline.tv_nsec = now.tv_nsec + nsecs;
      if (deadline.tv_nsec >= 1000000000) {
        deadline.tv_nsec -= 1000000000;
	deadline.tv_sec++;
      }
      clock_nanosleep(CLOCK_MONOTONIC,TIMER_ABSTIME,&deadline,NULL);
    }
  }
  FREE(samples);
  return 0;
}

int main(int argc,char *argv[]){
  App_path = argv[0];

  struct rtp_state rtp_state = {
    .ssrc = Default_ssrc
  };

  int c;
  while((c = getopt(argc,argv,"R:s:I:vS:P:L:")) != -1){
    switch(c){
    case 'v':
      Verbose++;
      break;
    case 'I':
      Input = optarg;
      break;
    case 's':
      rtp_state.ssrc = atoi(optarg);
      break;
    case 'R':
      Target = optarg;
      break;
    case 'S':
      CW_speed = strtod(optarg,NULL);
      break;
    case 'P':
      CW_pitch = strtod(optarg,NULL);
      break;
    case 'L':
      CW_level = strtod(optarg,NULL);
      break;
    default:
      fprintf(stdout,"Usage: %s [-v] [-I fifo_name] [-s ssrc] -R mcast_group [-S speed_wpm] [-P pitch_hz] [-L level16]\n",argv[0]);
      break;
    }
  }
  if(Target == NULL){
    fprintf(stdout,"Must specify -R mcast_group\n");
    exit(EX_USAGE);
  }

  setlocale(LC_ALL,""); // Accept all characters, not just the English subset of Latin
  Dit_length = init_morse(CW_speed,CW_pitch,CW_level,Samprate);
  struct sockaddr sock;
  int const fd = setup_mcast(NULL, NULL,Target, &sock,true,1,0,0,0);
  if(fd == -1){
    fprintf(stdout,"Can't resolve %s\n",Target);
    exit(EX_IOERR);
  }
  umask(0);
  if(mkfifo(Input,0666) != 0 && errno != EEXIST){
    fprintf(stdout,"Can't make input fifo %s\n",Input);
    exit(EX_CANTCREAT);
  }

  FILE *fp = fopen(Input,"r");
  if(fp == NULL){
    fprintf(stdout,"Can't open %s\n",Input);
    exit(EX_NOINPUT);
  }
  // Hold open (and idle) so we won't get EOF
  int out_fd = open(Input,O_WRONLY);

  wchar_t line[80];
  while(fgetws(line,sizeof line, fp) != NULL){
    send_cw(fd,&rtp_state,line);
  }

  close(fd);
  fclose(fp); fp = NULL;
  close(out_fd); out_fd = -1;
  exit(EX_OK);
}
