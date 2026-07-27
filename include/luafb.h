#ifndef __LUAFB_H
#define __LUAFB_H

#include <gfx.h>

// The `fb` Lua library draws through a "current surface" — the screen by
// default. ui.canvas:draw() redirects it to a canvas surface for the duration
// of the draw callback: pass the canvas surface, then pass NULL to restore the
// screen. This is the one thin implicit-current in the Lua binding layer; the
// gfx core primitives themselves always take an explicit surface.
void lua_fb_target(gfx_surface* s);

#endif
