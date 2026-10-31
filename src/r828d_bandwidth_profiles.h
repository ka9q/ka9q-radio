#ifndef R828D_BANDWIDTH_PROFILES_H
#define R828D_BANDWIDTH_PROFILES_H
#include <stdint.h>
#include <stddef.h>
/* Source: renardspark/SDDC_Driver SDDC_FX3/driver/tuner_r82xx.c,
 * symmetric IFi[] entries (nominal kHz plus fc correction), and 6/7/8M branches.
 * Preserve R0A high nibble including bit 7 (zero-decode regression when cleared).
 * Filter codes are not evidence of physical on-chip readback.
 */
typedef struct {uint32_t bandwidth_hz,if_hz;uint8_t r0a,r0b,r1e;} r828d_bw_profile;
static const r828d_bw_profile r828d_bw_profiles[]={
 {290000u,1925000u,0x0f,0xe7,0x00},
 {375000u,1857000u,0x0f,0xe8,0x00},
 {420000u,2121000u,0x0f,0xd7,0x00},
 {470000u,1788000u,0x0f,0xe9,0x00},
 {600000u,1706000u,0x0f,0xea,0x00},
 {860000u,1558000u,0x0f,0xeb,0x00},
 {950000u,2205000u,0x0f,0x88,0x00},
 {1100000u,2125000u,0x0f,0x89,0x00},
 {1200000u,1350000u,0x0f,0xee,0x00},
 {1300000u,2043000u,0x0f,0x8a,0x00},
 {1503000u,1276000u,0x0f,0xef,0x60},
 {1600000u,1900000u,0x0f,0x8b,0x00},
 {1753000u,1412000u,0x0f,0xcf,0x60},
 {1800000u,1400000u,0x0f,0xaf,0x00},
 {1953000u,1530000u,0x0f,0x8f,0x60},
 {2200000u,1600000u,0x0f,0x8f,0x00},
 {3000000u,2000000u,0x04,0x8f,0x00},
 {5000000u,3570000u,0x0b,0x6b,0x00},
 {6000000u,3570000u,0x10,0x6b,0x60},
 {7000000u,4570000u,0x10,0x2a,0x60},
 {8000000u,4570000u,0x10,0x0b,0x60},
};
static inline size_t r828d_bw_count(void){return sizeof(r828d_bw_profiles)/sizeof(r828d_bw_profiles[0]);}
static inline const r828d_bw_profile *r828d_bw_find(uint32_t hz){
 for(size_t n=0;n<r828d_bw_count();n++)if(r828d_bw_profiles[n].bandwidth_hz==hz)return &r828d_bw_profiles[n];
 return NULL;
}
static inline uint8_t r828d_bw_mask(uint8_t old,uint8_t value,uint8_t mask){
 return (uint8_t)((old & (uint8_t)~mask)|(value & mask));
}
#endif
