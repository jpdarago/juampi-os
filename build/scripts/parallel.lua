-- parallel.lua: parallel Lua across all cores (M9). Fill a shared array, then
-- sum it two ways -- serially, and split across every core with thread.parallel
-- -- and check they agree. Run with run("parallel.lua").

local N = 1 << 20 -- 1,048,576 u32 values
local buf = mem.shared(N * 4)
for i = 0, N - 1 do
    buf:u32(i * 4, i & 0xffff)
end

-- Serial reference sum.
local t0 = k.ns()
local serial = 0
for i = 0, N - 1 do
    serial = serial + buf:u32(i * 4)
end
local serial_ms = (k.ns() - t0) / 1e6

-- Parallel: each core sums its own contiguous slice of the shared buffer and
-- returns the partial; the main core adds the partials. The worker takes only
-- (cpu, buf, N, ncores) -- no upvalues cross the boundary.
local function worker(cpu, b, n, ncores)
    local lo = (n // ncores) * cpu
    local hi = (cpu == ncores - 1) and n or (n // ncores) * (cpu + 1)
    local s = 0
    for i = lo, hi - 1 do
        s = s + b:u32(i * 4)
    end
    return s
end

local nc = thread.cores()
local t1 = thread.ns()
local parts = thread.parallel(worker, buf, N, nc)
local par_ms = (thread.ns() - t1) / 1e6
local parallel = 0
for _, p in ipairs(parts) do
    parallel = parallel + p
end

print(string.format("cores=%d  serial=%d (%.1f ms)  parallel=%d (%.1f ms)", nc,
    serial, serial_ms, parallel, par_ms))
if serial == parallel then
    print(string.format("PARALLEL_OK  speedup %.2fx", serial_ms / par_ms))
else
    print("PARALLEL_MISMATCH")
end
