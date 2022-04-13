// $Id: misc.c,v 1.36 2022/02/14 21:00:40 karn Exp $
// Miscellaneous low-level routines, mostly time-related
// Copyright 2018, Phil Karn, KA9Q

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include <math.h>
#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>
#include <ctype.h>

#ifndef NULL
#define NULL ((void *)0)
#endif

#include "misc.h"


// Fill buffer from pipe
// Needed because reads from a pipe can be partial
int pipefill(const int fd,void *buffer,const int cnt){
  int i;
  unsigned char *bp = buffer;
  for(i=0;i<cnt;){
    int n = read(fd,bp+i,cnt-i);
    if(n < 0)
      return n;
    if(n == 0)
      break;
    i += n;
  }
  return i;

}

// Remove return or newline, if any, from end of string
void chomp(char *s){

  if(s == NULL)
    return;
  char *cp;
  if((cp = strchr(s,'\r')) != NULL)
    *cp = '\0';
  if((cp = strchr(s,'\n')) != NULL)
    *cp = '\0';
}


char *Days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
char *Months[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec" };

// Format a timestamp expressed as nanoseconds from the GPS epoch
// Not thread safe
char *lltime(long long t){
  struct tm tm;
  time_t utime;
  int t_usec;
  static char result[100];

  utime = (t / 1000000000LL) - GPS_UTC_OFFSET + UNIX_EPOCH;
  t_usec = (t % 1000000000LL) / 1000;
  if(t_usec < 0){
    t_usec += 1000000;
    utime -= 1;
  }


  gmtime_r(&utime,&tm);
  // Mon Feb 26 14:40:08.123456 UTC 2018
  snprintf(result,sizeof(result),"%s %s %d %02d:%02d:%02d.%06d UTC %4d",
	   Days[tm.tm_wday],Months[tm.tm_mon],tm.tm_mday,tm.tm_hour,tm.tm_min,tm.tm_sec,t_usec,tm.tm_year+1900);
  return result;

}
// Format a seconds count into hh:mm:ss
char *ftime(char * result,int size,long long t){

  // Init to blanks
  memset(result,0,size);
  char *cp = result;
  // Emit sign
  if(t < 0){
    *cp = '-';
    t = -t; // absolute value
  } else
    *cp = ' ';
  
  cp++;
  size--;

  long long hr = t / 3600; // Hours is potentially unlimited
  t -= 3600 * hr;

  // Show hours and the hour:minute colon only if hr > 0
  int r;
  if(hr > 0)
    r = snprintf(cp,size,"%3lld:",hr);
  else
    r = snprintf(cp,size,"    ");
    
  assert(r == 4);
  if(r < 0)
    return NULL;
  cp += r;
  size -= r;

  int mn = t / 60; // minutes is limited to 0-59
  t -= mn * 60;
  assert(mn < 60);
  assert(t < 60);

  r = 3;
  if(hr > 0)
    // hours field is present, show minutes with leading zero
    r = snprintf(cp,size,"%02d:",mn);
  else if(mn > 0)
    // Hours zero, show minute without leading 0
    r = snprintf(cp,size,"%2d:",mn);
  else
    r = snprintf(cp,size,"   ");    
  
  assert(r == 3);
  if(r < 0)
    return NULL;
  cp += r;
  size -= r;
      

  if(hr > 0 || mn > 0)
  // Hours or minutes are nonzero, show seconds with leading 0
    r = snprintf(cp,size,"%02lld",t);
  else if(t > 0)
    r = snprintf(cp,size,"%2lld",t);
  else
    r = snprintf(cp,size,"  "); // All zero, emit all blanks

  assert(r == 2);
  if(r < 0)
    return NULL;

  return result;
}


// Parse a frequency entry in the form
// 12345 (12345 Hz)
// 12k345 (12.345 kHz)
// 12m345 (12.345 MHz)
// 12g345 (12.345 GHz)
// If no g/m/k and number is too small, make a heuristic guess
// NB! This assumes radio covers 100 kHz - 2 GHz; should make more general
double const parse_frequency(const char *s){
  char * const ss = alloca(strlen(s));

  int i;
  for(i=0;i<strlen(s);i++)
    ss[i] = tolower(s[i]);

  ss[i] = '\0';
  
  // k, m or g in place of decimal point indicates scaling by 1k, 1M or 1G
  char *sp;
  double mult;
  if((sp = strchr(ss,'g')) != NULL){
    mult = 1e9;
    *sp = '.';
  } else if((sp = strchr(ss,'m')) != NULL){
    mult = 1e6;
    *sp = '.';
  } else if((sp = strchr(ss,'k')) != NULL){
    mult = 1e3;
    *sp = '.';
  } else
    mult = 1;

  char *endptr = NULL;
  double f = strtod(ss,&endptr);
  if(endptr == ss || f == 0)
    return 0; // Empty entry, or nothing decipherable
  
  if(mult != 1 || f >= 1e5) // If multiplier given, or frequency >= 100 kHz (lower limit), return as-is
    return f * mult;
    
  // If frequency would be out of range, guess kHz or MHz
  if(f < 100)
    f *= 1e6;              // 0.1 - 99.999 Only MHz can be valid
  else if(f < 500)         // Could be kHz or MHz, arbitrarily assume MHz
    f *= 1e6;
  else if(f < 2000)        // Could be kHz or MHz, arbitarily assume kHz
    f *= 1e3;
  else if(f < 100000)      // Can only be kHz
    f *= 1e3;

  return f;
}

// Return smallest integer greater than N with no factors > 7
// Useful for determining efficient FFT sizes
uint32_t nextfastfft(uint32_t n){

  // Do all internal arithmetic in 64 bits to avoid wraparound
  uint64_t result = 4288306050; // == 2 * 3^6 * 5^2 * 7^6, largest integer < 2^32 with small factors (biggest possible 32-bit result)
  if(n >= result)
    return 0; // Error
  for(uint64_t f7=1; f7 < result; f7 *= 7){
    for(uint64_t f5=f7; f5 < result; f5 *= 5){
      for(uint64_t f3=f5; f3 < result; f3 *= 3){
	for(uint64_t f2=f3; f2 < result; f2 *= 2){
	  if(f2 > n){
	    result = f2;
	    break;
	  }
	}
      }
    }
  }
  return result;
}

// The amplitude of a noisy FM signal has a Rice distribution
// Given the ratio 'r' of the mean and standard deviation measurements, find the
// ratio 'theta' of the Ricean parameters 'nu' and 'sigma', the true
// signal and noise amplitudes

// Pure noise is Rayleigh, which has mean/stddev = sqrt(pi/(4-pi)) or meansq/variance = pi/(4-pi) = 5.63 dB

// See Wikipedia article on "Rice Distribution"

// Modified Bessel function of the 0th kind
float i0(float const z){
  const float t = 0.25 * z * z;
  float sum = 1 + t;
  float term = t;
  for(int k=2; k<40; k++){
    term *= t/(k * k);
    sum += term;
    if(term < 1e-12 * sum)
      break;
  }
  return sum;
}

// Modified Bessel function of first kind
float i1(float const z){
  const float t = 0.25 * z * z;
  float term = 0.5 * t;
  float sum = 1 + term;

  for(int k=2; k<40; k++){
    term *= t / (k * (k+1));
    sum += term;
    if(term < 1e-12 * sum)
      break;
  }
  return 0.5 * z * sum;
}
float xi(float thetasq){

  float t = (2 + thetasq) * i0(0.25 * thetasq) + thetasq * i1(0.25 * thetasq);
  t *= t;
  return 2 + thetasq - (0.125 * M_PI) * expf(-0.5 * thetasq) * t;
}


// Given apparent signal-to-noise power ratio, return corrected value
float fm_snr(float r){

  if(r <= M_PI / (4 - M_PI)) // shouldn't be this low even on pure noise
    return 0;

  if(r > 100) // 20 dB
    return r; // Formula blows up for large SNR, and correction is tiny anyway

  float thetasq = r;
  for(int i=0;i < 10; i++){
    float othetasq = thetasq;
    thetasq = xi(thetasq) * (1+r) - 2;
    if(fabsf(thetasq - othetasq) <= 0.01)
      break; // converged
  }
  return thetasq;
}

// Simple non-crypto hash function
// Adapted from https://en.wikipedia.org/wiki/PJW_hash_function
uint32_t ElfHash(const unsigned char *s,int length){
    uint32_t h = 0;
    while(length-- > 0){
        h = (h << 4) + *s++;
	uint32_t high;
        if ((high = h & 0xF0000000) != 0){
            h ^= high >> 24;
	    h &= ~high;
	}
    }
    return h;
}
uint32_t ElfHashString(const char *s){
  return ElfHash((unsigned char *)s,strlen(s));
}



#if __APPLE__

// OSX doesn't have pthread_barrier_*
// Taken from https://stackoverflow.com/questions/3640853/performance-test-sem-t-v-s-dispatch-semaphore-t-and-pthread-once-t-v-s-dispat
// Apparently by David Cairns

#include <errno.h>

int pthread_barrier_init(pthread_barrier_t *barrier, const pthread_barrierattr_t *attr, unsigned int count)
{
    if(count == 0)
    {
        errno = EINVAL;
        return -1;
    }
    if(pthread_mutex_init(&barrier->mutex, 0) < 0)
    {
        return -1;
    }
    if(pthread_cond_init(&barrier->cond, 0) < 0)
    {
        pthread_mutex_destroy(&barrier->mutex);
        return -1;
    }
    barrier->tripCount = count;
    barrier->count = 0;

    return 0;
}

int pthread_barrier_destroy(pthread_barrier_t *barrier)
{
    pthread_cond_destroy(&barrier->cond);
    pthread_mutex_destroy(&barrier->mutex);
    return 0;
}

int pthread_barrier_wait(pthread_barrier_t *barrier)
{
    pthread_mutex_lock(&barrier->mutex);
    ++(barrier->count);
    if(barrier->count >= barrier->tripCount)
    {
        barrier->count = 0;
        pthread_cond_broadcast(&barrier->cond);
        pthread_mutex_unlock(&barrier->mutex);
        return 1;
    }
    else
    {
        pthread_cond_wait(&barrier->cond, &(barrier->mutex));
        pthread_mutex_unlock(&barrier->mutex);
        return 0;
    }
}

#endif // __APPLE__

