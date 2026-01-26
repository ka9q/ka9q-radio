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

static int const Samprate = 8000;
double CW_speed = 18.0;
double CW_pitch = 500.0;
double CW_level = -29.0; // dB
static int const Default_ssrc = 100;

const char *App_path;
int Verbose = 0;

char const *Input = "/run/cw/input";
char const *Target = NULL;

#define PKT_SIZE ((size_t)1500)
#define PCM_BUFSIZE ((PKT_SIZE-100)/2)        // 16-bit sample count per packet; must fit in Ethernet MTU
int Dit_length;

// Redo for loopback?
int send_cw(int sock, struct rtp_state *rtp_state, wchar_t *msg){
  // Should be longer than any character

  int const type = pt_from_info(Samprate,1, S16BE);
  if(type < 0)
    return 0; // Can't allocate!

  struct rtp_header rtp = {
    .type = (uint8_t)type,
    .version = RTP_VERS,
    .ssrc = rtp_state->ssrc,
    .marker = true // set on the first packet
  };
  float *fsamples = malloc(60 * Dit_length * sizeof *fsamples);
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC,&now);
  int rem = 0; // remainder from division by sample rate
  wchar_t c;
  while((c = *msg++) != 0){
    size_t sample_count = encode_morse_char(fsamples, c);
    float const *outp = fsamples;
    while(sample_count > 0){
      size_t const chunk = min(PCM_BUFSIZE, sample_count);
      sample_count -= chunk;

      rtp.timestamp = rtp_state->timestamp;
      rtp_state->timestamp += chunk;
      rtp.seq = rtp_state->seq++;
      rtp_state->packets++;
      rtp_state->bytes += chunk * sizeof(int16_t);

      uint8_t packet[PKT_SIZE];
      uint8_t *dp = hton_rtp(packet,&rtp);
      for(size_t i=0; i < chunk; i++){
	float const s = ldexpf(*outp++, 15);
	int16_t const is = s > 32767 ? 32767 : s < -32768 ? -32768 : (int16_t)s;
        *dp++ = is >> 8; // big-endian order
	*dp++ = is;
      }
      size_t const len = dp - packet;
      ssize_t const r = send(sock, packet, len, 0);
      if(r <= 0)
	fprintf(stdout,"sendmsg: (%d) %s\n",errno,strerror(errno));

      rtp.marker = false;

      // Wait until this finishes sending
      // Note 48 kHz has a factor of 3, so it's not necessarily a whole number of ns, hence the remainder
      uint64_t const num = rem + BILLION * chunk;
      uint64_t const chunk_ns = num / Samprate;
      rem = num % Samprate;

      now.tv_sec += chunk_ns / BILLION;
      now.tv_nsec += chunk_ns % BILLION;
      while(now.tv_nsec >= BILLION) {
        now.tv_nsec -= BILLION;
	now.tv_sec++;
      }
#ifdef __APPLE__
      {
	struct timespec nspec = {.tv_sec = chunk_ns/BILLION,.tv_nsec = chunk_ns % BILLION};
	nanosleep(&nspec,NULL);
      }
#else
      clock_nanosleep(CLOCK_MONOTONIC,TIMER_ABSTIME,&now,NULL);
#endif
    }
  }
  FREE(fsamples);
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
