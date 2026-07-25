#ifndef __STR_H
#define __STR_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// A small string-view library for the kernel. A `str` is a non-owning
// (pointer, length) slice — like a Go string — so splitting and trimming return
// sub-views without copying or allocating. Nothing here mutates its input or
// returns a NUL-terminated result; use str_copy() to get a C string. Modelled
// on Go's `strings` package and the nullprogram string-slice style the
// allocators already follow.
typedef struct {
    const char* data;
    size_t len;
} str;

// A view over a NUL-terminated C string.
str str_from(const char* s);

// A view over an explicit (pointer, length) span.
static inline str str_span(const char* p, size_t n)
{
    return (str){p, n};
}

// A view over a string literal (compile-time length; rejects non-literals).
#define S(lit) ((str){"" lit, sizeof(lit) - 1})

// ASCII case folding.
char to_lower(char c);
char to_upper(char c);

// Comparisons.
bool str_eq(str a, str b);
bool str_eq_ci(str a, str b); // case-insensitive
bool str_has_prefix(str s, str prefix);

// Drop `prefix` from the front of `s` if present, else return `s` unchanged.
str str_trim_prefix(str s, str prefix);

// Split `s` around the first occurrence of the separator, like Go's
// strings.Cut: on a match, *before/*after get the two sides (excluding the
// separator) and it returns true; otherwise *before = s, *after = "", and it
// returns false. Either output pointer may be NULL.
bool str_cut(str s, str sep, str* before, str* after);
bool str_cut_ch(str s, char sep, str* before, str* after);

// Parse a whole-string unsigned decimal. False on empty or any non-digit.
bool str_to_u32(str s, uint32_t* out);

// Copy `s` into `dst` (capacity `cap`), NUL-terminated and truncated to fit.
// Returns the number of bytes written, excluding the terminator.
size_t str_copy(char* dst, size_t cap, str s);

#endif
