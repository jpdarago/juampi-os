#ifndef __HIGHLIGHT_H
#define __HIGHLIGHT_H

#include <stddef.h>

// Write an ANSI-colorized (SGR) copy of `n` bytes of Lua source `src` into
// `out` (capacity `cap`), NUL-terminated. Keywords, strings, numbers and
// comments are colored; everything else is copied verbatim. Returns the number
// of bytes written (excluding the terminator). If the colorized form would not
// fit in `cap`, the raw source is copied instead (still NUL-terminated), so the
// caller always gets a printable line.
//
// The lexer is a Ragel scanner (src/highlight/highlight.rl) over a gperf
// perfect-hash keyword table (src/highlight/lua_keywords.gperf). Both generated
// .c files are committed; regenerate them with `make highlight-gen`.
size_t highlight_lua(const char* src, size_t n, char* out, size_t cap);

#endif
