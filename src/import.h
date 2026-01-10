// Import and export samples of various sizes with and without byte swapping
#include <stdint.h>
#include <string.h>
_Static_assert(sizeof(float) == 4, "requires 32-bit float");

static inline void import_f32_swap(float *out,uint8_t const *in, size_t count){
  for(size_t i=0; i < count; i++){
    int32_t t;
    memcpy(&t, in + i*sizeof t, sizeof t);
    t = __builtin_bswap32(t);
    memcpy(out + i, &t ,sizeof t);
  }
}

static inline void export_f32_swap(uint8_t *out, float const *in, size_t count){
  for(size_t i=0; i < count; i++){
    int32_t t;
    memcpy(&t, in + i, sizeof t);
    t = __builtin_bswap32(t);
    memcpy(out + i * sizeof t, &t, sizeof t);
  }
}

static inline void import_s16_swap(float *out, uint8_t const *in, size_t count){
  for(size_t i=0; i < count; i++){
    int16_t t;
    memcpy(&t, in + i * sizeof t, sizeof t);
    t = __builtin_bswap16(t);
    out[i] = ldexpf((float)t,-15); // scale down
  }
}
static inline void export_s16_swap(uint8_t *out,float const *in, size_t count){
  for(size_t i=0; i < count; i++){
    float t = ldexpf(in[i],15); // Scale to integer range +/- 32767
    if(fabsf(t) >= 32767)
      t = copysign(32767,t);    // and clip
    int16_t j = t;
    j = __builtin_bswap16(j);
    memcpy(out + i*sizeof j, &j, sizeof j);
  }
}
static inline void import_s16_noswap(float *out, uint8_t const *in, size_t count){
  for(size_t i=0; i < count; i++){
    int16_t t;
    memcpy(&t, in + i * sizeof t, sizeof t);
    out[i] = ldexpf((float)t,-15); // scale down
  }
}
static inline void export_s16_noswap(uint8_t *out,float const *in, size_t count){
  for(size_t i=0; i < count; i++){
    float t = ldexpf(in[i],15); // Scale to integer range +/- 32767
    if(fabsf(t) >= 32767)
      t = copysign(32767,t);    // and clip
    int16_t j = t;
    memcpy(out + i*sizeof j, &j, sizeof j);
  }
}

#ifdef HAS_FLOAT16
_Static_assert(sizeof(float16_t) == 2, "lacks float16_t; turn off HAS_FLOAT16");
static inline void import_f16_noswap(float *out, uint8_t const *in, size_t count){
  for(size_t i=0; i < count; i++){
    float16_t t;
    memcpy(&t, in + i * sizeof t, sizeof t);
    out[i] = (float)t;
  }
}

static inline void import_f16_swap(float *out, uint8_t const *in, size_t count){
  for(size_t i=0; i < count; i++){
    int16_t j;
    memcpy(&j, in + i * sizeof j, sizeof j);
    j = __builtin_bswap16(j);
    float16_t t;
    memcpy(&t, &j, sizeof t);
    out[i] = (float)t;
  }
}
static inline void export_f16_noswap(uint8_t *out,float const *in, size_t count){
  for(size_t i=0; i < count; i++){
    float16_t t = in[i];
    memcpy(out + i * sizeof t, &t, sizeof t);
  }
}
static inline void export_f16_swap(uint8_t *out,float const *in, size_t count){
  for(size_t i=0; i < count; i++){
    float16_t t = in[i];
    int16_t j;
    memcpy(&j,&t,sizeof t);
    j = __builtin_bswap16(j);
    memcpy(out + i*sizeof j, &j, sizeof j);
  }
}
#endif

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
static inline void import_f32_le(float *out,uint8_t const *in, size_t count){
  memcpy(out, in, count * sizeof(float));
}
static inline void import_f32_be(float *out,uint8_t const *in, size_t count){
  import_f32_swap(out,in,count);
}
static inline void export_f32_le(uint8_t *out, float const *in, size_t count){
  memcpy(out, in, count * sizeof(float));
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

static inline void import_f32_le(float *out,uint8_t const *in, size_t count){
  import_f32_swap(out, in, count);
}
static inline void import_f32_be(float *out,uint8_t const *in, size_t count){
  memcpy(out, in, count * sizeof(float));
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
