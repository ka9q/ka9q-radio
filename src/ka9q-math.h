// Math-related definitions for ka9q-radio
// Copyright 2026 Phil Karn, KA9Q

#ifndef _KA9Q_MATH_H
#define _KA9Q_MATH_H 1
#include <complex.h>
#include <math.h>

#define DEGPRA (180./M_PI)
#define RAPDEG (M_PI/180.)

// Complex norm (sum of squares of real and imaginary parts)
static inline float cnrmf(float complex const x){
  return crealf(x)*crealf(x) + cimagf(x) * cimagf(x);
}
static inline double cnrm(double complex const x){
  return creal(x)*creal(x) + cimag(x) * cimag(x);
}

#if !defined(M_SQRT1_2f)
# define M_SQRT1_2f	0.70710678118654752440f	/* 1/sqrt(2) */
#endif

#if !defined(M_SQRT2f)
#define M_SQRT2f (1.41421356237309504880f)
#endif

#if !defined(M_PIf)
#define M_PIf (0x1.921fb6p+1f)  // (float) pi
#endif

#if !defined(M_1_PIf)
#define M_1_PIf (0x1.45f306p-2f) // (float) (1/pi)
#endif

#if defined(__GLIBC__)
#include <features.h>
# if __GLIBC_PREREQ(2, 41)
#  define HAVE_MODERN_GLIBC 1
# endif
#endif

#if !defined(HAVE_MODERN_GLIBC)
#if defined(__APPLE__)
static inline void sincos(double x, double *s, double *c){
  __sincos(x,s,c);
}
static inline void sincosf(float x, float *s, float *c){
  __sincosf(x,s,c);
}
static inline float exp10f(float x){
  return __exp10f(x);
}
static inline double exp10(double x){
  return __exp10(x);
}
static inline double sinpi(double x){
  return __sinpi(x);
}
static inline float sinpif(float x){
  return __sinpif(x);
}
static inline double cospi(double x){
  return __cospi(x);
}
static inline float cospif(float x){
  return __cospif(x);
}
static inline double tanpi(double x){
  return __tanpi(x);
}
static inline float tanpif(float x){
  return __tanpif(x);
}
#else
// neither APPLE nor modern glibm - final fallback
static inline double sinpi(double x){
  return sin(M_PI * x);
}
static inline float sinpif(float x){
  return sinf(M_PIf * x);
}
static inline double cospi(double x){
  return cos(M_PI * x);
}
static inline float cospif(float x){
  return cosf(M_PIf * x);
}
static inline double tanpi(double x){
  return tan(M_PI * x);
}
static inline float tanpif(float x){
  return tanf(M_PIf * x);
}
#endif // defined(__APPLE__)
// Apple has sinpi/cospi/tanpi but not inverse functions divided by pi (gnu libm does)
static inline double asinpi(double x){
  return asin(x) * M_1_PI;
}
static inline double acospi(double x){
  return acos(x) * M_1_PI;
}
static inline double atanpi(double x){
  return atan(x) * M_1_PI;
}
static inline double atan2pi(double y, double x){
  return atan2(y,x) * M_1_PI;
}
static inline float asinpif(float x){
  return asinf(x) * M_1_PIf;
}
static inline float acospif(float x){
  return acosf(x) * M_1_PIf;
}
static inline float atanpif(float x){
  return atanf(x) * M_1_PIf;
}
static inline float atan2pif(float y, float x){
  return atan2f(y,x) * M_1_PIf;
}
#endif // #if !defined(HAVE_MODERN_GLIBC)

// Nobody defines these, apparently - return complex phasor angle in range -1, +1
static inline double cargpi(double complex x){
  return atan2pi(cimag(x), creal(x));
}
static inline float cargpif(float complex x){
  return atan2pif(cimagf(x), crealf(x));
}

// Complex exponentials
// Does anyone implement these natively for Linux?
// (I just did - KA9Q Jan 2026 -- see sincospi.c and sincospif.c)
// It's a big win in DSP to keep phases as rotations (or half rotations),
// rather than radians, to make phase wrap reduction really easy and accurate
void sincospi(double x, double *s, double *c);
void sincospif(float x, float *s, float *c);

static inline float complex csincosf(float const x){
  float s,c;
  sincosf(x,&s,&c);
  return CMPLXF(c,s);
}
static inline float complex csincospif(float const x){
  float s,c;
  sincospif(x,&s,&c);
  return CMPLXF(c,s);
}
// return unit magnitude complex number with given phase x
static inline double complex csincos(double const x){
  double s,c;
  sincos(x,&s,&c);
  return CMPLX(c,s);
}
static inline double complex csincospi(double const x){
  double s,c;
  sincospi(x,&s,&c);
  return CMPLX(c,s);
}
// How many names can people dream up for the same operation?
// (I know, I'm an EE so I should say 'j', not 'i')
// exp(i*x) = Cos(x) + i*sin(x)
// "cosine plus i sine" -- heard a lot in grad school
static inline float complex cisf(float x){
  return csincosf(x);
}
static inline float complex cispif(float x){
  return csincospif(x);
}
static inline double cis(double x){
  return csincos(x);
}
static inline double cispi(double x){
  return csincospi(x);
}

// power2dB and voltage2dB pass NAN to log10(), for debug trapping
// They do avoid the divide-by-zero exception for the common case of 0 -> -infinity dB
static inline float dB2power(float x){
  return exp10f(0.1f * x);
}
static inline float power2dB(float x){
  if(x <= 0.0f)
    return -INFINITY;
  return 10.0f * log10f(x);
}
static inline float dB2voltage(float x){
  return exp10f(0.05f * x);
}
static inline float voltage2dB(float x){
  if(x <= 0.0f)
    return -INFINITY;
  return 20.0f * log10f(x);
}
// Modified Bessel functions
double i0(double const z); // 0th kind
double i1(double const z); // 1st kind
float i0f(float const z); // 0th kind
float i1f(float const z); // 1st kind

static inline double sinc(double x){
  if(fabs(x) < 5.8e-9)
     return 1.0;
  if(fabs(x) < 1e-5)
    return 1.0 - (M_PI * M_PI / 6.0) * x * x;
  return sinpi(x) / (M_PI * x);
}
static inline float sincf(float x){
  if(fabsf(x) < 1.35e-4f)
    return 1.0f;
  if(fabsf(x) < 1e-3f)
    return 1.0f - 1.64493406685f * x * x; // = 1 - pi^2/6 * x^2; good approximation for 32-bit floats
  return sinpif(x) / (M_PIf * x);
}
#endif
