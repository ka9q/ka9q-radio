// $Id$
// Spectral analysis service
// Copyright 2023, Phil Karn, KA9Q
#define _GNU_SOURCE 1
#include <assert.h>
#include <pthread.h>
#include <string.h>
#include <math.h>
#include <complex.h>
#include <fftw3.h>

#include "misc.h"
#include "iir.h"
#include "filter.h"
#include "radio.h"

// FM demodulator thread
void *demod_spectrum(void *arg){
  assert(arg != NULL);
  struct demod * const demod = arg;  
  
  {
    char name[100];
    snprintf(name,sizeof(name),"spect %u",demod->output.rtp.ssrc);
    pthread_setname(name);
  }

  if(demod->filter.out)
    delete_filter_output(&demod->filter.out); // No filter needed

  return NULL; // Incomplete

}
