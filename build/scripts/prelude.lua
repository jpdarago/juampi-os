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

-- --- help & reference ------------------------------------------------------
-- help()/ref() open a graphical microui browser when a framebuffer is present
-- (mouse to scroll/expand, Esc or the [x] box to close), and fall back to
-- plain text over serial otherwise.

local DESC = {
    k = "kernel introspection: time, memory, cpuid, peek/poke, shutdown, random",
    fb = "framebuffer graphics, resolution, and a parallel canvas",
    fs = "ext2 filesystem (read + write) on NVMe, USB or ATA storage",
    disk = "raw block access: ATA, NVMe and USB storage (read + write)",
    pci = "PCI configuration space",
    usb = "USB (xHCI): device tree, HID keyboard/mouse status",
    thread = "parallel Lua across CPU cores",
    mem = "shared-memory buffers",
    net = "IPv4 networking: DHCP, DNS (net.resolve), ping, UDP/TCP sockets",
    http = "HTTP/1.1 client (http and https): http.get(url) -> status, body",
    ui = "graphical popups & windows: ui.window, ui.popup, ui.confirm",
}
local LIBS = {
    "k", "fb", "fs", "disk", "pci", "usb",
    "thread", "mem", "net", "http", "ui",
}

local OVERVIEW = [==[juampiOS - a scriptable ring-0 kernel with an embedded
Lua 5.4 shell. Everything runs in one address space at ring 0.

Core globals:
  run(name[,arg])    run a .lua script or a native .elf binary
  run()              list what you can run
  bench(t[,arg[,n]]) time a function/script/binary -> total, per_call
  edit(name)         vim-style editor window
  files([path])      graphical file browser
  dump(v) / pp(v)    pretty-print a value or table
  clear()            clear the screen
  quit()             power the machine off (ACPI S5)
  help() / help(lib) this reference;  ref(topic) for a page]==]

local KEYS = [==[Shell line editor:
  left/right, Home/End, Ctrl-A/E   move to char / line ends
  Ctrl-arrows, Alt-b / Alt-f       move by word
  Ctrl-U / Ctrl-K / Ctrl-W         kill line / to end / word
  up / down                        command history

Full-screen editor (edit):
  Ctrl-S save    Ctrl-X save & run    Ctrl-Q quit

UI windows (ui.window, help):
  mouse to click, drag and scroll
  Esc or the [x] title box to close]==]

local PAGES = {
    overview = { "Overview", OVERVIEW },
    keys = { "Keybindings", KEYS },
}

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

-- Format a typed arg list ({name=,type=,doc=} array) as "name: type, ...".
local function typed(list)
    local parts = {}
    for _, a in ipairs(list or {}) do
        parts[#parts + 1] = a.name .. ": " .. a.type
    end
    return table.concat(parts, ", ")
end

-- "fn(param: type, ...) -> ret: type, ..." from a __doc entry.
local function signature(name, info)
    local s = name .. "(" .. typed(info.params) .. ")"
    if info.returns and #info.returns > 0 then
        s = s .. " -> " .. typed(info.returns)
    end
    return s
end

-- One parameter/return as "name: type" plus " - description" when present.
local function arg_line(a)
    local s = a.name .. ": " .. a.type
    if a.doc and a.doc ~= "" then
        s = s .. " - " .. a.doc
    end
    return s
end

-- Graphical detail for one function (inside an expanded ui.treenode): the
-- docstring, then each parameter and return with its type and description.
local function fn_detail(info)
    if info.doc and info.doc ~= "" then
        ui.text(info.doc)
    end
    if info.params and #info.params > 0 then
        ui.label("Parameters:")
        for _, a in ipairs(info.params) do
            ui.text("  " .. arg_line(a))
        end
    end
    if info.returns and #info.returns > 0 then
        ui.label("Returns:")
        for _, a in ipairs(info.returns) do
            ui.text("  " .. arg_line(a))
        end
    end
end

-- Graphical per-function tree: one collapsible node per function (labelled with
-- its signature) that expands to fn_detail. For libraries registered with typed
-- docs (a __doc table, via luadoc_newlib in C); plain functions get a bare node.
local function lib_tree(t)
    local doc = t.__doc
    for _, fn in ipairs(fn_names(t)) do
        local info = doc and doc[fn]
        local label = info and signature(fn, info) or (fn .. "()")
        if ui.treenode(label) then
            if info then
                fn_detail(info)
            else
                ui.text("(no documentation)")
            end
            ui.endtreenode()
        end
    end
end

-- Text (headless) body for a library: signatures, docstrings, and per-parameter
-- descriptions; falls back to a bare list of names when there are no typed docs.
local function lib_body(name, t)
    local header = DESC[name] or ""
    local doc = t.__doc
    if not doc then
        return header .. "\n\n" .. table.concat(fn_names(t), "  ")
    end
    local lines = {}
    for _, fn in ipairs(fn_names(t)) do
        local info = doc[fn]
        if info then
            lines[#lines + 1] = signature(fn, info)
            if info.doc and info.doc ~= "" then
                lines[#lines + 1] = "    " .. info.doc
            end
            for _, a in ipairs(info.params or {}) do
                if a.doc and a.doc ~= "" then
                    lines[#lines + 1] = "      " .. arg_line(a)
                end
            end
        else
            lines[#lines + 1] = fn .. "()"
        end
    end
    return header .. "\n\n" .. table.concat(lines, "\n")
end

local function has_ui()
    return ui ~= nil and ui.available ~= nil and ui.available()
end

-- The graphical reference browser: a non-modal window that coexists with the
-- shell (overview + keybindings + a node per library).
local function browser()
    ui.open("juampiOS - Help & Reference", function()
        if ui.header("Overview") then
            ui.text(OVERVIEW)
        end
        if ui.header("Keybindings") then
            ui.text(KEYS)
        end
        for _, n in ipairs(LIBS) do
            local t = _G[n]
            if type(t) == "table" and ui.treenode(n .. "  -  " .. (DESC[n] or "")) then
                lib_tree(t) -- a collapsible node per function
                ui.endtreenode()
            end
        end
    end)
end

local function help_text()
    print("juampiOS Lua shell. Available:")
    print("  run(name[,arg])   run a .lua script or a native .elf binary")
    print("  run()             list what you can run")
    print("  edit(name)        full-screen editor (^S save, ^X run, ^Q quit)")
    print("  bench(t[,arg[,n]]) time a function/script/binary -> total,per_call")
    print("  dump(v) / pp(v)   pretty-print a value or table")
    print("  clear()           clear the screen;  up/down arrows recall history")
    print("  help(lib)         details for a library, e.g. help(fb) or help(net)")
    print("  libraries:")
    for _, n in ipairs(LIBS) do
        print(string.format("    %-7s %s", n, DESC[n] or ""))
    end
end

-- help() opens the reference; help(lib) or help("lib") focuses one library.
function help(topic)
    if topic == nil then
        if has_ui() then
            browser()
        else
            help_text()
        end
        return
    end
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
    if has_ui() then
        -- A focused window for one library: the description plus a collapsible,
        -- documented node per function.
        ui.open(name .. " - reference", function()
            ui.label(DESC[name] or "")
            lib_tree(t)
        end)
    else
        print(lib_body(name, t))
    end
end

-- ref() opens the browser; ref("overview")/ref("keys") show an authored page;
-- ref(lib) is the same as help(lib).
function ref(topic)
    if topic == nil then
        if has_ui() then
            browser()
        else
            help_text()
        end
        return
    end
    local page = PAGES[tostring(topic):lower()]
    if page then
        if has_ui() then
            ui.popup(page[1], page[2])
        else
            print(page[2])
        end
    else
        help(topic)
    end
end

-- --- file browser ----------------------------------------------------------

local function path_join(dir, name)
    if dir == "/" then
        return "/" .. name
    end
    return dir .. "/" .. name
end

local function path_parent(dir)
    local p = dir:match("^(.*)/[^/]+/?$")
    if not p or p == "" then
        return "/"
    end
    return p
end

-- files([path]): a graphical file browser on the ext2 data disk. Click a folder
-- to enter it, ".." to go up, a file to open it in the editor (a .elf launches
-- via run()). Falls back to a text listing when there is no framebuffer.
function files(start)
    local path = start or "/"
    if not has_ui() then
        local es = fs.list(path)
        if not es then
            print("files: cannot list " .. path)
            return
        end
        print(path .. ":")
        for _, e in ipairs(es) do
            print(string.format("  %-5s %s", e.type, e.name))
        end
        return
    end
    while true do
        local pending = nil
        ui.window("File browser", function()
            ui.row({ -1 }) -- one full-width column, so names aren't clipped
            ui.label(path)
            if path ~= "/" and ui.button(".. (up)") then
                path = path_parent(path)
            end
            local es = fs.list(path)
            if not es then
                ui.label("(cannot read directory)")
                return
            end
            table.sort(es, function(a, b)
                if (a.type == "dir") ~= (b.type == "dir") then
                    return a.type == "dir" -- folders first
                end
                return a.name < b.name
            end)
            for _, e in ipairs(es) do
                if e.name ~= "." and e.name ~= ".." then
                    local mark = (e.type == "dir") and "[+] " or "    "
                    if ui.button(mark .. e.name) then
                        if e.type == "dir" then
                            path = path_join(path, e.name)
                        else
                            pending = path_join(path, e.name)
                            ui.close() -- leave the modal loop to open the file
                        end
                    end
                end
            end
        end)
        if not pending then
            return
        end
        if pending:match("%.elf$") then
            run(pending) -- native program: opens its own window
            return
        end
        edit(pending) -- text / scripts: the vim editor; then reopen the browser
    end
end
browse = files
