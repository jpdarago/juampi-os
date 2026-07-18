#ifndef __LUASHELL_H
#define __LUASHELL_H

// The Lua REPL behind the kernel shell. luashell_init() creates a persistent
// interpreter (base/string/table/math/coroutine libraries) once;
// luashell_eval() runs one line of input and prints its result or error to the
// console.
void luashell_init(void);
void luashell_eval(const char* line);

#endif
