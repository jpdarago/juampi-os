// The edit() global: open the full-screen editor (src/editor.c) on an ext2 path.
// On Ctrl-X the editor saves and returns EDITOR_RUN, and we execute the file in
// the live interpreter — the edit -> save -> run loop, without leaving the box.

#include <console.h>
#include <editor.h>
#include <ext2.h>
#include <memory.h>

#include "lua.h"
#include "lauxlib.h"

// edit(path): edit `path`; if the user exits with Ctrl-X, load and run it.
static int l_edit(lua_State* L)
{
    const char* path = luaL_checkstring(L, 1);
    int action = editor_run(path);
    if (action != EDITOR_RUN) {
        return 0;
    }

    size_t size = 0;
    void* data = ext2_read_path(path, &size);
    if (data == NULL) {
        lua_pushfstring(L, "edit: cannot re-read %s to run", path);
        return lua_error(L);
    }
    int status = luaL_loadbuffer(L, (const char*)data, size, path);
    heap_free(heap_default(), data);
    if (status == LUA_OK) {
        status = lua_pcall(L, 0, 0, 0);
    }
    if (status != LUA_OK) {
        console_print(lua_tostring(L, -1));
        console_print("\n");
        lua_pop(L, 1);
    }
    return 0;
}

void lua_edit_open(lua_State* L)
{
    lua_pushcfunction(L, l_edit);
    lua_setglobal(L, "edit");
}
