#ifndef _WINDOW_H
#define _WINDOW_H 1

// Window functions
int make_kaiser(double * const window,int const M,double const beta);
int make_kaiserf(float * const window,int const M,double const beta);
int normalize_windowf(float * const window, int const M);
double gaussian_window(int n, int M, double s);
int gaussian_window_alpha(float *w, size_t N, double alpha, bool normalize_peak);
double exact_blackman_window(int n, int N);
double blackman_window(int n, int N);
double blackman_harris_window(int n, int N);
double hann_window(int n,int N);
double hamming_window(int n,int N);
double hp5ft_window(int n, int N);

enum window_type {
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
