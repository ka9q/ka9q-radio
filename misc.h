// $Id: misc.h,v 1.30 2022/08/05 06:35:10 karn Exp $
// Miscellaneous constants, macros and function prototypes
// Copyright 2018 Phil Karn, KA9Q
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
#include <stdlib.h> // for ldiv()

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
#define sincospif(x,s,c) sincosf((x)*M_PI,(s),(c))

#endif // ifdef __APPLE__





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

#define M_1_2PI (0.5 * M_1_PI) // fraction of a rotation in one radian
#define DEGPRA (180./M_PI)
#define RAPDEG (M_PI/180.)
#define GPS_UTC_OFFSET (18) // GPS ahead of utc by 18 seconds - make this a table!
#define UNIX_EPOCH ((time_t)315964800) // GPS epoch on unix time scale

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
char *format_gpstime(char *result,int len,long long t);
char *format_utctime(char *result,int len,long long t);
char *ftime(char *result,int size,long long t);
void normalize_time(struct timespec *x);
double parse_frequency(const char *);
uint32_t nextfastfft(uint32_t n);
int pipefill(int,void *,int);
void chomp(char *);
uint32_t ElfHash(unsigned char const *s,int length);
uint32_t ElfHashString(char const *s);
void *avahi_start(char const *service_name,char const *service_type,int service_port,char const *dns_name,int base_address,char const *description);

// Modified Bessel functions
float i0(float const z); // 0th kind
float i1(float const z); // 1st kind

float xi(float thetasq);
float fm_snr(float r);



static int16_t inline scaleclip(float const x){
  if(x >= 1.0)
    return SHRT_MAX;
  else if(x <= -1.0)
    return SHRT_MIN;
  return (int16_t)(SHRT_MAX * x);
}
static inline complex float const csincosf(float const x){
  float s,c;

  sincosf(x,&s,&c);
  return CMPLXF(c,s);
}
static inline complex float const csincospif(float const x){
  float s,c;
  sincospif(x,&s,&c);
  return CMPLXF(c,s);
}
// return unit magnitude complex number with given phase x
static inline complex double const csincos(double const x){
  double s,c;

  sincos(x,&s,&c);
  return CMPLX(c,s);
}
static inline complex double const csincospi(double const x){
  double s,c;
  sincospi(x,&s,&c);
  return CMPLX(c,s);
}
// Complex norm (sum of squares of real and imaginary parts)
static inline float const cnrmf(complex float const x){
  return crealf(x)*crealf(x) + cimagf(x) * cimagf(x);
}
static inline double const cnrm(complex double const x){
  return creal(x)*creal(x) + cimag(x) * cimag(x);
}
// Fast approximate square root, for signal magnitudes
// https://dspguru.com/dsp/tricks/magnitude-estimator/
static inline float approx_magf(complex float x){
  const static float Alpha = 0.947543636291;
  const static float Beta =  0.392485425092;

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
  if(a->tv_sec > b->tv_sec)
    return +1;
  if(a->tv_sec < b->tv_sec)
    return -1;
  if(a->tv_nsec > b->tv_nsec)
    return +1;
  if(a->tv_nsec < b->tv_nsec)
    return -1;
  return 0;
}
static long long const BILLION = 1000000000LL;

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


#endif // _MISC_H
