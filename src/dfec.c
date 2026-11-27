// Forward error correction for the digital mode decoders: Golay, Hamming, trellis codes
// (Viterbi, hard and soft) and CRCs (see dfec.h)
// Copyright 2026, Matthew Kaufman, KA6SQG
#include <assert.h>
#include <float.h>
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "dfec.h"

// Theory of operation
//
// The block code decoders are bounded-distance decoders: they correct every error pattern within
// the radius the code guarantees, and beyond it either return -1 or reach some other codeword
// (a perfect code never returns -1). The Viterbi decoders return the most likely path whatever
// was received and leave the judgment to a CRC.
//
// Golay. The binary Golay(23,12,7) code, generator g(x) = x^11 + x^10 + x^6 + x^5 + x^4 + x^2 + 1,
// is perfect: the spheres of radius 3 around its 4096 codewords fill all 2^23 words, so each of
// the 2048 syndromes is exactly one error pattern of weight 0 to 3. A table from syndrome to
// pattern, built once, is then a complete maximum likelihood decoder for a binary symmetric
// channel. Adding an overall parity bit gives Golay(24,12,8), which also detects a fourth error;
// shortening that by six data bits held at zero gives the HDU's Golay(18,6,8).
//
// Hamming. Hamming(15,11,3) has every nonzero 4-bit value as some bit's syndrome, so it corrects
// any single error and nothing else (it is perfect; p25p1unit.c decodes it). Hamming(10,6,3) is
// it shortened to six data bits: ten of the fifteen syndromes point at a bit, and the other five
// can only come from two or more errors and are reported uncorrectable. The soft variant also
// refuses a correction of a bit its caller is confident of.
//
// Trellis codes. The P25 trellis codes are finite state machines whose state is the previous
// input (a dibit for rate 1/2, a tribit for rate 3/4) and whose output, from a table indexed by
// previous and current input, is a 4-bit symbol: two dibits on the air. A block starts in state
// 0 and ends with a zero input. From the tables, the closest two paths that part and meet again
// differ in 5 bits at rate 1/2 and in 3 bits at rate 3/4; in the soft metric below the same
// figures are 9 and 3 times the squared distance between neighbouring dibit phases (pi/2)^2.
// The Viterbi decoder finds the path through the trellis closest to what was received: with 4 or
// 8 states and 49 steps it keeps every survivor decision and traces back once at the end. The
// hard decoder measures distance in bits (maximum likelihood for independent bit errors); the
// soft one in squared phase error, which for a phase disturbed by Gaussian noise of the same
// variance on every dibit is minus the log likelihood up to a constant, so its least-total path is
// the most likely one.
//
// CRCs. The register form computes the remainder of M(x) x^width modulo the generator, msb first,
// with a zero preset, and P25 sends it inverted. The code is linear apart from that constant, so
// the CRC of a received block XORed with the received CRC (the residual) depends only on the
// error pattern; a single bit error gives a known residual, which is what the one-bit repair
// looks for. That repair costs detection: of the 65,536 residuals, 97 are then accepted (0, the
// 80 single data bit errors and the 16 single CRC bit errors), so a random block passes about
// once in 676 tries rather than once in 65,536.

// ---------------------------------------------------------------------------
// Golay(24,12,8) and Golay(18,6,8)
// Golay(23,12) is perfect: each of its 2048 syndromes is exactly one error pattern of at
// most three bits, so a syndrome table corrects any 1-3 errors in bits 0-22; the overall
// parity bit then catches a fourth error (or locates a lone error in bit 23)

static uint32_t Golay_pattern[2048];     // Golay(23,12) syndrome -> error pattern

static pthread_once_t Once = PTHREAD_ONCE_INIT;
static void golay_init(void);
static inline void init(void){
  pthread_once(&Once,golay_init);
}

// Syndrome contribution of each of bits 0-22 (data 0-11, parity 12-22). Data bit i, the
// coefficient of x^(22-i), contributes x^(22-i) mod g(x); parity bit 12+j contributes x^(10-j)
static int const Golay_checksums[23] = {
  0x63A, 0x31D, 0x7B4, 0x3DA, 0x1ED, 0x6CC, 0x366, 0x1B3,
  0x6E3, 0x54B, 0x49F, 0x475, 0x400, 0x200, 0x100, 0x080,
  0x040, 0x020, 0x010, 0x008, 0x004, 0x002, 0x001
};

// w23: bit 22 = codeword bit 0
static uint32_t golay_syndrome(uint32_t w23){
  uint32_t s = 0;
  for(int i = 0; i < 23; i++){
    if((w23 >> (22 - i)) & 1)
      s ^= Golay_checksums[i];
  }
  return s;
}
static void golay_init(void){
  for(int a = 0; a < 23; a++){
    uint32_t const ea = 1u << (22 - a);
    Golay_pattern[Golay_checksums[a]] = ea;
    for(int b = a + 1; b < 23; b++){
      uint32_t const eb = 1u << (22 - b);
      Golay_pattern[Golay_checksums[a] ^ Golay_checksums[b]] = ea | eb;
      for(int c = b + 1; c < 23; c++)
        Golay_pattern[Golay_checksums[a] ^ Golay_checksums[b] ^ Golay_checksums[c]] = ea | eb | (1u << (22 - c));
    }
  }
}
// word: bit 23 = codeword bit 0. Returns -1 for four errors, else the corrected word
// After the 23-bit correction the overall parity must be even. If it is odd and the pattern
// found had three bits, there were at least four errors; if it had fewer, the parity bit itself
// was one of at most three errors
static int64_t golay_correct(uint32_t word){
  uint32_t const w23 = word >> 1;
  uint32_t const pattern = Golay_pattern[golay_syndrome(w23)];
  uint32_t c = ((w23 ^ pattern) << 1) | (word & 1);
  if(__builtin_popcount(c) & 1){
    if(__builtin_popcount(pattern) == 3)
      return -1; // four errors
    c ^= 1; // the overall parity bit was one of the errors
  }
  return c;
}

int golay24_decode(uint8_t bits[24]){
  assert(bits != NULL);
  init();
  uint32_t word = 0;
  for(int i = 0; i < 24; i++){
    if(bits[i])
      word |= 1u << (23 - i);
  }
  int64_t const c = golay_correct(word);
  if(c < 0)
    return -1;
  for(int i = 0; i < 24; i++)
    bits[i] = (c >> (23 - i)) & 1;
  return __builtin_popcount((uint32_t)c ^ word);
}

int golay23_decode(uint8_t bits[23],int *data_fixed){
  assert(bits != NULL);
  init();
  uint32_t w = 0;
  for(int i = 0; i < 23; i++){
    if(bits[i])
      w |= 1u << (22 - i);
  }
  uint32_t const pattern = Golay_pattern[golay_syndrome(w)];
  for(int i = 0; i < 23; i++)
    bits[i] ^= (pattern >> (22 - i)) & 1;
  if(data_fixed != NULL)
    *data_fixed = __builtin_popcount(pattern >> 11);
  return __builtin_popcount(pattern);
}

int golay18_decode(uint8_t bits[18]){
  assert(bits != NULL);
  init();
  uint32_t word = 0; // 24-bit form, the six shortened positions (bits 23-18) zero
  for(int i = 0; i < 18; i++){
    if(bits[i])
      word |= 1u << (17 - i);
  }
  int64_t const c = golay_correct(word);
  if(c < 0 || (c >> 18) != 0)
    return -1; // a correction in the shortened positions is not a codeword of this code
  for(int i = 0; i < 18; i++)
    bits[i] = (c >> (17 - i)) & 1;
  return __builtin_popcount((uint32_t)c ^ word);
}

// ---------------------------------------------------------------------------
// Hamming(10,6,3), the LDU1 / LDU2 hexbit code
// The four parity bits are sent 8s bit first (bit 6) to 1s bit (bit 9)

static int const Hamming10_checksums[6] = {0xE, 0xD, 0xB, 0x7, 0x3, 0xC};
// Syndrome -> bit of the word to flip, or -1 for a multiple error
static int const Hamming10_position[16] = {
  -1, 9, 8, 4, 7, -1, -1, 3, 6, -1, -1, 2, 5, 1, 0, -1
};

static int hamming10_syndrome(uint8_t const bits[10]){
  int s = 0;
  for(int i = 0; i < 6; i++){
    if(bits[i])
      s ^= Hamming10_checksums[i];
  }
  for(int i = 0; i < 4; i++){
    if(bits[6+i])
      s ^= 8 >> i;
  }
  return s;
}
int hamming10_decode(uint8_t bits[10]){
  return hamming10_decode_soft(bits,NULL,0);
}
int hamming10_decode_soft(uint8_t bits[10],float const confidence[10],float threshold){
  assert(bits != NULL);
  int const s = hamming10_syndrome(bits);
  if(s == 0)
    return 0;
  int const p = Hamming10_position[s];
  if(p < 0)
    return -1;
  // A syndrome pointing at a confident bit more likely means a double error elsewhere
  if(confidence != NULL && confidence[p] > threshold)
    return -1;
  bits[p] ^= 1;
  return 1;
}

// ---------------------------------------------------------------------------
// P25 trellis codes: output symbol = table[previous input][current input], the state being
// the previous input. Survivors are kept as a traceback table (pred[t][s]: the state before s
// on the best path into s at step t), and the path is read back from the flush. A state takes
// the first predecessor (lowest state number) with the strictly smallest metric, and the flush the
// first final state with the strictly smallest metric

static uint8_t const Trellis_12[4][4] = {
  {0x2, 0xC, 0x1, 0xF},
  {0xE, 0x0, 0xD, 0x3},
  {0x9, 0x7, 0xA, 0x4},
  {0x5, 0xB, 0x6, 0x8}
};
static uint8_t const Trellis_34[8][8] = {
  {2, 13, 14, 1, 7, 8, 11, 4},
  {14, 1, 7, 8, 11, 4, 2, 13},
  {10, 5, 6, 9, 15, 0, 3, 12},
  {6, 9, 15, 0, 3, 12, 10, 5},
  {15, 0, 3, 12, 10, 5, 6, 9},
  {3, 12, 10, 5, 6, 9, 15, 0},
  {7, 8, 11, 4, 2, 13, 14, 1},
  {11, 4, 2, 13, 14, 1, 7, 8}
};

static void traceback(uint8_t *out,uint8_t pred[][8],int steps,int k,int state){
  for(int i = steps-1; i >= 0; i--){
    for(int b = 0; b < k; b++)
      out[i*k + b] = (state >> (k - 1 - b)) & 1;
    state = pred[i][state];
  }
}

static int viterbi_hard(uint8_t *out,uint8_t const *in,int nsyms,int k,uint8_t const *table){
  assert(out != NULL && in != NULL);
  assert(nsyms >= 1 && nsyms <= TRELLIS_MAX_SYMS);
  int const nstates = 1 << k;
  int const steps = nsyms - 1;
  uint8_t pred[TRELLIS_MAX_SYMS][8];
  unsigned metric[8];
  bool valid[8] = {false};
  metric[0] = 0;
  valid[0] = true;
  for(int t = 0; t <= steps; t++){
    int sym = 0;
    for(int b = 0; b < 4; b++)
      sym = (sym << 1) | (in[4*t + b] ? 1 : 0);
    if(t == steps){
      // Flush to state 0
      bool found = false;
      unsigned best = 0;
      int best_state = 0;
      for(int p = 0; p < nstates; p++){
        if(!valid[p])
          continue;
        unsigned const m = metric[p] + __builtin_popcount(table[p * nstates] ^ sym);
        if(!found || m < best){
          best = m;
          best_state = p;
          found = true;
        }
      }
      traceback(out,pred,steps,k,best_state);
      return (int)best;
    }
    unsigned next[8] = {0};
    bool next_valid[8] = {false};
    for(int p = 0; p < nstates; p++){
      if(!valid[p])
        continue;
      for(int x = 0; x < nstates; x++){
        unsigned const m = metric[p] + __builtin_popcount(table[p * nstates + x] ^ sym);
        if(!next_valid[x] || m < next[x]){
          next[x] = m;
          pred[t][x] = p;
          next_valid[x] = true;
        }
      }
    }
    for(int s = 0; s < nstates; s++){
      metric[s] = next[s];
      valid[s] = next_valid[s];
    }
  }
  return 0; // not reached
}
int trellis_12_decode(uint8_t *out,uint8_t const *in,int nsyms){
  return viterbi_hard(out,in,nsyms,2,&Trellis_12[0][0]);
}
int trellis_34_decode(uint8_t *out,uint8_t const *in,int nsyms){
  return viterbi_hard(out,in,nsyms,3,&Trellis_34[0][0]);
}

// Circular phase difference
static float phase_diff(float a,float b){
  float d = a - b;
  if(!isfinite(d))
    return (float)M_PI; // garbage in: as far as possible from everything
  if(fabsf(d) > 64.0f)
    d = remainderf(d,(float)(2 * M_PI));
  while(d > M_PI)
    d -= 2.0f * M_PI;
  while(d < -M_PI)
    d += 2.0f * M_PI;
  return d;
}

// Squared phase distance of a received symbol (two dibit phases) to an expected output.
// Not weighted by dibit confidence: confidence is the distance to the nearest decision
// boundary, so weighting by it shrinks exactly the distances that separate competing paths
// (measured TSBK CRC pass rates on DQPSK in AWGN were better unweighted at 4, 6 and 8 dB Es/N0)
static float branch_metric(float phase0,float phase1,int expected){
  static float const ideal[4] = {
    0.7854f,   // 00: +45 degrees
    2.3562f,   // 01: +135
    -0.7854f,  // 10: -45
    -2.3562f,  // 11: -135
  };
  float const d0 = phase_diff(phase0,ideal[(expected >> 2) & 3]);
  float const d1 = phase_diff(phase1,ideal[expected & 3]);
  return d0 * d0 + d1 * d1;
}

static float viterbi_soft(uint8_t *out,float const *phase,int nsyms,int k,uint8_t const *table){
  assert(out != NULL && phase != NULL);
  assert(nsyms >= 1 && nsyms <= TRELLIS_MAX_SYMS);
  int const nstates = 1 << k;
  int const steps = nsyms - 1;
  uint8_t pred[TRELLIS_MAX_SYMS][8];
  float metric[8];
  bool valid[8];
  for(int s = 0; s < nstates; s++){
    metric[s] = FLT_MAX;
    valid[s] = false;
  }
  metric[0] = 0;
  valid[0] = true;

  float bm[16];
  for(int t = 0; t < steps; t++){
    for(int o = 0; o < 16; o++)
      bm[o] = branch_metric(phase[2*t],phase[2*t+1],o);
    float next[8] = {0};
    bool next_valid[8] = {false};
    for(int p = 0; p < nstates; p++){
      if(!valid[p])
        continue;
      for(int x = 0; x < nstates; x++){
        float const m = metric[p] + bm[table[p * nstates + x]];
        if(!next_valid[x] || m < next[x]){
          next[x] = m;
          pred[t][x] = p;
          next_valid[x] = true;
        }
      }
    }
    for(int s = 0; s < nstates; s++){
      metric[s] = next_valid[s] ? next[s] : FLT_MAX;
      valid[s] = next_valid[s];
    }
  }
  // Flush to state 0
  float best = FLT_MAX;
  bool found = false;
  int best_state = 0;
  for(int p = 0; p < nstates; p++){
    if(!valid[p])
      continue;
    float const m = metric[p] + branch_metric(phase[2*steps],phase[2*steps+1],table[p * nstates]);
    if(m < best){
      best = m;
      best_state = p;
      found = true;
    }
  }
  if(!found)
    memset(out,0,steps * k);
  else
    traceback(out,pred,steps,k,best_state);
  return best;
}
float trellis_12_decode_soft(uint8_t *out,float const *phase,int nsyms){
  return viterbi_soft(out,phase,nsyms,2,&Trellis_12[0][0]);
}
float trellis_34_decode_soft(uint8_t *out,float const *phase,int nsyms){
  return viterbi_soft(out,phase,nsyms,3,&Trellis_34[0][0]);
}

// ---------------------------------------------------------------------------
// CRCs: msb first, zero preset, the remainder of M(x) x^width inverted
// (the feedback is the register's top bit XOR the next message bit, so no zeros need be appended)

static unsigned crc_bits(uint8_t const *bits,int n,unsigned poly,int width){
  unsigned const mask = (1u << width) - 1;
  unsigned reg = 0;
  for(int i = 0; i < n; i++){
    unsigned const fb = ((reg >> (width - 1)) ^ bits[i]) & 1;
    reg = (reg << 1) & mask;
    if(fb)
      reg ^= poly;
  }
  return reg ^ mask;
}
static unsigned get_field(uint8_t const *bits,int n){
  unsigned v = 0;
  for(int i = 0; i < n; i++)
    v = (v << 1) | (bits[i] & 1);
  return v;
}

uint16_t crc_ccitt(uint8_t const *bits,int n){
  assert(bits != NULL && n >= 0);
  return crc_bits(bits,n,0x1021,16);
}

int crc_ccitt80_check(uint8_t bits[96],bool correct_one){
  assert(bits != NULL);
  unsigned const residual = crc_ccitt(bits,80) ^ get_field(bits + 80,16);
  if(residual == 0)
    return 0;
  if(!correct_one)
    return -1;
  // A single error in data bit i leaves the residual x^(16 + 79 - i) mod g(x)
  unsigned s = 0x1021; // bit 79: x^16 mod g(x)
  for(int i = 79; i >= 0; i--){
    if(s == residual){
      bits[i] ^= 1;
      return 1;
    }
    s = (s & 0x8000) ? ((s << 1) ^ 0x1021) & 0xffff : s << 1;
  }
  // A single error in CRC bit i leaves the residual 2^(15-i): the data is good
  if(__builtin_popcount(residual) == 1)
    return 0;
  return -1;
}

uint16_t crc12(uint8_t const *bits,int n){
  assert(bits != NULL && n >= 0);
  return crc_bits(bits,n,0x897,12);
}
bool crc12_check(uint8_t const *bits,int n){
  assert(bits != NULL && n >= 0);
  return crc12(bits,n) == get_field(bits + n,12);
}
