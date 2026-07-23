
#line 1 "src/highlight/highlight.rl"
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


#line 121 "src/highlight/highlight.rl"



#line 72 "src/highlight/highlight.c"
static const char _lua_hl_actions[] = {
	0, 1, 0, 1, 1, 1, 2, 1, 
	5, 1, 6, 1, 7, 1, 8, 1, 
	9, 1, 10, 1, 11, 1, 12, 1, 
	13, 1, 14, 1, 15, 1, 16, 1, 
	17, 1, 18, 1, 19, 1, 20, 1, 
	21, 2, 2, 3, 2, 2, 4
};

static const unsigned char _lua_hl_key_offsets[] = {
	0, 0, 0, 1, 3, 5, 7, 8, 
	9, 11, 13, 14, 15, 17, 18, 19, 
	20, 24, 26, 32, 34, 36, 37, 38, 
	40, 42, 44, 45, 46, 48, 50, 51, 
	52, 54, 55, 56, 57, 70, 73, 76, 
	77, 79, 80, 83, 86, 89, 91, 93, 
	96, 99, 102, 104, 106, 109, 112, 114, 
	116, 119, 121, 123, 125, 127, 131, 133, 
	140, 145, 154, 162, 169
};

static const char _lua_hl_trans_keys[] = {
	93, 61, 93, 61, 93, 61, 93, 93, 
	93, 61, 93, 61, 93, 93, 93, 61, 
	93, 93, 93, 93, 43, 45, 48, 57, 
	48, 57, 48, 57, 65, 70, 97, 102, 
	61, 91, 61, 91, 91, 93, 61, 93, 
	61, 93, 61, 93, 93, 93, 61, 93, 
	61, 93, 93, 93, 61, 93, 93, 93, 
	93, 34, 39, 45, 46, 48, 91, 95, 
	49, 57, 65, 90, 97, 122, 10, 34, 
	92, 10, 39, 92, 45, 10, 91, 10, 
	10, 61, 91, 10, 61, 91, 10, 61, 
	91, 10, 91, 10, 93, 10, 61, 93, 
	10, 61, 93, 10, 61, 93, 10, 93, 
	10, 93, 10, 61, 93, 10, 61, 93, 
	10, 93, 10, 93, 10, 61, 93, 10, 
	93, 10, 93, 10, 93, 48, 57, 69, 
	101, 48, 57, 48, 57, 46, 69, 88, 
	101, 120, 48, 57, 46, 69, 101, 48, 
	57, 46, 80, 112, 48, 57, 65, 70, 
	97, 102, 80, 112, 48, 57, 65, 70, 
	97, 102, 95, 48, 57, 65, 90, 97, 
	122, 61, 91, 0
};

static const char _lua_hl_single_lengths[] = {
	0, 0, 1, 2, 2, 2, 1, 1, 
	2, 2, 1, 1, 2, 1, 1, 1, 
	2, 0, 0, 2, 2, 1, 1, 2, 
	2, 2, 1, 1, 2, 2, 1, 1, 
	2, 1, 1, 1, 7, 3, 3, 1, 
	2, 1, 3, 3, 3, 2, 2, 3, 
	3, 3, 2, 2, 3, 3, 2, 2, 
	3, 2, 2, 2, 0, 2, 0, 5, 
	3, 3, 2, 1, 2
};

static const char _lua_hl_range_lengths[] = {
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	1, 1, 3, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 3, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 1, 1, 1, 1, 
	1, 3, 3, 3, 0
};

static const short _lua_hl_index_offsets[] = {
	0, 1, 2, 4, 7, 10, 13, 15, 
	17, 20, 23, 25, 27, 30, 32, 34, 
	36, 40, 42, 46, 49, 52, 54, 56, 
	59, 62, 65, 67, 69, 72, 75, 77, 
	79, 82, 84, 86, 88, 99, 103, 107, 
	109, 112, 114, 118, 122, 126, 129, 132, 
	136, 140, 144, 147, 150, 154, 158, 161, 
	164, 168, 171, 174, 177, 179, 183, 185, 
	192, 197, 204, 210, 215
};

static const char _lua_hl_indicies[] = {
	1, 3, 6, 5, 7, 6, 5, 8, 
	6, 5, 9, 6, 5, 10, 5, 12, 
	11, 13, 12, 11, 14, 12, 11, 10, 
	11, 16, 15, 17, 16, 15, 10, 15, 
	19, 18, 10, 18, 21, 21, 22, 20, 
	22, 20, 23, 23, 23, 20, 25, 26, 
	24, 27, 28, 24, 29, 24, 30, 29, 
	31, 30, 29, 32, 30, 29, 33, 30, 
	29, 34, 29, 35, 28, 36, 35, 28, 
	37, 35, 28, 34, 28, 38, 26, 39, 
	38, 26, 34, 26, 41, 40, 34, 40, 
	1, 3, 43, 44, 45, 48, 47, 46, 
	47, 47, 42, 49, 50, 51, 1, 52, 
	53, 54, 3, 56, 55, 57, 59, 58, 
	60, 58, 57, 61, 62, 58, 57, 63, 
	64, 58, 57, 65, 66, 58, 57, 67, 
	58, 5, 68, 67, 5, 69, 68, 67, 
	5, 70, 68, 67, 5, 71, 68, 67, 
	5, 72, 67, 11, 73, 66, 11, 74, 
	73, 66, 11, 75, 73, 66, 11, 72, 
	66, 15, 76, 64, 15, 77, 76, 64, 
	15, 72, 64, 18, 78, 62, 18, 72, 
	62, 79, 55, 81, 81, 79, 80, 22, 
	80, 79, 81, 82, 81, 82, 46, 80, 
	79, 81, 81, 46, 80, 83, 81, 81, 
	23, 23, 23, 80, 81, 81, 83, 83, 
	83, 80, 47, 47, 47, 47, 84, 85, 
	40, 55, 0
};

static const char _lua_hl_trans_targs[] = {
	36, 37, 36, 38, 36, 2, 3, 4, 
	5, 6, 36, 7, 8, 9, 10, 11, 
	12, 13, 14, 15, 36, 17, 62, 65, 
	36, 20, 31, 21, 27, 22, 23, 24, 
	25, 26, 36, 28, 29, 30, 32, 33, 
	34, 35, 36, 39, 60, 63, 64, 67, 
	68, 36, 36, 0, 36, 36, 1, 36, 
	40, 36, 41, 42, 36, 43, 58, 44, 
	55, 45, 51, 46, 47, 48, 49, 50, 
	41, 52, 53, 54, 56, 57, 59, 61, 
	36, 16, 18, 66, 36, 19
};

static const char _lua_hl_trans_actions[] = {
	31, 5, 33, 5, 29, 0, 0, 0, 
	0, 0, 7, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 35, 0, 0, 5, 
	37, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 9, 0, 0, 0, 0, 0, 
	0, 0, 15, 0, 0, 5, 5, 0, 
	5, 19, 11, 0, 21, 13, 0, 27, 
	0, 17, 44, 0, 39, 0, 5, 0, 
	5, 0, 5, 5, 5, 5, 5, 5, 
	41, 5, 5, 5, 5, 5, 5, 5, 
	23, 0, 0, 5, 25, 0
};

static const char _lua_hl_to_state_actions[] = {
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 1, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0
};

static const char _lua_hl_from_state_actions[] = {
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 3, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0
};

static const short _lua_hl_eof_trans[] = {
	1, 3, 5, 5, 5, 5, 5, 5, 
	5, 5, 5, 5, 5, 5, 5, 5, 
	21, 21, 21, 25, 25, 25, 25, 25, 
	25, 25, 25, 25, 25, 25, 25, 25, 
	25, 25, 25, 25, 0, 50, 53, 56, 
	58, 61, 58, 58, 58, 58, 58, 58, 
	58, 58, 58, 58, 58, 58, 58, 58, 
	58, 58, 58, 58, 56, 81, 81, 81, 
	81, 81, 81, 85, 56
};

static const int lua_hl_start = 36;
static const int lua_hl_first_final = 36;
static const int lua_hl_error = -1;

static const int lua_hl_en_main = 36;


#line 124 "src/highlight/highlight.rl"

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

    
#line 269 "src/highlight/highlight.c"
	{
	cs = lua_hl_start;
	ts = 0;
	te = 0;
	act = 0;
	}

#line 139 "src/highlight/highlight.rl"
    
#line 275 "src/highlight/highlight.c"
	{
	int _klen;
	unsigned int _trans;
	const char *_acts;
	unsigned int _nacts;
	const char *_keys;

	if ( p == pe )
		goto _test_eof;
_resume:
	_acts = _lua_hl_actions + _lua_hl_from_state_actions[cs];
	_nacts = (unsigned int) *_acts++;
	while ( _nacts-- > 0 ) {
		switch ( *_acts++ ) {
	case 1:
#line 1 "NONE"
	{ts = p;}
	break;
#line 292 "src/highlight/highlight.c"
		}
	}

	_keys = _lua_hl_trans_keys + _lua_hl_key_offsets[cs];
	_trans = _lua_hl_index_offsets[cs];

	_klen = _lua_hl_single_lengths[cs];
	if ( _klen > 0 ) {
		const char *_lower = _keys;
		const char *_mid;
		const char *_upper = _keys + _klen - 1;
		while (1) {
			if ( _upper < _lower )
				break;

			_mid = _lower + ((_upper-_lower) >> 1);
			if ( (*p) < *_mid )
				_upper = _mid - 1;
			else if ( (*p) > *_mid )
				_lower = _mid + 1;
			else {
				_trans += (unsigned int)(_mid - _keys);
				goto _match;
			}
		}
		_keys += _klen;
		_trans += _klen;
	}

	_klen = _lua_hl_range_lengths[cs];
	if ( _klen > 0 ) {
		const char *_lower = _keys;
		const char *_mid;
		const char *_upper = _keys + (_klen<<1) - 2;
		while (1) {
			if ( _upper < _lower )
				break;

			_mid = _lower + (((_upper-_lower) >> 1) & ~1);
			if ( (*p) < _mid[0] )
				_upper = _mid - 2;
			else if ( (*p) > _mid[1] )
				_lower = _mid + 2;
			else {
				_trans += (unsigned int)((_mid - _keys)>>1);
				goto _match;
			}
		}
		_trans += _klen;
	}

_match:
	_trans = _lua_hl_indicies[_trans];
_eof_trans:
	cs = _lua_hl_trans_targs[_trans];

	if ( _lua_hl_trans_actions[_trans] == 0 )
		goto _again;

	_acts = _lua_hl_actions + _lua_hl_trans_actions[_trans];
	_nacts = (unsigned int) *_acts++;
	while ( _nacts-- > 0 )
	{
		switch ( *_acts++ )
		{
	case 2:
#line 1 "NONE"
	{te = p+1;}
	break;
	case 3:
#line 80 "src/highlight/highlight.rl"
	{act = 1;}
	break;
	case 4:
#line 80 "src/highlight/highlight.rl"
	{act = 2;}
	break;
	case 5:
#line 80 "src/highlight/highlight.rl"
	{te = p+1;{ emit(&s, C_COMMENT, ts, te); }}
	break;
	case 6:
#line 79 "src/highlight/highlight.rl"
	{te = p+1;{ emit(&s, C_STRING, ts, te); }}
	break;
	case 7:
#line 79 "src/highlight/highlight.rl"
	{te = p+1;{ emit(&s, C_STRING, ts, te); }}
	break;
	case 8:
#line 79 "src/highlight/highlight.rl"
	{te = p+1;{ emit(&s, C_STRING, ts, te); }}
	break;
	case 9:
#line 81 "src/highlight/highlight.rl"
	{te = p+1;{ put_raw(&s, ts, (size_t)(te - ts)); }}
	break;
	case 10:
#line 80 "src/highlight/highlight.rl"
	{te = p;p--;{ emit(&s, C_COMMENT, ts, te); }}
	break;
	case 11:
#line 79 "src/highlight/highlight.rl"
	{te = p;p--;{ emit(&s, C_STRING, ts, te); }}
	break;
	case 12:
#line 79 "src/highlight/highlight.rl"
	{te = p;p--;{ emit(&s, C_STRING, ts, te); }}
	break;
	case 13:
#line 78 "src/highlight/highlight.rl"
	{te = p;p--;{ emit(&s, C_NUMBER, ts, te); }}
	break;
	case 14:
#line 72 "src/highlight/highlight.rl"
	{te = p;p--;{
        if (lua_kw_lookup(ts, (size_t)(te - ts)))
            emit(&s, C_KEYWORD, ts, te);
        else
            put_raw(&s, ts, (size_t)(te - ts));
    }}
	break;
	case 15:
#line 81 "src/highlight/highlight.rl"
	{te = p;p--;{ put_raw(&s, ts, (size_t)(te - ts)); }}
	break;
	case 16:
#line 80 "src/highlight/highlight.rl"
	{{p = ((te))-1;}{ emit(&s, C_COMMENT, ts, te); }}
	break;
	case 17:
#line 79 "src/highlight/highlight.rl"
	{{p = ((te))-1;}{ emit(&s, C_STRING, ts, te); }}
	break;
	case 18:
#line 79 "src/highlight/highlight.rl"
	{{p = ((te))-1;}{ emit(&s, C_STRING, ts, te); }}
	break;
	case 19:
#line 78 "src/highlight/highlight.rl"
	{{p = ((te))-1;}{ emit(&s, C_NUMBER, ts, te); }}
	break;
	case 20:
#line 81 "src/highlight/highlight.rl"
	{{p = ((te))-1;}{ put_raw(&s, ts, (size_t)(te - ts)); }}
	break;
	case 21:
#line 1 "NONE"
	{	switch( act ) {
	case 1:
	{{p = ((te))-1;} emit(&s, C_COMMENT, ts, te); }
	break;
	case 2:
	{{p = ((te))-1;} emit(&s, C_COMMENT, ts, te); }
	break;
	}
	}
	break;
#line 430 "src/highlight/highlight.c"
		}
	}

_again:
	_acts = _lua_hl_actions + _lua_hl_to_state_actions[cs];
	_nacts = (unsigned int) *_acts++;
	while ( _nacts-- > 0 ) {
		switch ( *_acts++ ) {
	case 0:
#line 1 "NONE"
	{ts = 0;}
	break;
#line 441 "src/highlight/highlight.c"
		}
	}

	if ( ++p != pe )
		goto _resume;
	_test_eof: {}
	if ( p == eof )
	{
	if ( _lua_hl_eof_trans[cs] > 0 ) {
		_trans = _lua_hl_eof_trans[cs] - 1;
		goto _eof_trans;
	}
	}

	}

#line 140 "src/highlight/highlight.rl"

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
