// raytracer.c — the Lua raytracer (build/scripts/raytracer.lua) ported to a
// native ring-0 program and shipped on the ext2 disk (NOT as a Limine module),
// so run("raytracer.elf") loads it from the filesystem. Same scene and
// algorithm, compiled -O2 with SSE2 → dramatically faster than the interpreted
// version. Uses the lab ABI: fb*() for the framebuffer, run_on/join to render
// horizontal bands in parallel across every core.
//
// Freestanding (-nostdlib): no libm, so sqrt/floor are provided inline below.

#include <lab.h>

static inline double dsqrt(double x)
{
    double r;
    __asm__("sqrtsd %1, %0" : "=x"(r) : "x"(x));
    return r;
}
static inline double dfloor(double x)
{
    double t = (double)(long long)x; // truncates toward zero
    return t > x ? t - 1.0 : t;
}

#define NS 4       // spheres
#define MAXDEPTH 5 // glass needs a couple extra bounces (enter + exit)
#define FLOOR_REFL 0.35

// {cx,cy,cz, radius, r,g,b, reflect, specular, ior}. ior 0 = opaque.
static const double SPH[NS][10] = {
        {-1.1, 1.0, -3.0, 1.0, 0.90, 0.20, 0.20, 0.25, 1, 0},   // red, glossy
        {1.2, 1.0, -3.6, 1.0, 0.20, 0.45, 0.90, 0.30, 1, 0},    // blue, glossy
        {2.7, 1.5, -5.2, 1.5, 0.16, 0.16, 0.20, 0.85, 1, 0},    // chrome mirror
        {-0.2, 0.8, -1.6, 0.8, 1.00, 1.00, 1.00, 0.10, 1, 1.5}, // glass
};

// Anti-aliasing sub-pixel offsets (matches the Lua version's 2-sample
// diagonal).
static const double SAMP[][2] = {{0.25, 0.25}, {0.75, 0.75}};
#define NSAMP ((int)(sizeof(SAMP) / sizeof(SAMP[0])))

// Light direction, pre-normalized (0.6,0.9,0.3)/|.|.
#define LX 0.5345224838248488
#define LY 0.8017837257372732
#define LZ 0.2672612419124244

struct col {
    double r, g, b;
};

// Trace one ray from (ex,ey,ez) along (dx,dy,dz); recurse for mirror
// reflections and glass refraction.
static struct col trace(double ex, double ey, double ez, double dx, double dy,
                        double dz, int depth)
{
    double best = 1e30;
    double nx = 0, ny = 0, nz = 0, px = 0, py = 0, pz = 0;
    double hr = 0, hg = 0, hb = 0, refl = 0, spec = 0, ior = 0;

    for (int i = 0; i < NS; i++) {
        const double* s = SPH[i];
        double ocx = ex - s[0], ocy = ey - s[1], ocz = ez - s[2];
        double b = ocx * dx + ocy * dy + ocz * dz;
        double c = ocx * ocx + ocy * ocy + ocz * ocz - s[3] * s[3];
        double disc = b * b - c;
        if (disc > 0) {
            double sd = dsqrt(disc);
            double t = -b - sd;
            if (t <= 0.001)
                t = -b + sd; // inside the sphere: far root
            if (t > 0.001 && t < best) {
                best = t;
                px = ex + dx * t, py = ey + dy * t, pz = ez + dz * t;
                double rd = s[3];
                nx = (px - s[0]) / rd, ny = (py - s[1]) / rd,
                nz = (pz - s[2]) / rd;
                hr = s[4], hg = s[5], hb = s[6], refl = s[7], spec = s[8],
                ior = s[9];
            }
        }
    }

    if (dy < -1e-4) { // floor plane y = 0
        double t = -ey / dy;
        if (t > 0.001 && t < best) {
            best = t;
            px = ex + dx * t, py = ey + dy * t, pz = ez + dz * t;
            nx = 0, ny = 1, nz = 0;
            if ((((long long)dfloor(px)) + ((long long)dfloor(pz))) & 1LL) {
                hr = 0.20, hg = 0.20, hb = 0.28;
            } else {
                hr = 0.9, hg = 0.9, hb = 0.9;
            }
            refl = FLOOR_REFL, spec = 0, ior = 0;
        }
    }

    if (best > 1e29) { // sky gradient
        double t = 0.5 * (dy + 1);
        struct col sky = {0.15 + 0.35 * t, 0.25 + 0.4 * t, 0.5 + 0.5 * t};
        return sky;
    }

    // Glass: Fresnel blend of a reflected and a refracted ray (Snell's law).
    if (ior > 0 && depth > 0) {
        double ndot = dx * nx + dy * ny + dz * nz;
        double eta;
        if (ndot < 0) { // entering
            eta = 1.0 / ior;
        } else { // leaving: flip the normal, invert the ratio
            eta = ior;
            nx = -nx, ny = -ny, nz = -nz;
            ndot = -ndot;
        }
        double cosi = -ndot;
        double rk = 2 * (dx * nx + dy * ny + dz * nz);
        double rx = dx - rk * nx, ry = dy - rk * ny, rz = dz - rk * nz;
        double k = 1 - eta * eta * (1 - cosi * cosi);
        if (k < 0) { // total internal reflection
            return trace(px + nx * 0.001, py + ny * 0.001, pz + nz * 0.001, rx,
                         ry, rz, depth - 1);
        }
        double cost = dsqrt(k);
        double tx = eta * dx + (eta * cosi - cost) * nx;
        double ty = eta * dy + (eta * cosi - cost) * ny;
        double tz = eta * dz + (eta * cosi - cost) * nz;
        double r0 = (1 - ior) / (1 + ior);
        r0 = r0 * r0;
        double om = 1 - cosi;
        double f = r0 + (1 - r0) * (om * om * om * om * om); // Schlick
        struct col rc = trace(px + nx * 0.001, py + ny * 0.001, pz + nz * 0.001,
                              rx, ry, rz, depth - 1);
        struct col tc = trace(px - nx * 0.001, py - ny * 0.001, pz - nz * 0.001,
                              tx, ty, tz, depth - 1);
        struct col g = {rc.r * f + tc.r * (1 - f) * hr,
                        rc.g * f + tc.g * (1 - f) * hg,
                        rc.b * f + tc.b * (1 - f) * hb};
        return g;
    }

    // Diffuse + hard shadow (glass spheres don't cast an opaque shadow).
    double diff = nx * LX + ny * LY + nz * LZ;
    if (diff < 0)
        diff = 0;
    double shadow = 1.0;
    double sxo = px + nx * 0.01, syo = py + ny * 0.01, szo = pz + nz * 0.01;
    for (int i = 0; i < NS; i++) {
        const double* s = SPH[i];
        if (s[9] == 0) {
            double ocx = sxo - s[0], ocy = syo - s[1], ocz = szo - s[2];
            double b = ocx * LX + ocy * LY + ocz * LZ;
            double c = ocx * ocx + ocy * ocy + ocz * ocz - s[3] * s[3];
            double disc = b * b - c;
            if (disc > 0 && (-b - dsqrt(disc)) > 0.001) {
                shadow = 0.25;
                break;
            }
        }
    }
    double sh = 0.15 + diff * 0.85 * shadow;
    struct col out = {hr * sh, hg * sh, hb * sh};

    // Phong specular highlight.
    if (spec > 0 && shadow > 0.5 && diff > 0) {
        double rlx = 2 * diff * nx - LX;
        double rly = 2 * diff * ny - LY;
        double rlz = 2 * diff * nz - LZ;
        double sp = rlx * (-dx) + rly * (-dy) + rlz * (-dz);
        if (sp > 0) {
            sp = sp * sp;
            sp = sp * sp;
            sp = sp * sp;
            sp = sp * sp;
            sp = sp * sp; // sp^32
            sp = sp * 0.9 * spec;
            out.r += sp, out.g += sp, out.b += sp;
        }
    }

    // Mirror reflection.
    if (refl > 0 && depth > 0) {
        double rk = 2 * (dx * nx + dy * ny + dz * nz);
        double rx = dx - rk * nx, ry = dy - rk * ny, rz = dz - rk * nz;
        struct col rc = trace(px + nx * 0.001, py + ny * 0.001, pz + nz * 0.001,
                              rx, ry, rz, depth - 1);
        out.r = out.r * (1 - refl) + rc.r * refl;
        out.g = out.g * (1 - refl) + rc.g * refl;
        out.b = out.b * (1 - refl) + rc.b * refl;
    }
    return out;
}

struct band {
    unsigned* fb; // framebuffer base (32bpp)
    unsigned W, H, pxpitch;
    unsigned rs, gs, bs;
    unsigned cpu, nc;
};

// Render this core's horizontal band straight into the framebuffer.
static void render_band(void* p)
{
    struct band* bd = (struct band*)p;
    unsigned W = bd->W, H = bd->H, pxpitch = bd->pxpitch;
    unsigned rs = bd->rs, gs = bd->gs, bs = bd->bs;
    unsigned* fb = bd->fb;
    double aspect = (double)W / (double)H;
    double ox = 0.0, oy = 1.3, oz = 1.0; // camera
    double inv = 1.0 / NSAMP;
    unsigned lo = (H / bd->nc) * bd->cpu;
    unsigned hi = (bd->cpu == bd->nc - 1) ? H : (H / bd->nc) * (bd->cpu + 1);

    for (unsigned y = lo; y < hi; y++) {
        for (unsigned x = 0; x < W; x++) {
            double cr = 0, cg = 0, cb = 0;
            for (int si = 0; si < NSAMP; si++) {
                double sx = (2.0 * (x + SAMP[si][0]) / W - 1.0) * aspect;
                double sy = 1.0 - 2.0 * (y + SAMP[si][1]) / H;
                double dx = sx, dy = sy, dz = -1.0;
                double dl = dsqrt(dx * dx + dy * dy + dz * dz);
                dx /= dl, dy /= dl, dz /= dl;
                struct col c = trace(ox, oy, oz, dx, dy, dz, MAXDEPTH);
                cr += c.r, cg += c.g, cb += c.b;
            }
            cr *= inv, cg *= inv, cb *= inv;

            int ri = (int)dfloor((cr < 0 ? 0 : cr) * 255);
            int gi = (int)dfloor((cg < 0 ? 0 : cg) * 255);
            int bi = (int)dfloor((cb < 0 ? 0 : cb) * 255);
            if (ri > 255)
                ri = 255;
            if (gi > 255)
                gi = 255;
            if (bi > 255)
                bi = 255;
            fb[y * pxpitch + x] = ((unsigned)ri << rs) | ((unsigned)gi << gs) |
                                  ((unsigned)bi << bs);
        }
    }
}

static void print_uint(const struct lab_api* api, unsigned long v)
{
    if (v == 0) {
        api->print("0");
        return;
    }
    char tmp[21];
    int n = 0;
    while (v) {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    }
    char out[22];
    int j = 0;
    while (n)
        out[j++] = tmp[--n];
    out[j] = '\0';
    api->print(out);
}

long bench(const struct lab_api* api, long arg)
{
    (void)arg;
    void* base = api->fb ? api->fb() : (void*)0;
    unsigned W = api->fb_width ? (unsigned)api->fb_width() : 0;
    unsigned H = api->fb_height ? (unsigned)api->fb_height() : 0;
    if (base == (void*)0 || W == 0 || H == 0) {
        api->print("raytracer: no framebuffer\n");
        return 0;
    }
    unsigned pxpitch = (unsigned)(api->fb_pitch() / 4);
    unsigned char rs, gs, bs;
    api->fb_shifts(&rs, &gs, &bs);
    unsigned nc = (unsigned)api->ncores();
    if (nc > 64)
        nc = 64;

    struct band bands[64];
    for (unsigned i = 0; i < nc; i++) {
        bands[i].fb = (unsigned*)base;
        bands[i].W = W, bands[i].H = H, bands[i].pxpitch = pxpitch;
        bands[i].rs = rs, bands[i].gs = gs, bands[i].bs = bs;
        bands[i].cpu = i, bands[i].nc = nc;
    }

    unsigned long t0 = api->ns();
    for (unsigned i = 1; i < nc; i++)
        api->run_on(i, render_band, &bands[i]);
    render_band(&bands[0]); // the BSP renders band 0
    for (unsigned i = 1; i < nc; i++)
        api->join(i);
    unsigned long ms = (api->ns() - t0) / 1000000UL;

    api->print("raytraced ");
    print_uint(api, W);
    api->print("x");
    print_uint(api, H);
    api->print(" on ");
    print_uint(api, nc);
    api->print(" cores in ");
    print_uint(api, ms);
    api->print(" ms (native)\n");
    api->print("RAYTRACER_OK\n");
    return (long)H;
}
