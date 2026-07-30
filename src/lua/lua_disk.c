// The `disk` library: raw 512-byte block access to the ATA data disk, the
// low-level counterpart to the `fs` (ext2) library — mirroring how `pci`
// exposes raw config space beneath the `fs`-style conveniences.

#include <ata.h>
#include <nvme.h>
#include <xhci.h>
#include <luadoc.h>

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

// disk.nvme_read(lba [,count=1]) -> string | nil,err. The NVMe counterpart of
// disk.read, in this namespace's logical-block units (see disk.nvme_info).
static int l_nvme_read(lua_State* L)
{
    lua_Integer lba = luaL_checkinteger(L, 1);
    lua_Integer count = luaL_optinteger(L, 2, 1);
    if (lba < 0) {
        return luaL_error(L, "disk.nvme_read: negative lba");
    }
    if (count < 1) {
        count = 1;
    }
    uint32_t bs = nvme_block_size();
    if (bs == 0) {
        lua_pushnil(L);
        lua_pushstring(L, "no nvme controller");
        return 2;
    }
    size_t per = 0x200000u / bs; // cap a single call at ~2 MiB
    if (per < 1) {
        per = 1;
    }
    if ((size_t)count > per) {
        count = (lua_Integer)per;
    }
    size_t bytes = (size_t)count * bs;
    luaL_Buffer b;
    char* p = luaL_buffinitsize(L, &b, bytes);
    if (!nvme_read((uint64_t)lba, (uint32_t)count, p)) {
        luaL_pushresultsize(&b, 0);
        lua_pop(L, 1);
        lua_pushnil(L);
        lua_pushstring(L, "nvme read failed");
        return 2;
    }
    luaL_pushresultsize(&b, bytes);
    return 1;
}

// disk.nvme_info() -> present, model, blocks, block_size.
static int l_nvme_info(lua_State* L)
{
    lua_pushboolean(L, nvme_present());
    lua_pushstring(L, nvme_model());
    lua_pushinteger(L, (lua_Integer)nvme_blocks());
    lua_pushinteger(L, nvme_block_size());
    return 4;
}

// disk.usb_read(lba [,count=1]) -> string | nil,err. Raw logical blocks from
// the USB mass-storage stick (through the SCSI/BOT path).
static int l_usb_read(lua_State* L)
{
    lua_Integer lba = luaL_checkinteger(L, 1);
    lua_Integer count = luaL_optinteger(L, 2, 1);
    if (lba < 0) {
        return luaL_error(L, "disk.usb_read: negative lba");
    }
    if (count < 1) {
        count = 1;
    }
    uint32_t bs = xhci_msc_block_size();
    if (!xhci_msc_ready() || bs == 0) {
        lua_pushnil(L);
        lua_pushstring(L, "no usb mass-storage device");
        return 2;
    }
    size_t per = 0x200000u / bs; // cap a single call at ~2 MiB
    if ((size_t)count > per) {
        count = (lua_Integer)per;
    }
    size_t bytes = (size_t)count * bs;
    luaL_Buffer b;
    char* p = luaL_buffinitsize(L, &b, bytes);
    if (!xhci_msc_blockdev()->read((uint64_t)lba, (uint32_t)count, p)) {
        luaL_pushresultsize(&b, 0);
        lua_pop(L, 1);
        lua_pushnil(L);
        lua_pushstring(L, "usb read failed");
        return 2;
    }
    luaL_pushresultsize(&b, bytes);
    return 1;
}

// disk.usb_info() -> present, blocks, block_size.
static int l_usb_info(lua_State* L)
{
    lua_pushboolean(L, xhci_msc_ready());
    lua_pushinteger(L, (lua_Integer)xhci_msc_blocks());
    lua_pushinteger(L, xhci_msc_block_size());
    return 3;
}

// Shared body for the raw block writers: `data` must be a whole number of
// `bs`-byte blocks; writes it at `lba` via `write`. Returns true | nil,err.
static int raw_write(lua_State* L, uint32_t bs,
                     bool (*write)(uint64_t, uint32_t, const void*),
                     const char* what)
{
    lua_Integer lba = luaL_checkinteger(L, 1);
    size_t len = 0;
    const char* data = luaL_checklstring(L, 2, &len);
    if (lba < 0) {
        return luaL_error(L, "%s: negative lba", what);
    }
    if (bs == 0) {
        lua_pushnil(L);
        lua_pushstring(L, "device not present");
        return 2;
    }
    if (len == 0 || len % bs != 0 || len > 0x200000u) {
        return luaL_error(L, "%s: data must be 1..4096 whole %d-byte blocks",
                          what, (int)bs);
    }
    if (!write((uint64_t)lba, (uint32_t)(len / bs), data)) {
        lua_pushnil(L);
        lua_pushstring(L, "write failed");
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

// disk.nvme_write(lba, data) -> true | nil,err.
static int l_nvme_write(lua_State* L)
{
    return raw_write(L, nvme_block_size(), nvme_write, "disk.nvme_write");
}

// disk.usb_write(lba, data) -> true | nil,err.
static int l_usb_write(lua_State* L)
{
    return raw_write(L, xhci_msc_block_size(), xhci_msc_blockdev()->write,
                     "disk.usb_write");
}

static const struct lua_fndoc disklib[] = {
        {"present", l_present, "Whether an ATA data disk is attached.",
         .rets = {{"ok", "boolean", "true if a disk is present"}}},
        {"sectors", l_sectors, "Number of 512-byte sectors on the disk.",
         .rets = {{"n", "number", "sector count"}}},
        {"read", l_read, "Read raw 512-byte sectors as a byte string.",
         .args = {{"lba", "number", "starting logical block address"},
                  {"count", "number?", "sectors to read (default 1, cap 4096)"}},
         .rets = {{"data", "string?", "the bytes, or nil on error"},
                  {"err", "string?", "error message when data is nil"}}},
        {"nvme_info", l_nvme_info, "NVMe controller presence and geometry.",
         .rets = {{"present", "boolean", "true if an NVMe controller is up"},
                  {"model", "string", "controller model string"},
                  {"blocks", "number", "logical blocks in namespace 1"},
                  {"block_size", "number", "bytes per logical block"}}},
        {"nvme_read", l_nvme_read,
         "Read raw NVMe logical blocks as a byte string.",
         .args = {{"lba", "number", "starting logical block address"},
                  {"count", "number?", "blocks to read (default 1)"}},
         .rets = {{"data", "string?", "the bytes, or nil on error"},
                  {"err", "string?", "error message when data is nil"}}},
        {"usb_info", l_usb_info, "USB mass-storage presence and geometry.",
         .rets = {{"present", "boolean", "true if a stick is configured"},
                  {"blocks", "number", "logical blocks on the device"},
                  {"block_size", "number", "bytes per logical block"}}},
        {"usb_read", l_usb_read,
         "Read raw USB mass-storage blocks as a byte string.",
         .args = {{"lba", "number", "starting logical block address"},
                  {"count", "number?", "blocks to read (default 1)"}},
         .rets = {{"data", "string?", "the bytes, or nil on error"},
                  {"err", "string?", "error message when data is nil"}}},
        {"nvme_write", l_nvme_write,
         "Write whole raw blocks to the NVMe namespace.",
         .args = {{"lba", "number", "starting logical block address"},
                  {"data", "string", "a whole number of blocks of bytes"}},
         .rets = {{"ok", "boolean?", "true, or nil on error"},
                  {"err", "string?", "error message when ok is nil"}}},
        {"usb_write", l_usb_write,
         "Write whole raw blocks to the USB mass-storage device.",
         .args = {{"lba", "number", "starting logical block address"},
                  {"data", "string", "a whole number of blocks of bytes"}},
         .rets = {{"ok", "boolean?", "true, or nil on error"},
                  {"err", "string?", "error message when ok is nil"}}},
        {0},
};

int luaopen_disk(lua_State* L)
{
    luadoc_newlib(L, disklib);
    return 1;
}
