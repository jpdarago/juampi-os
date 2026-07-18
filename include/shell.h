#ifndef __SHELL_H
#define __SHELL_H

// Interactive kernel shell over the COM1 serial console. Reads a line,
// evaluates it, repeats. shell_eval is the single evaluation hook — a small set
// of builtins now, the Lua interpreter later.
void shell_run(void);

#endif
