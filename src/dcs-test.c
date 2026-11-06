#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "dcs.h"

static void check_stream(int code,bool inverted,int error_symbol){
  float const rate = 48000.0f;
  uint32_t const word = dcs_codeword(code);
  struct dcs_decoder decoder;
  init_dcs_decoder(&decoder,rate);

  for(int n = 0; n < 96000; n++){
    double const bit_position = 0.37 + n * DCS_BITRATE / rate;
    int const bit_index = (int)floor(bit_position) % 23;
    int bit = (word >> bit_index) & 1;
    if(inverted)
      bit = !bit;
    if(error_symbol >= 0 && bit_index == error_symbol &&
       (int)(bit_position / 23.0) == 1)
      bit = !bit;
    process_dcs_sample(&decoder,bit ? 1.0f : -1.0f);
  }
  if(!dcs_decoder_matches(&decoder,code,inverted)){
    fprintf(stderr,"DCS %03d %s decode failed\n",
            code,inverted ? "inverted" : "normal");
    exit(EXIT_FAILURE);
  }
  if(dcs_decoder_matches(&decoder,25,false)){
    fprintf(stderr,"DCS decoder falsely matched code 025\n");
    exit(EXIT_FAILURE);
  }
  if(code == 23 && !inverted && !dcs_decoder_matches(&decoder,47,true)){
    fprintf(stderr,"DCS decoder missed the known cyclic polarity alias\n");
    exit(EXIT_FAILURE);
  }
  if(code == 72 && inverted &&
     !dcs_decoder_matches(&decoder,245,false)){
    fprintf(stderr,"DCS decoder missed the D072/I245 cyclic alias\n");
    exit(EXIT_FAILURE);
  }
  if(code == 72 && inverted &&
     (decoder.detected_code != 245 || decoder.detected_inverted)){
    fprintf(stderr,"DCS decoder did not report D245 for the D072/I245 alias\n");
    exit(EXIT_FAILURE);
  }
}

int main(void){
  int code;
  bool inverted;
  if(!parse_dcs_code("023",&code,&inverted) || code != 23 || inverted)
    return EXIT_FAILURE;
  if(!parse_dcs_code("I023",&code,&inverted) || code != 23 || !inverted)
    return EXIT_FAILURE;
  if(!parse_dcs_code("off",&code,&inverted) || code != 0 || inverted)
    return EXIT_FAILURE;
  if(!parse_dcs_code("025",&code,&inverted) || code != 25)
    return EXIT_FAILURE;
  if(parse_dcs_code("028",&code,&inverted))
    return EXIT_FAILURE;
  if(dcs_codeword(23) != 0x763813u)
    return EXIT_FAILURE;

  check_stream(23,false,-1);
  check_stream(23,true,-1);
  check_stream(72,true,-1);
  check_stream(411,false,-1);
  check_stream(754,false,-1);
  check_stream(23,false,7);
  puts("DCS decoder tests passed");
  return 0;
}
