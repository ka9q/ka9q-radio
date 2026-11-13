#include <assert.h>
#include <stdio.h>
#include "../src/r828d_bandwidth_profiles.h"
int main(void){
 assert(r828d_bw_count()==21);
 unsigned previous=0;
 for(size_t i=0;i<r828d_bw_count();i++){
    const r828d_bw_profile *p=&r828d_bw_profiles[i];
    assert(p->bandwidth_hz>previous);previous=p->bandwidth_hz;
    assert(p->if_hz>0 && p->if_hz<10000000);
    assert(r828d_bw_find(p->bandwidth_hz)==p);
    /* R0A bit 7 must remain unchanged for all profiles. */
    assert((r828d_bw_mask(0xdb,p->r0a,0x0f)&0x80)==0x80);
    assert((r828d_bw_mask(0x6b,p->r0b,0xef)&0x10)==(0x6b&0x10));
 }
 assert(r828d_bw_find(290000)->if_hz==1925000);
 assert(r828d_bw_find(8000000)->if_hz==4570000);
 assert(r828d_bw_find(123456)==NULL);
 puts("PASS 21 IF profiles, monotonic order, masked R0A and IF centres");
 return 0;
}
