// The `disk` library: raw 512-byte block access to the ATA data disk, the
// low-level counterpart to the `fs` (ext2) library — mirroring how `pci`
// exposes raw config space beneath the `fs`-style conveniences.

#include <ata.h>

#include "lua.h"
#include "lauxlib.h"

static int l_present(lua_State* L)
{
    lua_pushboolean(L, ata_present());
    return 1;
}

static int l_sectors(lua_State* L)
{
    lua_pushinteger(L, ata_sectors());
    return 1;
}

// disk.read(lba [,count=1]) -> string | nil,err. Read `count` 512-byte sectors
// (capped) starting at `lba`, returned as a byte string.
static int l_read(lua_State* L)
{
    lua_Integer lba = luaL_checkinteger(L, 1);
    lua_Integer count = luaL_optinteger(L, 2, 1);
    if (lba < 0) {
        return luaL_error(L, "disk.read: negative lba");
    }
    if (count < 1) {
        count = 1;
    }
    if (count > 4096) {
        count = 4096; // cap a single read at 2 MiB
    }
    size_t bytes = (size_t)count * 512;
    luaL_Buffer b;
    char* p = luaL_buffinitsize(L, &b, bytes);
    if (!ata_read((uint64_t)lba, (uint32_t)count, p)) {
        luaL_pushresultsize(&b, 0);
        lua_pop(L, 1);
        lua_pushnil(L);
        lua_pushstring(L, "disk read failed");
        return 2;
    }
    luaL_pushresultsize(&b, bytes);
    return 1;
}

static const luaL_Reg disklib[] = {
        {"present", l_present},
        {"sectors", l_sectors},
        {"read", l_read},
        {NULL, NULL},
};

int luaopen_disk(lua_State* L)
{
    luaL_newlib(L, disklib);
    return 1;
}
