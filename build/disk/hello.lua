-- hello.lua: lives on the ext2 data disk (build/disk/), NOT in a Limine module.
-- Running it proves run() can load scripts straight off the disk via the fs
-- library. Try: run("hello.lua")
print("HELLO_FROM_EXT2")
local motd = fs.read("/etc/motd.txt")
if motd then
    print(motd)
end
