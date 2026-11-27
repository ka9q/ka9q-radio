// P25 Phase 1 message decoding: trunking signalling blocks, link control, multi-block
// trunking, packet data headers and encryption sync, into the struct p25_msg of p25msg.h
// Copyright 2026, Matthew Kaufman, KA6SQG

#ifndef _P25P1MSG_H
#define _P25P1MSG_H 1

#include <stdint.h>
#include <stdbool.h>
#include "p25msg.h"

// Every input is the error-corrected message as octets, most significant bit first:
//   TSBK    12 octets (80 bits of message, then the 16-bit CRC)       TIA-102.AABC
//   LC       9 octets (LDU1 or TDULC link control word)               TIA-102.AABF
//   PDU     12-octet header, then the data blocks as the sink decoded them (12 octets
//           each, 18 for a confirmed packet's blocks); AMBTC is a PDU   TIA-102.BAAA, .AABC
//   HDU     15 octets (MI, MFID, ALGID, KID, TGID)                    TIA-102.BAAA
//   ES      12 octets (LDU2 encryption sync: MI, ALGID, KID)
// The decoders check nothing: they are given only what has passed its error correction and,
// where it has one, its CRC.
// Nothing allocates and nothing is global: a decoded message and a band plan are the
// caller's, so channels can be decoded on separate threads.

// Decoders: 0 on success, -1 if the message is too short (msg is then zeroed)
int p25_decode_tsbk(struct p25_msg *msg, uint8_t const tsbk[12]);
int p25_decode_lc(struct p25_msg *msg, uint8_t const lc[9], bool terminator);
int p25_decode_pdu(struct p25_msg *msg, uint8_t const *pdu, int len);
int p25_decode_hdu(struct p25_msg *msg, uint8_t const hdu[15]);
int p25_decode_es(struct p25_msg *msg, uint8_t const es[12]);

// L3Harris talker GPS from its two link control words (block 1, block 2)
void p25_harris_gps(uint8_t const block1[9], uint8_t const block2[9], double *latitude,
                    double *longitude, uint32_t *seconds_of_day, uint16_t *heading);

#endif
