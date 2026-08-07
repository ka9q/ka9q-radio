#define _GNU_SOURCE 1
//#define AES 1
#if VAES
#include <immintrin.h>
#include <stdint.h>

typedef struct {
    __m128i rk[11];   /* AES-128: 11 round keys */
} aes128_key;

static inline __m128i
aes128_encrypt_block(__m128i x, const aes128_key *k)
{
    x = _mm256_xor_si256(x, k->rk[0]);

    x = _mm256_aesenc_epi128(x, k->rk[1]);
    x = _mm256_aesenc_epi128(x, k->rk[2]);
    x = _mm256_aesenc_epi128(x, k->rk[3]);
    x = _mm256_aesenc_epi128(x, k->rk[4]);
    x = _mm256_aesenc_epi128(x, k->rk[5]);
    x = _mm256_aesenc_epi128(x, k->rk[6]);
    x = _mm256_aesenc_epi128(x, k->rk[7]);
    x = _mm256_aesenc_epi128(x, k->rk[8]);
    x = _mm256_aesenc_epi128(x, k->rk[9]);

    return _mm256_aesenclast_epi128(x, k->rk[10]);
}

static __m128i counter_block[2];
static bool odd = false;
static __m128i random_block[2];

static inline __m128i
aes_rand(){
  if(!odd){
    counter_block[0]++;
    counter_block[1]++;
    random_block = aes128_encrypt_block(counter_block, &key);
    return random_block[0];
    odd = true;
  } else {
    odd = false;
    return random_block[1];
  }
}


#elif AES

#include <wmmintrin.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>


typedef struct {
    __m128i rk[11];   /* AES-128: 11 round keys */
} aes128_key;


#define AES128_EXPAND_STEP(prev, rcon)                                      \
    do {                                                                    \
        __m128i t = _mm_aeskeygenassist_si128((prev), (rcon));              \
        t = _mm_shuffle_epi32(t, 0xff);                                     \
        __m128i x = (prev);                                                 \
        x = _mm_xor_si128(x, _mm_slli_si128(x, 4));                         \
        x = _mm_xor_si128(x, _mm_slli_si128(x, 4));                         \
        x = _mm_xor_si128(x, _mm_slli_si128(x, 4));                         \
        (prev) = _mm_xor_si128(x, t);                                       \
    } while (0)

static inline void
aes128_init(aes128_key *ks, const uint8_t key[16]){
    __m128i k = _mm_loadu_si128((const __m128i *)key);

    ks->rk[0] = k;

    AES128_EXPAND_STEP(k, 0x01); ks->rk[1]  = k;
    AES128_EXPAND_STEP(k, 0x02); ks->rk[2]  = k;
    AES128_EXPAND_STEP(k, 0x04); ks->rk[3]  = k;
    AES128_EXPAND_STEP(k, 0x08); ks->rk[4]  = k;
    AES128_EXPAND_STEP(k, 0x10); ks->rk[5]  = k;
    AES128_EXPAND_STEP(k, 0x20); ks->rk[6]  = k;
    AES128_EXPAND_STEP(k, 0x40); ks->rk[7]  = k;
    AES128_EXPAND_STEP(k, 0x80); ks->rk[8]  = k;
    AES128_EXPAND_STEP(k, 0x1b); ks->rk[9]  = k;
    AES128_EXPAND_STEP(k, 0x36); ks->rk[10] = k;
}

#undef AES128_EXPAND_STEP

static aes128_key ks;


typedef struct {
    uint64_t lo;
    uint64_t hi;
} aes_ctr128;

static aes_ctr128 counter_block;

static inline __m128i
ctr_to_m128i(aes_ctr128 c)
{
    return _mm_set_epi64x((long long)c.hi, (long long)c.lo);
}

static inline void
ctr_inc(aes_ctr128 *c)
{
    c->lo++;
    if (c->lo == 0)
        c->hi++;
}

static inline __m128i
aes128_encrypt_block(__m128i x, const aes128_key *k){
    x = _mm_xor_si128(x, k->rk[0]);

    x = _mm_aesenc_si128(x, k->rk[1]);
    x = _mm_aesenc_si128(x, k->rk[2]);
    x = _mm_aesenc_si128(x, k->rk[3]);
    x = _mm_aesenc_si128(x, k->rk[4]);
    x = _mm_aesenc_si128(x, k->rk[5]);
    x = _mm_aesenc_si128(x, k->rk[6]);
    x = _mm_aesenc_si128(x, k->rk[7]);
    x = _mm_aesenc_si128(x, k->rk[8]);
    x = _mm_aesenc_si128(x, k->rk[9]);

    return _mm_aesenclast_si128(x, k->rk[10]);
}

static inline __m128i
aes_rand(){
  ctr_inc(&counter_block);
  __m128i random_block = aes128_encrypt_block(ctr_to_m128i(counter_block), &ks);
  return random_block;
}

static void aes_rand_init(){
  int fd = open("/dev/random",O_RDONLY);
  uint8_t key[16];
  read(fd,key,sizeof key);
  close(fd);
  aes128_init(&ks, key);
}


#endif


#include <stdint.h>
#include <stdbool.h>
#include "misc.h"

/* xoshiro256** PRNG
 *
 * Public domain reference implementation adapted from:
 *   http://prng.di.unimi.it/
 *
 * State must be nonzero. Use the seed function below.
 */

_Thread_local xoshiro256ss_state Rand_state;
_Thread_local bool Rand_init;

/* Rotate left */
static inline uint64_t rotl64(uint64_t x, int k){
    return (x << k) | (x >> (64 - k));
}

/* SplitMix64 for seeding xoshiro256** */
static uint64_t splitmix64(uint64_t *x){
    uint64_t z = (*x += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

/* Initialize xoshiro256** state from a 64-bit seed */
void xoshiro256ss_seed(xoshiro256ss_state *st, uint64_t seed){
    /* Expand a single 64-bit seed into 4 nonzero 64-bit words */
    uint64_t x = seed;
    st->s[0] = splitmix64(&x);
    st->s[1] = splitmix64(&x);
    st->s[2] = splitmix64(&x);
    st->s[3] = splitmix64(&x);

    /* Extremely unlikely, but ensure not all zeros */
    if ((st->s[0] | st->s[1] | st->s[2] | st->s[3]) == 0) {
        st->s[0] = 1; /* arbitrary nonzero */
    }
}

/* Generate next 64-bit output */
uint64_t xoshiro256ss_next(xoshiro256ss_state *st){
    const uint64_t result = rotl64(st->s[1] * 5, 7) * 9;

    const uint64_t t = st->s[1] << 17;

    st->s[2] ^= st->s[0];
    st->s[3] ^= st->s[1];
    st->s[1] ^= st->s[2];
    st->s[0] ^= st->s[3];

    st->s[2] ^= t;
    st->s[3] = rotl64(st->s[3], 45);

    return result;
}

/* Optional: jump function for 2^128 steps ahead (independent streams) */
void xoshiro256ss_jump(xoshiro256ss_state *st){
    static const uint64_t JUMP[] = {
        0x180ec6d33cfd0abaULL,
        0xd5a61266f0c9392cULL,
        0xa9582618e03fc9aaULL,
        0x39abdc4529b1661cULL
    };

    uint64_t s0 = 0;
    uint64_t s1 = 0;
    uint64_t s2 = 0;
    uint64_t s3 = 0;

    for (int i = 0; i < 4; i++) {
        for (int b = 0; b < 64; b++) {
            if (JUMP[i] & (1ULL << b)) {
                s0 ^= st->s[0];
                s1 ^= st->s[1];
                s2 ^= st->s[2];
                s3 ^= st->s[3];
            }
            (void)xoshiro256ss_next(st);
        }
    }

    st->s[0] = s0;
    st->s[1] = s1;
    st->s[2] = s2;
    st->s[3] = s3;
}
// Fast Gaussian approximation
void rand_init(void){
  if(Rand_init)
    return; // already done

#if AES
  aes_rand_init();
#endif
  xoshiro256ss_seed(&Rand_state,1);
  Rand_init = true;
}

#if AES
static uint64_t out[2];
static bool gauss_odd;

double real_gauss(void){
  //  uint64_t u = xoshiro256ss_next(&Rand_state);
  uint64_t u;
  if(!gauss_odd){
    __m128i ar = aes_rand();
    _mm_storeu_si128((__m128i *)out,ar);
    gauss_odd = true;
    u = out[0];
  } else {
    u = out[1];
    gauss_odd = false;
  }

  double x = __builtin_popcountll(u*0x2c1b3c6dULL) +
    __builtin_popcountll(u*0x297a2d39ULL) - 64;
  x += (int64_t)u * (1 / 9223372036854775808.);
  x *= 0.1765469659009499; /* sqrt(1/(32 + 4/12)) */
  return x;
}
#else
double real_gauss(void){
  uint64_t u = xoshiro256ss_next(&Rand_state);
  double x = __builtin_popcountll(u*0x2c1b3c6dULL) +
    __builtin_popcountll(u*0x297a2d39ULL) - 64;
  x += (int64_t)u * (1 / 9223372036854775808.);
  x *= 0.1765469659009499; /* sqrt(1/(32 + 4/12)) */
  return x;
}

#endif


