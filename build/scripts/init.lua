-- init.lua: run at startup by the juampiOS Lua shell (shipped as a Limine
-- module). Edit this and rebuild the image to customize the boot experience.

print("init.lua: hello from " .. k.cpubrand())
print(string.format("init.lua: %d MiB free of %d, TSC %d MHz",
    k.freemem() // (1024 * 1024), k.totalmem() // (1024 * 1024),
    k.tsc_hz() // 1000000))

-- The boot logo is drawn (and kept redrawn) by the kernel shell itself, after
-- the banner, so a console scroll doesn't wipe it. Display it yourself any time
-- with fb.image("logo.qoi") or run("logo.lua").

-- run(name[,arg]) launches a Lua script or a native binary; bench(target[,arg
-- [,iters]]) times a function, script, or binary the same way (both are C
-- globals). A convenience helper, available in the shell after boot:
function uptime()
    return string.format("%.3f s", k.uptime())
end
