#ifndef _WINDOW_H
#define _WINDOW_H 1

#include <stddef.h>
#include <stdbool.h>

// Window functions
int make_kaiser(double * const window,int const M,double const beta);
int make_kaiserf(float * const window,int const M,float const beta);
int normalize_windowf(float * const window, int const M);
float gaussian_window(int n, int M, float s);
int gaussian_window_alpha(float *w, size_t N, float alpha, bool normalize_peak);
float exact_blackman_window(int n, int N);
float blackman_window(int n, int N);
float blackman_harris_window(int n, int N);
float hann_window(int n,int N);
float hamming_window(int n,int N);
float hp5ft_window(int n, int N);

enum window_type {
  INVALID_WINDOW = -1,
  KAISER_WINDOW,
  RECT_WINDOW, // essentially kaiser with beta = 0
  BLACKMAN_WINDOW,
  EXACT_BLACKMAN_WINDOW,
  GAUSSIAN_WINDOW,
  HANN_WINDOW,
  HAMMING_WINDOW,
  BLACKMAN_HARRIS_WINDOW,
  HP5FT_WINDOW,
  N_WINDOW,
};

#endif
