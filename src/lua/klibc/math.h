#ifndef KLIBC_MATH_H
#define KLIBC_MATH_H
// Freestanding <math.h> for the vendored Lua, implemented over x87/SSE in
// klibc.c. Accuracy is adequate for a shell (x87 transcendentals; no argument
// range reduction for very large trig inputs).

#define HUGE_VAL (__builtin_inf())
#define INFINITY (__builtin_inf())
#define NAN (__builtin_nan(""))

double fabs(double x);
double floor(double x);
double ceil(double x);
double trunc(double x);
double sqrt(double x);
double fmod(double x, double y);
double pow(double x, double y);
double exp(double x);
double log(double x);
double log2(double x);
double log10(double x);
double sin(double x);
double cos(double x);
double tan(double x);
double asin(double x);
double acos(double x);
double atan(double x);
double atan2(double y, double x);
double ldexp(double x, int e);
double frexp(double x, int* e);
double modf(double x, double* ip);
#endif
