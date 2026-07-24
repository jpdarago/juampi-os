-- raytracer.lua: a tiny recursive raytracer rendered across every core (an M8 +
-- M9 showcase). Each core traces a horizontal band straight into the framebuffer
-- via fb.canvas(), so the image fills in band by band, in parallel. The scene is
-- glossy spheres, a chrome mirror ball, and a refractive glass ball on a
-- reflective checkered floor under a sky gradient — with hard shadows, Phong
-- specular highlights, recursive reflections, and Fresnel refraction (the glass
-- ball bends the scene behind it and mirrors it at grazing angles). Edges are
-- anti-aliased by supersampling SAMPLES rays per pixel.
--   fb.setmode(1024, 768)   -- optional: crank the resolution first
--   run("raytracer.lua")

-- The render function is serialized to bytecode and run on each core, so it must
-- be self-contained (no upvalues from module scope) and take everything as
-- arguments. The nested trace() is fine: its upvalues resolve to render's own
-- locals, which travel with the dumped prototype.
local function render(cpu, canvas, W, H, pitch, nc, rs, gs, bs)
    local sqrt, floor, max = math.sqrt, math.floor, math.max
    -- Spheres: {cx,cy,cz, radius, r,g,b, reflect, specular, ior}. ior 0 = opaque;
    -- ior > 0 = glass (index of refraction). Floor is the plane y = 0.
    local S = {
        { -1.1, 1.0, -3.0, 1.0, 0.90, 0.20, 0.20, 0.25, 1, 0 },   -- red, glossy
        { 1.2, 1.0, -3.6, 1.0, 0.20, 0.45, 0.90, 0.30, 1, 0 },    -- blue, glossy
        { 2.7, 1.5, -5.2, 1.5, 0.16, 0.16, 0.20, 0.85, 1, 0 },    -- chrome mirror
        { -0.2, 0.8, -1.6, 0.8, 1.00, 1.00, 1.00, 0.10, 1, 1.5 }, -- glass
    }
    local ns = #S
    local FLOOR_REFL = 0.35
    local MAXDEPTH = 5 -- glass needs a couple extra bounces (enter + exit)
    -- Anti-aliasing: sub-pixel sample offsets in [0,1). Two on the diagonal
    -- roughly halve edge jaggies at 2x cost; a 2x2/3x3 grid is smoother but
    -- linearly slower — tune against your time budget.
    local SAMPLES = { { 0.25, 0.25 }, { 0.75, 0.75 } }
    local nsamp = #SAMPLES
    local lx, ly, lz = 0.6, 0.9, 0.3 -- light direction
    local ll = sqrt(lx * lx + ly * ly + lz * lz)
    lx, ly, lz = lx / ll, ly / ll, lz / ll

    -- Reflect ray d about normal n (both normalized).
    local function reflect(dx, dy, dz, nx, ny, nz)
        local k = 2 * (dx * nx + dy * ny + dz * nz)
        return dx - k * nx, dy - k * ny, dz - k * nz
    end

    -- Trace one ray from (ex,ey,ez) along (dx,dy,dz); return its colour,
    -- recursing for mirror reflections and glass refraction.
    local function trace(ex, ey, ez, dx, dy, dz, depth)
        local best = 1e30
        local nx, ny, nz, px, py, pz = 0, 0, 0, 0, 0, 0
        local hr, hg, hb, refl, spec, ior = 0, 0, 0, 0, 0, 0

        for i = 1, ns do
            local s = S[i]
            local ocx, ocy, ocz = ex - s[1], ey - s[2], ez - s[3]
            local b = ocx * dx + ocy * dy + ocz * dz
            local c = ocx * ocx + ocy * ocy + ocz * ocz - s[4] * s[4]
            local disc = b * b - c
            if disc > 0 then
                local sd = sqrt(disc)
                local t = -b - sd
                if t <= 0.001 then t = -b + sd end -- inside the sphere: far root
                if t > 0.001 and t < best then
                    best = t
                    px, py, pz = ex + dx * t, ey + dy * t, ez + dz * t
                    nx = (px - s[1]) / s[4]
                    ny = (py - s[2]) / s[4]
                    nz = (pz - s[3]) / s[4]
                    hr, hg, hb, refl, spec, ior = s[5], s[6], s[7], s[8], s[9], s[10]
                end
            end
        end

        if dy < -1e-4 then -- floor plane y = 0
            local t = -ey / dy
            if t > 0.001 and t < best then
                best = t
                px, py, pz = ex + dx * t, ey + dy * t, ez + dz * t
                nx, ny, nz = 0, 1, 0
                if (floor(px) + floor(pz)) & 1 == 0 then
                    hr, hg, hb = 0.9, 0.9, 0.9
                else
                    hr, hg, hb = 0.20, 0.20, 0.28
                end
                refl, spec, ior = FLOOR_REFL, 0, 0
            end
        end

        if best > 1e29 then -- sky gradient
            local t = 0.5 * (dy + 1)
            return 0.15 + 0.35 * t, 0.25 + 0.4 * t, 0.5 + 0.5 * t
        end

        -- Glass: Fresnel blend of a reflected and a refracted ray (Snell's law).
        if ior > 0 and depth > 0 then
            -- Orient the normal against the ray and pick the index ratio for
            -- entering vs. leaving the glass.
            local ndot = dx * nx + dy * ny + dz * nz
            local eta
            if ndot < 0 then -- entering
                eta = 1 / ior
            else             -- leaving: flip the normal, invert the ratio
                eta = ior
                nx, ny, nz = -nx, -ny, -nz
                ndot = -ndot
            end
            local cosi = -ndot
            local rx, ry, rz = reflect(dx, dy, dz, nx, ny, nz)
            local k = 1 - eta * eta * (1 - cosi * cosi)
            if k < 0 then -- total internal reflection
                return trace(px + nx * 0.001, py + ny * 0.001, pz + nz * 0.001,
                    rx, ry, rz, depth - 1)
            end
            local cost = sqrt(k)
            local tx = eta * dx + (eta * cosi - cost) * nx
            local ty = eta * dy + (eta * cosi - cost) * ny
            local tz = eta * dz + (eta * cosi - cost) * nz
            -- Schlick's Fresnel: more mirror-like at grazing angles.
            local r0 = (1 - ior) / (1 + ior)
            r0 = r0 * r0
            local f = r0 + (1 - r0) * (1 - cosi) ^ 5
            local rr, rg, rb = trace(px + nx * 0.001, py + ny * 0.001,
                pz + nz * 0.001, rx, ry, rz, depth - 1)
            local tr, tg, tb = trace(px - nx * 0.001, py - ny * 0.001,
                pz - nz * 0.001, tx, ty, tz, depth - 1)
            return rr * f + tr * (1 - f) * hr,
                rg * f + tg * (1 - f) * hg,
                rb * f + tb * (1 - f) * hb
        end

        -- Diffuse term with a hard shadow (occlusion toward the light). Glass
        -- spheres don't cast an opaque shadow.
        local diff = max(0, nx * lx + ny * ly + nz * lz)
        local shadow = 1.0
        local sxo, syo, szo = px + nx * 0.01, py + ny * 0.01, pz + nz * 0.01
        for i = 1, ns do
            local s = S[i]
            if s[10] == 0 then
                local ocx, ocy, ocz = sxo - s[1], syo - s[2], szo - s[3]
                local b = ocx * lx + ocy * ly + ocz * lz
                local c = ocx * ocx + ocy * ocy + ocz * ocz - s[4] * s[4]
                local disc = b * b - c
                if disc > 0 and (-b - sqrt(disc)) > 0.001 then
                    shadow = 0.25
                    break
                end
            end
        end
        local sh = 0.15 + diff * 0.85 * shadow
        local cr, cg, cb = hr * sh, hg * sh, hb * sh

        -- Phong specular highlight: reflect the light about the normal and dot
        -- with the view ray; raise to a high power for a tight glossy spot.
        if spec > 0 and shadow > 0.5 and diff > 0 then
            local rlx = 2 * diff * nx - lx
            local rly = 2 * diff * ny - ly
            local rlz = 2 * diff * nz - lz
            local sp = rlx * (-dx) + rly * (-dy) + rlz * (-dz)
            if sp > 0 then
                sp = sp * sp
                sp = sp * sp
                sp = sp * sp
                sp = sp * sp
                sp = sp * sp -- sp^32
                sp = sp * 0.9 * spec
                cr, cg, cb = cr + sp, cg + sp, cb + sp
            end
        end

        -- Mirror reflection: shoot a secondary ray about the normal and blend.
        if refl > 0 and depth > 0 then
            local rx, ry, rz = reflect(dx, dy, dz, nx, ny, nz)
            local rr, rg, rb = trace(px + nx * 0.001, py + ny * 0.001,
                pz + nz * 0.001, rx, ry, rz, depth - 1)
            cr = cr * (1 - refl) + rr * refl
            cg = cg * (1 - refl) + rg * refl
            cb = cb * (1 - refl) + rb * refl
        end
        return cr, cg, cb
    end

    local ox, oy, oz = 0.0, 1.3, 1.0 -- camera
    local aspect = W / H
    local lo = (H // nc) * cpu
    local hi = (cpu == nc - 1) and H or (H // nc) * (cpu + 1)
    local inv = 1.0 / nsamp

    for y = lo, hi - 1 do
        for x = 0, W - 1 do
            local cr, cg, cb = 0, 0, 0
            for si = 1, nsamp do
                local jx, jy = SAMPLES[si][1], SAMPLES[si][2]
                local sx = (2 * (x + jx) / W - 1) * aspect
                local sy = 1 - 2 * (y + jy) / H
                local dx, dy, dz = sx, sy, -1.0
                local dl = sqrt(dx * dx + dy * dy + dz * dz)
                dx, dy, dz = dx / dl, dy / dl, dz / dl
                local r, g, b = trace(ox, oy, oz, dx, dy, dz, MAXDEPTH)
                cr, cg, cb = cr + r, cg + g, cb + b
            end
            cr, cg, cb = cr * inv, cg * inv, cb * inv

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
