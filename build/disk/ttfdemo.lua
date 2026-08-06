-- ttfdemo.lua — scalable anti-aliased text via the TrueType rasterizer.
-- Draws the same string at a range of sizes plus a paragraph, to show off the
-- coverage-based anti-aliasing (fb.ttf) versus the fixed 8x16 bitmap (fb.text).
-- Run from the shell with:  run("ttfdemo.lua")

fb.buffer(true) -- draw off-screen, show it in one flip (no flicker)
fb.clear(0x101018)

-- The same word at growing sizes; y is the text baseline.
local y = 64
for _, sz in ipairs({ 16, 24, 36, 54, 80 }) do
  local adv = fb.ttf(30, y, "juampiOS  Ag!?", sz, 0x9fe0ff)
  fb.ttf(30 + adv + 20, y, tostring(sz) .. "px", math.floor(sz / 2), 0x556070)
  y = y + sz + 10
end

-- A paragraph in a warm colour, and the bitmap font underneath for comparison.
fb.ttf(30, y + 24, "the quick brown fox jumps over the lazy dog", 22, 0xffd080)
fb.text(30, y + 44, "fb.text: the built-in 8x16 bitmap font", 0x00ff88)

fb.flip()
print("TTF_DEMO_OK") -- marker so tests/boot-smoke.sh can assert it ran
