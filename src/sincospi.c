#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>

static inline double mod2(double x) {
    // reduce x into [0, 2)
    x -= floor(x * 0.5) * 2.0;
    if (x < 0) x += 2.0;
    if(x >= 2.0)
      x -= 2.0;
    return x;
}

#define PI_D (3.141592653589793238462643383279502884)

__attribute__((weak))
void sincospi(double x, double *s, double *c)
{
    if (!isfinite(x)) { *s = *c = NAN; return; }

    // sin(pi*x), cos(pi*x) have period 2 in x
    double y = mod2(x);        // [0,2)

    // Quadrant of pi*x in steps of pi/2 => x steps of 0.5
    // q = floor(2y) gives 0..3 for y in [0,2)
    int q = (int)(2.0 * y);    // 0,1,2,3

    // Reduce to r in [0,0.5)
    double r = y - 0.5 * q;    // [0,0.5)

    // Further reduce to [0,0.25] using symmetry so trig sees <= pi/4
    double z = r;
    bool flip = false;
    if (z > 0.25) { z = 0.5 - z; flip = true; }  // now z in [0,0.25]

    // Now angle = pi*z in [0, pi/4]
    // sincos() isn't on macos, but it turns out that the win is in sharing argument reduction.
    // It still computes both sin() and cos(), but here our angles are already reduced
    double piz = PI_D * z;
    double ss = sin(piz);
    double cc = cos(piz);

    // Undo the [0,0.25] symmetry (sin(pi*(0.5 - z)) = cos(pi*z), etc.)
    if (flip) {
        double tmp = ss;
        ss = cc;
        cc = tmp;
    }

    // Reconstruct according to quadrant q of (pi*y)
    // q=0: 0..pi/2, q=1: pi/2..pi, q=2: pi..3pi/2, q=3: 3pi/2..2pi
    switch (q) {
    case 0: *s =  ss; *c =  cc; break;
    case 1: *s =  cc; *c = -ss; break;
    case 2: *s = -ss; *c = -cc; break;
    default:*s = -cc; *c =  ss; break;
    }
}
#if 0
int main(){
  for(double x = -2; x <= +2.0; x += 0.01){
    double s,c,ss,cc;

    s = sin(M_PI * x);
    c = cos(M_PI * x);

    sincospi(x, &ss,&cc);
    printf("%.16lf (%.16lf,%.16lf) (%.16lf,%.16lf)\n",
	   x,s,c,ss,cc);
    
  }

}
#endif
