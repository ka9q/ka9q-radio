// DCS (Digital-Coded Squelch) decoder
#ifndef _DCS_H
#define _DCS_H 1

#include <stdbool.h>
#include <stdint.h>

#define DCS_BITRATE 134.4f
#define DCS_WORD_MASK 0x7fffffu
#define DCS_PHASES 23

struct dcs_candidate {
  int code;
  bool inverted;
  int age;
  int repeats;
};

struct dcs_decoder {
  float sample_rate;
  float filter_alpha;
  float lowpass1;
  float lowpass2;
  float dc_alpha;
  float dc;
  float phase;
  float bit_integral;
  int previous_sign;
  uint32_t shift;
  int bit_count;

  uint32_t codewords[83];
  int codes[83];
  uint64_t bit_number;
  struct dcs_candidate candidates[DCS_PHASES];
  int detected_code;
  bool detected_inverted;
};

bool parse_dcs_code(char const *text,int *code,bool *inverted);
uint32_t dcs_codeword(int code);
void init_dcs_decoder(struct dcs_decoder *decoder,float sample_rate);
void reset_dcs_decoder(struct dcs_decoder *decoder);
int process_dcs_sample(struct dcs_decoder *decoder,float sample);
bool dcs_decoder_matches(struct dcs_decoder const *decoder,int code,bool inverted);

#endif
