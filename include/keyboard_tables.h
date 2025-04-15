#ifndef __KEYBOARD_TABLES_H
#define __KEYBOARD_TABLES_H

#include <types.h>

#define SEQ_KLL 3
#define SEQ_EOD 4
#define SEQ_STP 32

#define CTRL_KEY        0x1D
#define ALT_KEY         0x11
#define SHIFT_KEY       0x2A
#define BACKSPACE_KEY   0x66
#define CAPSLOCK_KEY    0x58

extern uchar kbdus[];
extern bool pressed[];

#endif
