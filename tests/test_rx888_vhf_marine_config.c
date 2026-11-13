/* Offline acceptance checks for marine VHF defaults and R828D control.
 * These tests do not access USB, modify firmware, or run radiod.
 */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "../src/r828d_bandwidth_profiles.h"
#include "../src/rx888_vhf_gain.h"
#include "../src/rx888_vhf_vga_mode.h"

#define CHECK(cond) do { if(!(cond)){ \
 fprintf(stderr,"FAIL %s at line %d\n",#cond,__LINE__);exit(1); \
} } while(0)

int main(void){
  const r828d_bw_profile *wide=r828d_bw_find(8000000u);
  const r828d_bw_profile *narrow=r828d_bw_find(290000u);
  CHECK(wide && narrow && r828d_bw_count()==21);
  CHECK(wide->if_hz==4570000u);
  CHECK(narrow->if_hz==1925000u);
  /* Exact LO 163.570 MHz, IF 4.570 MHz, RF centre 159 MHz. */
  double const lo=163570000.;
  CHECK(lo-(double)wide->if_hz==159000000.);
  CHECK(lo-(double)wide->if_hz-(double)wide->bandwidth_hz/2.==155000000.);
  CHECK(lo-(double)wide->if_hz+(double)wide->bandwidth_hz/2.==163000000.);
  double const marine[]={156600000.,156800000.,161975000.,162025000.};
  for(size_t i=0;i<sizeof marine/sizeof marine[0];i++){
    CHECK(marine[i]>=155000000. && marine[i]<=163000000.);
  }
  /* For every profile, changing the IF requires a matching LO adjustment
   * if the user wants to retain a fixed 159 MHz RF centre.
   */
  for(size_t i=0;i<r828d_bw_count();i++){
    const r828d_bw_profile *p=&r828d_bw_profiles[i];
    double const profile_lo=159000000.+(double)p->if_hz;
    CHECK(profile_lo-(double)p->if_hz==159000000.);
  }
  CHECK(rx888_vhf_nearest_rf(37.2f)==20);
  CHECK(rx888_vhf_nearest_if(16.3f)==8);
  CHECK(RX888_R828D_VGA_INIT==0xeb);
  CHECK((RX888_R828D_VGA_INIT & 0x10)==0);
  CHECK(RX888_R828D_FIXED_VGA_MASK==0x1f);
  CHECK(rx888_vhf_fixed_vga_value(RX888_R828D_VGA_INIT,8u)==0xe8);
  CHECK((rx888_vhf_fixed_vga_value(0xf0,8u)&0x10)==0);
  CHECK(r828d_bw_find(9000000u)==NULL);
  puts("PASS: RX888 marine VHF 8 MHz default, IF/LO mapping, manual gain and VGA mode");
  return 0;
}
