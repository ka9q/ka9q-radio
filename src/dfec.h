// Forward error correction for the digital mode decoders: Golay, Hamming, trellis codes
// (Viterbi, hard and soft) and CRCs. Reed-Solomon and BCH over GF(64) are in rs63.h
// Copyright 2026, Matthew Kaufman, KA6SQG
#ifndef _DFEC_H
#define _DFEC_H 1

#include <stdbool.h>
#include <stdint.h>

// Conventions, unless stated otherwise:
// Bits are one per uint8_t (0 or 1), in transmitted order, msb of each field first.
// A decoder corrects in place and returns the number of bits it changed,
// 0 for a clean word, or -1 if the word is uncorrectable, in which case it is left as received.

// Golay(24,12,8): bits 0-11 data, 12-22 Golay(23,12) parity, 23 overall even parity.
// Corrects up to 3 errors, detects 4. Returns bits changed (0-3) or -1
int golay24_decode(uint8_t bits[24]);
// Golay(23,12): bits 0-11 data, 12-22 parity. Perfect: every word is within three bits of one codeword,
// so this always corrects. Returns the bits changed (0-3); *data_fixed (may be NULL) those among bits 0-11
int golay23_decode(uint8_t bits[23],int *data_fixed);
// Golay(18,6,8): Golay(24,12,8) shortened by six leading zero data bits (HDU). bits 0-5 data
int golay18_decode(uint8_t bits[18]);

// Hamming(10,6,3): bits 0-5 data, 6-9 parity (8s bit first). Returns 0, 1 (one bit fixed) or -1
int hamming10_decode(uint8_t bits[10]);
// Same, but a correction of a bit whose confidence[] (one per bit of the word) exceeds
// threshold is taken as a double error elsewhere: -1. confidence may be NULL (hard decode)
int hamming10_decode_soft(uint8_t bits[10],float const confidence[10],float threshold);
#define HAMMING10_HIGH_CONFIDENCE 0.7f

// P25 trellis codes (rate 1/2: 2 bits per 4-bit symbol; rate 3/4: 3 bits), Viterbi decoded.
// nsyms 4-bit symbols (49 for P25), the last flushing the encoder to state 0;
// produces (nsyms - 1) * 2 or * 3 bits in out[].
// Hard: in[] is 4 * nsyms deinterleaved bits; returns the path's Hamming distance from them.
// Ties go to the lowest-numbered predecessor state.
enum { TRELLIS_MAX_SYMS = 128 };
int trellis_12_decode(uint8_t *out,uint8_t const *in,int nsyms);
int trellis_34_decode(uint8_t *out,uint8_t const *in,int nsyms);
// Soft: phase[] is 2 * nsyms deinterleaved dibit phases (radians, the differential phase of
// each pi/4 DQPSK dibit: 00 +45, 01 +135, 10 -45, 11 -135 degrees).
// The branch metric is the squared phase distance to the two ideal dibit phases, unweighted.
// Returns the path metric (FLT_MAX, out[] zeroed, if no path has a finite metric)
float trellis_12_decode_soft(uint8_t *out,float const *phase,int nsyms);
float trellis_34_decode_soft(uint8_t *out,float const *phase,int nsyms);

// CRC-CCITT as P25 uses it (TSBK, PDU header): x^16+x^12+x^5+1, zero preset, msb first, inverted
uint16_t crc_ccitt(uint8_t const *bits,int n);
// 80 data bits followed by their 16-bit CRC. Returns 0 if the CRC checks (also when
// correct_one is set and the single bit error was in the CRC itself), 1 if a single data bit
// error was found and fixed (only with correct_one), -1 otherwise. With correct_one a random
// block passes about once in 676 tries instead of once in 65,536 (see dfec.c)
int crc_ccitt80_check(uint8_t bits[96],bool correct_one);
// Phase 2 MAC CRC-12: x^12+x^11+x^7+x^4+x^2+x+1 (0x897), zero preset, msb first, inverted
uint16_t crc12(uint8_t const *bits,int n);
// n data bits (144 FACCH, 168 SACCH) followed by their 12-bit CRC
bool crc12_check(uint8_t const *bits,int n);

#endif
