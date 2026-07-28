#ifndef __LINEEDIT_H
#define __LINEEDIT_H

// Limits shared by the two line editors — the serial shell (src/shell.c) and
// the GUI terminal's input line (src/term.c) — so their editing and history
// behaviour stays identical.
#define LINE_MAX 256 // max bytes in one input line
#define HIST_MAX 32  // command-history depth

#endif
