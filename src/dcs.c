// DCS (Digital-Coded Squelch) code parsing and streaming decoder
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "dcs.h"

static int const Standard_codes[] = {
   23,  25,  26,  31,  32,  43,  47,  51,  54,  65,  71,  72,  73,  74,
  114, 115, 116, 125, 131, 132, 134, 143, 152, 155, 156, 162, 165, 172, 174,
  205, 223, 226, 243, 244, 245, 251, 261, 263, 265, 271,
  306, 311, 315, 331, 343, 346, 351, 364, 365, 371,
  411, 412, 413, 423, 431, 432, 445, 464, 465, 466,
  503, 506, 516, 532, 546, 565,
  606, 612, 624, 627, 631, 632, 654, 662, 664,
  703, 712, 723, 731, 732, 734, 743, 754
};

static int code_to_bin(int code){
  return ((code / 100) << 6) | (((code / 10) % 10) << 3) | (code % 10);
}

uint32_t dcs_codeword(int code){
  if(code < 0 || code > 777 ||
     code % 10 > 7 || (code / 10) % 10 > 7 || code / 100 > 7)
    return 0;

  uint32_t const info = 0x800u | (uint32_t)code_to_bin(code);
  uint32_t rem = info << 11;
  for(int i = 22; i >= 11; i--){
    if(rem & (1u << i))
      rem ^= 0xc75u << (i - 11);
  }
  return ((rem & 0x7ffu) << 12) | info;
}

bool parse_dcs_code(char const *text,int *code,bool *inverted){
  if(text == NULL || code == NULL || inverted == NULL)
    return false;
  while(isspace((unsigned char)*text))
    text++;
  size_t len = strlen(text);
  while(len > 0 && isspace((unsigned char)text[len-1]))
    len--;
  if((len == 3 && strncasecmp(text,"off",len) == 0) ||
     (len == 4 && strncasecmp(text,"none",len) == 0)){
    *code = 0;
    *inverted = false;
    return true;
  }
  bool inv = false;
  if(*text == 'I' || *text == 'i' || *text == 'D' || *text == 'd'){
    inv = *text == 'I' || *text == 'i';
    text++;
    len--;
  }
  if(len != 3)
    return false;
  int value = 0;
  for(int i = 0; i < 3; i++){
    if(text[i] < '0' || text[i] > '7')
      return false;
    value = 10 * value + (text[i] - '0');
  }
  bool standard = false;
  for(size_t i = 0; i < sizeof Standard_codes / sizeof Standard_codes[0]; i++){
    if(Standard_codes[i] == value){
      standard = true;
      break;
    }
  }
  if(!standard)
    return false;
  *code = value;
  *inverted = inv;
  return true;
}

void reset_dcs_decoder(struct dcs_decoder *decoder){
  if(decoder == NULL)
    return;
  decoder->lowpass1 = 0;
  decoder->lowpass2 = 0;
  decoder->dc = 0;
  decoder->phase = 0;
  decoder->bit_integral = 0;
  decoder->previous_sign = 0;
  decoder->shift = 0;
  decoder->bit_count = 0;
  decoder->bit_number = 0;
  memset(decoder->candidates,0,sizeof decoder->candidates);
  decoder->detected_code = 0;
  decoder->detected_inverted = false;
}

void init_dcs_decoder(struct dcs_decoder *decoder,float sample_rate){
  if(decoder == NULL)
    return;
  memset(decoder,0,sizeof *decoder);
  decoder->sample_rate = sample_rate;
  if(!isfinite(sample_rate) || sample_rate <= 0)
    return;
  decoder->filter_alpha = -expm1f(-2.0f * 3.14159265358979323846f * 300.0f / sample_rate);
  decoder->dc_alpha = -expm1f(-1.0f / (0.5f * sample_rate));
  for(size_t i = 0; i < sizeof Standard_codes / sizeof Standard_codes[0]; i++){
    decoder->codes[i] = Standard_codes[i];
    decoder->codewords[i] = dcs_codeword(Standard_codes[i]);
  }
  reset_dcs_decoder(decoder);
}

static int popcount23(uint32_t value){
  int count = 0;
  while(value){
    value &= value - 1;
    count++;
  }
  return count;
}

static void decode_bit(struct dcs_decoder *decoder,int bit){
  decoder->shift = (decoder->shift >> 1) | ((uint32_t)(bit != 0) << 22);
  if(decoder->bit_count < 23)
    decoder->bit_count++;
  if(decoder->bit_count < 23)
    return;

  for(int i = 0; i < DCS_PHASES; i++){
    if(decoder->candidates[i].age < 1000)
      decoder->candidates[i].age++;
    if(decoder->candidates[i].age > 48)
      decoder->candidates[i].repeats = 0;
  }
  int const phase = (int)(decoder->bit_number++ % DCS_PHASES);
  int best_code = 0;
  bool best_inverted = false;
  int best_distance = 4;
  for(size_t i = 0; i < sizeof Standard_codes / sizeof Standard_codes[0]; i++){
    int const normal_distance = popcount23(decoder->shift ^ decoder->codewords[i]);
    if(normal_distance < best_distance){
      best_distance = normal_distance;
      best_code = decoder->codes[i];
      best_inverted = false;
    }
    int const inverted_distance =
      popcount23((decoder->shift ^ ~decoder->codewords[i]) & DCS_WORD_MASK);
    if(inverted_distance < best_distance){
      best_distance = inverted_distance;
      best_code = decoder->codes[i];
      best_inverted = true;
    }
  }
  if(best_distance > 3){
    goto report;
  }
  struct dcs_candidate *candidate = &decoder->candidates[phase];
  if(candidate->code == best_code &&
     candidate->inverted == best_inverted &&
     candidate->age >= 22 && candidate->age <= 24){
    candidate->repeats++;
  } else {
    candidate->code = best_code;
    candidate->inverted = best_inverted;
    candidate->repeats = 1;
  }
  candidate->age = 0;

 report:
  decoder->detected_code = 0;
  decoder->detected_inverted = false;
  for(int i = 0; i < DCS_PHASES; i++){
    struct dcs_candidate const *candidate = &decoder->candidates[i];
    if(candidate->repeats < 2 || candidate->age > 48)
      continue;
    if(decoder->detected_code == 0 ||
       (decoder->detected_inverted && !candidate->inverted)){
      decoder->detected_code = candidate->code;
      decoder->detected_inverted = candidate->inverted;
    }
  }
}

int process_dcs_sample(struct dcs_decoder *decoder,float sample){
  if(decoder == NULL || !isfinite(decoder->sample_rate) || decoder->sample_rate <= 0)
    return 0;

  decoder->lowpass1 += decoder->filter_alpha * (sample - decoder->lowpass1);
  decoder->lowpass2 += decoder->filter_alpha * (decoder->lowpass1 - decoder->lowpass2);
  float x = decoder->lowpass2;
  decoder->dc += decoder->dc_alpha * (x - decoder->dc);
  x -= decoder->dc;
  decoder->phase += DCS_BITRATE / decoder->sample_rate;

  if(decoder->phase >= 1.0f){
    decoder->phase -= 1.0f;
    decode_bit(decoder,decoder->bit_integral > 0);
    decoder->bit_integral = 0;
  }
  decoder->bit_integral += x;

  int const sign = x >= 0 ? 1 : -1;
  if(decoder->previous_sign != 0 && sign != decoder->previous_sign){
    float const error = decoder->phase < 0.5f ? decoder->phase : decoder->phase - 1.0f;
    decoder->phase -= 0.2f * error;
  }
  decoder->previous_sign = sign;
  return decoder->detected_code;
}

bool dcs_decoder_matches(struct dcs_decoder const *decoder,int code,bool inverted){
  if(decoder == NULL || code == 0)
    return false;
  for(int i = 0; i < DCS_PHASES; i++){
    struct dcs_candidate const *candidate = &decoder->candidates[i];
    if(candidate->code == code && candidate->inverted == inverted &&
       candidate->repeats >= 2 && candidate->age <= 48)
      return true;
  }
  return false;
}
