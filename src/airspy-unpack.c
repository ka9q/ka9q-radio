// Helper routines for Airspy R2/Hydra SDR 12-bit packed format
// 8 12-bit A/D samples (offset +2048) are packed in 3 32-bit words
// AVX2 and C versions
// Copyright Phil Karn, KA9Q, 2026

#include <stdint.h>
#include <assert.h>
#include "airspy.h"
#if defined(__x86_64__)
// AVX2 savings aren't as dramatic as for rx888, saves maybe 2%
// Non-temporal stores don't help because the FFTs are only 500K points and inputs remain in cache
#define CACHED_STORE 1

#include <immintrin.h>

__attribute__((target("avx2")))
int airspy_unpack_avx2(float *restrict wptr, uint32_t const *restrict up,
				   int sampcount, float scale, uint64_t *energy){
  assert(wptr != NULL);
  assert(up != NULL);
  assert(sampcount > 0 && (sampcount & 7) == 0);
  assert(((uintptr_t)up & 0xf) == 0);

  int i = 0;
  const __m256i src_index = _mm256_setr_epi32(0, 0, 1, 1, 1, 2, 2, 2);
  const __m256i right_count = _mm256_setr_epi32(20, 8, 28, 16, 4, 24, 12, 0);
  const __m256i left_index =  _mm256_setr_epi32(0, 0, 0, 0, 0, 1, 0, 0);
  const __m256i left_count =  _mm256_setr_epi32(32, 32, 4, 32, 32, 8, 32, 32);
  const __m256i mask12 = _mm256_set1_epi32(0xfff);
  const __m256i midpoint = _mm256_set1_epi32(2048);
  const __m256i positive_clip = _mm256_set1_epi32(2047);
  const __m256i negative_compare = _mm256_set1_epi32(-2046);
  const __m256 vscale = _mm256_set1_ps(scale);

  __m256i energy_even = _mm256_setzero_si256();
  __m256i energy_odd = _mm256_setzero_si256();
  int over = 0;
  // The load consumes this 12-byte group plus four bytes from the next
  // group.  Stopping one group early guarantees that the load is valid.
  for (; i + 8 < sampcount; i += 8, up += 3, wptr += 8) {
    __m128i raw = _mm_loadu_si128((__m128i const *)up);
    __m256i words = _mm256_broadcastsi128_si256(raw);

    __m256i right_source = _mm256_permutevar8x32_epi32(words, src_index);
    __m256i right_part = _mm256_srlv_epi32(right_source, right_count);

    __m256i left_source =  _mm256_permutevar8x32_epi32(words, left_index);
    __m256i left_part = _mm256_sllv_epi32(left_source, left_count);

    __m256i x = _mm256_or_si256(right_part, left_part);
    x = _mm256_sub_epi32(_mm256_and_si256(x, mask12), midpoint);

    // x == 2047 || x <= -2047
    __m256i clipped = _mm256_or_si256(_mm256_cmpeq_epi32(x, positive_clip),
				      _mm256_cmpgt_epi32(negative_compare, x));
    unsigned bits = (unsigned)_mm256_movemask_ps(_mm256_castsi256_ps(clipped));
    over += (unsigned)__builtin_popcount(bits);
    // _mm256_mul_epi32 multiplies lanes 0, 2, 4, and 6.
    energy_even = _mm256_add_epi64(energy_even, _mm256_mul_epi32(x, x));

    // Move lanes 1, 3, 5, and 7 into the low 32-bit positions.
    __m256i odd = _mm256_srli_epi64(x, 32);
    energy_odd = _mm256_add_epi64(energy_odd, _mm256_mul_epi32(odd, odd));

    __m256 output = _mm256_mul_ps(_mm256_cvtepi32_ps(x), vscale);
#ifdef CACHED_STORE // cached version makes fft faster because it's smaller
    _mm256_storeu_ps(wptr, output);
#else
    _mm256_stream_ps(wptr, output); // nontemporal, bypass cache
#endif
  }
  __m256i energies = _mm256_add_epi64(energy_even, energy_odd);
  __m128i sum = _mm_add_epi64(_mm256_castsi256_si128(energies),
			      _mm256_extracti128_si256(energies, 1));

  sum = _mm_add_epi64(sum,_mm_unpackhi_epi64(sum, sum));

  *energy += (uint64_t)_mm_cvtsi128_si64(sum);
  // Process the final group without reading beyond its twelve bytes.
  if (i < sampcount) {
    uint32_t s[8];
    s[0] = up[0] >> 20;
    s[1] = up[0] >> 8;
    s[2] = (up[0] << 4) | (up[1] >> 28);
    s[3] = up[1] >> 16;
    s[4] = up[1] >> 4;
    s[5] = (up[1] << 8) | (up[2] >> 24);
    s[6] = up[2] >> 12;
    s[7] = up[2];
    for (int j = 0; j < 8; j++) {
      int const x = (int)(s[j] & 0xfff) - 2048;
      if (x == 2047 || x <= -2047)
	over++;
      wptr[j] = scale * (float)x;
      *energy += (uint64_t) ((int64_t)x * x);
    }
  }
#ifndef CACHED_STORE
  _mm_sfence();
#endif
  return over;
}
#endif
// Portable C version
int airspy_unpack(float *restrict wptr, uint32_t const *restrict up,
				   int sampcount, float scale, uint64_t *energy){
  int over = 0;
  for(int i=0; i < sampcount; i+= 8){ // assumes multiple of 8
    int s[8];
    s[0] =  up[0] >> 20;
    s[1] =  up[0] >> 8;
    s[2] =  (up[0] << 4) | (up[1] >> 28);
    s[3] =  up[1] >> 16;
    s[4] =  up[1] >> 4;
    s[5] =  (up[1] << 8) | (up[2] >> 24);
    s[6] =  up[2] >> 12;
    s[7] =  up[2];
    for(int j=0; j < 8; j++){
      int const x = (s[j] & 0xfff) - 2048; // mask not actually necessary for s[0]
      if(x == 2047 || x <= -2047)
	over++;
      wptr[j] = (float)(scale * x);
      *energy += (uint64_t) ((int64_t)x * x);
    }
    wptr += 8;
    up += 3;
  }
  return over;
}
