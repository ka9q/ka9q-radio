// RX888 MkII R828D VHF manual gain tables, from SDDC_Driver's
// RX888R2Radio and tuner_r82xx.c. Values are nominal stage gains in dB.
#ifndef RX888_VHF_GAIN_H
#define RX888_VHF_GAIN_H
#include <math.h>

static const float rx888_vhf_rf_db[29] = {
  0.0f,0.9f,1.4f,2.7f,3.7f,7.7f,8.7f,12.5f,14.4f,15.7f,
  16.6f,19.7f,20.7f,22.9f,25.4f,28.0f,29.7f,32.8f,33.8f,
  36.4f,37.2f,38.6f,40.2f,42.1f,43.4f,43.9f,44.5f,48.0f,49.6f
};
static const float rx888_vhf_if_db[16] = {
  -4.7f,-2.1f,0.5f,3.5f,7.7f,11.2f,13.6f,14.9f,
  16.3f,19.5f,23.1f,26.5f,30.0f,33.7f,37.2f,40.8f
};
static const unsigned char rx888_vhf_lna[29] = {
  0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13,14,15
};
static const unsigned char rx888_vhf_mixer[29] = {
  0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13,14
};
static inline int rx888_vhf_nearest_rf(float db){
  if(!isfinite(db)) db = 29.7f;
  int best = 0;
  for(int i=1;i<29;i++)
    if(fabsf(db-rx888_vhf_rf_db[i]) < fabsf(db-rx888_vhf_rf_db[best])) best=i;
  return best;
}
static inline int rx888_vhf_nearest_if(float db){
  if(!isfinite(db)) db = 16.3f;
  int best = 0;
  for(int i=1;i<16;i++)
    if(fabsf(db-rx888_vhf_if_db[i]) < fabsf(db-rx888_vhf_if_db[best])) best=i;
  return best;
}
#endif
