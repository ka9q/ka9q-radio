// Error correction and field extraction for one assembled P25 Phase 1 data unit:
// header, voice (LDU1, LDU2, with their IMBE voice frames), terminator link control,
// trunking signalling blocks and packet data (header, blocks, sequence assembly)
// Copyright 2026, Matthew Kaufman, KA6SQG
#ifndef _P25P1UNIT_H
#define _P25P1UNIT_H 1

#include <stdbool.h>
#include <stdint.h>

// Conventions:
// Bits are one per uint8_t (0 or 1).
// A data unit's payload is in transmitted order, starting with the first bit after the NID,
// status symbols removed (TIA-102.BAAA), and is as long as P25_*_BITS below.
// conf[] has one confidence (0-1) per payload bit, the two bits of a dibit sharing one value.
// phase[] has one differential phase (radians) per payload dibit: 00 +45, 01 +135, 10 -45,
// 11 -135 degrees. Either may be NULL, meaning hard decisions only.
// Every decoded field is msb first, the fields in the order the standard lists them.
// Nothing allocates and nothing is global: all state is in structures the caller owns.

enum {
  P25_HDU_BITS = 658,     // 36 Golay(18,6,8) words, then 10 bits not used
  P25_TDU_BITS = 28,
  P25_LDU_BITS = 1568,    // nine IMBE frames, 24 Hamming(10,6,3) words, low speed data
  P25_TDULC_BITS = 308,   // 12 Golay(24,12,8) words, then 20 bits not used
  P25_BLOCK_BITS = 196,   // one TSBK, PDU header or PDU data block (98 dibits, interleaved)
  P25_IMBE_FRAMES = 9,
  P25_IMBE_BITS = 144,    // one IMBE frame as sent
  P25_IMBE_INFO_BITS = 88, // its information bits (vocoder input)
  P25_PDU_MAX_BLOCKS = 127,
  P25_PDU_MAX_BITS = 96 + P25_PDU_MAX_BLOCKS * 144,
};

// A word of hexbits (6-bit symbols), each protected by an inner code (Golay or Hamming) and
// all by an outer Reed-Solomon code: the HDU header, LDU1 and TDULC link control, LDU2
// encryption sync. Inner codewords that cannot be corrected are Reed-Solomon erasures
struct p25_word {
  bool ok;             // Reed-Solomon decoded; bits[] valid (all zero otherwise)
  int nbits;           // 120 HDU (MI 72, MFID 8, ALGID 8, KID 16, TGID 16), 72 LC, 96 ES (MI, ALGID, KID)
  uint8_t bits[120];
  int inner_fixed;     // bits the inner codes corrected (parity bits included)
  int erasures;        // inner codewords they could not correct
  int rs_fixed;        // hexbits Reed-Solomon changed (0 if !ok)
};

// One IMBE voice frame (TIA-102.BABA): eight codewords c0-c7 interleaved over 72 dibits;
// c0-c3 Golay(23,12), c4-c6 Hamming(15,11), c7 seven bits unprotected, c1-c6 masked by a
// pseudo-random sequence seeded from the information bits of c0.
// The vocoder's frame repeat and muting use the counts: with the error rate
// eR = 0.95 eR(previous frame) + 0.000365 total, a frame is repeated when e0 >= 2 and
// total >= 10 + 40 eR, and the output muted while eR > 0.0875 (TIA-102.BABA)
struct p25_imbe {
  uint8_t raw[144];    // the frame as received (interleaved and masked codewords)
  uint8_t u[88];       // information bits u0 (12) u1 u2 u3 (12 each) u4 u5 u6 (11 each) u7 (7),
                       // each vector msb first: the order the vocoder takes them
  int errors[7];       // bits corrected in c0 .. c6 (Golay 0-3, Hamming 0-1)
  int e0;              // errors in c0 (epsilon 0 of TIA-102.BABA)
  int e4;              // errors in c4 (epsilon 4)
  int total;           // errors in c0-c6 (epsilon T)
};

struct p25_ldu {
  int duid;            // 5 LDU1, 10 LDU2
  // Soft pre-correction of the 24 Hamming words in the payload, in place; only with conf[]:
  // a Hamming correction of a bit more confident than 0.7 is taken as a double error, and
  // if any word fails, Reed-Solomon with those words as erasures rewrites all 24 hexbits
  bool soft;           // ran (conf given)
  int soft_fixed;      // Hamming words corrected
  int soft_erasures;   // Hamming words not corrected
  int soft_rs;         // 0 not needed (no erasures), 1 hexbits rewritten, -1 Reed-Solomon failed
  int soft_rs_fixed;   // hexbits it changed
  // Then a hard Hamming + Reed-Solomon decode of the (pre-corrected) payload
  struct p25_word word; // LDU1 link control (72 bits) or LDU2 encryption sync (96)
  struct p25_imbe voice[P25_IMBE_FRAMES];
  uint8_t lsd[32];     // the two low speed data octets with their check octets, not corrected
};

// A trellis-decoded 196-bit block: TSBK, PDU header (rate 1/2, 96 bits) or PDU data block
// (rate 1/2 unconfirmed, 96 bits; rate 3/4 confirmed, 144)
struct p25_block {
  int nbits;
  uint8_t bits[144];   // TSBK and PDU header: 80 message bits, then the CRC
  bool soft;           // soft Viterbi on the phases (else hard on the bits)
  float metric;        // soft: path metric; hard: Hamming distance of the path from the bits
  int crc;             // TSBK, header: 0 passed, 1 passed after one data bit was fixed, -1 failed
  bool last;           // TSBK: last block of its TSDU (CRC passed and bit 0 set)
};

enum p25_pdu_kind {
  P25_PDUKIND_DATA = 0,    // any format but these two
  P25_PDUKIND_UMBTC,       // format 21: unconfirmed multi-block trunking control
  P25_PDUKIND_AMBTC,       // format 23: alternate multi-block trunking control
};

// Packet data sequence assembler: a header announcing N blocks, then N data blocks
struct p25_pdu {
  bool pending;        // a header has been taken and blocks are expected
  bool confirmed;      // header bit 1: confirmed delivery, data blocks rate 3/4
  bool outbound;       // header bit 2
  int format;          // header bits 3-7
  uint8_t vendor;      // header bits 16-23 (MFID)
  int blocks_to_follow; // header bits 49-55
  int nblocks;         // data blocks taken
  int block_bits;      // 96 or 144
  // Completed sequence, valid when a call has just returned 1
  int kind;            // enum p25_pdu_kind
  int nbits;           // 96 + nblocks * block_bits
  int crc;             // packet CRC over the data blocks: 1 passed, 0 failed, -1 there are no data blocks
  uint8_t payload[P25_PDU_MAX_BITS]; // the 96 header bits, then each data block's bits
};

// HDU: Golay(18,6,8) on each of 36 hexbits, then RS(36,20,17). Returns w->ok
bool p25_hdu_decode(struct p25_word *w,uint8_t const bits[P25_HDU_BITS]);
// TDULC: Golay(24,12,8) on 12 pairs of hexbits, then RS(24,12,13). Returns w->ok
bool p25_tdulc_decode(struct p25_word *w,uint8_t const bits[P25_TDULC_BITS]);
// LDU1 or LDU2 (duid 5 or 10). bits[] is corrected in place by the soft stage (conf != NULL);
// the IMBE frames are decoded from it. Returns ldu->word.ok
bool p25_ldu_decode(struct p25_ldu *ldu,int duid,uint8_t bits[P25_LDU_BITS],float const conf[P25_LDU_BITS]);
// One IMBE frame: deinterleave, c0, unmask, c1-c6, information bits and error counts
void p25_imbe_decode(struct p25_imbe *f,uint8_t const raw[P25_IMBE_BITS]);

// TSBK: deinterleave, rate 1/2 Viterbi (soft if phase != NULL), CRC-CCITT check, fixing a
// single data bit error if correct_one. Returns b->crc
int p25_tsbk_decode(struct p25_block *b,uint8_t const bits[P25_BLOCK_BITS],float const phase[P25_BLOCK_BITS/2],
                    bool correct_one);

// Packet data. Each returns 1 when a sequence is complete (p->payload, nbits, kind),
// 0 when more data blocks are expected (keep assembling under the same frame sync),
// -1 when the block is refused: a header failing its CRC (the assembler is reset), or a data
// block with no header pending. Either way the decoded block is in *b
void p25_pdu_reset(struct p25_pdu *p); // also when a new frame sync cuts a sequence short
int p25_pdu_header(struct p25_pdu *p,struct p25_block *b,uint8_t const bits[P25_BLOCK_BITS],
                   float const phase[P25_BLOCK_BITS/2],bool correct_one);
int p25_pdu_block(struct p25_pdu *p,struct p25_block *b,uint8_t const bits[P25_BLOCK_BITS],
                  float const phase[P25_BLOCK_BITS/2]);

// P25 Phase 1 data deinterleave (196 bits = 98 dibits of a TSBK, PDU header or data block)
// out[p25_data_deinterleave[i]] = in[i]
extern uint8_t const p25_data_deinterleave[196];
void p25_deinterleave_bits(uint8_t out[196],uint8_t const in[196]);
void p25_deinterleave_bit_floats(float out[196],float const in[196]); // per-bit confidences
void p25_deinterleave_dibit_floats(float out[98],float const in[98]); // per-dibit phases or confidences

// Pack bits msb first into octets (the last one zero-filled), e.g. for p25msg.h
void p25_pack_bits(uint8_t *octets,uint8_t const *bits,int nbits);

#endif
