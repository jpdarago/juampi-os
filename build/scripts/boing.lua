-- boing.lua: the Amiga "Boing Ball" — a red/white checkered ball bouncing
-- around the framebuffer at constant speed, the checker pattern scrolling to
-- fake the spin, over the classic magenta grid. A sampler and stress test for
-- the fb library (fb.clear/line/rect) and k.ns() frame pacing.
-- Run with run("boing.lua"); it plays for a few seconds then returns.

if fb.width() == 0 then
    print("boing.lua: no framebuffer available")
    return
end

local W, H = fb.width(), fb.height()
local R = math.min(W, H) // 6         -- ball radius
local cell = math.max(R // 4, 1)      -- checker cell size
local RED, WHITE = 0xd42020, 0xf0f0f0
local BG, GRID = 0x101018, 0x582c8c   -- dark wall, magenta grid

local bx, by = W // 3, H // 3
local vx, vy = 7, 5                    -- constant velocity (no gravity)
local rot = 0                         -- checker scroll offset (fake rotation)

local floor, sqrt, min, max = math.floor, math.sqrt, math.min, math.max

local function shade(rgb, f)
    local r = floor(((rgb >> 16) & 0xff) * f)
    local g = floor(((rgb >> 8) & 0xff) * f)
    local b = floor((rgb & 0xff) * f)
    return (r << 16) | (g << 8) | b
end

-- Filled ellipse via horizontal spans (used for the floor shadow).
local function oval(cx, cy, rx, ry, rgb)
    for dy = -ry, ry do
        local yy = cy + dy
        if yy >= 0 and yy < H then
            local hw = floor(rx * sqrt(1 - (dy / ry) * (dy / ry)))
            fb.rect(cx - hw, yy, 2 * hw + 1, 1, rgb)
        end
    end
end

-- The ball: for each scanline, walk the checker cells left to right, colour
-- each by the (scrolling) checker parity and shade it by the sphere normal so
-- it reads as a lit 3-D ball.
local function draw_ball(cx, cy)
    for dy = -R, R do
        local yy = cy + dy
        if yy >= 0 and yy < H then
            local hw = floor(sqrt(R * R - dy * dy))
            local x, xend = cx - hw, cx + hw
            local cj = dy // cell
            local my = dy / R
            while x <= xend do
                local ci = (x - cx + rot) // cell
                local seg_end = min(cx - rot + (ci + 1) * cell - 1, xend)
                local base = (((ci + cj) & 1) == 0) and RED or WHITE
                local mx = ((x + seg_end) / 2 - cx) / R
                local nz2 = 1 - mx * mx - my * my
                local f = 0.35
                if nz2 > 0 then
                    local d = (-mx - my) * 0.35 + sqrt(nz2) * 0.9
                    f = min(1, 0.35 + 0.65 * max(0, d))
                end
                fb.rect(x, yy, seg_end - x + 1, 1, shade(base, f))
                x = seg_end + 1
            end
        end
    end
end

for _ = 1, 480 do
    local t = k.ns()
    -- Backdrop: wall + grid.
    fb.clear(BG)
    for gx = 0, W, 48 do fb.line(gx, 0, gx, H - 1, GRID) end
    for gy = 0, H, 48 do fb.line(0, gy, W - 1, gy, GRID) end
    -- Physics: bounce inside the box (constant speed, like the real Boing).
    bx, by = bx + vx, by + vy
    if bx < R then bx, vx = R, -vx elseif bx > W - R then bx, vx = W - R, -vx end
    if by < R then by, vy = R, -vy elseif by > H - R then by, vy = H - R, -vy end
    -- Shadow tracks the ball along the floor, then the ball itself.
    oval(bx, H - 14, R, R // 5, 0x0a0a12)
    draw_ball(bx, by)
    rot = rot + 2
    -- Pace to ~50 fps.
    local nxt = t + 20000000
    while k.ns() < nxt do end
end

print("boing.lua: done")
