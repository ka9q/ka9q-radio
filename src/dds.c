#include <stdio.h>
#include <complex.h>
#include <math.h>
#include <stdbool.h>
#include "misc.h"
#include "nco.h"

int Verbose = 0;

int main(){
  uint64_t phase_step = (UINT64_MAX / 131071) | UINT64_C(1);
  double avg_err = 0;
  double avg_err2 = 0;
  uint64_t phase = 0;
  double max_err = 0;
  uint64_t max_err_phase = 0;

  int const N = 2000000;

  for(int i=0; i < N; i++){
    double s,c,x;
    x = (double)phase * 0x1p-63; // 2 * pi * f
    sincospi(x,&s,&c);
    float complex t = nco(phase);
    double err2 = (cimagf(t) - s) * (cimagf(t)-s) + (crealf(t) - c) * (crealf(t) - c);
    double err = sqrt(err2);
    avg_err2 += err2;
    avg_err += err;
    //    printf("%llu %f %f %f %f %f %lg\n",phase, M_PIf * ldexpf((float)phase,-64), s, c, st, ct, err);
    if(err > max_err){
      max_err = err;
      max_err_phase = phase;
    }
    phase += phase_step;
  }
  avg_err /= N;
  avg_err2 = sqrt(avg_err2 / N);
  printf("tab bits %d avg err %lg RMS %lg\n", TAB_BITS, avg_err, avg_err2);
  printf("max err %lg at phase %llu (%f rad)\n", max_err, max_err_phase, M_PIf * (float)max_err_phase * 0x1p-63);
}
