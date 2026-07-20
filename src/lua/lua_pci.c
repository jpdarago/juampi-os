// The `pci` library: enumerate and poke PCI configuration space from Lua.
// Presentation (class names, formatting) is left to Lua — see lspci.lua.

#include <pci.h>

#include "lua.h"
#include "lauxlib.h"

static int l_read(lua_State* L)
{
    lua_pushinteger(L,
                    pci_read32((uint8_t)luaL_checkinteger(L, 1),
                               (uint8_t)luaL_checkinteger(L, 2),
                               (uint8_t)luaL_checkinteger(L, 3),
                               (uint8_t)luaL_checkinteger(L, 4)));
    return 1;
}

static int l_write(lua_State* L)
{
    pci_write32((uint8_t)luaL_checkinteger(L, 1),
                (uint8_t)luaL_checkinteger(L, 2),
                (uint8_t)luaL_checkinteger(L, 3),
                (uint8_t)luaL_checkinteger(L, 4),
                (uint32_t)luaL_checkinteger(L, 5));
    return 0;
}

// Push a device descriptor table {bus, dev, func, vendor, device, class,
// subclass, prog_if, header} for the function at (bus, dev, func).
static void push_device(lua_State* L, uint8_t bus, uint8_t dev, uint8_t func)
{
    uint32_t id = pci_read32(bus, dev, func, 0x00);
    uint32_t cls = pci_read32(bus, dev, func, 0x08);
    uint32_t hdr = pci_read32(bus, dev, func, 0x0C);

    lua_createtable(L, 0, 9);
#define FIELD(name, val)                                                       \
    do {                                                                       \
        lua_pushinteger(L, (val));                                \
        lua_setfield(L, -2, name);                                             \
    } while (0)
    FIELD("bus", bus);
    FIELD("dev", dev);
    FIELD("func", func);
    FIELD("vendor", id & 0xFFFF);
    FIELD("device", (id >> 16) & 0xFFFF);
    FIELD("class", (cls >> 24) & 0xFF);
    FIELD("subclass", (cls >> 16) & 0xFF);
    FIELD("prog_if", (cls >> 8) & 0xFF);
    FIELD("header", (hdr >> 16) & 0xFF);
#undef FIELD
}

// pci.list() -> array of device descriptor tables (a flat scan of all buses).
static int l_list(lua_State* L)
{
    lua_newtable(L);
    int n = 0;
    for (int bus = 0; bus < 256; bus++) {
        for (int dev = 0; dev < 32; dev++) {
            if ((pci_read32(bus, dev, 0, 0) & 0xFFFF) == 0xFFFF) {
                continue; // no device
            }
            int header = (pci_read32(bus, dev, 0, 0x0C) >> 16) & 0xFF;
            int funcs = (header & 0x80) ? 8 : 1; // multi-function?
            for (int func = 0; func < funcs; func++) {
                if ((pci_read32(bus, dev, func, 0) & 0xFFFF) == 0xFFFF) {
                    continue;
                }
                push_device(L, bus, dev, func);
                lua_rawseti(L, -2, ++n);
            }
        }
    }
    return 1;
}

static const luaL_Reg pcilib[] = {
        {"read", l_read},
        {"write", l_write},
        {"list", l_list},
        {NULL, NULL},
};

int luaopen_pci(lua_State* L)
{
    luaL_newlib(L, pcilib);
    return 1;
}
