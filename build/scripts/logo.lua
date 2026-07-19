-- logo.lua: display the JP-OS boot logo centred on the framebuffer.
-- Run with run("logo.lua"). The image is a QOI file (logo.qoi) shipped as a
-- Limine module and decoded by the kernel's fb.image().

if fb.width() == 0 then
    print("logo.lua: no framebuffer available")
    return
end

local w, h = fb.image("logo.qoi") -- default position: centred
print(string.format("logo.lua: %dx%d QOI image, centred", w, h))
