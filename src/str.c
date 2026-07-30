// Small string-view helpers (see str.h). Freestanding and allocation-free.

#include <str.h>

struct str str_from(const char* s)
{
    size_t n = 0;
    while (s[n]) {
        n++;
    }
    return (struct str){s, n};
}

char to_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

char to_upper(char c)
{
    return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
}

bool str_eq(struct str a, struct str b)
{
    if (a.len != b.len) {
        return false;
    }
    for (size_t i = 0; i < a.len; i++) {
        if (a.data[i] != b.data[i]) {
            return false;
        }
    }
    return true;
}

bool str_eq_ci(struct str a, struct str b)
{
    if (a.len != b.len) {
        return false;
    }
    for (size_t i = 0; i < a.len; i++) {
        if (to_lower(a.data[i]) != to_lower(b.data[i])) {
            return false;
        }
    }
    return true;
}

bool str_has_prefix(struct str s, struct str prefix)
{
    if (s.len < prefix.len) {
        return false;
    }
    for (size_t i = 0; i < prefix.len; i++) {
        if (s.data[i] != prefix.data[i]) {
            return false;
        }
    }
    return true;
}

bool str_has_suffix(struct str s, struct str suffix)
{
    if (s.len < suffix.len) {
        return false;
    }
    size_t off = s.len - suffix.len;
    for (size_t i = 0; i < suffix.len; i++) {
        if (s.data[off + i] != suffix.data[i]) {
            return false;
        }
    }
    return true;
}

struct str str_trim_prefix(struct str s, struct str prefix)
{
    if (str_has_prefix(s, prefix)) {
        s.data += prefix.len;
        s.len -= prefix.len;
    }
    return s;
}

struct str str_trim_suffix(struct str s, struct str suffix)
{
    if (str_has_suffix(s, suffix)) {
        s.len -= suffix.len;
    }
    return s;
}

bool str_cut(struct str s, struct str sep, struct str* before,
             struct str* after)
{
    // Empty separator: match at the front (before = "", after = s), like Go.
    if (sep.len != 0 && s.len >= sep.len) {
        for (size_t i = 0; i + sep.len <= s.len; i++) {
            bool match = true;
            for (size_t j = 0; j < sep.len; j++) {
                if (s.data[i + j] != sep.data[j]) {
                    match = false;
                    break;
                }
            }
            if (match) {
                if (before) {
                    *before = str_span(s.data, i);
                }
                if (after) {
                    *after =
                            str_span(s.data + i + sep.len, s.len - i - sep.len);
                }
                return true;
            }
        }
    } else if (sep.len == 0) {
        if (before) {
            *before = str_span(s.data, 0);
        }
        if (after) {
            *after = s;
        }
        return true;
    }
    if (before) {
        *before = s;
    }
    if (after) {
        *after = str_span(s.data + s.len, 0);
    }
    return false;
}

bool str_cut_ch(struct str s, char sep, struct str* before, struct str* after)
{
    for (size_t i = 0; i < s.len; i++) {
        if (s.data[i] == sep) {
            if (before) {
                *before = str_span(s.data, i);
            }
            if (after) {
                *after = str_span(s.data + i + 1, s.len - i - 1);
            }
            return true;
        }
    }
    if (before) {
        *before = s;
    }
    if (after) {
        *after = str_span(s.data + s.len, 0);
    }
    return false;
}

bool str_to_u32(struct str s, uint32_t* out)
{
    if (s.len == 0) {
        return false;
    }
    uint32_t v = 0;
    for (size_t i = 0; i < s.len; i++) {
        char c = s.data[i];
        if (c < '0' || c > '9') {
            return false;
        }
        v = v * 10 + (uint32_t)(c - '0');
    }
    *out = v;
    return true;
}

size_t str_copy(char* dst, size_t cap, struct str s)
{
    if (cap == 0) {
        return 0;
    }
    size_t n = s.len < cap - 1 ? s.len : cap - 1;
    for (size_t i = 0; i < n; i++) {
        dst[i] = s.data[i];
    }
    dst[n] = '\0';
    return n;
}
