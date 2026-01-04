#include <math.h>
#include <stdbool.h>

// Single precision version
static inline double mod2f(double x) {
    // reduce x into [0, 2)
    x -= floorf(x * 0.5f) * 2.0f;
    if (x < 0.0f) x += 2.0f;
    if(x >= 2.0f)
      x -= 2.0f;
    return x;
}

#define PI_F (3.14159265358979323846f)

__attribute__((weak))
void sincospif(float x, float *s, float *c)
{
    if (!isfinite(x)) { *s = *c = NAN; return; }

    // sin(pi*x), cos(pi*x) have period 2 in x
    float y = mod2f(x);        // [0,2)

    // Quadrant of pi*x in steps of pi/2 => x steps of 0.5
    // q = floor(2y) gives 0..3 for y in [0,2)
    int q = (int)(2.0f * y);    // 0,1,2,3

    // Reduce to r in [0,0.5)
    float r = y - 0.5f * q;    // [0,0.5)

    // Further reduce to [0,0.25] using symmetry so trig sees <= pi/4
    float z = r;
    bool flip = false;
    if (z > 0.25f) { z = 0.5f - z; flip = true; }  // now z in [0,0.25]

    // Now angle = pi*z in [0, pi/4]
    float piz = PI_F * z;
    float ss = sinf(piz);
    float cc = cosf(piz);

    // Undo the [0,0.25] symmetry (sin(pi*(0.5 - z)) = cos(pi*z), etc.)
    if (flip) {
        float tmp = ss;
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
