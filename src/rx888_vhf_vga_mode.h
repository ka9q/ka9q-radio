#ifndef RX888_VHF_VGA_MODE_H
#define RX888_VHF_VGA_MODE_H
#include <stdint.h>

/* R828D fixed VGA mode (bit 4 = 0) from confirmed KA9Q/W1EUJ AIS path.
 * Original VHF code initialized R0C=F0, and a 0x0f gain mask left
 * VGA_MODE=1 (the proven one-bit receiver regression).
 */
#define RX888_R828D_VGA_INIT ((uint8_t)0xeb)
#define RX888_R828D_FIXED_VGA_MASK ((uint8_t)0x1f)

static inline uint8_t rx888_vhf_fixed_vga_value(uint8_t existing,
                                                 unsigned gain_index){
  return (uint8_t)((existing & (uint8_t)~RX888_R828D_FIXED_VGA_MASK) |
                   ((uint8_t)gain_index & RX888_R828D_FIXED_VGA_MASK));
}
#endif
