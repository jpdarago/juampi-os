// The `fs` library: read-only access to the ext2 filesystem on the ATA data
// disk (src/ext2.c). Load, list, and stat files from Lua; run() also falls
// back to this to load scripts that live on disk rather than in a Limine module.

#include <ext2.h>
#include <luadoc.h>

#include "lua.h"
#include "lauxlib.h"

static int l_mounted(lua_State* L)
{
    lua_pushboolean(L, ext2_mounted());
    return 1;
}

// fs.read(path) -> string | nil,err.
static int l_read(lua_State* L)
{
    const char* path = luaL_checkstring(L, 1);
    size_t size = 0;
    void* data = ext2_read_path(path, &size);
    if (data == NULL) {
        lua_pushnil(L);
        lua_pushfstring(L, "no such file: %s", path);
        return 2;
    }
    lua_pushlstring(L, (const char*)data, size);
    ext2_free(data);
    return 1;
}

static const char* type_name(uint8_t t)
{
    switch (t) {
    case 1:
        return "file";
    case 2:
        return "dir";
    case 3:
        return "chardev";
    case 4:
        return "blockdev";
    case 5:
        return "fifo";
    case 6:
        return "socket";
    case 7:
        return "symlink";
    default:
        return "unknown";
    }
}

// Append {name=, inode=, type=} to the result array sitting on top of the stack.
struct list_ctx {
    lua_State* L;
    int n;
};

static void emit(void* ctx, const char* name, uint32_t inode, uint8_t type)
{
    struct list_ctx* c = ctx;
    lua_State* L = c->L;
    lua_createtable(L, 0, 3);
    lua_pushstring(L, name);
    lua_setfield(L, -2, "name");
    lua_pushinteger(L, inode);
    lua_setfield(L, -2, "inode");
    lua_pushstring(L, type_name(type));
    lua_setfield(L, -2, "type");
    lua_rawseti(L, -2, ++c->n); // result[n] = entry (array is just below)
}

// fs.list(path) -> array of {name, inode, type} | nil,err.
static int l_list(lua_State* L)
{
    const char* path = luaL_checkstring(L, 1);
    lua_newtable(L);
    struct list_ctx c = {.L = L, .n = 0};
    if (ext2_list(path, emit, &c) < 0) {
        lua_pop(L, 1);
        lua_pushnil(L);
        lua_pushfstring(L, "not a directory: %s", path);
        return 2;
    }
    return 1;
}

// fs.stat(path) -> {size, dir, inode, mode} | nil,err.
static int l_stat(lua_State* L)
{
    const char* path = luaL_checkstring(L, 1);
    struct ext2_stat st;
    if (!ext2_stat_path(path, &st)) {
        lua_pushnil(L);
        lua_pushfstring(L, "no such path: %s", path);
        return 2;
    }
    lua_createtable(L, 0, 4);
    lua_pushinteger(L, st.size);
    lua_setfield(L, -2, "size");
    lua_pushboolean(L, st.is_dir);
    lua_setfield(L, -2, "dir");
    lua_pushinteger(L, st.inode);
    lua_setfield(L, -2, "inode");
    lua_pushinteger(L, st.mode);
    lua_setfield(L, -2, "mode");
    return 1;
}

static int l_exists(lua_State* L)
{
    const char* path = luaL_checkstring(L, 1);
    struct ext2_stat st;
    lua_pushboolean(L, ext2_stat_path(path, &st));
    return 1;
}

// fs.write(path, data) -> true | nil,err. Create or overwrite a regular file.
static int l_write(lua_State* L)
{
    const char* path = luaL_checkstring(L, 1);
    size_t len = 0;
    const char* data = luaL_checklstring(L, 2, &len);
    if (!ext2_write_file(path, data, len)) {
        lua_pushnil(L);
        lua_pushfstring(L, "could not write: %s", path);
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

// fs.mkdir(path) -> true | nil,err.
static int l_mkdir(lua_State* L)
{
    const char* path = luaL_checkstring(L, 1);
    if (!ext2_mkdir(path)) {
        lua_pushnil(L);
        lua_pushfstring(L, "could not mkdir: %s", path);
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

// fs.remove(path) -> true | nil,err. Delete a file or an empty directory.
static int l_remove(lua_State* L)
{
    const char* path = luaL_checkstring(L, 1);
    if (!ext2_remove(path)) {
        lua_pushnil(L);
        lua_pushfstring(L, "could not remove: %s", path);
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static const struct lua_fndoc fslib[] = {
        {"mounted", l_mounted, "Whether the ext2 data disk is mounted.",
         .rets = {{"ok", "boolean", "true if a filesystem is mounted"}}},
        {"read", l_read, "Read a whole file into a string.",
         .args = {{"path", "string", "absolute path on the data disk"}},
         .rets = {{"data", "string?", "file contents, or nil on error"},
                  {"err", "string?", "error message when data is nil"}}},
        {"list", l_list, "List a directory.",
         .args = {{"path", "string", "directory path"}},
         .rets = {{"entries", "table?",
                   "array of {name, inode, type}, or nil on error"},
                  {"err", "string?", "error message when entries is nil"}}},
        {"stat", l_stat, "Stat a path.",
         .args = {{"path", "string", "path to stat"}},
         .rets = {{"info", "table?", "{size, dir, inode, mode}, or nil"},
                  {"err", "string?", "error message when info is nil"}}},
        {"exists", l_exists, "Whether a path resolves.",
         .args = {{"path", "string", "path to check"}},
         .rets = {{"ok", "boolean", "true if the path exists"}}},
        {"write", l_write, "Create or overwrite a regular file.",
         .args = {{"path", "string", "file path"},
                  {"data", "string", "bytes to write"}},
         .rets = {{"ok", "boolean?", "true on success, else nil"},
                  {"err", "string?", "error message on failure"}}},
        {"mkdir", l_mkdir, "Create a directory (its parent must exist).",
         .args = {{"path", "string", "directory path"}},
         .rets = {{"ok", "boolean?", "true on success, else nil"},
                  {"err", "string?", "error message on failure"}}},
        {"remove", l_remove, "Delete a file or an empty directory.",
         .args = {{"path", "string", "path to remove"}},
         .rets = {{"ok", "boolean?", "true on success, else nil"},
                  {"err", "string?", "error message on failure"}}},
        {0},
};

int luaopen_fs(lua_State* L)
{
    luadoc_newlib(L, fslib);
    return 1;
}
