// Simple sample rate decimators & half-band filters by powers of 2
// Not currently used in ka9q-radio, but here for reference or possible future use
// Copyright 2017-2023, Phil Karn, KA9Q
#ifndef _DECIMATE_H
#define _DECIMATE_H 1

struct hb15_state {
  float coeffs[4];
  float even_samples[4];
  float odd_samples[4];
  float old_odd_samples[4];
};
void hb15_block(struct hb15_state *state,float *output,float *input,int cnt);
void hb3_block(float *state,float *output,float *input,int cnt);


#endif
