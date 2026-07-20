-- raytracer.lua: a tiny raytracer rendered across every core (an M8 + M9
-- showcase). Each core traces a horizontal band straight into the framebuffer
-- via fb.canvas(), so the image fills in band by band, in parallel. The scene
-- is three spheres on a checkered floor under a sky gradient, with hard shadows.
--   fb.setmode(1024, 768)   -- optional: crank the resolution first
--   run("raytracer.lua")

-- The render function is serialized to bytecode and run on each core, so it must
-- be self-contained (no upvalues) and take everything as arguments.
local function render(cpu, canvas, W, H, pitch, nc, rs, gs, bs)
    local sqrt, floor, max = math.sqrt, math.floor, math.max
    -- Spheres: {cx, cy, cz, radius, r, g, b}. The floor is the plane y = 0.
    local S = {
        { -1.1, 1.0, -3.0, 1.0, 0.9, 0.2, 0.2 },
        { 1.2, 1.0, -3.6, 1.0, 0.2, 0.45, 0.9 },
        { 0.1, 0.6, -2.0, 0.6, 0.95, 0.8, 0.2 },
    }
    local ns = #S
    local lx, ly, lz = 0.6, 0.9, 0.3 -- light direction
    local ll = sqrt(lx * lx + ly * ly + lz * lz)
    lx, ly, lz = lx / ll, ly / ll, lz / ll
    local ox, oy, oz = 0.0, 1.3, 1.0 -- camera
    local aspect = W / H

    local lo = (H // nc) * cpu
    local hi = (cpu == nc - 1) and H or (H // nc) * (cpu + 1)

    for y = lo, hi - 1 do
        local sy = 1 - 2 * (y + 0.5) / H
        for x = 0, W - 1 do
            local sx = (2 * (x + 0.5) / W - 1) * aspect
            local dx, dy, dz = sx, sy, -1.0
            local dl = sqrt(dx * dx + dy * dy + dz * dz)
            dx, dy, dz = dx / dl, dy / dl, dz / dl

            local best, hr, hg, hb = 1e30, 0, 0, 0
            local nx, ny, nz, px, py, pz = 0, 0, 0, 0, 0, 0
            for i = 1, ns do
                local s = S[i]
                local ocx, ocy, ocz = ox - s[1], oy - s[2], oz - s[3]
                local b = ocx * dx + ocy * dy + ocz * dz
                local c = ocx * ocx + ocy * ocy + ocz * ocz - s[4] * s[4]
                local disc = b * b - c
                if disc > 0 then
                    local t = -b - sqrt(disc)
                    if t > 0.001 and t < best then
                        best = t
                        px, py, pz = ox + dx * t, oy + dy * t, oz + dz * t
                        nx = (px - s[1]) / s[4]
                        ny = (py - s[2]) / s[4]
                        nz = (pz - s[3]) / s[4]
                        hr, hg, hb = s[5], s[6], s[7]
                    end
                end
            end
            if dy < -1e-4 then
                local t = -oy / dy
                if t > 0.001 and t < best then
                    best = t
                    px, py, pz = ox + dx * t, oy + dy * t, oz + dz * t
                    nx, ny, nz = 0, 1, 0
                    if (floor(px) + floor(pz)) & 1 == 0 then
                        hr, hg, hb = 0.9, 0.9, 0.9
                    else
                        hr, hg, hb = 0.25, 0.25, 0.32
                    end
                end
            end

            local cr, cg, cb
            if best < 1e29 then
                local diff = max(0, nx * lx + ny * ly + nz * lz)
                local shadow = 1.0
                local sxo, syo, szo = px + nx * 0.01, py + ny * 0.01,
                    pz + nz * 0.01
                for i = 1, ns do
                    local s = S[i]
                    local ocx, ocy, ocz = sxo - s[1], syo - s[2], szo - s[3]
                    local b = ocx * lx + ocy * ly + ocz * lz
                    local c = ocx * ocx + ocy * ocy + ocz * ocz - s[4] * s[4]
                    local disc = b * b - c
                    if disc > 0 and (-b - sqrt(disc)) > 0.001 then
                        shadow = 0.25
                        break
                    end
                end
                local sh = 0.15 + diff * 0.85 * shadow
                cr, cg, cb = hr * sh, hg * sh, hb * sh
            else
                local t = 0.5 * (dy + 1)
                cr, cg, cb = 0.15 + 0.35 * t, 0.25 + 0.4 * t, 0.5 + 0.5 * t
            end

            local ri = floor(max(0, cr) * 255)
            local gi = floor(max(0, cg) * 255)
            local bi = floor(max(0, cb) * 255)
            if ri > 255 then ri = 255 end
            if gi > 255 then gi = 255 end
            if bi > 255 then bi = 255 end
            canvas:u32(y * pitch + x * 4, (ri << rs) | (gi << gs) | (bi << bs))
        end
    end
    return hi - lo
end

if fb.width() == 0 then
    print("raytracer: no framebuffer")
    return
end

local W, H = fb.width(), fb.height()
local canvas = fb.canvas()
local pitch = fb.pitch()
local nc = thread.cores()
local rs, gs, bs = fb.shifts()

local t0 = k.ns()
local rows = thread.parallel(render, canvas, W, H, pitch, nc, rs, gs, bs)
local ms = (k.ns() - t0) / 1e6

local total = 0
for _, r in ipairs(rows) do total = total + r end
print(string.format("raytraced %dx%d on %d cores in %.0f ms", W, H, nc, ms))
print("RAYTRACER_OK")
