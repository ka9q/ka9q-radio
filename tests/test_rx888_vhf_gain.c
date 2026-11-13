#include "rx888_vhf_gain.h"
#include <assert.h>
#include <stdio.h>
int main(void){
  for(int i=0;i<29;i++) assert(rx888_vhf_nearest_rf(rx888_vhf_rf_db[i])==i);
  for(int i=0;i<16;i++) assert(rx888_vhf_nearest_if(rx888_vhf_if_db[i])==i);
  assert(rx888_vhf_nearest_rf(43.4f)==24);
  assert(rx888_vhf_nearest_if(26.5f)==11);
  assert(rx888_vhf_lna[24]==12 && rx888_vhf_mixer[24]==12);
  assert(rx888_vhf_nearest_rf(-100)==0 && rx888_vhf_nearest_rf(100)==28);
  assert(rx888_vhf_nearest_if(-100)==0 && rx888_vhf_nearest_if(100)==15);
  assert(rx888_vhf_nearest_if(NAN)==8);
  puts("PASS: 29 RF, 16 IF, limits, NaN, known-good settings");
  return 0;
}
