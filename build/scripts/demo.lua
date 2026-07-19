-- demo.lua: a small framebuffer graphics demo. Run it from the shell with
-- run("demo.lua"). It draws over the console (they share the screen).

local W, H = fb.width(), fb.height()

fb.clear(0x101018) -- dark background

-- A colour gradient band.
for x = 0, W - 1 do
    local r = (x * 255) // W
    fb.rect(x, 0, 1, 80, (r << 16) | (0x40 << 8) | (255 - r))
end

-- Concentric-ish star of lines from the centre.
local cx, cy = W // 2, H // 2
for a = 0, 359, 12 do
    local rad = a * 3.14159 / 180
    local len = 160
    fb.line(cx, cy, cx + math.floor(len * math.cos(rad)),
        cy + math.floor(len * math.sin(rad)), 0x30ff90)
end

-- A framed box.
fb.rect(cx - 200, cy - 120, 400, 240, 0x202030)
fb.line(cx - 200, cy - 120, cx + 200, cy - 120, 0xffffff)
fb.line(cx - 200, cy + 120, cx + 200, cy + 120, 0xffffff)

print(string.format("drew a %dx%d framebuffer demo", W, H))
