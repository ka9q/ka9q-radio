// Error correction and field extraction for one P25 Phase 2 burst (see p25p2unit.h)
// DUID, ISCH, AMBE+2 voice frames, encryption sync, SACCH / FACCH / LCCH MAC PDUs
// Copyright 2026, Matthew Kaufman, KA6SQG
#include <assert.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "dfec.h"
#include "rs63.h"
#include "p25p2unit.h"

// Theory of operation
//
// A burst is the 320 bits of one outbound timeslot, descrambled by p25p2.c where it is scrambled.
// Its type, the DUID, is sent in eight bits that are never scrambled, at 0-1, 74-75, 244-245 and
// 318-319; everything else depends on the type (TIA-102.BBAC 4.2, 4.3 and the bit tables of
// Annex E):
//
//   4V     AMBE+2 frames at 2, 76, 172 and 246 (72 bits each), ESS-B (24 bits) at 148-171
//   2V     AMBE+2 frames at 2 and 76, ESS-A (168 bits) at 148-243 and 246-317
//   SACCH  the outbound signalling burst without sync (I-OEMI): a 180-bit MAC PDU as 30 hexbits,
//          and 22 parity hexbits, in all the other 312 bits
//   FACCH  the outbound signalling burst with sync (S-OEMI): a 156-bit MAC PDU as 26 hexbits, and
//          19 parity hexbits, around a 42-bit sync sequence at 138-179
//   LCCH   the TDMA control channel's signalling burst without sync (OECI): a 180-bit MAC PDU
//          ending in a 12-bit color code and a CRC-16 (TIA-102.BBAD-A 2.1, 2.2.1), the same size
//          as a SACCH's and taken here in its layout; it is not in the original TIA-102.BBAC
//
// The DUID. Four information bits in an (8,4,4) code, minimum distance 4 (TIA-102.BBAC 5.4; the
// parity rows below are its generator matrix, 5.4.1): one error is corrected with certainty, two
// leave candidates at an equal distance, among which the dibit confidences choose only when they
// say clearly which. The original standard defines six values (4V, 2V, and SACCH and FACCH each
// scrambled or not, 5.4.1); this code also takes 10 (DATCH, scrambled) and 13 (LCCH). A decode to
// any other value, like a decode that fails, is P25P2_DUID_UNKNOWN.
//
// The ISCH. Nine information bits in a (40,9,16) code with a fixed offset added to every codeword,
// so that a coset of the code is sent (TIA-102.BBAC 5.5: Isch_generator and Isch_offset are its
// generator matrix and offset). It is decoded by comparing the received word with each candidate
// codeword: only those whose channel field is the timeslot the I-ISCH stands before, and whose
// ISCH location is not the reserved value, 96 of 512, so up to 7 errors are corrected. The table
// is built on first use, once for all threads.
//
// AMBE+2 frames. Each 72-bit frame holds four vectors: u0 (12 bits) in c0, an extended Golay
// (24,12) codeword; u1 (12) in c1, a Golay (23,12) codeword; u2 (11) and u3 (14) as they are.
// The frame is sent column by column (ambe_bit(); the order of TIA-102.BBAC Annex E), which
// spreads a run of bit errors over the vectors. c1 is masked by a sequence seeded from u0, so a
// wrongly corrected u0 also shows as errors in c1. The errors found in c0 and c1 go with the
// frame, for the vocoder.
//
// The encryption sync (TIA-102.BBAC 5.6.2). ALGID (8 bits), KID (16) and MI (72), 16 hexbits, in
// a Reed-Solomon (44,16,29) code: the (63,35,29) mother code of rs63.c shortened by 19 symbols,
// none punctured. A timeslot's voice bursts repeat four 4Vs then a 2V, counted from the start of
// the transmission and so not tied to the superframe (TIA-102.BBAC 6.1). Each 4V carries four of
// the information hexbits (ESS-B 1 to 4: the first has the ALGID and KID, the other three the MI,
// Annex E), the 2V the 28 parity hexbits (ESS-A), so the ESS is decoded at the 2V from it and the
// four 4Vs before (struct p25p2_ess_collector, matched by voice position where p25p2.c knows it,
// else by arrival). A missing ESS-B is four erasures: with all present the code corrects 14
// errors, with none (ESS-A alone) 6.
//
// Signalling. A MAC PDU is protected by a Reed-Solomon code made from the same mother code
// (TIA-102.BBAC 5.6): shortened by taking its most significant information hexbits as zero, and
// punctured by not sending its least significant parity hexbits, which are decoded as erasures.
// The SACCH's code is (52,30,23), 5 shortened and 6 punctured (5.6.1.3), leaving 11 errors
// correctable; the FACCH's is (45,26,20), 9 and 9 (5.6.1.2), leaving 9. When a decode fails, the
// weakest received hexbits by dibit confidence are erased too, more each time, and it is tried
// again. An erasure costs half what an unknown error does, so this gains when the weakest hexbits
// are the wrong ones. The PDU's CRC decides: for SACCH and FACCH the CRC-12 that ends every MAC
// PDU, generator x^12 + x^11 + x^7 + x^4 + x^2 + x + 1, sent inverted (TIA-102.BBAC 8.1, 8.4.5);
// for LCCH a CRC-16 over the 164 bits before it (TIA-102.BBAD-A 2.2). An attempt whose
// Reed-Solomon decode succeeds but whose CRC fails goes on to the next.

// ---------------- DUID ----------------

// Burst bits holding the DUID codeword, msb first, and the dibit each falls in
static int const Duid_bits[8] = { 0, 1, 74, 75, 244, 245, 318, 319 };
// A two-error correction needs the cheapest candidate cheaper than the next by this much,
// in units of one fully confident bit
static float const Duid_soft_margin = 0.5f;

// (8,4,4) code: value << 4 | parity, the parity the sum of these rows for the value's bits
uint8_t p25p2_duid_codeword(int duid){
  static uint8_t const rows[4] = { 0xd, 0xb, 0xe, 0x7 }; // parity of value bits 3, 2, 1, 0
  if(duid < 0 || duid > 15)
    return 0;
  uint8_t p = 0;
  for(int i=0; i < 4; i++)
    if(duid & (8 >> i))
      p ^= rows[i];
  return (uint8_t)(duid << 4 | p);
}
static bool duid_defined(int v){
  switch(v){
  case P25P2_DUID_4V:
  case P25P2_DUID_SACCH_S:
  case P25P2_DUID_2V:
  case P25P2_DUID_FACCH_S:
  case P25P2_DUID_DATCH_S:
  case P25P2_DUID_SACCH:
  case P25P2_DUID_LCCH:
  case P25P2_DUID_FACCH:
    return true;
  default:
    return false;
  }
}
int p25p2_duid_decode(uint8_t const bits[P25P2_BURST_BITS],float const conf[P25P2_BURST_DIBITS],int *errors){
  assert(bits != NULL);
  if(errors != NULL)
    *errors = -1;
  int received = 0;
  for(int i=0; i < 8; i++)
    received = (received << 1) | (bits[Duid_bits[i]] & 1);
  int best_hard = 99,best_index = -1,soft_index = -1;
  float best_soft = 1e9f,second_soft = 1e9f;
  for(int c=0; c < 16; c++){
    int const diff = received ^ p25p2_duid_codeword(c);
    int const hard = __builtin_popcount(diff);
    if(hard < best_hard){
      best_hard = hard;
      best_index = c;
    }
    float soft = 0;
    for(int i=0; i < 8; i++)
      if((diff >> (7 - i)) & 1)
	soft += conf != NULL ? conf[Duid_bits[i] / 2] : 1.0f;
    if(soft < best_soft){
      second_soft = best_soft;
      best_soft = soft;
      soft_index = c;
    } else if(soft < second_soft)
      second_soft = soft;
  }
  int chosen = -1;
  if(best_hard <= 1)
    chosen = best_index; // distance 4: within one bit the nearest codeword is unique
  else if(best_hard == 2 && conf != NULL && soft_index >= 0
	  && __builtin_popcount(received ^ p25p2_duid_codeword(soft_index)) == 2
	  && second_soft - best_soft >= Duid_soft_margin)
    chosen = soft_index;
  if(chosen < 0 || !duid_defined(chosen))
    return P25P2_DUID_UNKNOWN;
  if(errors != NULL)
    *errors = __builtin_popcount(received ^ p25p2_duid_codeword(chosen));
  return chosen;
}
bool p25p2_duid_scrambled(int duid){
  return duid == P25P2_DUID_4V || duid == P25P2_DUID_2V || duid == P25P2_DUID_SACCH_S
    || duid == P25P2_DUID_FACCH_S || duid == P25P2_DUID_DATCH_S;
}
bool p25p2_duid_voice(int duid){
  return duid == P25P2_DUID_4V || duid == P25P2_DUID_2V;
}
int p25p2_duid_mac_chan(int duid){
  switch(duid){
  case P25P2_DUID_SACCH_S:
  case P25P2_DUID_SACCH:
    return P25_MAC_SACCH;
  case P25P2_DUID_FACCH_S:
  case P25P2_DUID_FACCH:
    return P25_MAC_FACCH;
  case P25P2_DUID_LCCH:
    return P25_MAC_LCCH;
  default:
    return -1;
  }
}
char const *p25p2_duid_name(int duid){
  switch(duid){
  case P25P2_DUID_4V: return "4V";
  case P25P2_DUID_SACCH_S: return "SACCH-S";
  case P25P2_DUID_2V: return "2V";
  case P25P2_DUID_FACCH_S: return "FACCH-S";
  case P25P2_DUID_DATCH_S: return "DATCH-S";
  case P25P2_DUID_SACCH: return "SACCH";
  case P25P2_DUID_LCCH: return "LCCH";
  case P25P2_DUID_FACCH: return "FACCH";
  default: return "unknown";
  }
}

// ---------------- ISCH ----------------

// Phase 2 ISCH (40,9) code

static uint64_t Isch_codeword[512];      // ISCH info -> codeword

static pthread_once_t Once = PTHREAD_ONCE_INIT;
static void isch_init(void);
static inline void init(void){
  pthread_once(&Once,isch_init);
}

static uint64_t const Isch_generator[9] = {
  0x8816CE36D7ULL, 0x201DFD4F64ULL, 0x100F4B1758ULL,
  0x0C00DED18EULL, 0x020807F7FFULL, 0x09048D9B72ULL,
  0x009DA3A171ULL, 0x0058CBAA4EULL, 0x00343D8597ULL
};
static uint64_t const Isch_offset = 0x184229D461ULL;

static void isch_init(void){
  for(int x = 0; x < 512; x++){
    uint64_t c = 0;
    for(int i = 0; i < 9; i++){
      if(x & (1 << (8 - i)))
        c ^= Isch_generator[i];
    }
    Isch_codeword[x] = c ^ Isch_offset;
  }
}

int isch_decode_word(uint64_t word,int timeslot,int *info){
  assert(timeslot == 0 || timeslot == 1);
  init();
  word &= 0xFFFFFFFFFFULL;
  int best = 8; // corrects up to 7
  int best_info = -1;
  for(int x = 0; x < 512; x++){
    if(((x >> 5) & 3) != timeslot || ((x >> 3) & 3) == 3)
      continue; // other channel, or the reserved ISCH location
    int const e = __builtin_popcountll(word ^ Isch_codeword[x]);
    if(e < best){
      best = e;
      best_info = x;
      if(e == 0)
        break;
    }
  }
  if(best_info < 0)
    return -1;
  if(info != NULL)
    *info = best_info;
  return best;
}
int isch_decode(uint8_t const bits[40],int timeslot,int *info){
  assert(bits != NULL);
  uint64_t w = 0;
  for(int i = 0; i < 40; i++)
    w = (w << 1) | (bits[i] & 1);
  return isch_decode_word(w,timeslot,info);
}

// ---------------- AMBE+2 voice frames ----------------

enum { AMBE_C0 = 24, AMBE_C1 = 23, AMBE_C2 = 11, AMBE_C3 = 14 };
static int const Frame_start[P25P2_MAX_FRAMES] = { 2, 76, 172, 246 };

// Entry j of the frame's bit list is bit 4 (j mod 18) + j / 18 as sent
static inline int ambe_bit(int j){
  return 4 * (j % 18) + j / 18;
}
// The 23-bit mask of c1 (TIA-102.BABA-1): a linear congruential sequence seeded by 16 u0
static uint32_t ambe_mask(uint16_t u0){
  uint32_t pr = 16u * u0,out = 0;
  for(int i=0; i < 23; i++){
    pr = (173u * pr + 13849u) % 65536u;
    out = (out << 1) | (pr >= 32768u ? 1u : 0u);
  }
  return out;
}
void p25p2_ambe_decode(struct p25p2_ambe *f,uint8_t const raw[P25P2_AMBE_BITS]){
  assert(f != NULL && raw != NULL);
  memcpy(f->raw,raw,P25P2_AMBE_BITS);
  uint8_t c0[24],c1[24];
  int j = 0;
  for(int i=0; i < AMBE_C0; i++)
    c0[i] = raw[ambe_bit(j++)] & 1;
  for(int i=0; i < AMBE_C1; i++)
    c1[i] = raw[ambe_bit(j++)] & 1;
  f->errors[0] = golay23_decode(c0,&f->data_errors[0]);
  // The parity bit counts a fourth error only when three were corrected: with fewer the
  // odd parity is the parity bit's own error
  int parity = 0;
  for(int i=0; i < 24; i++)
    parity ^= c0[i];
  if(parity != 0 && f->errors[0] == 3)
    f->errors[0] = 4;
  uint16_t u0 = 0;
  for(int i=0; i < 12; i++)
    u0 = (uint16_t)(u0 << 1 | c0[i]);
  uint32_t const mask = ambe_mask(u0);
  for(int i=0; i < AMBE_C1; i++)
    c1[i] ^= (mask >> (22 - i)) & 1;
  f->errors[1] = golay23_decode(c1,&f->data_errors[1]);
  uint16_t u1 = 0,u2 = 0,u3 = 0;
  for(int i=0; i < 12; i++)
    u1 = (uint16_t)(u1 << 1 | c1[i]);
  for(int i=0; i < AMBE_C2; i++)
    u2 = (uint16_t)(u2 << 1 | (raw[ambe_bit(j++)] & 1));
  for(int i=0; i < AMBE_C3; i++)
    u3 = (uint16_t)(u3 << 1 | (raw[ambe_bit(j++)] & 1));
  f->u[0] = u0;
  f->u[1] = u1;
  f->u[2] = u2;
  f->u[3] = u3;
  int n = 0;
  for(int i=11; i >= 0; i--)
    f->bits[n++] = (u0 >> i) & 1;
  for(int i=11; i >= 0; i--)
    f->bits[n++] = (u1 >> i) & 1;
  for(int i=10; i >= 0; i--)
    f->bits[n++] = (u2 >> i) & 1;
  for(int i=13; i >= 0; i--)
    f->bits[n++] = (u3 >> i) & 1;
}
int p25p2_voice_decode(struct p25p2_ambe frames[P25P2_MAX_FRAMES],int duid,uint8_t const bits[P25P2_BURST_BITS]){
  int const n = duid == P25P2_DUID_4V ? 4 : duid == P25P2_DUID_2V ? 2 : 0;
  for(int i=0; i < n; i++)
    p25p2_ambe_decode(&frames[i],bits + Frame_start[i]);
  return n;
}

// ---------------- encryption sync ----------------

enum {
  ESS_B_START = 148,
  ESS_A1_START = 148, ESS_A1_BITS = 96,
  ESS_A2_START = 246,
  ESS_LENGTH = 44,     // RS(44,16,29): the shortened length
  ESS_PARITY = 28,
};
static int hexbit(uint8_t const *bits){
  int v = 0;
  for(int i=0; i < 6; i++)
    v = (v << 1) | (bits[i] & 1);
  return v;
}
void p25p2_ess_reset(struct p25p2_ess_collector *c){
  c->nseg = 0;
  c->arrivals_at_2v = c->arrivals;
}
void p25p2_ess_add(struct p25p2_ess_collector *c,uint8_t const bits[P25P2_BURST_BITS],int64_t voice_index){
  if(c->nseg == P25P2_ESS_SEGMENTS){
    memmove(&c->seg[0],&c->seg[1],(P25P2_ESS_SEGMENTS - 1) * sizeof c->seg[0]);
    c->nseg--;
  }
  for(int i=0; i < 4; i++)
    c->seg[c->nseg].sym[i] = (uint8_t)hexbit(bits + ESS_B_START + 6 * i);
  c->seg[c->nseg].voice_index = voice_index;
  c->seg[c->nseg].arrival = c->arrivals++;
  c->nseg++;
}
bool p25p2_ess_decode(struct p25p2_ess_collector *c,struct p25p2_ess *ess,uint8_t const bits[P25P2_BURST_BITS],
		      int64_t voice_index){
  memset(ess,0,sizeof *ess);
  ess->algid = P25_ALGID_CLEAR;
  // ESS-B k (1-4) is from the 4V k voice positions after the 2V before this one
  // Without voice positions, the first four segments to arrive since the last 2V (or reset) are
  // taken: after a missed 2V those are an earlier superframe's, whose MI may differ
  int b[4] = { -1, -1, -1, -1 };
  if(voice_index >= 0){
    for(int i=0; i < c->nseg; i++){
      if(c->seg[i].voice_index < 0)
	continue;
      int64_t const k = c->seg[i].voice_index - (voice_index - 5);
      if(k >= 1 && k <= 4)
	b[k - 1] = i;
    }
  } else {
    int k = 0;
    for(int i=0; i < c->nseg && k < 4; i++)
      if(c->seg[i].arrival >= c->arrivals_at_2v)
	b[k++] = i;
  }
  // Mother code positions: ESS-A hexbits 27..0 at 0..27 (parity), ESS-B4..B1 (each hexbits
  // 3..0) at 28..43, the rest shortened
  uint8_t sym[63] = {0};
  int erasures[16];
  int nerasures = 0;
  uint8_t a[168];
  memcpy(a,bits + ESS_A1_START,ESS_A1_BITS);
  memcpy(a + ESS_A1_BITS,bits + ESS_A2_START,168 - ESS_A1_BITS);
  for(int i=0; i < ESS_PARITY; i++)
    sym[i] = (uint8_t)hexbit(a + 6 * (27 - i));
  for(int s=0; s < 4; s++){
    int const base = 28 + (3 - s) * 4;
    for(int k=0; k < 4; k++){
      if(b[s] >= 0)
	sym[base + 3 - k] = c->seg[b[s]].sym[k];
      else
	erasures[nerasures++] = base + 3 - k;
    }
  }
  c->nseg = 0;
  c->arrivals_at_2v = c->arrivals;
  int const fixed = rs_44_16_29_decode(sym,erasures,nerasures);
  if(fixed < 0)
    return false;
  uint8_t data[96];
  for(int i=0; i < 16; i++){
    int const v = sym[43 - i];
    for(int k=0; k < 6; k++)
      data[6 * i + k] = (v >> (5 - k)) & 1;
  }
  int v = 0;
  for(int i=0; i < 8; i++)
    v = (v << 1) | data[i];
  ess->algid = (uint8_t)v;
  v = 0;
  for(int i=8; i < 24; i++)
    v = (v << 1) | data[i];
  ess->kid = (uint16_t)v;
  for(int o=0; o < 9; o++){
    v = 0;
    for(int i=0; i < 8; i++)
      v = (v << 1) | data[24 + 8 * o + i];
    ess->mi[o] = (uint8_t)v;
  }
  ess->ok = true;
  ess->erasures = nerasures;
  ess->rs_fixed = fixed;
  return true;
}

// ---------------- signalling: SACCH, FACCH, LCCH ----------------

enum {
  MOTHER_PARITY = 28,   // RS(63,35,29)
  SACCH_INFO = 30, SACCH_PAR = 22, SACCH_PUNCTURED = 6,
  FACCH_INFO = 26, FACCH_PAR = 19, FACCH_PUNCTURED = 9,
};
// Weakest received hexbits erased in the retries after a failed decode
static int const Extra_erasures[] = { 4, 8, 12 };

// Burst bit of bit k (0-5, msb first) of information hexbit j (INFO_1 = 0) or parity hexbit j
// (PARITY_1 = 0); the hexbits fill the burst in order around the DUID (74-75, 244-245) and, in
// a FACCH, its 42-bit sync at 138-179 (TIA-102.BBAC 4.3)
static int sacch_bit(bool parity,int j,int k){
  if(!parity)
    return j < 12 ? 2 + 6 * j + k : 76 + 6 * (j - 12) + k;
  return j < 10 ? 184 + 6 * j + k : 246 + 6 * (j - 10) + k;
}
static int facch_bit(bool parity,int j,int k){
  if(!parity){
    if(j < 12)
      return 2 + 6 * j + k;
    if(j < 22)
      return 76 + 6 * (j - 12) + k;
    if(j == 22)
      return k < 2 ? 136 + k : 180 + (k - 2);
    return 184 + 6 * (j - 23) + k;
  }
  return j < 7 ? 202 + 6 * j + k : 246 + 6 * (j - 7) + k;
}

bool p25p2_mac_decode_burst(struct p25p2_mac *m,int duid,uint8_t const bits[P25P2_BURST_BITS],
			    float const conf[P25P2_BURST_DIBITS]){
  assert(m != NULL && bits != NULL);
  memset(m,0,sizeof *m);
  int const chan = p25p2_duid_mac_chan(duid);
  m->chan = chan;
  if(chan < 0)
    return false;
  bool const facch = chan == P25_MAC_FACCH;
  int const ninfo = facch ? FACCH_INFO : SACCH_INFO;
  int const npar = facch ? FACCH_PAR : SACCH_PAR;
  int const punctured = facch ? FACCH_PUNCTURED : SACCH_PUNCTURED;
  int const info_top = MOTHER_PARITY - 1 + ninfo; // mother code position of INFO_1
  m->nbits = facch ? 156 : 180;

  // Received hexbits at their mother code positions (PARITY_1 at 27 down, INFO_1 at info_top
  // down), each trusted as much as its weakest dibit
  uint8_t received[63] = {0};
  float trust[63];
  for(int p=0; p < 63; p++)
    trust[p] = 1e9f;
  for(int pass=0; pass < 2; pass++){
    bool const parity = pass == 0;
    int const n = parity ? npar : ninfo;
    for(int j=0; j < n; j++){
      int v = 0;
      float t = 1e9f;
      for(int k=0; k < 6; k++){
	int const b = facch ? facch_bit(parity,j,k) : sacch_bit(parity,j,k);
	v = (v << 1) | (bits[b] & 1);
	if(conf != NULL && conf[b / 2] < t)
	  t = conf[b / 2];
      }
      int const pos = parity ? MOTHER_PARITY - 1 - j : info_top - j;
      received[pos] = (uint8_t)v;
      trust[pos] = t;
    }
  }
  // Received positions, weakest first; ties keep position order
  int order[63];
  int norder = 0;
  for(int p=punctured; p <= info_top; p++){
    int i = norder++;
    while(i > 0 && trust[order[i-1]] > trust[p]){
      order[i] = order[i-1];
      i--;
    }
    order[i] = p;
  }
  int const attempts = conf != NULL ? 1 + (int)(sizeof Extra_erasures / sizeof Extra_erasures[0]) : 1;
  uint8_t sym[63];
  for(int attempt=0; attempt < attempts; attempt++){
    int erasures[MOTHER_PARITY];
    int nerasures = 0;
    for(int p=0; p < punctured; p++)
      erasures[nerasures++] = p;
    if(attempt > 0){
      int const extra = Extra_erasures[attempt - 1];
      if(punctured + extra > MOTHER_PARITY)
	break;
      for(int i=0; i < extra; i++)
	erasures[nerasures++] = order[i];
    }
    memcpy(sym,received,sizeof sym);
    for(int i=0; i < nerasures; i++)
      sym[erasures[i]] = 0;
    m->attempts = attempt + 1;
    m->erasures = nerasures;
    m->rs_ok = rs63_decode(sym,MOTHER_PARITY,info_top + 1,erasures,nerasures) >= 0;
    m->corrected = 0;
    if(m->rs_ok){
      for(int p=punctured; p <= info_top; p++)
	m->corrected += sym[p] != received[p];
    }
    // The PDU: INFO_1 first, then the CRC
    for(int j=0; j < ninfo; j++){
      int const v = m->rs_ok ? sym[info_top - j] : received[info_top - j];
      for(int k=0; k < 6 && 6 * j + k < m->nbits; k++)
	m->bits[6 * j + k] = (v >> (5 - k)) & 1;
    }
    if(m->rs_ok){
      switch(chan){
      case P25_MAC_SACCH:
	m->crc_ok = crc12_check(m->bits,168);
	break;
      case P25_MAC_FACCH:
	m->crc_ok = crc12_check(m->bits,144);
	break;
      default:
	{
	  int v = 0;
	  for(int i=164; i < 180; i++)
	    v = (v << 1) | m->bits[i];
	  m->crc_ok = crc_ccitt(m->bits,164) == v;
	}
	break;
      }
    }
    if(m->crc_ok)
      break;
  }
  memset(m->octets,0,sizeof m->octets);
  for(int i=0; i < m->nbits; i++)
    m->octets[i / 8] |= (uint8_t)(m->bits[i] << (7 - i % 8));
  return m->crc_ok;
}
