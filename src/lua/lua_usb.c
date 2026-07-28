// The `usb` library: introspection over the xHCI stack — controller state, the
// enumerated device tree (including devices behind hubs), and the HID
// keyboard/mouse status. The storage side of USB lives in `disk`
// (disk.usb_read/usb_write/usb_info), mirroring how NVMe and ATA are exposed.

#include <xhci.h>
#include <luadoc.h>

#include "lua.h"
#include "lauxlib.h"

// usb.info() -> present, ports, irq_driven, events.
static int l_info(lua_State* L)
{
    lua_pushboolean(L, xhci_present());
    lua_pushinteger(L, xhci_ports());
    lua_pushboolean(L, xhci_irq_driven());
    lua_pushinteger(L, (lua_Integer)xhci_irq_count());
    return 4;
}

// usb.devices() -> array of {vid=, pid=, class=} for every enumerated device.
static int l_devices(lua_State* L)
{
    lua_createtable(L, (int)xhci_device_count(), 0);
    for (uint32_t i = 0; i < xhci_device_count(); i++) {
        uint16_t vid, pid;
        uint8_t cls;
        if (!xhci_device_info(i, &vid, &pid, &cls)) {
            break;
        }
        lua_createtable(L, 0, 3);
        lua_pushinteger(L, vid);
        lua_setfield(L, -2, "vid");
        lua_pushinteger(L, pid);
        lua_setfield(L, -2, "pid");
        lua_pushinteger(L, cls);
        lua_setfield(L, -2, "class");
        lua_rawseti(L, -2, (lua_Integer)i + 1);
    }
    return 1;
}

// usb.kbd() -> present, reports received.
static int l_kbd(lua_State* L)
{
    lua_pushboolean(L, xhci_kbd_present());
    lua_pushinteger(L, (lua_Integer)xhci_kbd_reports());
    return 2;
}

// usb.mouse() -> present, reports received.
static int l_mouse(lua_State* L)
{
    lua_pushboolean(L, xhci_mouse_present());
    lua_pushinteger(L, (lua_Integer)xhci_mouse_reports());
    return 2;
}

static const lua_fndoc usblib[] = {
        {"info", l_info, "xHCI controller state.",
         .rets = {{"present", "boolean", "true if the controller is up"},
                  {"ports", "number", "root-hub port count"},
                  {"irq", "boolean", "true when MSI-X interrupt-driven"},
                  {"events", "number", "interrupts taken so far"}}},
        {"devices", l_devices,
         "Enumerated USB devices (including behind hubs).",
         .rets = {{"list", "table",
                   "array of {vid, pid, class}; class 9 is a hub"}}},
        {"kbd", l_kbd, "USB HID keyboard status.",
         .rets = {{"present", "boolean", "true if a boot keyboard is live"},
                  {"reports", "number", "input reports received"}}},
        {"mouse", l_mouse, "USB HID mouse status.",
         .rets = {{"present", "boolean", "true if a boot mouse is live"},
                  {"reports", "number", "input reports received"}}},
        {0},
};

int luaopen_usb(lua_State* L)
{
    luadoc_newlib(L, usblib);
    return 1;
}
