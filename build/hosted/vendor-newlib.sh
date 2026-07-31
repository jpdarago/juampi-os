#!/usr/bin/env bash
# Vendor the curated newlib subset into src/newlib/: copy exactly the libc
# source files our hosted programs pull in (from the linker map), preserving the
# subdirectory layout so same-dir "local.h" includes still resolve, plus every
# header from those dirs and the installed public include tree. Compiled per-file
# by the host GCC in the Makefile (see the newlib rules there), like the other
# vendored libraries. Re-runnable: wipes src/newlib/ first.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
NL="$ROOT/.newlib/newlib-4.5.0.20241231/newlib/libc"
INC="$ROOT/.newlib/prefix/x86_64-elf/include"
DST="$ROOT/src/newlib"

[ -d "$NL" ] || { echo "newlib source missing ($NL); run build-newlib.sh" >&2; exit 1; }
[ -d "$INC" ] || { echo "installed include tree missing ($INC)" >&2; exit 1; }

# member <subdir>/<name> : the .c files the demos actually linked (mlock omitted;
# we supply empty __malloc_lock/unlock stubs below since the kernel is
# single-threaded).
members="
ctype/ctype_ errno/errno locale/locale locale/localeconv misc/init
reent/closer reent/fstatr reent/impure reent/isattyr reent/lseekr reent/openr
reent/readr reent/reent reent/sbrkr reent/signalr reent/writer
signal/signal
stdio/fclose stdio/fflush stdio/findfp stdio/fiprintf stdio/flags stdio/fopen
stdio/fprintf stdio/fputs stdio/fread stdio/fseek stdio/fseeko stdio/fvwrite
stdio/fwalk stdio/makebuf stdio/printf stdio/puts stdio/refill stdio/snprintf
stdio/sprint_r stdio/ssprint_r stdio/ssputs_r stdio/stdio stdio/svfprintf
stdio/vfiprintf stdio/vfprintf stdio/wsetup
stdio/fwrite stdio/ftell stdio/ftello stdio/putchar stdio/putc stdio/vsnprintf
stdio/sprintf stdio/wbuf
stdlib/abort stdlib/assert stdlib/__call_atexit stdlib/callocr stdlib/dtoa
stdlib/exit stdlib/freer stdlib/malloc stdlib/mallocr stdlib/mbtowc_r
stdlib/mprec stdlib/reallocr stdlib/wctomb_r
stdlib/atoi stdlib/strtol stdlib/strtoul stdlib/abs stdlib/labs stdlib/rand
stdlib/calloc stdlib/realloc
string/memchr string/memcpy string/memmove string/memset string/strcmp
string/strlen string/strncmp string/strchr string/strrchr string/strstr
string/strcasecmp string/strncasecmp string/strcpy string/strncpy string/strcat
string/strncat string/strdup string/strdup_r string/strtok_r string/strnlen
string/strspn
string/strcspn string/strpbrk string/memcmp
search/qsort
"

# fdlibm math functions Doom uses (fabs/atan/tan and the tan reduction chain).
# Flattened into libm/ with fdlibm.h; compiled by the same per-file rule.
LM="$ROOT/.newlib/newlib-4.5.0.20241231/newlib/libm"
libm_files="math/s_fabs math/s_atan math/s_tan math/k_tan math/e_rem_pio2 \
math/k_rem_pio2 common/s_scalbn common/s_copysign"

rm -rf "$DST"
mkdir -p "$DST"

# The public/installed headers (stdio.h, sys/, machine/, generated newlib.h /
# _newlib_version.h / sys/config.h, ...).
cp -r "$INC" "$DST/include"

# The .c members, preserving their subdir; plus every header from each touched
# dir (private local.h/fvwrite.h/mprec.h/... resolve as same-dir includes).
touched=""
for m in $members; do
    sub="${m%/*}"
    cp "$NL/$m.c" "$DST/$sub/" 2>/dev/null || { mkdir -p "$DST/$sub"; cp "$NL/$m.c" "$DST/$sub/"; }
    case " $touched " in *" $sub "*) ;; *) touched="$touched $sub" ;; esac
done
for sub in $touched; do
    cp "$NL/$sub"/*.h "$DST/$sub/" 2>/dev/null || true
done

# mallocr.c/freer.c/... #include "_mallocr.c" (the shared impl); copy it too. It
# is compiled-in via those, never on its own (the Makefile filters it out).
cp "$NL/stdlib/_mallocr.c" "$DST/stdlib/"

# fdlibm math subset: flatten into libm/ with its private header.
mkdir -p "$DST/libm"
cp "$LM/common/fdlibm.h" "$LM/common/math_config.h" "$DST/libm/"
for m in $libm_files; do
    cp "$LM/$m.c" "$DST/libm/"
done

# Single-threaded malloc locking: empty stubs (newlib's mlock.c is otherwise
# arch-specific and unnecessary here).
cat > "$DST/juampi_mlock.c" <<'EOF'
// Single-threaded malloc lock hooks for the vendored newlib (juampiOS runs one
// hosted program at a time, cooperatively) — nothing to lock.
struct _reent;
void __malloc_lock(struct _reent* r)
{
    (void)r;
}
void __malloc_unlock(struct _reent* r)
{
    (void)r;
}
EOF

echo "vendored $(echo $members | wc -w) .c members into $DST"
echo "subdirs:$touched"
find "$DST" -name '*.c' | wc -l | xargs echo "total .c files:"
