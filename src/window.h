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
double hft95_window(int n, int N);

