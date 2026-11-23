// Error correction and field extraction for one assembled P25 Phase 1 data unit,
// and IMBE voice frame decoding to vocoder input (see p25p1unit.h)
// Copyright 2026, Matthew Kaufman, KA6SQG
#include <assert.h>
#include <stddef.h>
#include <string.h>

#include "dfec.h"
#include "rs63.h"
#include "p25p1unit.h"

// Theory of operation
//
// What arrives. One data unit's payload from p25p1.c: the bits after the NID with the status
// symbols taken out, in the order received, and for each bit the confidence of its dibit, and
// for each dibit its phase change. Everything here works on that, one data unit at a time.
//
// Hexbit words. The header, link control and encryption sync words are strings of 6-bit
// symbols (hexbits) under a Reed-Solomon code over GF(64), each hexbit (or pair) sent inside a
// short binary codeword:
//   HDU     RS(36,20,17), each hexbit the data of a Golay(18,6,8) word
//   TDULC   RS(24,12,13), each pair of hexbits the data of a Golay(24,12,8) word
//   LDU1    RS(24,12,13) link control, each hexbit in a Hamming(10,6,3) word
//   LDU2    RS(24,16,9) encryption sync, the same way
// The inner code corrects what it can, and a word it cannot correct marks its hexbits as
// erasures for the outer code; Reed-Solomon corrects e errors and f erasures when 2e + f does
// not exceed the parity count, so an erasure costs half an error. The word is good only if the
// Reed-Solomon decode succeeds; p25_word counts what each stage did.
//
// LDUs. A voice data unit is nine IMBE frames with the 24 Hamming words spread between them and
// 32 bits of low speed data before the last frame (layout below). The LDU has a soft stage: a
// Hamming correction of a bit whose confidence is high is refused and the word erased, since one
// error in a confident bit is less likely than two errors elsewhere; if that erases anything,
// Reed-Solomon runs on the result and writes its corrected hexbits back into the payload. The
// word is then decoded again from hard bits. The Hamming parity bits are left as received, so
// that second decode is a check on the first: measured on generated LDUs, when the soft result
// is right the hard one agrees (all but 1 of 623,000 words), and when it is wrong the hard one
// refuses about half of them. Taking the soft result as the word instead fails 4% fewer
// encryption sync words at 6 dB Es/N0 but accepts 56% more wrong ones.
//
// IMBE frames (TIA-102.BABA). 144 bits: c0-c3 Golay(23,12), c4-c6 Hamming(15,11), c7 seven bits
// unprotected, 88 information bits u0-u7 in all. They are interleaved over the frame, and c1-c6
// are masked by a pseudo-random sequence seeded from u0, the twelve data bits of c0. A wrong c0
// thus unmasks the rest with the wrong sequence, which shows as many corrections in c1-c6: the
// total count, which the vocoder's repeat rule reads with c0's own count, rises with it. The
// counts per codeword are delivered with the bits; deciding what to do with a bad frame is the
// vocoder's job (p25audio.c).
//
// Trunking blocks and packet data (TIA-102.BAAA). 196 bits, 98 dibits, 49 four-bit trellis
// symbols, interleaved so that symbols sent one after another are four apart in the trellis
// (see p25_data_deinterleave), which spreads a burst of errors along the decoder's path. Rate
// 1/2 gives 96 bits (a trunking block or packet header: 80 bits and their CRC-CCITT; or an
// unconfirmed packet's data block), rate 3/4 144 (a confirmed packet's data block). With
// phases supplied the Viterbi decoder is soft
// (dfec.c).
// A packet's header has its own CRC, but that says nothing of the data blocks after it. Those
// are covered by the packet CRC, the last 32 bits of the last block, taken over the data of all
// the blocks (a confirmed block's serial number and CRC-9 left out). It matters most for
// multi-block trunking messages, whose grants and channel numbers are in the data blocks: on a
// weak control channel, messages whose header passed and whose data block did not were seen
// to grant calls on channels that do not exist, before this was checked.

// ---------------------------------------------------------------------------
// Hexbit words (TIA-102.BAAA)

static int get_hex(uint8_t const *bits){
  int v = 0;
  for(int i = 0; i < 6; i++)
    v = (v << 1) | (bits[i] & 1);
  return v;
}
static void put_hex(uint8_t *bits,int v){
  for(int i = 0; i < 6; i++)
    bits[i] = (v >> (5 - i)) & 1;
}

// Reed-Solomon over a word's nhex hexbits, hex[0] sent first: data hexbits 0 .. ndata-1,
// then the parity. The Phase 1 codes take the last hexbit sent as the coefficient of x^0,
// so the parity sits at positions 0 .. nhex-ndata-1 and the data above it.
// Corrects hex[] in place; returns the hexbits changed or -1 (hex[] untouched)
static int rs_hexbits(int hex[],int nhex,int ndata,bool const erased[]){
  uint8_t sym[63];
  int eras[36];
  int nerased = 0;
  memset(sym,0,sizeof sym);
  for(int i = 0; i < nhex; i++){
    sym[nhex-1-i] = (uint8_t)hex[i];
    if(erased[i])
      eras[nerased++] = nhex-1-i;
  }
  int const r = rs63_decode(sym,nhex-ndata,nhex,nerased > 0 ? eras : NULL,nerased);
  if(r < 0)
    return -1;
  for(int i = 0; i < nhex; i++)
    hex[i] = sym[nhex-1-i];
  return r;
}

static void word_init(struct p25_word *w,int nbits){
  memset(w,0,sizeof *w);
  w->nbits = nbits;
}
// RS-decode and deliver the data hexbits
static bool word_finish(struct p25_word *w,int hex[],int nhex,int ndata,bool const erased[]){
  int const r = rs_hexbits(hex,nhex,ndata,erased);
  if(r < 0)
    return false;
  w->rs_fixed = r;
  for(int i = 0; i < ndata; i++)
    put_hex(w->bits + 6*i,hex[i]);
  w->ok = true;
  return true;
}

// HDU: hexbit i is the data of the Golay(18,6,8) word at bit 18 i; 20 data hexbits
// (MI, MFID, ALGID, KID, TGID), 16 RS(36,20,17) parity
bool p25_hdu_decode(struct p25_word *w,uint8_t const bits[P25_HDU_BITS]){
  assert(w != NULL && bits != NULL);
  word_init(w,120);
  uint8_t work[36 * 18];
  memcpy(work,bits,sizeof work);
  int hex[36];
  bool erased[36];
  for(int i = 0; i < 36; i++){
    int const r = golay18_decode(work + 18*i);
    erased[i] = r < 0;
    if(r < 0)
      w->erasures++;
    else
      w->inner_fixed += r;
    hex[i] = get_hex(work + 18*i);
  }
  return word_finish(w,hex,36,20,erased);
}

// TDULC: Golay(24,12,8) word i at bit 24 i carries hexbits 2i and 2i+1; 12 link control
// hexbits, 12 RS(24,12,13) parity. A failed Golay word erases both its hexbits
bool p25_tdulc_decode(struct p25_word *w,uint8_t const bits[P25_TDULC_BITS]){
  assert(w != NULL && bits != NULL);
  word_init(w,72);
  uint8_t work[12 * 24];
  memcpy(work,bits,sizeof work);
  int hex[24];
  bool erased[24];
  for(int i = 0; i < 12; i++){
    int const r = golay24_decode(work + 24*i);
    erased[2*i] = erased[2*i+1] = r < 0;
    if(r < 0)
      w->erasures++;
    else
      w->inner_fixed += r;
    hex[2*i] = get_hex(work + 24*i);
    hex[2*i+1] = get_hex(work + 24*i + 6);
  }
  return word_finish(w,hex,24,12,erased);
}

// ---------------------------------------------------------------------------
// LDU1 and LDU2

// The LDU payload: IMBE frames 1 and 2, then six groups of four Hamming(10,6,3) words
// (40 bits) each followed by an IMBE frame (frames 3 to 8), then the 32 low speed data bits
// and frame 9
static int const Imbe_offset[P25_IMBE_FRAMES] = {0, 144, 328, 512, 696, 880, 1064, 1248, 1424};
enum { LSD_OFFSET = 1392 };

// Hamming word i (0-23), hexbit i of the link control or encryption sync
static inline int ldu_word(int i){
  return 288 + 184 * (i / 4) + 10 * (i % 4);
}

bool p25_ldu_decode(struct p25_ldu *ldu,int duid,uint8_t bits[P25_LDU_BITS],float const conf[P25_LDU_BITS]){
  assert(ldu != NULL && bits != NULL);
  memset(ldu,0,sizeof *ldu);
  ldu->duid = duid;
  // LDU1: RS(24,12,13) link control. LDU2: RS(24,16,9) encryption sync
  int const ndata = duid == 10 ? 16 : 12;
  int hex[24];
  bool erased[24];

  if(conf != NULL){
    // Soft stage, in place. The threshold is on the confidence of the bit's dibit, which is
    // the margin of that dibit's less certain bit (dsync.c), so it is conservative for the other
    ldu->soft = true;
    for(int i = 0; i < 24; i++){
      int const s = ldu_word(i);
      int const r = hamming10_decode_soft(bits + s,conf + s,HAMMING10_HIGH_CONFIDENCE);
      erased[i] = r < 0;
      if(r < 0)
        ldu->soft_erasures++;
      else if(r > 0)
        ldu->soft_fixed++;
    }
    if(ldu->soft_erasures > 0){
      for(int i = 0; i < 24; i++)
        hex[i] = get_hex(bits + ldu_word(i));
      int const r = rs_hexbits(hex,24,ndata,erased);
      if(r < 0){
        ldu->soft_rs = -1; // the Hamming corrections stay
      } else {
        ldu->soft_rs = 1;
        ldu->soft_rs_fixed = r;
        for(int i = 0; i < 24; i++)
          put_hex(bits + ldu_word(i),hex[i]); // the Hamming parity bits are left as they are (see above)
      }
    }
  }
  // Hard decode of the link control or encryption sync, on a copy
  struct p25_word *w = &ldu->word;
  word_init(w,6 * ndata);
  for(int i = 0; i < 24; i++){
    uint8_t cw[10];
    memcpy(cw,bits + ldu_word(i),sizeof cw);
    int const r = hamming10_decode(cw);
    erased[i] = r < 0;
    if(r < 0)
      w->erasures++;
    else
      w->inner_fixed += r;
    hex[i] = get_hex(cw);
  }
  word_finish(w,hex,24,ndata,erased);

  for(int f = 0; f < P25_IMBE_FRAMES; f++)
    p25_imbe_decode(&ldu->voice[f],bits + Imbe_offset[f]);
  memcpy(ldu->lsd,bits + LSD_OFFSET,sizeof ldu->lsd);
  return w->ok;
}

// ---------------------------------------------------------------------------
// IMBE voice frames (TIA-102.BABA). New: beyond extracting the 144 bits of each frame

// Hamming(15,11,3): bits 0-10 data, 11-14 parity (8s bit first). The parity checks of data
// bit i are the bits of the i-th of the 4-bit values of weight two or more, largest first
static uint8_t const Hamming15_checksums[11] = {0xf, 0xe, 0xd, 0xc, 0xb, 0xa, 0x9, 0x7, 0x6, 0x5, 0x3};

// Returns bits changed (0 or 1): every syndrome of a perfect code is a single error
static int hamming15_decode(uint8_t bits[15]){
  int s = 0;
  for(int i = 0; i < 11; i++){
    if(bits[i])
      s ^= Hamming15_checksums[i];
  }
  for(int i = 0; i < 4; i++){
    if(bits[11+i])
      s ^= 8 >> i;
  }
  if(s == 0)
    return 0;
  int p = -1;
  if((s & (s - 1)) == 0){
    p = s == 8 ? 11 : s == 4 ? 12 : s == 2 ? 13 : 14;
  } else {
    for(int i = 0; i < 11; i++){
      if(Hamming15_checksums[i] == s)
        p = i;
    }
  }
  bits[p] ^= 1;
  return 1;
}

// Codeword layout after deinterleaving: c0 at 0, c1 23, c2 46, c3 69 (23 bits each),
// c4 92, c5 107, c6 122 (15 each), c7 137 (7)
static int const Imbe_cw_start[8] = {0, 23, 46, 69, 92, 107, 122, 137};
static int const Imbe_info_bits[8] = {12, 12, 12, 12, 11, 11, 11, 7};

void p25_imbe_decode(struct p25_imbe *f,uint8_t const raw[P25_IMBE_BITS]){
  assert(f != NULL && raw != NULL);
  memset(f,0,sizeof *f);
  memcpy(f->raw,raw,sizeof f->raw);
  // Deinterleave: the codeword bits, c0 msb first through c7 lsb, read as six rows of 24
  // are sent column by column, six bits (three dibits) per column; in the odd columns the
  // two bits of each dibit are exchanged
  // The six bits of a column, three dibits received one after another, land in six different
  // rows, 24 bits apart in the codeword vector, so a burst that short puts at most one error
  // in each
  uint8_t v[144];
  for(int j = 0; j < 144; j++){
    int const col = j / 6;
    int const row = (j % 6) ^ (col & 1);
    v[24 * row + col] = raw[j] & 1;
  }
  f->errors[0] = golay23_decode(v,NULL);
  // c1-c6 (vector bits 23-136) are masked by the msb of p(n) = 173 p(n-1) + 13849 mod 65536,
  // n = 1 .. 114, p(0) = 16 u0
  uint32_t const u0 = (uint32_t)(get_hex(v) << 6 | get_hex(v + 6));
  uint32_t pr = 16 * u0;
  for(int n = 0; n < 114; n++){
    pr = (173 * pr + 13849) & 0xffff;
    v[23 + n] ^= (uint8_t)(pr >> 15);
  }
  for(int c = 1; c < 4; c++)
    f->errors[c] = golay23_decode(v + Imbe_cw_start[c],NULL);
  for(int c = 4; c < 7; c++)
    f->errors[c] = hamming15_decode(v + Imbe_cw_start[c]);
  int k = 0;
  for(int c = 0; c < 8; c++){
    memcpy(f->u + k,v + Imbe_cw_start[c],Imbe_info_bits[c]);
    k += Imbe_info_bits[c];
  }
  assert(k == P25_IMBE_INFO_BITS);
  f->e0 = f->errors[0];
  f->e4 = f->errors[4];
  for(int c = 0; c < 7; c++)
    f->total += f->errors[c];
}

// ---------------------------------------------------------------------------
// P25 Phase 1 data deinterleave

// Received bit i goes to position p25_data_deinterleave[i]. The table moves whole 4-bit trellis
// symbols: received symbols 0-12 are trellis symbols 0, 4, .. 48, then 13-24 are 1, 5, .. 45,
// 25-36 are 2, 6, .. 46 and 37-48 are 3, 7, .. 47
uint8_t const p25_data_deinterleave[196] = {
  0, 1, 2, 3, 16, 17, 18, 19, 32, 33, 34, 35, 48, 49, 50, 51,
  64, 65, 66, 67, 80, 81, 82, 83, 96, 97, 98, 99, 112, 113, 114, 115,
  128, 129, 130, 131, 144, 145, 146, 147, 160, 161, 162, 163, 176, 177, 178, 179,
  192, 193, 194, 195, 4, 5, 6, 7, 20, 21, 22, 23, 36, 37, 38, 39,
  52, 53, 54, 55, 68, 69, 70, 71, 84, 85, 86, 87, 100, 101, 102, 103,
  116, 117, 118, 119, 132, 133, 134, 135, 148, 149, 150, 151, 164, 165, 166, 167,
  180, 181, 182, 183, 8, 9, 10, 11, 24, 25, 26, 27, 40, 41, 42, 43,
  56, 57, 58, 59, 72, 73, 74, 75, 88, 89, 90, 91, 104, 105, 106, 107,
  120, 121, 122, 123, 136, 137, 138, 139, 152, 153, 154, 155, 168, 169, 170, 171,
  184, 185, 186, 187, 12, 13, 14, 15, 28, 29, 30, 31, 44, 45, 46, 47,
  60, 61, 62, 63, 76, 77, 78, 79, 92, 93, 94, 95, 108, 109, 110, 111,
  124, 125, 126, 127, 140, 141, 142, 143, 156, 157, 158, 159, 172, 173, 174, 175,
  188, 189, 190, 191
};

void p25_deinterleave_bits(uint8_t out[196],uint8_t const in[196]){
  assert(out != NULL && in != NULL && out != in);
  for(int i = 0; i < 196; i++)
    out[p25_data_deinterleave[i]] = in[i];
}
void p25_deinterleave_bit_floats(float out[196],float const in[196]){
  assert(out != NULL && in != NULL && out != in);
  for(int i = 0; i < 196; i++)
    out[p25_data_deinterleave[i]] = in[i];
}
// The table moves dibits whole (entries 2i and 2i+1 are an aligned pair)
void p25_deinterleave_dibit_floats(float out[98],float const in[98]){
  assert(out != NULL && in != NULL && out != in);
  for(int i = 0; i < 98; i++)
    out[p25_data_deinterleave[2*i] / 2] = in[i];
}

// ---------------------------------------------------------------------------
// Trunking signalling blocks and packet data

// Deinterleave and Viterbi-decode one 196-bit block (TIA-102.BAAA trellis codes)
static void trellis_block(struct p25_block *b,uint8_t const bits[P25_BLOCK_BITS],float const phase[P25_BLOCK_BITS/2],
                          bool three_quarter){
  memset(b,0,sizeof *b);
  b->nbits = three_quarter ? 144 : 96;
  if(phase != NULL){
    float ph[98];
    p25_deinterleave_dibit_floats(ph,phase);
    b->soft = true;
    b->metric = three_quarter ? trellis_34_decode_soft(b->bits,ph,49) : trellis_12_decode_soft(b->bits,ph,49);
  } else {
    uint8_t d[196];
    p25_deinterleave_bits(d,bits);
    b->metric = (float)(three_quarter ? trellis_34_decode(b->bits,d,49) : trellis_12_decode(b->bits,d,49));
  }
}

int p25_tsbk_decode(struct p25_block *b,uint8_t const bits[P25_BLOCK_BITS],float const phase[P25_BLOCK_BITS/2],
                    bool correct_one){
  assert(b != NULL && bits != NULL);
  trellis_block(b,bits,phase,false);
  b->crc = crc_ccitt80_check(b->bits,correct_one);
  // A block failing its CRC has an untrustworthy last-block bit: assume more may follow
  b->last = b->crc >= 0 && b->bits[0];
  return b->crc;
}

void p25_pdu_reset(struct p25_pdu *p){
  assert(p != NULL);
  p->pending = false;
}

static int get_field(uint8_t const *bits,int offset,int len){
  int v = 0;
  for(int i = 0; i < len; i++)
    v = (v << 1) | bits[offset + i];
  return v;
}

int p25_pdu_header(struct p25_pdu *p,struct p25_block *b,uint8_t const bits[P25_BLOCK_BITS],
                   float const phase[P25_BLOCK_BITS/2],bool correct_one){
  assert(p != NULL && b != NULL && bits != NULL);
  trellis_block(b,bits,phase,false);
  b->crc = crc_ccitt80_check(b->bits,correct_one);
  p25_pdu_reset(p);
  if(b->crc < 0)
    return -1; // blocks to follow is trusted only from a header that passes
  p->confirmed = b->bits[1];
  p->outbound = b->bits[2];
  p->format = get_field(b->bits,3,5);
  p->vendor = (uint8_t)get_field(b->bits,16,8);
  p->blocks_to_follow = get_field(b->bits,49,7);
  p->kind = p->format == 23 ? P25_PDUKIND_AMBTC : p->format == 21 ? P25_PDUKIND_UMBTC : P25_PDUKIND_DATA;
  p->nblocks = 0;
  p->block_bits = p->confirmed ? 144 : 96;
  memcpy(p->payload,b->bits,96);
  p->nbits = 96;
  p->crc = -1;
  if(p->blocks_to_follow == 0)
    return 1;
  p->pending = true;
  return 0;
}

// The packet CRC (TIA-102.BAAA): generator x^32+x^26+x^23+x^22+x^16+x^12+x^11+x^10+x^8+x^7+
// x^5+x^4+x^2+x+1, no preset, inverted, most significant bit first. The data is each block's
// bits, less the 16 that begin a confirmed block; the last 32 are the CRC
static bool packet_crc_ok(struct p25_pdu const *p){
  int const skip = p->confirmed ? 16 : 0;
  int const per = p->block_bits - skip;
  int const total = p->nblocks * per;
  uint32_t crc = 0;
  uint32_t sent = 0;
  for(int i = 0; i < total; i++){
    uint8_t const b = p->payload[96 + (i / per) * p->block_bits + skip + i % per] & 1;
    if(i < total - 32){
      bool const top = (crc >> 31) != b;
      crc <<= 1;
      if(top)
        crc ^= 0x04c11db7;
    } else
      sent = (sent << 1) | b;
  }
  return (crc ^ 0xffffffff) == sent;
}

int p25_pdu_block(struct p25_pdu *p,struct p25_block *b,uint8_t const bits[P25_BLOCK_BITS],
                  float const phase[P25_BLOCK_BITS/2]){
  assert(p != NULL && b != NULL && bits != NULL);
  trellis_block(b,bits,phase,p->pending && p->confirmed);
  if(!p->pending)
    return -1;
  memcpy(p->payload + 96 + p->nblocks * p->block_bits,b->bits,(size_t)p->block_bits);
  p->nblocks++;
  p->nbits = 96 + p->nblocks * p->block_bits;
  if(p->nblocks < p->blocks_to_follow)
    return 0;
  p->pending = false;
  p->crc = packet_crc_ok(p);
  return 1;
}

void p25_pack_bits(uint8_t *octets,uint8_t const *bits,int nbits){
  assert(octets != NULL && bits != NULL);
  for(int i = 0; i < (nbits + 7) / 8; i++){
    uint8_t o = 0;
    for(int j = 0; j < 8; j++){
      int const k = 8*i + j;
      o = (uint8_t)((o << 1) | (k < nbits ? (bits[k] & 1) : 0));
    }
    octets[i] = o;
  }
}
