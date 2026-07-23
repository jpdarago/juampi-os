// Ragel scanner that colorizes a line of Lua for the kernel shell's line
// editor. It is a pure, freestanding tokenizer: no libc beyond memcmp (via the
// gperf keyword table). highlight_lua() re-lexes the whole edit buffer on every
// keystroke and writes an SGR-colorized copy that redraw() prints.
//
// Generated file: src/highlight/highlight.c (committed). Regenerate after
// editing this grammar with `make highlight-gen` (needs ragel; it is in the
// devenv). The kernel build compiles the committed .c, so a tool-less checkout
// still builds.

#include <highlight.h>
#include <stddef.h>

// gperf perfect-hash keyword table (src/highlight/lua_keywords.c). An identifier
// is a reserved word iff this returns non-NULL.
struct lua_kw {
    const char* name;
};
const struct lua_kw* lua_kw_lookup(const char* str, size_t len);

// SGR color escapes. Plain 16-color ANSI so it renders identically on the
// framebuffer terminal (flanterm) and over the serial console.
#define C_KEYWORD "\033[35m" // magenta
#define C_STRING "\033[32m"  // green
#define C_NUMBER "\033[33m"  // yellow
#define C_COMMENT "\033[90m" // bright black (grey)
#define C_RESET "\033[0m"

// Bounded output builder. Once an append would overflow, `ok` latches false and
// further appends are dropped; highlight_lua() then falls back to raw text.
typedef struct {
    char* out;
    size_t cap;
    size_t len;
    int ok;
} sink;

static void put_raw(sink* s, const char* p, size_t n)
{
    if (!s->ok) {
        return;
    }
    if (s->len + n >= s->cap) {
        s->ok = 0;
        return;
    }
    for (size_t i = 0; i < n; i++) {
        s->out[s->len++] = p[i];
    }
}

static void put_z(sink* s, const char* z)
{
    size_t n = 0;
    while (z[n]) {
        n++;
    }
    put_raw(s, z, n);
}

// Emit the token [ts,te) wrapped in `color` ... reset.
static void emit(sink* s, const char* color, const char* ts, const char* te)
{
    put_z(s, color);
    put_raw(s, ts, (size_t)(te - ts));
    put_z(s, C_RESET);
}

%%{
    machine lua_hl;

    action kw_or_ident {
        if (lua_kw_lookup(ts, (size_t)(te - ts)))
            emit(&s, C_KEYWORD, ts, te);
        else
            put_raw(&s, ts, (size_t)(te - ts));
    }
    action num { emit(&s, C_NUMBER, ts, te); }
    action str { emit(&s, C_STRING, ts, te); }
    action cmt { emit(&s, C_COMMENT, ts, te); }
    action raw { put_raw(&s, ts, (size_t)(te - ts)); }

    ident = [A-Za-z_][A-Za-z0-9_]*;

    hexnum = '0' [xX] xdigit+ ('.' xdigit*)? ([pP] [+\-]? digit+)?;
    decnum = ( digit+ ('.' digit*)? | '.' digit+ ) ([eE] [+\-]? digit+)?;
    number = hexnum | decnum;

    # Short strings. The "open" variants (no closing quote) color the string as
    # it is being typed; the closed variants are longer, so they win once the
    # quote is finished.
    dqstr  = '"'  ( [^"\\\n]  | '\\' any )* '"';
    sqstr  = "'"  ( [^'\\\n]  | '\\' any )* "'";
    dqopen = '"'  ( [^"\\\n]  | '\\' any )*;
    sqopen = "'"  ( [^'\\\n]  | '\\' any )*;

    # Long-bracket strings, levels 0..4 ( [[..]] , [=[..]=] , ... ). Lua allows
    # any level; deeper ones are vanishingly rare in a REPL line and fall back to
    # uncolored. `:>>` stops at the first matching close, as Lua's lexer does.
    longstr =
        ( '[' '['             any* :>> (']' ']')             ) |
        ( '[' '=' '['         any* :>> (']' '=' ']')         ) |
        ( '[' '=' '=' '['     any* :>> (']' '=' '=' ']')     ) |
        ( '[' '=' '=' '=' '[' any* :>> (']' '=' '=' '=' ']') ) ;

    linecomment = '--' [^\n]*;
    longcomment = '--' longstr;

    main := |*
        longcomment => cmt;
        linecomment => cmt;
        longstr     => str;
        dqstr       => str;
        sqstr       => str;
        dqopen      => str;
        sqopen      => str;
        number      => num;
        ident       => kw_or_ident;
        any         => raw;
    *|;
}%%

%% write data;

size_t highlight_lua(const char* src, size_t n, char* out, size_t cap)
{
    sink s = { out, cap, 0, 1 };
    const char* p = src;
    const char* pe = src + n;
    const char* eof = pe;
    const char* ts;
    const char* te;
    int cs;
    int act;
    (void)act;
    (void)lua_hl_en_main;

    %% write init;
    %% write exec;

    if (!s.ok) {
        // Colorized form overflowed `cap`; show the raw line instead.
        s.len = 0;
        s.ok = 1;
        put_raw(&s, src, n);
    }
    if (s.len < cap) {
        out[s.len] = '\0';
    } else if (cap > 0) {
        out[cap - 1] = '\0';
    }
    return s.len;
}
