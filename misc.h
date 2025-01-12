// Miscellaneous constants, macros and function prototypes for ka9q-radio
// Copyright 2018-2023 Phil Karn, KA9Q
#ifndef _MISC_H
#define _MISC_H 1

// Note: files that include <math.h> before us must define _GNU_SOURCE prior to including math.h
// or Linux will generate warnings about a lack of declarations for sincos and sincosf.
// Apparently they are defined in math.h only when _GNU_SOURCE is defined.
// Our re-defining _GNU_SOURCE and re-including math.h doesn't help if it has already been included
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include <pthread.h>
#include <stdint.h>
#include <limits.h>
#include <complex.h>
#include <math.h> // Get M_PI
#include <stdlib.h> // for ldiv(), free()
#include <stdbool.h>
#ifdef __linux__
#include <bsd/string.h>
#endif
#include <assert.h>

// Must be a macro so __FILE__ and __TIMESTAMP__ will substitute correctly
#define VERSION() { fprintf(stdout,"KA9Q Multichannel SDR %s last modified %s\n",__FILE__,__TIMESTAMP__); \
		    fprintf(stdout,"Copyright 2023, Phil Karn, KA9Q. May be used under the terms of the GNU Public License\n");}


// 16-bit floating point is not consistent across platforms
#ifdef __arm__  // ARM platform
  #if defined(__ARM_FP16_FORMAT_IEEE)
    typedef __fp16 float16_t;  // ARM-specific half-precision support
  #else
    typedef float float16_t;  // Fallback on older ARM CPUs
  #endif
  #define HAS_FLOAT16 = 1
#else  // Non-ARM platforms
  #if defined(__FLT16_MAX__)  // Check if _Float16 is natively supported
    typedef _Float16 float16_t;
    #define HAS_FLOAT16 = 1
  #endif
#endif

#ifndef M_PIf
#define M_PIf ((float)(M_PI))
#endif
#ifndef M_1_PIf
#define M_1_PIf (1 / M_PIf)
#endif

#define M_1_2PIf (0.5f * M_1_PIf) // fraction of a rotation in one radian
#define DEGPRA (180./M_PI)
#define RAPDEG (M_PI/180.)
#define GPS_UTC_OFFSET (18) // GPS ahead of utc by 18 seconds - make this a table!
#define UNIX_EPOCH ((time_t)315964800) // GPS epoch on unix time scale

#define BOLTZMANN (1.380649e-23) // Boltzmann's constant, J/K

static float const SCALE16 = 1./INT16_MAX;
static float const SCALE12 = 1/2048.;
static float const SCALE8 = 1./INT8_MAX;  // Scale signed 8-bit int to float in range -1, +1


void realtime(void);
bool norealtime(void);

// I *hate* this sort of pointless, stupid, gratuitous incompatibility that
// makes a lot of code impossible to read and debug

#ifdef __APPLE__
// OSX doesn't have pthread_barrier_*
#include <pthread.h>

typedef int pthread_barrierattr_t;
typedef struct
{
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int count;
    int tripCount;
} pthread_barrier_t;
int pthread_barrier_init(pthread_barrier_t *barrier, const pthread_barrierattr_t *attr, unsigned int count);
int pthread_barrier_destroy(pthread_barrier_t *barrier);
int pthread_barrier_wait(pthread_barrier_t *barrier);

// The Linux version of pthread_setname_np takes two args, the OSx version only one
// The GNU malloc_usable_size() does exactly the same thing as the BSD/OSX malloc_size()
// except that the former is defined in <malloc.h>, the latter is in <malloc/malloc.h>

#define pthread_setname(x) pthread_setname_np(x)
#include <malloc/malloc.h>
#define malloc_usable_size(x) malloc_size(x)
#define sincos(x,s,c) __sincos((x),(s),(c))
#define sincosf(x,s,c) __sincosf((x),(s),(c))
#define sincospi(x,s,c) __sincospi((x),(s),(c))
#define sincospif(x,s,c) __sincospif((x),(s),(c))

#else // !__APPLE__
// Not apple (Linux, etc)

#include <malloc.h>
#define pthread_setname(x) pthread_setname_np(pthread_self(),(x))
// Does anyone implement these natively for Linux?
#define sincospi(x,s,c) sincos((x)*M_PI,(s),(c))
#define sincospif(x,s,c) sincosf((x)*M_PIf,(s),(c))

#endif // ifdef __APPLE__

// Portable mutex initializer for recursive mutexes
static inline int init_recursive_mutex(pthread_mutex_t *m){
  pthread_mutexattr_t attr;
  pthread_mutexattr_init(&attr);
  pthread_mutexattr_settype(&attr,PTHREAD_MUTEX_RECURSIVE);
  return pthread_mutex_init(m,&attr);
}


// Stolen from the Linux kernel -- enforce type matching of arguments
#define min(x,y) ({			\
		typeof(x) _x = (x);	\
		typeof(y) _y = (y);	\
		(void) (&_x == &_y);	\
		_x < _y ? _x : _y; })

#define max(x,y) ({ \
		typeof(x) _x = (x);	\
		typeof(y) _y = (y);	\
		(void) (&_x == &_y);	\
		_x > _y ? _x : _y; })


#define dB2power(x) (powf(10.0f,(x)/10.0f))
#define power2dB(x) (10.0f * log10f(x))
#define dB2voltage(x) (powf(10.0f, (x)/20.0f))
#define voltage2dB(x) (20.0f * log10f(x))

// Cos(x) + j*sin(x)
#define cisf(x) csincosf(x)
#define cispif(x) csincospif(x)
#define cis(x) csincos(x)
#define cispi(x) csincospi(x)

extern const char *App_path;
extern int Verbose;
extern char const *Months[12];

int dist_path(char *path,int path_len,const char *fname);
char *format_gpstime(char *result,int len,int64_t t);
char *format_gpstime_iso8601(char *result,int len,int64_t t);
char *format_utctime(char *result,int len,int64_t t);
char *format_utctime_iso8601(char *result,int len,int64_t t);
char *ftime(char *result,int size,int64_t t);
void normalize_time(struct timespec *x);
double parse_frequency(char const *,bool);
uint32_t nextfastfft(uint32_t n);
int pipefill(int,void *,int);
void chomp(char *);
uint32_t ElfHash(uint8_t const *s,int length);
uint32_t ElfHashString(char const *s);
uint32_t fnv1hash(const uint8_t *s,int length);

// Modified Bessel functions
float i0(float const z); // 0th kind
float i1(float const z); // 1st kind

float xi(float thetasq);
float fm_snr(float r);

// Convert floating point sample to 16-bit integer, with clipping
inline static int16_t scaleclip(float const x){
  return (x >= 1.0) ? INT16_MAX : (x <= -1.0) ? -INT16_MAX : (int16_t)(INT16_MAX * x);
}
static inline complex float csincosf(float const x){
  float s,c;

  sincosf(x,&s,&c);
  return CMPLXF(c,s);
}
static inline complex float csincospif(float const x){
  float s,c;
  sincospif(x,&s,&c);
  return CMPLXF(c,s);
}
// return unit magnitude complex number with given phase x
static inline complex double csincos(double const x){
  double s,c;

  sincos(x,&s,&c);
  return CMPLX(c,s);
}
static inline complex double csincospi(double const x){
  double s,c;
  sincospi(x,&s,&c);
  return CMPLX(c,s);
}
// Complex norm (sum of squares of real and imaginary parts)
static inline float cnrmf(complex float const x){
  return crealf(x)*crealf(x) + cimagf(x) * cimagf(x);
}
static inline double cnrm(complex double const x){
  return creal(x)*creal(x) + cimag(x) * cimag(x);
}
// Fast approximate square root, for signal magnitudes
// https://dspguru.com/dsp/tricks/magnitude-estimator/
static inline float approx_magf(complex float x){
  static float const Alpha = 0.947543636291;
  static float const Beta =  0.392485425092;

  float absr = fabsf(__real__ x);
  float absi = fabsf(__imag__ x);

  return Alpha * max(absr,absi) + Beta * min(absr,absi);
}

// Result = a - b
static inline void time_sub(struct timespec *result,struct timespec const *a, struct timespec const *b){
  result->tv_sec = a->tv_sec - b->tv_sec;
  result->tv_nsec = a->tv_nsec - b->tv_nsec;
  normalize_time(result);
}
// Result = a + b
static inline void time_add(struct timespec *result,struct timespec const *a, struct timespec const *b){
  result->tv_sec = a->tv_sec + b->tv_sec;
  result->tv_nsec = a->tv_nsec + b->tv_nsec;
  normalize_time(result);
}

// Compare two timespec structures, assuming normalized
// a > b: +1
// a < b: -1
// a == b: 0
static inline int time_cmp(struct timespec const *a,struct timespec const *b){
  // Will this long conditional help the optimizer?
  return (a->tv_sec > b->tv_sec) ? 1
    : (a->tv_sec < b->tv_sec) ? -1
    : (a->tv_nsec > b->tv_nsec) ? +1
    : (a->tv_nsec < b->tv_nsec) ? -1
    : 0;
}
static long long const BILLION = 1000000000LL;
static long const MILLION = 1000000L;
static int const THOUSAND = 1000;

// Convert timespec (seconds, nanoseconds) to integer nanoseconds
// Integer nanoseconds overflows past 584.94242 years. That's probably long enough
static inline long long ts2ns(struct timespec const *ts){
  return ts->tv_sec * BILLION + ts->tv_nsec;
}
// Convert integer nanosec count to timspec
static inline void ns2ts(struct timespec *ts,long long ns){
  lldiv_t r = lldiv(ns,BILLION);
  ts->tv_sec = r.quot;
  ts->tv_nsec = r.rem;
}

// Return time of day as seconds (truncated) from UTC epoch
static inline time_t utc_time_sec(void){
  struct timespec now;
  clock_gettime(CLOCK_REALTIME,&now);
  return (time_t)now.tv_sec;
}
// Same from GPS epoch
static inline time_t gps_time_sec(void){
  return utc_time_sec() - (UNIX_EPOCH - GPS_UTC_OFFSET);
}

// Return time of day as nanosec from UTC epoch
static inline long long utc_time_ns(void){
  struct timespec now;
  clock_gettime(CLOCK_REALTIME,&now);
  return ts2ns(&now);
}

// Return time of day as nanosec from GPS epoch
// Note: assumes fixed leap second offset
// Could be better derived direct from a GPS receiver without applying the leap second offset
static inline long long gps_time_ns(void){
  return utc_time_ns() - BILLION * (UNIX_EPOCH - GPS_UTC_OFFSET);
}

// How the free() library routine should have been all along: null the pointer after freeing!
#define FREE(p) (free(p), p = NULL)

// Create allocation followed immediately by its mirror, useful for ring buffers
// size is rounded up to next page boundary
void *mirror_alloc(size_t size);
void mirror_free(void **p,size_t size);

// Wrap pointer p to keep it in range (base, base + size), where size is in bytes
// The callers use C casts in a somewhat dodgy fashion, but is OK because size is always a multiple of the page size,
// and there's an integral number of the objects we're pointing to in a page (we hope!!)
static inline void mirror_wrap(void const **p, void const * const base,size_t const size){
  assert(*p >= base); // Shouldn't be THIS low
  assert(*p < base + 2 * size); // Or this high

  if((uint8_t *)*p >= (uint8_t *)base + size)
    *p = (uint8_t *)*p - size;
}

// round argument up to an even number of system pages
size_t round_to_page(size_t size);

#endif // _MISC_H
