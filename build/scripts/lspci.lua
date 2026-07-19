-- lspci.lua: list PCI devices, like lspci. Run with run("lspci.lua").

local class_name = {
    [0x00] = "unclassified",
    [0x01] = "storage",
    [0x02] = "network",
    [0x03] = "display",
    [0x04] = "multimedia",
    [0x05] = "memory",
    [0x06] = "bridge",
    [0x07] = "communication",
    [0x08] = "system peripheral",
    [0x09] = "input",
    [0x0c] = "serial bus",
    [0x0d] = "wireless",
}

local devices = pci.list()
print(string.format("%d PCI device(s):", #devices))
for _, d in ipairs(devices) do
    print(string.format("  %02x:%02x.%x  %04x:%04x  class %02x.%02x  %s",
        d.bus, d.dev, d.func, d.vendor, d.device, d.class, d.subclass,
        class_name[d.class] or "?"))
end
