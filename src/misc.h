// Miscellaneous constants, macros and function prototypes for ka9q-radio
// Copyright 2018-2026 Phil Karn, KA9Q
#ifndef _MISC_H
#define _MISC_H 1

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h> // for ldiv(), free()
#include <stdbool.h>
#include <assert.h>
#include <errno.h>
#include "config_paths.h" // pick up GIT macros

#if 0
// Must be a macro so __FILE__ and __TIMESTAMP__ will substitute correctly
#define VERSION() { fprintf(stderr,"KA9Q Multichannel SDR %s\n",__FILE__); \
  fprintf(stderr,"Copyright 2026, Phil Karn, KA9Q. May be used under the terms of the GNU Public License\n"); \
  fprintf(stderr,"   Repo: %s\n",GIT_REMOTE_URL); \
  fprintf(stderr," Commit: %s\n",GIT_HASH); \
  fprintf(stderr,"   Date: %s\n",GIT_TIME); \
  fprintf(stderr," Branch:%s\n",GIT_BRANCH); \
  fprintf(stderr,"Version: %s\n",GIT_VERSION); \
  fprintf(stderr,"Summary: %s\n",GIT_SUMMARY); \
}
#else
#define VERSION() { fprintf(stderr,"KA9Q Multichannel SDR %s\n",__FILE__); \
  fprintf(stderr,"Copyright 2026, Phil Karn, KA9Q. May be used under the terms of the GNU Public License\n"); \
  fprintf(stderr,"Git repository: %s\n",GIT_REMOTE_URL); \
  fprintf(stderr,"Git branch: %s\n",GIT_BRANCH); \
  fprintf(stderr,"Git commit hash: %s\n",GIT_HASH); \
}
#endif
#define ASSERT_ZEROED(ptr, size) assert(memcmp(ptr, &(typeof(*(ptr))){0}, size) == 0)

static inline void ASSERT_UNLOCKED(pthread_mutex_t *mutex){
#ifndef NDEBUG
  int rc = pthread_mutex_trylock(mutex);
  assert(rc != EBUSY);
  pthread_mutex_unlock(mutex);
#else
  (void)mutex;
#endif
}
// 16-bit floating point is not consistent across platforms
#ifdef __arm__  // ARM platform
  #if defined(__ARM_FP16_FORMAT_IEEE)
    typedef __fp16 float16_t;  // ARM-specific half-precision support
    #define HAS_FLOAT16 = 1
  #endif
#else  // Non-ARM platforms
  #if defined(__FLT16_MAX__)  // Check if _Float16 is natively supported
    typedef _Float16 float16_t;
    #define HAS_FLOAT16 = 1
  #endif
#endif

#define BOLTZMANN (1.380649e-23) // Boltzmann's constant, J/K, now defined exactly
#define LIGHTSPEED (299792458)   // speed of light, m/s, defined exactly
#define PLANCK (6.62607015e−34)  // Planck's constant, Js
#define AVOGADRO (6.02214076e23) // Avogadro constant

#define FULL_SAMPRATE (48000)    // Standard sampling rate in professional digital audio, widely supported

// I *hate* this sort of pointless, stupid, gratuitous incompatibility that
// makes a lot of code impossible to read and debug
#ifdef __APPLE__
// The Linux version of pthread_setname_np takes two args, the OSx version only one
// The GNU malloc_usable_size() does exactly the same thing as the BSD/OSX malloc_size()
// except that the former is defined in <malloc.h>, the latter is in <malloc/malloc.h>

#define pthread_setname(x) pthread_setname_np(x)
#include <malloc/malloc.h>
#define malloc_usable_size(x) malloc_size(x)

#else // !__APPLE__
// Not apple (Linux, etc)

#include <malloc.h>
#define pthread_setname(x) pthread_setname_np(pthread_self(),(x))

#endif // ifdef __APPLE__

// Stolen from the Linux kernel -- enforce type matching of arguments
#define min(x,y) ({typeof(x) _x = (x); typeof(y) _y = (y); (void) (&_x == &_y);	_x < _y ? _x : _y; })
#define max(x,y) ({typeof(x) _x = (x); typeof(y) _y = (y); (void) (&_x == &_y);	_x > _y ? _x : _y; })

#include "ka9q-math.h"

extern const char *App_path;
extern int Verbose;

int dist_path(char *path,int path_len,const char *fname);
double parse_frequency(char const *,bool);
void chomp(char *);

// Convert floating point sample to 16-bit integer, with clipping
inline static int16_t scaleclip(float const x){
  return (x >= 1.0f) ? INT16_MAX : (x <= -1.0f) ? -INT16_MAX : (int16_t)(INT16_MAX * x);
}

// memory allocation helpers
// How the free() library routine should have been all along: null the pointer after freeing!
#define FREE(p) (free(p), p = NULL)

void rand_init(void);
float real_gauss(void);
float uniform_rv(void);
void rayleigh_init(void);
void random_buffer(uint32_t *output, int count);
void gaussian_buffer(float *output, int count);
void rayleigh_buffer(float *output, int count);
#endif // _MISC_H
