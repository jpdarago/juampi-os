// Freestanding <math.h> for the vendored Lua, over SSE (sqrt) and the x87 FPU
// (transcendentals). Accuracy is adequate for a shell; the x87 sin/cos/tan do
// no argument range reduction, so very large inputs lose precision.

#include <stdint.h>

#define NAN (__builtin_nan(""))

double fabs(double x)
{
    union {
        double d;
        uint64_t u;
    } v = {x};
    v.u &= 0x7FFFFFFFFFFFFFFFull;
    return v.d;
}

double trunc(double x)
{
    if (fabs(x) >= 4503599627370496.0) { // >= 2^52: already integral
        return x;
    }
    long long i = (long long)x; // truncates toward zero
    return (double)i;
}

double floor(double x)
{
    double t = trunc(x);
    return (t > x) ? t - 1.0 : t;
}

double ceil(double x)
{
    double t = trunc(x);
    return (t < x) ? t + 1.0 : t;
}

double sqrt(double x)
{
    double r;
    __asm__("sqrtsd %1, %0" : "=x"(r) : "x"(x));
    return r;
}

double fmod(double x, double y)
{
    double r;
    __asm__("1: fprem\n\t"
            "fnstsw %%ax\n\t"
            "testb $4, %%ah\n\t"
            "jnz 1b"
            : "=t"(r)
            : "0"(x), "u"(y)
            : "ax", "cc");
    return r;
}

double sin(double x)
{
    double r;
    __asm__("fsin" : "=t"(r) : "0"(x));
    return r;
}

double cos(double x)
{
    double r;
    __asm__("fcos" : "=t"(r) : "0"(x));
    return r;
}

double tan(double x)
{
    double r;
    __asm__("fptan\n\t"
            "fstp %%st(0)"
            : "=t"(r)
            : "0"(x));
    return r;
}

// fpatan computes atan2(st1, st0), popping both.
double atan2(double y, double x)
{
    double r;
    __asm__("fpatan" : "=t"(r) : "0"(x), "u"(y) : "st(1)");
    return r;
}

double atan(double x) { return atan2(x, 1.0); }
double asin(double x) { return atan2(x, sqrt(1.0 - x * x)); }
double acos(double x) { return atan2(sqrt(1.0 - x * x), x); }

double exp(double x)
{
    double r;
    __asm__("fldl2e\n\t"                  // st0=log2(e), st1=x
            "fmulp\n\t"                   // st0 = x*log2(e)
            "fld %%st(0)\n\t"             // dup
            "frndint\n\t"                 // st0=int part
            "fsub %%st(0), %%st(1)\n\t"   // st1 = frac
            "fxch\n\t"                    // st0=frac, st1=int
            "f2xm1\n\t"                   // 2^frac - 1
            "fld1\n\t"
            "faddp\n\t"                   // 2^frac
            "fscale\n\t"                  // 2^frac * 2^int
            "fstp %%st(1)"                // drop the int, result in st0
            : "=t"(r)
            : "0"(x)
            : "st(1)");
    return r;
}

double log(double x)
{
    double r;
    __asm__("fldln2\n\t" // st0=ln2, st1=x
            "fxch\n\t"   // st0=x, st1=ln2
            "fyl2x"      // ln2 * log2(x) = ln(x)
            : "=t"(r)
            : "0"(x)
            : "st(1)");
    return r;
}

double log2(double x)
{
    double r;
    __asm__("fld1\n\t" // st0=1, st1=x
            "fxch\n\t"  // st0=x, st1=1
            "fyl2x"     // 1 * log2(x)
            : "=t"(r)
            : "0"(x)
            : "st(1)");
    return r;
}

double log10(double x)
{
    double r;
    __asm__("fldlg2\n\t" // st0=log10(2), st1=x
            "fxch\n\t"   // st0=x, st1=log10(2)
            "fyl2x"      // log10(2) * log2(x) = log10(x)
            : "=t"(r)
            : "0"(x)
            : "st(1)");
    return r;
}

double pow(double x, double y)
{
    if (y == 0.0 || x == 1.0) {
        return 1.0;
    }
    int neg = 0;
    if (x < 0.0) {
        double yi = floor(y);
        if (yi != y) {
            return NAN; // negative base, non-integer exponent
        }
        if (fmod(yi, 2.0) != 0.0) {
            neg = 1;
        }
        x = -x;
    }
    if (x == 0.0) {
        return 0.0;
    }
    double r;
    __asm__("fyl2x\n\t" // st0 = y*log2(x)  (inputs st0=x, st1=y; pops)
            "fld %%st(0)\n\t"
            "frndint\n\t"
            "fsub %%st(0), %%st(1)\n\t"
            "fxch\n\t"
            "f2xm1\n\t"
            "fld1\n\t"
            "faddp\n\t"
            "fscale\n\t"
            "fstp %%st(1)"
            : "=t"(r)
            : "0"(x), "u"(y)
            : "st(1)");
    return neg ? -r : r;
}

double ldexp(double x, int e) { return x * pow(2.0, (double)e); }

double frexp(double x, int* e)
{
    if (x == 0.0) {
        *e = 0;
        return 0.0;
    }
    union {
        double d;
        uint64_t u;
    } v = {x};
    *e = (int)((v.u >> 52) & 0x7FF) - 1022;
    v.u = (v.u & ~(0x7FFull << 52)) | (1022ull << 52);
    return v.d;
}

double modf(double x, double* ip)
{
    double t = trunc(x);
    *ip = t;
    return x - t;
}
