#ifndef __BIOS_H
#define __BIOS_H

#include <types.h>

#define VIDEO_WIDTH 80
#define VIDEO_HEIGHT 25
#define TAB_WIDTH 4

enum color {
    BLACK = 0, BLUE, GREEN, CYAN,
    RED, MAGENTA, BROWN, LIGHTGREY,
    DARKGREY, LIGHTBLUE, LIGHTGREEN, LIGHTCYAN,
    LIGHTRED, LIGHTMAGENTA, LIGHTBROWN, WHITE
};
typedef enum color color;
// Clears the screen
void scrn_cls(void);
// Sets the background color and the font.
void scrn_setmode(color,color);
// Returns the background and font format
ushort scrn_getmode(void);
// Returns the cursor row
uchar scrn_getrow(void);
// Returns the cursor column
uchar scrn_getcol(void);
// Places the cursor at a location.
// PRE: The coordinates passed are valid within video memory
void scrn_setcursor(uchar, uchar);
// Prints a character to video memory. Does not use format.
void scrn_putc(char, ushort);
void scrn_move_back(void);
// Prints the message, if it is possible to do so
void scrn_print(const char*);
// Prints the message, with C printf-style formatting.
// PRE: The number of parameters passed MUST be correct
void scrn_printf(const char*,...);
// Prints the message at the indicated address. Returns 0
// if everything is fine, -1 in case of error. It is used in the
// direct-to-screen printing syscall
int scrn_pos_print(uchar row, uchar col, const char * msg);
// Like scrn_pos_print but printf
int scrn_pos_printf(uchar row, uchar col, const char * msg,...);
bool in_video_mem(uint address);
#endif
