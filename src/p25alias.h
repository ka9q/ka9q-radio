// P25 talker alias reassembly: Motorola (a header and data blocks, in Phase 1 link control
// or Phase 2 MAC structures, obfuscated and CRC protected) and L3Harris (link control
// blocks 1-4; a Phase 2 MAC structure carries the whole alias)
// Copyright 2026, Matthew Kaufman, KA6SQG

#ifndef _P25ALIAS_H
#define _P25ALIAS_H 1

#include <stdint.h>
#include <stdbool.h>
#include "p25msg.h"
#include "p25p2mac.h"

// Data blocks one Motorola alias may have; a header announcing more is never completed
#define P25_ALIAS_MAX_BLOCKS 16
#define P25_ALIAS_MAX 64

// One Motorola reassembly
struct p25_mot_alias {
  int sequence;                // -1 before the first header or block
  bool have_header;
  uint8_t count;               // data blocks the header announced
  uint8_t header[8];           // Phase 2: the header's 64-bit fragment
  uint32_t have;               // bit b-1: data block b is held
  uint8_t block[P25_ALIAS_MAX_BLOCKS][13];  // 44 bits (link control) or 100 (MAC), msb first
};

// The caller's, one per channel or timeslot. Zero it with p25_alias_init().
struct p25_alias {
  struct p25_mot_alias lc, mac;
  uint8_t harris[7];           // L3Harris: the first half of an alias (link control block 1 or 3)
  uint8_t harris_len;          // its octets, NULs off the end
  uint8_t harris_first;        // which block that was (1 or 3), 0 when none is held
  uint32_t harris_src;         // the talker it belongs to, 0 when not known
  uint32_t crc_failures;       // Motorola aliases dropped for a bad CRC (or no text)
};

struct p25_alias_result {
  uint8_t mfid;                // P25_MFID_MOTOROLA or P25_MFID_HARRIS
  bool has_suid;               // Motorola: the talker's WACN, system and unit
  uint32_t wacn;
  uint16_t sysid;
  uint32_t unit;
  int len;                     // octets of text
  char text[P25_ALIAS_MAX];    // UTF-8 as sent, NUL terminated (it may hold a NUL before len)
};

void p25_alias_init(struct p25_alias *a);
// Feed a decoded link control word (any; others are ignored). True when it completes an
// alias, which is then in *out
bool p25_alias_lc(struct p25_alias *a, struct p25_msg const *lc, struct p25_alias_result *out);
// A transmission began or ended (header, terminator, timeout): an L3Harris block held is
// another transmission's and is dropped
void p25_alias_end(struct p25_alias *a);
// Feed a decoded MAC structure (any; others are ignored)
bool p25_alias_mac(struct p25_alias *a, struct p25_mac_struct const *s, struct p25_alias_result *out);
// Decode an assembled Motorola alias: WACN, system and unit (56 bits), the obfuscated
// UTF-16 text, then a CRC-16. False if the CRC fails or there is no text.
bool p25_alias_motorola(uint8_t const *bits, int nbits, struct p25_alias_result *out);

#endif
