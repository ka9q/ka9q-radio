#define HUGEPAGE 1

// Miscellaneous low-level routines for ka9q-radio
// Copyright 2018, Phil Karn, KA9Q

#include <math.h>
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>
#include <ctype.h>
#include <libgen.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <pthread.h>
#include <sys/mman.h>
#if defined(__linux__)
#include <linux/memfd.h>
#endif
#include <errno.h>
#include <locale.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <sys/timex.h>
#include <stdbool.h>
#if (defined(__i386__) || defined(__x86_64__)) && (defined(__SSE__) || defined(__AVX__))
#include <immintrin.h>   // _mm_getcsr, _mm_setcsr
#endif

#ifndef NULL
#define NULL ((void *)0)
#endif

#include "misc.h"
#include "config_paths.h"

#ifndef PKGDATADIR
#define PKGDATADIR "/usr/local/share/ka9q-radio"
#endif

#ifndef CONFDIR
#define CONFDIR "/etc/radio"
#endif

char const *Pkgdatadir = PKGDATADIR;
char const *Confdir = CONFDIR;


// Return path to file which is part of the application distribution.
// This allows to run the program either from build directory or from
// installation directory.
int dist_path(char *path,int path_len,const char *fname){
  if(path == NULL)
    return -1;
  struct stat st = {0};

  if(fname[0] == '/') {
    strlcpy(path, fname, path_len);
    return 0;
  }
  // first look in /etc/radio for local override
  snprintf(path,path_len,"%s/%s",Confdir,fname);
  if(stat(path, &st) == 0 && (st.st_mode & S_IFMT) == S_IFREG)
    return 0;

  // Then look in packaged files /usr[/local]/share/ka9q-radio
  snprintf(path,path_len,"%s/%s",Pkgdatadir,fname);
  if(stat(path, &st) == 0 && (st.st_mode & S_IFMT) == S_IFREG)
    return 0;

  return -1;
}

#if 0
// Fill buffer from pipe
// Needed because reads from a pipe can be partial
ssize_t pipefill(int const fd,void *buffer,size_t const cnt){
  size_t i;
  uint8_t *bp = buffer;
  for(i=0;i<cnt;){
    ssize_t n = read(fd,bp+i,cnt-i);
    if(n < 0)
      return n;
    if(n == 0)
      break;
    i += n;
  }
  return i;

}
#endif

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


// Modified Bessel function of the 0th kind
double i0(double const z){
  double const t = 0.25 * z * z;
  double sum = 1 + t;
  double term = t;
  for(int k=2; k<40; k++){
    term *= t/(k * k);
    sum += term;
    if(term < 1e-12 * sum)
      break;
  }
  return sum;
}

// Modified Bessel function of first kind
double i1(double const z){
  double const t = 0.25 * z * z;
  double term = 0.5 * t;
  double sum = 1 + term;
  for(int k=2; k<40; k++){
    term *= t / (k * (k+1));
    sum += term;
    if(term < 1e-12 * sum)
      break;
  }
  return 0.5 * z * sum;
}
// Modified Bessel function of the 0th kind
float i0f(float const z){
  float const t = 0.25f * z * z;
  float sum = 1 + t;
  float term = t;
  for(int k=2; k<40; k++){
    term *= t/(k * k);
    sum += term;
    if(term < 1e-12f * sum)
      break;
  }
  return sum;
}

// Modified Bessel function of first kind
float i1f(float const z){
  float const t = 0.25f * z * z;
  float term = 0.5f * t;
  float sum = 1 + term;
  for(int k=2; k<40; k++){
    term *= t / (k * (k+1));
    sum += term;
    if(term < 1e-12f * sum)
      break;
  }
  return 0.5 * z * sum;
}


// Fast approximate square root, for signal magnitudes
// https://dspguru.com/dsp/tricks/magnitude-estimator/
float approx_magf(float complex x){
  static float const Alpha = 0.947543636291f;
  static float const Beta =  0.392485425092f;
  float absr = fabsf(__real__ x);
  float absi = fabsf(__imag__ x);
  return Alpha * max(absr,absi) + Beta * min(absr,absi);
}



