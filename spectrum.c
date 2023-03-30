// $Id$
// Spectral analysis service - far from complete!
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

// Spectrum analysis thread
void *demod_spectrum(void *arg){
  assert(arg != NULL);
  struct demod * const demod = arg;  
  
  {
    char name[100];
    snprintf(name,sizeof(name),"spect %u",demod->output.rtp.ssrc); // SSRC is dummy for ID, there's no RTP stream
    pthread_setname(name);
  }

  if(demod->filter.out)
    delete_filter_output(&demod->filter.out);
  int const blocksize = demod->output.samprate * Blocktime / 1000.0F;
  // Special filter without a response curve or IFFT
  demod->filter.out = create_filter_output(Frontend.in,NULL,blocksize,SPECTRUM);
  if(demod->filter.out == NULL){
    fprintf(stdout,"unable to create filter for ssrc %lu\n",(unsigned long)demod->output.rtp.ssrc);
    goto quit;
  }
  if(demod->spectrum.bin_data == NULL)
    demod->spectrum.bin_data = calloc(demod->spectrum.bin_count,sizeof(*demod->spectrum.bin_data));

  while(!demod->terminate){
    // Although we don't use filter_output, demod->filter.min_IF and max_IF still need to be set
    // so radio.c:set_freq() will set the front end tuner properly

    // Determine bin for lowest requested frequency
    float const blockrate = 1000.0f / Blocktime; // Width in Hz of frequency bins (> FFT bin spacing)
    int const N = Frontend.L + Frontend.M - 1;
    float const spacing = (1 - (float)(Frontend.M-1)/N) * blockrate; // Hz between FFT bins (< FFT bin width)

    int binsperbin = demod->spectrum.bin_bw / spacing; // FFT bins in each noncoherent integration bin
    if(binsperbin < 1)
      binsperbin = 1; // enforce reasonableness

    if(downconvert(demod) == -1)
      break; // received terminate

    // https://en.wikipedia.org/wiki/Exponential_smoothing#Time constant
    // smooth = 1 - exp(-blocktime/tc)
    // expm1(x) = exp(x) - 1 (to preserve precision)
     float const smooth = -expm1f(-Blocktime / (1000 * demod->spectrum.integrate_tc)); // Blocktime is in millisec!
    for(int i=0; i < demod->spectrum.bin_count; i++){ // For each noncoherent integration bin
      float p = 0;
      for(int j=0; j < binsperbin; j++) // Add energy of each fft bin part of this integration bin
	p += cnrmf(demod->filter.out->fdomain[i * binsperbin + j]);

      // Exponential smoothing
      demod->spectrum.bin_data[i] += smooth * (p - demod->spectrum.bin_data[i]);
    }
  }
 quit:;
  if(demod->filter.energies)
    free(demod->filter.energies);
  demod->filter.energies = NULL;
  delete_filter_output(&demod->filter.out);
  return NULL;
}
// Unique to spectrum energies, seems like a kludge, may be replaced
int encode_vector(uint8_t **buf,enum status_type type,float *array){
  uint8_t *cp = *buf;
  *cp++ = type;
  int length = 256; // 64 4-byte floats
  *cp++ = length;

  for(int i=0; i < 64; i++){ // For each float
    union {
      uint32_t u;
      float f;
    } d;
    d.f = array[i];
    for(int j=0; j < 4; j++){ // for each byte in float, MSB first
      *cp++ = (d.u >> 24);
      d.u <<= 24;
    }
  }
  return length + 2;
}
