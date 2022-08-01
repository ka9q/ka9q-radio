// $Id: cwd.c,v 1.1 2022/08/01 02:50:59 karn Exp $
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
#include <string.h>
#include <errno.h>
#include "misc.h"
#include "multicast.h"
#include "osc.h"

int const Samprate = 48000; // Too hard to change
float CW_speed = 18.0;
float CW_pitch = 500.0;
float CW_level = 12000.0;
int const Default_ssrc = 100;

int Verbose = 0;

char const *Input = "/run/cw/input";
char const *Target = NULL;

#define PCM_BUFSIZE 480        // 16-bit sample count per packet; must fit in Ethernet MTU

struct morse {
  wint_t c;
  char const *code;
};

// Gets sorted at startup for binary search
// Table from Wikipedia: http://en.wikipedia.org/wiki/Morse_code
static struct morse Morse_table[] = {
  { ' ', " " },
  { 'a', "._" },
  { 'b', "_..." },
  { 'c', "_._." },
  { 'd', "_.." },
  { 'e', "." },
  { 'f', ".._." },
  { 'g', "__." },
  { 'h', "...." },
  { 'i', ".." },
  { 'j', ".___" },
  { 'k', "_._" },
  { 'l', "._.." },
  { 'm', "__" },
  { 'n', "_." },
  { 'o', "___" },
  { 'p', ".__." },
  { 'q', "__._", },
  { 'r', "._." },
  { 's', "..." },
  { 't', "_" },
  { 'u', ".._" },
  { 'v', "..._" },
  { 'w', ".__" },
  { 'x', "_.._" },
  { 'y', "_.__" },
  { 'z', "__.." },
  { '0', "_____" },
  { '1', ".____" },
  { '2', "..___" },
  { '3', "...__" },
  { '4', "...._" },
  { '5', "....." },
  { '6', "_...." },
  { '7', "__..." },
  { '8', "___.." },
  { '9', "____." },
  { '.', "._._._" },
  { ',', "__..__" },
  { '?', "..__.." },
  { '\'', ".____." },
  { '!', "_._.__" },
  { '/', "_.._." },
  { '(', "_.__." },
  { ')', "_.__._" },
  { '&', "._..." },
  { ':', "___..." },
  { ';', "_._._." },
  { '=', "_..._" },
  { '+', "._._." },
  { '-', "_...._" },
  { '_', "..__._" },
  { '"', "._.._." },
  { '$', "..._.._" },
  { '@', ".__._." },

  // Accented Latin
  { L'à', ".__._" },  // a + accent grave
  { L'ä', "._._" },   // a + umlaut
  { L'ą', "._._"},    //a + ogonek
  { L'æ', "._._" },   // ae
  { L'å', ".__._" }, 
  { L'ć', "_._.." },  // c/C + accent acute
  { L'ĉ', "_._.." },  // c/C + circumflex
  { L'ç', "_.-.." },  
  // ch as a digraph has no unicode encoding
  { L'đ', ".._.." },  // d/D with stroke
  { L'ð', "..__." },  // eth (very similar to D with stroke)
  { L'é', ".._.." },  // e/E with accent acute
  { L'ę', ".._.." },  // e/E with tail
  { L'ĝ', "__._." },  // g/G with circumflex */
  { L'ĥ', "____" },   // h/H with circumflex */
  { L'ĵ', ".___." },  // j/J with circumflex */
  { L'ł', "._.._" },  // l/L with stroke */
  { L'ń', "__.__" },  // n/N with accent acute */
  { L'ñ', "__.__" },  // n/N with tilde (Spanish ene) 
  { L'ó', "___." },   // o/O with accent acute
  { L'ö', "___." },   // o/O with umlaut
  { L'ø', "___." },   // o/O with stroke
  { L'ś', "..._..." },// s/S with accent acute
  { L'ŝ', "..._." },  // s/S with circumflex (esperanto)
  { L'š', "____" },   // s/S with caron
  { L'þ', ".__.." },  // Thorn
  { L'ü', "..__" },   // u/U with umlaut
  { L'ŭ', "..__" },   // u/U with breve
  { L'ź', "__.._." }, // z/Z with accent acute
  { L'ż', "__.._" },  // z/Z with overdot

  // Greek
  { L'α', "._" }, // alpha
  { L'β', "_..."},// beta
  { L'γ', "__."}, // gamma
  { L'δ', "_.."}, // delta
  { L'ε', "."},   // epsilon
  { L'ζ', "__.."},// zeta
  { L'η', "...."},// eta
  { L'θ', "_._."},// theta
  { L'ι', ".."},  // iota
  { L'κ',"_._"},  // kappa
  { L'λ',"._.."}, // lambda
  { L'μ',"__"},   // mu
  { L'ν',"_."},   // nu
  { L'ξ',"_.._"}, // xi
  { L'ο',"___"},  // omicron
  { L'π',".__."}, // pi
  { L'ρ',"._."},  // rho
  { L'σ',"..."},  // sigma
  { L'ς',"..."},  // final sigma (stigma)
  { L'τ',"_"},    // tau
  { L'υ',"_.__"}, // upsilon
  { L'φ',".._."}, // phi
  { L'χ',"____"}, // chi
  { L'ψ',"__._"}, // psi
  { L'ω',".__"},  // omega

  // Russian
  { L'а',"._"},
  { L'б',"_..."},
  { L'в',".__"},
  { L'г',"__."},
  { L'д',"_.."},
  { L'е',"."},
  { L'ж',"..._"},
  { L'з',"__.."},
#ifdef UKRAINIAN
  { L'и',"_.__"}, // conflicts with same character in Russian
#else // Russian
  { L'и',".."},
#endif
  { L'й',".___"},
  { L'к',"_._"},
  { L'л',"._.."},
  { L'м',"__"},
  { L'н',"_."},
  { L'о',"___"},
  { L'п',".__."},
  { L'р',"._."},
  { L'с',"..."},
  { L'т',"_"},
  { L'у',".._"},
  { L'ф',".._."},
  { L'х',"...."},
  { L'ц',"_._."},
  { L'ч',"___."},
  { L'ш',"____"},
  { L'щ',"__._"},
  { L'ь',"_.._"},
  { L'ы',"_.__"},
  { L'э',".._.."},
  { L'ю',"..__"},
  { L'я',"._._"},
  { L'ё',"."}, // Same as 'е'
  // Ukrainian variants that don't conflict with Russian
  { L'є',".._.."},
  { L'і',".."},
  { L'ї',".___."},

  // Hebrew (did I get this right?)
  { L'א',"._"},   // alef 
  { L'ב',"_..."}, // bet  
  { L'ג',"__."},  // gimel
  { L'ד',"_.."},  // dalet
  { L'ה',"___"},  // he
  { L'ו',"."},    // vav
  { L'ז',"__.."}, // zayin
  { L'ח',"...."}, // het
  { L'ט',".._"},  // tet
  { L'י',".."},   // yod
  { L'ך',"_._"},  // final kaf
  { L'כ',"_._"},  // kaf
  { L'ל',"._.."}, // lamed
  { L'ם',"__"},   // final mem
  { L'מ',"__"},   // mem
  { L'ן',"_."},   // final nun
  { L'נ',"_."},   // nun
  { L'ס',"_._."}, // samekh
  { L'ע',".___"}, // ayin
  { L'ף',".__."}, // final pe
  { L'פ',".__."}, // pe
  { L'ץ',".__"},  // final tsadi
  { L'צ',".__"},  // tsadi
  { L'ק',"__._"}, // qof
  { L'ר',"._."},  // resh
  { L'ש',"..."},  // shin
  { L'ת',"_"},    // tav
};
    
#define TABSIZE (sizeof(Morse_table)/sizeof(Morse_table[0]))

// Comparison function for sort and bsearch on Morse table
static int mcompar(const void *a,const void *b){
  const struct morse * const am = (struct morse *)a;
  const struct morse * const bm = (struct morse *)b;
  
  if(am->c < bm->c)
    return -1;
  else if(am->c > bm->c)
    return +1;
  else
    return 0;
}


// Precomputed dots and dashes, stored in network byte order
static int Dit_length; // # samples in the key-down period of a dit
static int16_t *Dit;  // one element key-down, one element key-up
static int16_t *Dah;  // three elements key-down, one element key-up


int send_cw(int sock, struct rtp_state *rtp_state, wint_t c){
  // Should be longer than any character
  int16_t * const samples = malloc(60 * Dit_length * sizeof(samples[0]));

  struct rtp_header rtp;
  memset(&rtp,0,sizeof(rtp));
  rtp.type = PCM_MONO_PT;
  rtp.version = RTP_VERS;
  rtp.ssrc = rtp_state->ssrc;
  rtp.marker = true; // Start with marker bit on to reset playout buffer

  c = towlower(c);

  struct morse const * const mp = bsearch(&c,Morse_table,TABSIZE,sizeof(Morse_table[0]),mcompar);
  if(mp == NULL)
    return -1;

  int16_t *outp = samples;
  for(int j=0;mp->code[j] != 0; j++){
    switch(mp->code[j]){
    case ' ':
      // inter-word space, 4 dits
      for(int k=0; k < 4 * Dit_length; k++)
	*outp++ = 0;
      break;
    case '.':
      // One dit on, one dit off
      for(int k=0; k < 2 * Dit_length; k++)
	*outp++ = ntohs(Dit[k]);
      break;
    case '-':
    case '_':
      // three dits on, one dit off
      for(int k=0; k < 4 * Dit_length; k++)
	*outp++ = ntohs(Dah[k]);
      break;
    default:
      break; // Ignore
    }
  }
  // Inter-letter space (2 additional dits = 3 total)
  for(int k=0; k < 2 * Dit_length; k++)
    *outp++ = 0;
  
  int sample_count = outp - samples;
  outp = samples;
  
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
      fprintf(stdout,"iovec[0] = (%p,%lu) iovec[1] = (%p,%lu)\n",iovec[0].iov_base,iovec[0].iov_len,
	      iovec[1].iov_base,iovec[1].iov_len);

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
      long long nanosec = (long long)BILLION * chunk / Samprate;
      struct timespec delay;
      
      delay.tv_nsec = nanosec % BILLION;
      delay.tv_sec = nanosec / BILLION;
      nanosleep(&delay,NULL);
    }
  }
  free(samples);
  return 0;
}

#include <stdio.h>

void init_cw(float const speed,float const pitch,float const level,float const samprate){
  qsort(Morse_table,TABSIZE,sizeof(Morse_table[0]),mcompar);

  Dit_length = samprate * 1.2 / speed; // Samples per dit
  float const cycles_per_sample = pitch / samprate;

  if(Verbose){
    fprintf(stdout,"speed %.1f wpm, pitch %.1f Hz, level %.1f, samprate %.1f\n",
	    speed,pitch,level,samprate);
    fprintf(stdout,"dit length %d samples; cycles per sample %f\n",Dit_length,cycles_per_sample);
  }
  // Precompute element audio
  struct osc tone;
  set_osc(&tone,cycles_per_sample,0.0);

  // Exponential envelope shaping to avoid key clicks
  float const tau = .005; // 5 ms time constant sounds good
  float const g = -expm1(-1/(samprate * tau)); // -expm1(x) = 1 - exp(x)

  if(Dit)
    free(Dit);
  Dit = calloc(2*Dit_length,sizeof(Dit[0]));

  if(Dah)
    free(Dah);
  Dah = calloc(4*Dit_length,sizeof(Dah[0]));

  // First element of dit and dah are the same
  int k;
  float envelope = 0;
  for(k=0; k < Dit_length; k++){
    float s = level * creal(step_osc(&tone));
    Dah[k] = Dit[k] = s * envelope;
    envelope += g * (1 - envelope);
  }

  // Second element of dah continues while dit decays
  float dit_envelope = envelope;
  float dah_envelope = envelope;

  for(; k < 2*Dit_length; k++){
    float s = level * creal(step_osc(&tone));
    Dit[k] = s * dit_envelope;
    Dah[k] = s * dah_envelope;    
    dit_envelope += g * (0 - dit_envelope);
    dah_envelope += g * (1 - dah_envelope);    
  }
  // Third element of dah continues
  for(; k < 3*Dit_length; k++){
    float s = level * creal(step_osc(&tone));
    Dah[k] = s * dah_envelope;    
    dah_envelope += g * (1 - dah_envelope);    
  }
  // Fourth element of dah decays
  for(; k < 4*Dit_length; k++){
    float s = level * creal(step_osc(&tone));
    Dah[k] = s * dah_envelope;
    dah_envelope += g * (0 - dah_envelope);    
  }
  // end initialization
}

int main(int argc,char *argv[]){
  
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
  init_cw(CW_speed,CW_pitch,CW_level,Samprate);
  struct sockaddr sock;
  int const fd = setup_mcast(Target,&sock,1,1,0,0);
  if(fd == -1){
    fprintf(stdout,"Can't resolve %s\n",Target);
    exit(1);
  }
  if(mkfifo(Input,0666) != 0 && errno != EEXIST){
    fprintf(stdout,"Can't make input fifo %s\n",Input);
    exit(1);
  }

  FILE *fp = fopen(Input,"r");
  if(fp == NULL){
    fprintf(stdout,"Can't open %s\n",Input);
    exit(1);
  }
  FILE *fp_out = fopen(Input,"w"); // Hold open (and idle) so we won't get EOF

  wint_t cc;
  while((cc = fgetwc(fp)) != WEOF){
    send_cw(fd,&rtp_state,cc);
  }
  perror("fgetwc");

  close(fd);
  fclose(fp); fp = NULL;
  fclose(fp_out); fp_out = NULL;
  exit(0);
}
