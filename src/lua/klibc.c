// Freestanding libc shim for the vendored Lua: the string/stdlib/stdio/locale
// pieces Lua needs, backed by the kernel console and heap. (memset/memcpy come
// from the kernel's utils.c; math is in klibc_math.c; setjmp/longjmp in
// klibc_setjmp.asm.) Compiled with the kernel include path, so it uses the real
// kernel headers rather than the klibc stubs.

#include <memory.h>
#include <console.h>
#include <panic.h>
#include <utils.h> // memcpy/memset (kernel implementations)
#include <idt.h>   // timer_ticks (for time/clock)

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>

#include <printf/printf.h>

// --- string.h ---------------------------------------------------------------

void* memmove(void* d, const void* s, size_t n)
{
    unsigned char* dp = d;
    const unsigned char* sp = s;
    if (dp < sp) {
        for (size_t i = 0; i < n; i++) {
            dp[i] = sp[i];
        }
    } else {
        for (size_t i = n; i-- > 0;) {
            dp[i] = sp[i];
        }
    }
    return d;
}

int memcmp(const void* a, const void* b, size_t n)
{
    const unsigned char *x = a, *y = b;
    for (size_t i = 0; i < n; i++) {
        if (x[i] != y[i]) {
            return (int)x[i] - (int)y[i];
        }
    }
    return 0;
}

void* memchr(const void* s, int c, size_t n)
{
    const unsigned char* p = s;
    for (size_t i = 0; i < n; i++) {
        if (p[i] == (unsigned char)c) {
            return (void*)(p + i);
        }
    }
    return NULL;
}

size_t strlen(const char* s)
{
    size_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

int strcmp(const char* a, const char* b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char* a, const char* b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i] || a[i] == 0) {
            return (int)(unsigned char)a[i] - (int)(unsigned char)b[i];
        }
    }
    return 0;
}

int strcoll(const char* a, const char* b) { return strcmp(a, b); }

char* strcpy(char* d, const char* s)
{
    char* r = d;
    while ((*d++ = *s++)) {
    }
    return r;
}

char* strchr(const char* s, int c)
{
    for (;; s++) {
        if (*s == (char)c) {
            return (char*)s;
        }
        if (*s == 0) {
            return NULL;
        }
    }
}

size_t strspn(const char* s, const char* set)
{
    size_t n = 0;
    for (; s[n]; n++) {
        if (!strchr(set, s[n])) {
            break;
        }
    }
    return n;
}

char* strpbrk(const char* s, const char* set)
{
    for (; *s; s++) {
        if (strchr(set, *s)) {
            return (char*)s;
        }
    }
    return NULL;
}

char* strstr(const char* h, const char* n)
{
    if (!*n) {
        return (char*)h;
    }
    for (; *h; h++) {
        const char *a = h, *b = n;
        while (*a && *b && *a == *b) {
            a++;
            b++;
        }
        if (!*b) {
            return (char*)h;
        }
    }
    return NULL;
}

char* strerror(int e)
{
    (void)e;
    return "error";
}

// --- stdlib.h ---------------------------------------------------------------

void* malloc(size_t n)
{
    if (n == 0) {
        n = 1;
    }
    return alloc(&heap_default()->base, 1, 1, (ptrdiff_t)n);
}

void* calloc(size_t n, size_t sz) { return malloc(n * sz); } // heap zeroes

void free(void* p)
{
    if (p) {
        heap_free(heap_default(), p);
    }
}

void* realloc(void* p, size_t n)
{
    if (p == NULL) {
        return malloc(n);
    }
    if (n == 0) {
        free(p);
        return NULL;
    }
    size_t old = heap_usable_size(heap_default(), p);
    void* q = malloc(n);
    memcpy(q, p, old < n ? old : n);
    free(p);
    return q;
}

static int c_isspace(int c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
static int c_isdigit(int c) { return c >= '0' && c <= '9'; }

double pow(double, double); // from klibc_math.c

double strtod(const char* s, char** end)
{
    const char* p = s;
    while (c_isspace(*p)) {
        p++;
    }
    int sign = 1;
    if (*p == '+' || *p == '-') {
        sign = (*p == '-') ? -1 : 1;
        p++;
    }
    double val = 0.0;
    int any = 0;
    while (c_isdigit(*p)) {
        val = val * 10.0 + (*p - '0');
        p++;
        any = 1;
    }
    if (*p == '.') {
        p++;
        double frac = 0.1;
        while (c_isdigit(*p)) {
            val += (*p - '0') * frac;
            frac *= 0.1;
            p++;
            any = 1;
        }
    }
    if (any && (*p == 'e' || *p == 'E')) {
        const char* mark = p++;
        int es = 1;
        if (*p == '+' || *p == '-') {
            es = (*p == '-') ? -1 : 1;
            p++;
        }
        int ev = 0, ed = 0;
        while (c_isdigit(*p)) {
            ev = ev * 10 + (*p - '0');
            p++;
            ed = 1;
        }
        if (ed) {
            val *= pow(10.0, (double)(es * ev));
        } else {
            p = mark; // no exponent digits: the 'e' is not part of the number
        }
    }
    if (!any) {
        if (end) {
            *end = (char*)s;
        }
        return 0.0;
    }
    if (end) {
        *end = (char*)p;
    }
    return sign * val;
}

long strtol(const char* s, char** end, int base)
{
    const char* p = s;
    while (c_isspace(*p)) {
        p++;
    }
    int sign = 1;
    if (*p == '+' || *p == '-') {
        sign = (*p == '-') ? -1 : 1;
        p++;
    }
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] | 32) == 'x') {
        p += 2;
        base = 16;
    } else if (base == 0) {
        base = (*p == '0') ? 8 : 10;
    }
    long val = 0;
    for (;; p++) {
        int c = *p, d;
        if (c_isdigit(c)) {
            d = c - '0';
        } else if ((c | 32) >= 'a' && (c | 32) <= 'z') {
            d = (c | 32) - 'a' + 10;
        } else {
            break;
        }
        if (d >= base) {
            break;
        }
        val = val * base + d;
    }
    if (end) {
        *end = (char*)p;
    }
    return sign * val;
}

void abort(void) { kernel_panic("libc abort()"); }

void exit(int code)
{
    (void)code;
    for (;;) {
        __asm__ __volatile__("cli; hlt");
    }
}

char* getenv(const char* name)
{
    (void)name;
    return NULL;
}

// --- stdio.h (everything routes to the console) -----------------------------

struct __FILE {
    int dummy;
};
static struct __FILE stdout_file, stderr_file, stdin_file;
void* stdout = &stdout_file;
void* stderr = &stderr_file;
void* stdin = &stdin_file;

size_t fwrite(const void* p, size_t sz, size_t n, void* f)
{
    (void)f;
    const char* c = p;
    for (size_t i = 0; i < sz * n; i++) {
        console_putc(c[i]);
    }
    return n;
}

int fputs(const char* s, void* f)
{
    (void)f;
    console_print(s);
    return 0;
}

int fputc(int c, void* f)
{
    (void)f;
    console_putc((char)c);
    return c;
}

int fflush(void* f)
{
    (void)f;
    return 0;
}

int vfprintf(void* f, const char* fmt, va_list ap)
{
    (void)f;
    return vprintf(fmt, ap);
}

int fprintf(void* f, const char* fmt, ...)
{
    (void)f;
    va_list ap;
    va_start(ap, fmt);
    int r = vprintf(fmt, ap);
    va_end(ap);
    return r;
}

// File input is unsupported; these always fail, so Lua's luaL_loadfile reports
// an error rather than reading anything.
void* fopen(const char* path, const char* mode)
{
    (void)path;
    (void)mode;
    return NULL;
}
void* freopen(const char* path, const char* mode, void* f)
{
    (void)path;
    (void)mode;
    (void)f;
    return NULL;
}
int fclose(void* f)
{
    (void)f;
    return 0;
}
size_t fread(void* p, size_t sz, size_t n, void* f)
{
    (void)p;
    (void)sz;
    (void)n;
    (void)f;
    return 0;
}
int getc(void* f)
{
    (void)f;
    return -1;
}
int ungetc(int c, void* f)
{
    (void)f;
    return c;
}
int feof(void* f)
{
    (void)f;
    return 1;
}
int ferror(void* f)
{
    (void)f;
    return 0;
}

// --- locale.h / errno.h -----------------------------------------------------

int errno = 0;

// --- time.h (tick-based; used only to seed Lua's RNG) -----------------------

long time(long* t)
{
    long v = (long)timer_ticks();
    if (t) {
        *t = v;
    }
    return v;
}

long clock(void) { return (long)timer_ticks(); }

struct lconv {
    char* decimal_point;
    char* thousands_sep;
};
static struct lconv c_locale = {".", ""};
struct lconv* localeconv(void) { return &c_locale; }
char* setlocale(int cat, const char* name)
{
    (void)cat;
    (void)name;
    return "C";
}
