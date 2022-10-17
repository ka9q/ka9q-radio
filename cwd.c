// $Id: cwd.c,v 1.5 2022/08/17 21:47:15 karn Exp $
// CW generator for ka9q-radio
// Runs as daemon, reads from a named pipe, sends audio to a specified multicast group + RTP SSRC
// Useful for IDs and other messages in repeater mode
// Phil Karn, KA9Q, July 31, 2022

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
#include "misc.h"
#include "multicast.h"
#include "morse.h"

int const Samprate = 48000; // Too hard to change
float CW_speed = 18.0;
float CW_pitch = 500.0;
float CW_level = -29.0; // dB
int const Default_ssrc = 100;

const char *App_path;
int Verbose = 0;

char const *Input = "/run/cw/input";
char const *Target = NULL;

#define PCM_BUFSIZE 480        // 16-bit sample count per packet; must fit in Ethernet MTU
int Dit_length;

int send_cw(int sock, struct rtp_state *rtp_state, wint_t c){
  // Should be longer than any character
  int16_t * const samples = malloc(60 * Dit_length * sizeof(samples[0]));

  struct rtp_header rtp;
  memset(&rtp,0,sizeof(rtp));
  rtp.type = PCM_MONO_PT;
  rtp.version = RTP_VERS;
  rtp.ssrc = rtp_state->ssrc;
  rtp.marker = true; // Start with marker bit on to reset playout buffer

  int sample_count = encode_morse_char(samples,c);
  // byte swap for network
  for(int i=0; i < sample_count;i++)
    samples[i] = htons(samples[i]);
  int16_t *outp = samples;
  
  // Use gather-output to avoid copying data
  // 0th element for RTP header, 1st element for sample data
  struct iovec iovec[2];
  struct msghdr msghdr;
  memset(&msghdr,0,sizeof(msghdr));
  msghdr.msg_iov = iovec;
  msghdr.msg_iovlen = 2;

  while(sample_count > 0){
    int const chunk = min(PCM_BUFSIZE,sample_count);
    rtp.timestamp = rtp_state->timestamp;
    rtp_state->timestamp += chunk;
    rtp.seq = rtp_state->seq++;
    rtp_state->packets++;
    rtp_state->bytes += sizeof(samples[0]) * chunk;
    
    unsigned char encoded_rtp_header[128]; // longer than any possible RTP header?
    int const encoded_rtp_header_size = (unsigned char *)hton_rtp(encoded_rtp_header,&rtp) - encoded_rtp_header;

    iovec[0].iov_base = &encoded_rtp_header;
    iovec[0].iov_len = encoded_rtp_header_size;

    iovec[1].iov_base = outp;
    iovec[1].iov_len = sizeof(samples[0]) * chunk;

    if(Verbose > 1)
      fprintf(stdout,"iovec[0] = (%p,%lu) iovec[1] = (%p,%lu)\n",
	      iovec[0].iov_base,(unsigned long)iovec[0].iov_len,
	      iovec[1].iov_base,(unsigned long)iovec[1].iov_len);

    int const r = sendmsg(sock,&msghdr,0);
    if(r <= 0){
      perror("pcm send");
      return -1;
    }
    sample_count -= chunk;
    outp += chunk;
    rtp.marker = 0; // Subsequent frames are not marked
    {
      // Sleep pacing - how long will this take to send?
      long long const nanosec = BILLION * chunk / Samprate;
      struct timespec delay;
      
      ns2ts(&delay,nanosec);
      nanosleep(&delay,NULL);
    }
  }
  free(samples);
  return 0;
}

int main(int argc,char *argv[]){
  App_path = argv[0];
  
  struct rtp_state rtp_state;
  memset(&rtp_state,0,sizeof(rtp_state));
  rtp_state.ssrc = Default_ssrc;

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
      rtp_state.ssrc = strtol(optarg,NULL,0);
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
    exit(1);
  }

  setlocale(LC_ALL,""); // Accept all characters, not just the English subset of Latin
  Dit_length = init_morse(CW_speed,CW_pitch,CW_level,Samprate);
  struct sockaddr sock;
  int const fd = setup_mcast(Target,&sock,1,1,0,0);
  if(fd == -1){
    fprintf(stdout,"Can't resolve %s\n",Target);
    exit(1);
  }
  umask(0);
  if(mkfifo(Input,0666) != 0 && errno != EEXIST){
    fprintf(stdout,"Can't make input fifo %s\n",Input);
    exit(1);
  }

  FILE *fp = fopen(Input,"r");
  if(fp == NULL){
    fprintf(stdout,"Can't open %s\n",Input);
    exit(1);
  }
  // Hold open (and idle) so we won't get EOF
  int out_fd = open(Input,O_WRONLY);

  wint_t cc;
  while((cc = fgetwc(fp)) != WEOF){
    send_cw(fd,&rtp_state,cc);
  }
  perror("fgetwc");

  close(fd);
  fclose(fp); fp = NULL;
  close(out_fd); out_fd = -1;
  exit(0);
}
