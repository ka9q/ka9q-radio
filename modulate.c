// $Id: modulate.c,v 1.28 2022/06/27 03:24:55 karn Exp $
// Simple I/Q AM modulator - will eventually support other modes
// Copyright 2017, Phil Karn, KA9Q
#define _GNU_SOURCE 1
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <complex.h>
#include <math.h>
#include <limits.h>
#include <fftw3.h>
#include <string.h>
#include <pthread.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <errno.h>

#include "conf.h"
#include "misc.h"
#include "filter.h"
#include "radio.h"

#define BLOCKSIZE 4096

float const scale = 1./SHRT_MAX;

int Samprate = 192000;

const char *App_path;
int Verbose = 0;

int main(int argc,char *argv[]){
  App_path = argv[0];
  // Set defaults
  double frequency = 48000;
  double amplitude = -20;
  double sweep = 0;

  char *modtype = "am";
  int c;
  while((c = getopt(argc,argv,"f:a:s:r:vm:W:")) != EOF){
    switch(c){
    case 'v':
      Verbose++;
      break;
    case 'r':
      Samprate = strtol(optarg,NULL,0);
      break;
    case 'f':
      frequency = strtod(optarg,NULL);
      break;
    case 'a':
      amplitude = strtod(optarg,NULL);
      break;
    case 's':
      sweep = strtod(optarg,NULL); // sweep rate, Hz/sec
      break;
    case 'm':
      modtype = optarg;
      break;
    case 'W':
      Wisdom_file = optarg;
      break;
    }
  }
  float low;
  float high;
  float carrier;

  if(strcasecmp(modtype,"am") == 0){
    carrier = 1;
    high = +5000;
    low = -5000;
  } else if(strcasecmp(modtype,"usb") == 0){
    carrier = 0;
    high = +3000;
    low = 0;
  } else if(strcasecmp(modtype,"lsb") == 0){
    carrier = 0;
    high = 0;
    low = -3000;
  } else if(strcasecmp(modtype,"ame") == 0){
    // AM enhanced: upper sideband + carrier (as in CHU)
    carrier = 1;
    high = +3000;
    low = 0;
  } else if(strcasecmp(modtype,"dsb") == 0){
    // Double sideband AM, no carrier
    carrier = 0;
    high = +5000;
    low = -5000;
  } else {
    fprintf(stderr,"Unknown modulation %s\n",modtype);
    exit(1);
  }
  if(Verbose){
    fprintf(stderr,"%s modulation on %.1f Hz IF, swept %.1f Hz/s, amplitude %5.1f dBFS, filter blocksize %'d, input/output sample rate %d\n",
	    modtype,frequency,sweep,amplitude,BLOCKSIZE,Samprate);
  }
  if(frequency < -low && frequency > -high){
    fprintf(stderr,"Warning: low carrier frequency may interfere with receiver DC suppression\n");
  }

  frequency *= 1./Samprate;       // cycles/sample
  amplitude = pow(10.,amplitude/20.); // Convert to amplitude ratio
  sweep *= 1. / ((double)Samprate*Samprate);  // cycles/sample

  struct osc osc;
  memset(&osc,0,sizeof(osc));
  set_osc(&osc,frequency,sweep);
  int const L = BLOCKSIZE;
  int const M = BLOCKSIZE + 1;
  int const N = L + M - 1;

  complex float * const response = fftwf_alloc_complex(N);
  memset(response,0,N*sizeof(response[0]));
  {
    float gain = 1./N; // Compensate for FFT/IFFT scaling
    for(int i=0;i<N;i++){
      float f;
      f = Samprate * ((float)i/N);
      if(f > Samprate/2)
	f -= Samprate;
      if(f >= low && f <= high)
	response[i] = gain;
      else
	response[i] = 0;
    }
  }
  window_filter(L,M,response,3.0);
  struct filter_in * filter_in = create_filter_input(L,M,REAL);
  struct filter_out * filter_out = create_filter_output(filter_in,response,L,COMPLEX);
  

  while(1){
    int16_t samp[L];
    int r = pipefill(0,samp,sizeof(samp));
    if(r <= 0){
      if(Verbose)
	fprintf(stderr,"modulate: pipefill returns %d errno %d\n",r,errno);
      break;
    }
    // Assume input and output sample rates are same
    for(int j=0;j<L;j++){
      if(put_rfilter(filter_in,samp[j++] * scale) == 0)
	continue;

      // Form baseband signal (analytic for SSB, pure real for AM/DSB)
      execute_filter_output(filter_out,0);
      
      int16_t output[2*L];
      for(int i=0;i<L;i++){
	complex float const c = carrier + step_osc(&osc) * amplitude * filter_out->output.c[i];
	output[2*i] = scaleclip(crealf(c));
	output[2*i+1] = scaleclip(cimagf(c));
      }
      int wlen = write(1,output,sizeof(output));
      if(wlen != sizeof(output)){
	perror("write");
	break;
      }
    }
  }
  delete_filter_input(&filter_in);
  delete_filter_output(&filter_out);
  exit(0);
}
