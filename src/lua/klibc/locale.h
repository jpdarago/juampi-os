#ifndef KLIBC_LOCALE_H
#define KLIBC_LOCALE_H
// Minimal <locale.h>: the C locale only. Lua reads localeconv()->decimal_point.
#define LC_ALL 0
#define LC_NUMERIC 4
struct lconv {
    char* decimal_point;
    char* thousands_sep;
};
struct lconv* localeconv(void);
char* setlocale(int cat, const char* name);
#endif
