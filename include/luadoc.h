#ifndef __LUADOC_H
#define __LUADOC_H

#include "lua.h"

// A self-documenting registration layer for the kernel's Lua libraries: instead
// of a bare luaL_Reg name->function table, each function carries a docstring
// and typed parameter / return metadata. luadoc_newlib() registers the
// functions AND attaches a `__doc` sub-table describing them, which the help
// browser (build/scripts/prelude.lua) renders as signatures + descriptions.

// One documented parameter or return value. `type` is a LuaLS-style type string
// ("number", "string", "boolean", "table", "any", "number?", "string|number").
struct lua_arg {
    const char* name;
    const char* type;
    const char* doc;
};

#define LUADOC_MAX_ARGS 8
#define LUADOC_MAX_RETS 4

// A documented library function. `args`/`rets` are terminated by a zeroed entry
// (name == NULL), so a partial list just leaves the trailing slots zero. A
// lua_fndoc[] array is likewise terminated by a zeroed entry.
struct lua_fndoc {
    const char* name;
    lua_CFunction fn;
    const char* doc;
    struct lua_arg args[LUADOC_MAX_ARGS];
    struct lua_arg rets[LUADOC_MAX_RETS];
};

// Create a new library table (like luaL_newlib) from a zero-terminated
// lua_fndoc[]: register each function under its name and attach `__doc[name] =
// {doc=, params={{name,type,doc}...}, returns={...}}`. Leaves the library table
// on the stack.
void luadoc_newlib(lua_State* L, const struct lua_fndoc* fns);

#endif
