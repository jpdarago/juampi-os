-- prelude.lua: built-in shell helpers, loaded once at boot BEFORE init.lua so
-- they're always present. Defines the globals help(), dump()/pp(), used at the
-- interactive prompt.

-- --- dump / pp: pretty-print any value -------------------------------------

local function fmtkey(k)
    if type(k) == "string" and k:match("^[%a_][%w_]*$") then
        return k
    end
    return "[" .. tostring(k) .. "]"
end

local function keys_sorted(t)
    local ks = {}
    for k in pairs(t) do
        ks[#ks + 1] = k
    end
    table.sort(ks, function(a, b)
        local ta, tb = type(a), type(b)
        if ta ~= tb then
            return ta < tb
        end
        if ta == "number" or ta == "string" then
            return a < b
        end
        return tostring(a) < tostring(b)
    end)
    return ks
end

local function dumpv(v, ind, seen, out)
    local t = type(v)
    if t == "table" then
        if seen[v] then
            out[#out + 1] = "<cycle>"
            return
        end
        seen[v] = true
        out[#out + 1] = "{\n"
        for _, k in ipairs(keys_sorted(v)) do
            out[#out + 1] = ind .. "  " .. fmtkey(k) .. " = "
            dumpv(v[k], ind .. "  ", seen, out)
            out[#out + 1] = ",\n"
        end
        out[#out + 1] = ind .. "}"
        seen[v] = nil
    elseif t == "string" then
        out[#out + 1] = string.format("%q", v)
    else
        out[#out + 1] = tostring(v)
    end
end

-- dump(v): print a readable, indented view of v (tables recurse, cycle-safe).
function dump(v)
    local out = {}
    dumpv(v, "", {}, out)
    print(table.concat(out))
end
pp = dump

-- --- help ------------------------------------------------------------------

local DESC = {
    k = "kernel introspection: time, memory, cpuid, peek/poke, shutdown, random",
    fb = "framebuffer graphics, resolution, and a parallel canvas",
    fs = "read-only ext2 filesystem on the data disk",
    disk = "raw ATA block access",
    pci = "PCI configuration space",
    thread = "parallel Lua across CPU cores",
    mem = "shared-memory buffers",
}
local LIBS = { "k", "fb", "fs", "disk", "pci", "thread", "mem" }

local function fn_names(t)
    local ns = {}
    for k, v in pairs(t) do
        if type(v) == "function" then
            ns[#ns + 1] = k
        end
    end
    table.sort(ns)
    return ns
end

-- help() prints an overview; help(lib) or help("lib") lists a library's members.
function help(topic)
    if topic ~= nil then
        local name, t = tostring(topic), topic
        if type(topic) == "string" then
            t = _G[topic]
        else
            for k, v in pairs(_G) do
                if v == topic then
                    name = k
                    break
                end
            end
        end
        if type(t) ~= "table" then
            print("help: no such library: " .. tostring(topic))
            return
        end
        print(name .. " — " .. (DESC[name] or ""))
        print("  " .. table.concat(fn_names(t), "  "))
        return
    end
    print("juampiOS Lua shell. Available:")
    print("  run(name[,arg])   run a .lua script or a native .elf binary")
    print("  bench(t[,arg[,n]]) time a function/script/binary -> total,per_call")
    print("  dump(v) / pp(v)   pretty-print a value or table")
    print("  clear()           clear the screen;  up/down arrows recall history")
    print("  help(lib)         details for a library, e.g. help(fb)")
    print("  libraries:")
    for _, n in ipairs(LIBS) do
        print(string.format("    %-7s %s", n, DESC[n] or ""))
    end
end
