-- boing.lua: the Amiga "Boing Ball" — a red/white checkered sphere whose spin
-- axis is tilted to the side, bouncing around the classic magenta grid room.
-- The pattern is mapped onto a real sphere (columns curve toward the poles and
-- wrap around the back), so it reads as a globe rotating, not a texture sliding.
--
-- On the windowed desktop it animates inside a "Boing Ball" window (one physics
-- step + redraw per desktop frame, drawn into an off-screen ui.canvas that the
-- compositor blits); with no desktop it plays full-screen for a few seconds.

local floor, sqrt, sin, cos = math.floor, math.sqrt, math.sin, math.cos
local atan, min, max = math.atan, math.min, math.max

local RED, WHITE = 0xd42020, 0xe8e8e8
local BG, GRID = 0x101018, 0x582c8c -- dark wall, magenta grid
local NLON, NLAT = 16, 9            -- checker divisions (longitude, latitude)
local TILT = 0.42                   -- spin-axis lean, radians (~24 deg)

-- Render into a fixed-size canvas on the desktop, else the whole framebuffer.
local windowed = ui and ui.available and ui.available()
local W, H
if windowed then
    W, H = 520, 400
else
    if fb.width() == 0 then
        print("boing.lua: no framebuffer available")
        return
    end
    W, H = fb.width(), fb.height()
end
local R = min(W, H) // 7 -- ball radius

-- Shade an RGB colour by factor f in [0,1].
local function shade(rgb, f)
    local r = min(255, floor(((rgb >> 16) & 0xff) * f))
    local g = min(255, floor(((rgb >> 8) & 0xff) * f))
    local b = min(255, floor((rgb & 0xff) * f))
    return (r << 16) | (g << 8) | b
end

-- Precompute, for every pixel of the ball, its longitude on the sphere, the
-- parity of its latitude band, and the two possible shaded colours. The spin
-- axis is tilted by TILT in the screen plane, so the whole checker leans over.
local sT, cT = sin(TILT), cos(TILT)
local Lx, Ly, Lz = -0.5, -0.62, 0.6 -- light direction
local Llen = sqrt(Lx * Lx + Ly * Ly + Lz * Lz)
Lx, Ly, Lz = Lx / Llen, Ly / Llen, Lz / Llen
local KLON = NLON / (2 * math.pi)

local plon, ppar, pca, pcb = {}, {}, {}, {}
local rows = {}
local idx = 0
for dy = -R, R do
    local hw = floor(sqrt(R * R - dy * dy))
    rows[#rows + 1] = {dy = dy, hw = hw, start = idx + 1}
    local ny = dy / R
    for dx = -hw, hw do
        local nx = dx / R
        local s = 1 - nx * nx - ny * ny
        local nz = (s > 0) and sqrt(s) or 0
        local sinlat = nx * sT + ny * cT
        local lon = atan(nz, nx * cT - ny * sT)
        local lat_band = floor((sinlat + 1) * 0.5 * NLAT)
        local d = nx * Lx + ny * Ly + nz * Lz
        local f = 0.30 + 0.70 * max(0, d)
        idx = idx + 1
        plon[idx] = lon
        ppar[idx] = lat_band & 1
        pca[idx] = shade(RED, f)
        pcb[idx] = shade(WHITE, f)
    end
end

-- Draw the ball at (cx, cy) spun by phi: walk each row, recolour by longitude,
-- and coalesce equal-colour runs into single fb.rect spans.
local function draw_ball(cx, cy, phi)
    for r = 1, #rows do
        local row = rows[r]
        local yy = cy + row.dy
        if yy >= 0 and yy < H then
            local p = row.start
            local xx = cx - row.hw
            local runc, runx = nil, xx
            for _ = 0, 2 * row.hw do
                local band = floor((plon[p] + phi) * KLON)
                local col = (((band + ppar[p]) & 1) == 0) and pca[p] or pcb[p]
                if col ~= runc then
                    if runc then
                        fb.rect(runx, yy, xx - runx, 1, runc)
                    end
                    runc, runx = col, xx
                end
                p = p + 1
                xx = xx + 1
            end
            fb.rect(runx, yy, xx - runx, 1, runc)
        end
    end
end

-- Filled ellipse via horizontal spans (the floor shadow).
local function oval(cx, cy, rx, ry, rgb)
    for dy = -ry, ry do
        local yy = cy + dy
        if yy >= 0 and yy < H then
            local hw = floor(rx * sqrt(1 - (dy / ry) * (dy / ry)))
            fb.rect(cx - hw, yy, 2 * hw + 1, 1, rgb)
        end
    end
end

-- Mutable physics state, advanced one step per frame.
local bx, by = W // 3, H // 3
local vx, vy = 4, 3 -- velocity
local phi = 0       -- spin angle
local spin = 0.10   -- spin per frame; reverses on a side wall

-- A "boing" on every wall bounce, if there's an audio device (harmless
-- otherwise). pcall so a missing sound asset never breaks the animation.
local has_audio = audio and select(1, audio.info())
local function boing_sfx()
    if has_audio then pcall(audio.play, "boing.qoa", false, 0.7) end
end

-- Draw one frame (backdrop + physics step + shadow + ball) into the current
-- draw target (the canvas when windowed, the framebuffer otherwise).
local function frame()
    fb.clear(BG)
    for gx = 0, W, 48 do fb.line(gx, 0, gx, H - 1, GRID) end
    for gy = 0, H, 48 do fb.line(0, gy, W - 1, gy, GRID) end
    bx, by = bx + vx, by + vy
    if bx < R then
        bx, vx, spin = R, -vx, -spin
        boing_sfx()
    elseif bx > W - R then
        bx, vx, spin = W - R, -vx, -spin
        boing_sfx()
    end
    if by < R then
        by, vy = R, -vy
        boing_sfx()
    elseif by > H - R then
        by, vy = H - R, -vy
        boing_sfx()
    end
    oval(bx, H - 14, R, R // 5, 0x0a0a12)
    draw_ball(bx, by, phi)
    phi = phi + spin
end

if windowed then
    local cv = ui.canvas(W, H)
    -- Window sized to the canvas plus the 28px title bar, so it fits snugly.
    ui.open("Boing Ball", function()
        cv:draw(frame)
        cv:show()
    end, W, H + 28)
else
    fb.buffer(true)
    local stop = k.ns() + 14000000000 -- ~14 seconds
    while k.ns() < stop do
        local t = k.ns()
        frame()
        fb.flip()
        local nxt = t + 25000000 -- pace to ~40 fps
        while k.ns() < nxt do end
    end
    fb.buffer(false)
    print("boing.lua: done")
end
