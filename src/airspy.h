#if defined(__x86_64__)
__attribute__((target("avx2")))
int airspy_unpack_avx2(float *restrict wptr, uint32_t const *restrict up,
		       int sampcount, float samp_scale, uint64_t *energy);
#endif
int airspy_unpack(float *restrict wptr, uint32_t const *restrict up,
		  int sampcount, float scale, uint64_t *energy);

