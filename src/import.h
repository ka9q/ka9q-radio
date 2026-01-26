// Import and export samples of various sizes with and without byte swapping
// part of ka9q-radio
// January 2026, Phil Karn, KA9Q
#include <stdint.h>
#include <string.h>
#include <float.h>
#include <limits.h>
#include <float.h>
#include <limits.h>
#include "rtp.h"

_Static_assert(sizeof(float) == 4, "float must be 32-bit");
_Static_assert(FLT_RADIX == 2, "float must be base-2");
_Static_assert(FLT_MANT_DIG == 24, "float must have 24-bit significand");
_Static_assert(FLT_MAX_EXP == 128, "float must have IEEE-754 exponent range");

static inline void import_mulaw(float *out, uint8_t const *in, size_t count) {
  for(size_t i=0; i < count; i++)
    out[i] = mulaw_to_float(in[i]);
}

static inline void export_mulaw(uint8_t *out, float const *in, size_t count) {
  for (size_t i = 0; i < count; i++)
    out[i] = float_to_mulaw(in[i]);
}

static inline void import_alaw(float *out, uint8_t const *in, size_t count) {
  for(size_t i=0; i < count; i++)
    out[i] = alaw_to_float(in[i]);
}

static inline void export_alaw(uint8_t *out, float const *in, size_t count) {
  for (size_t i = 0; i < count; i++)
    out[i] = float_to_alaw(in[i]);
}
static inline void import_f64_swap(double *out,uint8_t const *in, size_t count){
  for(size_t i=0; i < count; i++){
    uint64_t temp_int;
    memcpy(&temp_int, in + i*sizeof temp_int, sizeof temp_int);
    temp_int = __builtin_bswap64(temp_int);
    memcpy(out + i, &temp_int, sizeof temp_int);
  }
}

static inline void export_f64_swap(uint8_t *out, double const *in, size_t count){
  for(size_t i=0; i < count; i++){
    uint64_t temp_int;
    memcpy(&temp_int, in + i, sizeof temp_int);
    temp_int = __builtin_bswap64(temp_int);
    memcpy(out + i * sizeof temp_int, &temp_int, sizeof temp_int);
  }
}

static inline void import_f32_swap(float *out,uint8_t const *in, size_t count){
  for(size_t i=0; i < count; i++){
    uint32_t temp_int;
    memcpy(&temp_int, in + i*sizeof temp_int, sizeof temp_int);
    temp_int = __builtin_bswap32(temp_int);
    memcpy(out + i, &temp_int, sizeof temp_int);
  }
}

static inline void export_f32_swap(uint8_t *out, float const *in, size_t count){
  for(size_t i=0; i < count; i++){
    uint32_t temp_int;
    memcpy(&temp_int, in + i, sizeof temp_int);
    temp_int = __builtin_bswap32(temp_int);
    memcpy(out + i * sizeof temp_int, &temp_int, sizeof temp_int);
  }
}

static inline void import_s16_swap(float *out, uint8_t const *in, size_t count){
  for(size_t i=0; i < count; i++){
    int16_t temp_int;
    memcpy(&temp_int, in + i * sizeof temp_int, sizeof temp_int);
    temp_int = (int16_t) __builtin_bswap16((uint16_t)temp_int);
    out[i] = ldexpf((float)temp_int, -15); // scale down
  }
}
static inline void export_s16_swap(uint8_t *out,float const *in, size_t count){
  for(size_t i=0; i < count; i++){
    float temp_float = ldexpf(in[i],15); // Scale to integer range and clip to +/- 32767
    temp_float = temp_float > 32767.0 ? 32767.0
      : temp_float < -32767.0 ? -32767.0
      : temp_float;
    int16_t temp_int = lrintf(temp_float);
    temp_int = (int16_t)__builtin_bswap16((uint16_t)temp_int);
    memcpy(out + i*sizeof temp_int, &temp_int, sizeof temp_int);
  }
}
static inline void import_s16_noswap(float *out, uint8_t const *in, size_t count){
  for(size_t i=0; i < count; i++){
    int16_t temp_int;
    memcpy(&temp_int, in + i * sizeof temp_int, sizeof temp_int);
    out[i] = ldexpf((float)temp_int, -15); // scale integer to +/- float
  }
}
static inline void export_s16_noswap(uint8_t *out,float const *in, size_t count){
  for(size_t i=0; i < count; i++){
    float temp_float = ldexpf(in[i],15); // Scale to integer range and clip to+/- 32767
    temp_float = temp_float > 32767.0f ? 32767.0f
      : temp_float < -32767.0f ? -32767.0f
      : temp_float;

    int16_t temp_int = lrintf(temp_float);
    memcpy(out + i*sizeof temp_int, &temp_int, sizeof temp_int);
  }
}

#ifdef HAS_FLOAT16
_Static_assert(sizeof(float16_t) == 2, "lacks float16_t; turn off HAS_FLOAT16");
static inline void import_f16_noswap(float *out, uint8_t const *in, size_t count){
  for(size_t i=0; i < count; i++){
    float16_t temp_float;
    memcpy(&temp_float, in + i * sizeof temp_float, sizeof temp_float);
    out[i] = (float)temp_float;
  }
}

static inline void import_f16_swap(float *out, uint8_t const *in, size_t count){
  for(size_t i=0; i < count; i++){
    uint16_t temp_int;
    memcpy(&temp_int, in + i * sizeof temp_int, sizeof temp_int);
    temp_int = __builtin_bswap16(temp_int);
    float16_t temp_float;
    memcpy(&temp_float, &temp_int, sizeof temp_float);
    out[i] = (float)temp_float;
  }
}
static inline void export_f16_noswap(uint8_t *out,float const *in, size_t count){
  for(size_t i=0; i < count; i++){
    float16_t temp_float = in[i];
    memcpy(out + i * sizeof temp_float, &temp_float, sizeof temp_float);
  }
}
static inline void export_f16_swap(uint8_t *out,float const *in, size_t count){
  for(size_t i=0; i < count; i++){
    float16_t temp_float = in[i];
    uint16_t temp_int;
    memcpy(&temp_int, &temp_float, sizeof temp_int);
    temp_int = __builtin_bswap16(temp_int);
    memcpy(out + i*sizeof temp_int, &temp_int, sizeof temp_int);
  }
}
#endif

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
static inline void import_f64_le(double *out,uint8_t const *in, size_t count){
  memcpy(out, in, count * sizeof *out);
}
static inline void import_f64_be(double *out,uint8_t const *in, size_t count){
  import_f64_swap(out,in,count);
}
static inline void export_f64_le(uint8_t *out, double const *in, size_t count){
  memcpy(out, in, count * sizeof *in);
}
static inline void export_f64_be(uint8_t *out, double const *in, size_t count){
  export_f64_swap(out, in, count);
}

static inline void import_f32_le(float *out,uint8_t const *in, size_t count){
  memcpy(out, in, count * sizeof *out);
}
static inline void import_f32_be(float *out,uint8_t const *in, size_t count){
  import_f32_swap(out,in,count);
}
static inline void export_f32_le(uint8_t *out, float const *in, size_t count){
  memcpy(out, in, count * sizeof *in);
}
static inline void export_f32_be(uint8_t *out, float const *in, size_t count){
  export_f32_swap(out, in, count);
}

static inline void import_s16_le(float *out,uint8_t const *in, size_t count){
  import_s16_noswap(out, in, count);
}
static inline void import_s16_be(float *out,uint8_t const *in, size_t count){
  import_s16_swap(out, in, count);
}
static inline void export_s16_le(uint8_t *out,float const *in, size_t count){
  export_s16_noswap(out, in, count);
}
static inline void export_s16_be(uint8_t *out,float const *in, size_t count){
  export_s16_swap(out, in, count);
}

#ifdef HAS_FLOAT16
_Static_assert(sizeof(float16_t) == 2, "lacks 16-bit float; turn off HAS_FLOAT16");
static inline void import_f16_le(float *out,uint8_t const *in, size_t count){
  import_f16_noswap(out, in, count);
}
static inline void import_f16_be(float *out, uint8_t const *in, size_t count){
  import_f16_swap(out, in, count);
}
static inline void export_f16_le(uint8_t *out,float const *in, size_t count){
  export_f16_noswap(out, in, count);
}
static inline void export_f16_be(uint8_t *out,float const *in, size_t count){
  export_f16_swap(out, in, count);
}
#endif

#else // big endian machine

static inline void import_f64_le(double *out,uint8_t const *in, size_t count){
  import_f64_swap(out, in, count);
}
static inline void import_f64_be(double *out,uint8_t const *in, size_t count){
  memcpy(out, in, count * sizeof *out);
}
static inline void export_f64_le(uint8_t *out, double const *in, size_t count){
  export_f64_swap(out, in, count);
}
static inline void export_f64_be(uint8_t *out, double const *in, size_t count){
  memcpy(out, in, count * sizeof *in);
}

static inline void import_f32_le(float *out,uint8_t const *in, size_t count){
  import_f32_swap(out, in, count);
}
static inline void import_f32_be(float *out,uint8_t const *in, size_t count){
  memcpy(out, in, count * sizeof *out);
}
static inline void export_f32_le(uint8_t *out, float const *in, size_t count){
  export_f32_swap(out, in, count);
}
static inline void export_f32_be(uint8_t *out, float const *in, size_t count){
  memcpy(out, in, count * sizeof(float));
}

static inline void import_s16_le(float *out,uint8_t const *in, size_t count){
  import_s16_swap(out, in, count);
}
static inline void import_s16_be(float *out,uint8_t const *in, size_t count){
  import_s16_noswap(out, in, count);
}
static inline void export_s16_le(uint8_t *out,float const *in, size_t count){
  export_s16_swap(out, in, count);
}
static inline void export_s16_be(uint8_t *out,float const *in, size_t count){
  export_s16_noswap(out, in, count);
}

#ifdef HAS_FLOAT16
_Static_assert(sizeof(float16_t) == 2, "lacks 16-bit float; turn off HAS_FLOAT16");
static inline void import_f16_le(float *out, uint8_t const *in, size_t count){
  import_f16_swap(out, in, count);
}
static inline void import_f16_be(float *out, uint8_t const *in, size_t count){
  import_f16_noswap(out, in, count);

}
static inline void export_f16_le(uint8_t *out,float const *in, size_t count){
  export_f16_swap(out,in,count);
}
static inline void export_f16_be(uint8_t *out,float const *in, size_t count){
  export_f16_noswap(out, in, count);
}
#endif
#endif
