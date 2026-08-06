-- ttfdemo.lua — scalable anti-aliased text via the TrueType rasterizer.
-- Shows the same string at a range of sizes plus a paragraph, comparing the
-- coverage-based anti-aliasing (fb.ttf) with the fixed 8x16 bitmap (fb.text).
-- On the windowed desktop it opens a window; with no desktop it draws
-- full-screen. Run from the shell:  run("ttfdemo.lua")

local font = fb.loadfont("/font.ttf") -- GC-freed when it goes out of scope
local W, H = 600, 340

-- Draw the sample into the current target (the window canvas, or the fb).
local function frame()
  fb.clear(0x101018)
  local y = 40
  for _, sz in ipairs({ 16, 22, 30, 42, 54 }) do
    local adv = fb.ttf(font, 20, y, "juampiOS  Ag!?", sz, 0x9fe0ff)
    fb.ttf(font, 20 + adv + 16, y, tostring(sz) .. "px", math.floor(sz / 2),
      0x556070)
    y = y + sz + 8
  end
  fb.ttf(font, 20, y + 16, "the quick brown fox jumps over the lazy dog", 20,
    0xffd080)
  fb.text(20, y + 36, "fb.text: the built-in 8x16 bitmap font", 0x00ff88)
  -- A translucent panel over the text (the phase-2 blend pipeline): 0xAARRGGBB,
  -- alpha 0x80 ~= 50%, so you read the text through it.
  fb.blend(210, 54, 360, 96, 0x8030b0ff, "over")
end

print("TTF_DEMO_OK") -- marker for tests/boot-smoke.sh

if ui and ui.available and ui.available() then
  -- Desktop: the compositor owns the screen, so drawing straight to the
  -- framebuffer would be painted over — render into a canvas and show it in a
  -- window instead (built once, blitted each desktop frame).
  local cv = ui.canvas(W, H)
  cv:draw(frame)
  ui.open("TrueType demo", function() cv:show() end, W, H + 28)
else
  -- No desktop: draw straight to the framebuffer, double-buffered, and hold it
  -- briefly so it's visible before the shell prompt returns.
  if fb.width() == 0 then return end
  fb.buffer(true)
  frame()
  fb.flip()
  if k and k.sleep then k.sleep(4000) end
  fb.buffer(false)
end
