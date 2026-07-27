// Self-documenting Lua library registration (see luadoc.h). Builds the library
// table plus a `__doc` sidecar the help browser reads.

#include <luadoc.h>

#include "lauxlib.h"

// Push an array of {name=, type=, doc=} tables describing the first `max` slots
// of `args`, stopping early at a zeroed entry. Bounding by `max` matters when an
// arg/ret list fills its whole fixed-size array (no room for a NULL sentinel).
// Leaves the array table on the stack.
static void push_args(lua_State* L, const lua_arg* args, int max)
{
    lua_newtable(L);
    int n = 0;
    for (int i = 0; i < max && args[i].name != NULL; i++) {
        const lua_arg* a = &args[i];
        lua_createtable(L, 0, 3);
        lua_pushstring(L, a->name);
        lua_setfield(L, -2, "name");
        lua_pushstring(L, a->type != NULL ? a->type : "any");
        lua_setfield(L, -2, "type");
        lua_pushstring(L, a->doc != NULL ? a->doc : "");
        lua_setfield(L, -2, "doc");
        lua_rawseti(L, -2, ++n); // array[n] = entry
    }
}

void luadoc_newlib(lua_State* L, const lua_fndoc* fns)
{
    lua_newtable(L); // the library table   (stack: lib)
    lua_newtable(L); // the __doc table     (stack: lib, __doc)
    for (const lua_fndoc* f = fns; f->name != NULL; f++) {
        // lib[name] = fn
        lua_pushcfunction(L, f->fn);
        lua_setfield(L, -3, f->name); // lib is at -3 (lib, __doc, fn)

        // __doc[name] = { doc=, params=, returns= }
        lua_createtable(L, 0, 3);
        lua_pushstring(L, f->doc != NULL ? f->doc : "");
        lua_setfield(L, -2, "doc");
        push_args(L, f->args, LUADOC_MAX_ARGS);
        lua_setfield(L, -2, "params");
        push_args(L, f->rets, LUADOC_MAX_RETS);
        lua_setfield(L, -2, "returns");
        lua_setfield(L, -2, f->name); // __doc[name] = entry (__doc at -2)
    }
    lua_setfield(L, -2, "__doc"); // lib.__doc = __doc; leaves lib on top
}
