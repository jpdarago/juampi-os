-- init.lua: run at startup by the juampiOS Lua shell (shipped as a Limine
-- module). Edit this and rebuild the image to customize the boot experience.

print("init.lua: hello from " .. k.cpubrand())
print(string.format("init.lua: %d MiB free of %d, TSC %d MHz",
    k.freemem() // (1024 * 1024), k.totalmem() // (1024 * 1024),
    k.tsc_hz() // 1000000))

-- The boot logo is drawn (and kept redrawn) by the kernel shell itself, after
-- the banner, so a console scroll doesn't wipe it. Display it yourself any time
-- with fb.image("logo.qoi") or run("logo.lua").

-- A couple of convenience helpers, available in the shell after boot:
function bench(fn, n)
    local total, per = k.bench(fn, n or 1000)
    print(string.format("%d calls: %d cycles total, %d per call", n or 1000,
        total, per))
    return per
end

function uptime()
    return string.format("%.3f s", k.uptime())
end
