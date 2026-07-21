---
title: Networking
tags: [design, networking, driver, e1000, tcp-ip, in-progress]
status: in-progress
milestone: M10
related: ["[[x86-64-port]]", "[[lua-shell]]", "[[Index]]"]
created: 2026-07-20
---

# Networking

> [!abstract] Goal
> Give the [[lua-shell|ring-0 Lua shell]] a TCP/IP stack: bring up a NIC, speak
> Ethernet/ARP/IPv4/ICMP/UDP (TCP as a stretch), auto-configure via DHCP, resolve
> names with DNS, and expose a `net.*` socket library to Lua — all testable under
> QEMU with **zero host privileges**.

> [!success] Status — **vertical slice landed (M10)**. The "ping" path is
> implemented and tested: e1000 driver → Ethernet → ARP → IPv4 → ICMP, static
> config, exposed to Lua as `net.ready/ip/mac/config/ping`. `net.ping("10.0.2.2")`
> round-trips the SLIRP gateway (~0.1–0.9 ms); `tests/net-smoke.sh` gates it. The
> rest of this note (UDP/sockets/DHCP/DNS/TCP) is still the forward design.
>
> **What shipped vs. the original plan:** flat `src/` files (matching the repo's
> convention) rather than a `src/net/` tree — `src/e1000.c`, `src/net.c` (Ethernet
> + ARP + IPv4 + ICMP consolidated), `src/lua/lua_net.c`; plus `PAGEF_UC` in
> paging, `pci_find`/`pci_bar`/`pci_enable_bus_master` helpers, a `net_poll()` pump
> in `console_getch`, and `-nic user,model=e1000` in `run`/`test`. No `pbuf` pool
> yet (static frame buffers + a per-TX-slot bounce buffer); split into per-layer
> files when UDP lands.

> [!info] Foundation. The [[x86-64-port]] gave us paging + HHDM + PCI + interrupts;
> [[lua-shell]] gave us the shell and the polled-SMP execution model this stack
> deliberately reuses.

---

## 1. Guiding decisions

Everything below follows from five load-bearing choices.

### 1.1 Cooperative *polled* execution — no new interrupt path

The kernel has **no LAPIC / IOAPIC / MSI**, and its SMP is spin-polled by design
(see [[lua-shell]]). Building PCI `INTx` routing (ACPI `_PRT` / PIC `ELCR`) purely
to service a NIC would be a large, orthogonal project. Instead the whole stack is
driven by a single pump:

```c
void net_poll(void);   // drain RX ring -> layers; service timers; non-blocking
```

`net_poll()` is called from exactly two places:

1. **The shell idle path** — inside `console_getch()` while it waits for a
   keystroke (today it busy-waits / `hlt`s on the keyboard ring; it will pump
   `net_poll()` there instead).
2. **Every blocking Lua socket call** — `recvfrom`, `ping`, `resolve`, `dhcp`
   spin `net_poll()` until data arrives or a `ktime_ms()` deadline passes.

> [!note] Consequence: **no packet is ever processed in interrupt context.** RX is
> pulled from the descriptor ring, not pushed. This matches `ata.c`'s existing
> timed-poll idiom and keeps every layer re-entrancy-free.

Interrupt-driven RX is a possible **Phase 4** optimization, not part of v1.

### 1.2 DMA buffers from the frame allocator + HHDM

NIC descriptor rings and packet buffers must be **physically addressed** (the card
DMAs to/from physical RAM). We already have exactly the right primitives:

- `frame_alloc()` → a 4 KiB **physical** frame (`include/frames.h`).
- `phys_to_virt(pa)` → its **HHDM** virtual address (`include/paging.h`).

So a DMA buffer needs no new mapping: give the card `pa`, let the CPU touch
`phys_to_virt(pa)`. x86 DMA is **cache-coherent**, so there are **no cache
flushes** — only compiler / `mfence` barriers around ring-pointer (`TDT`/`RDT`)
writes so the descriptor stores are visible before we ring the doorbell.

> [!warning] The one thing that *does* need new mapping is the NIC's **MMIO
> register BAR**, which must be mapped **uncacheable** (see §5.2). Only RAM is
> already mapped by the HHDM; device MMIO is not.

### 1.3 A layered, ring-0, **BSP-only** stack

```
          ┌─────────────────────────────────────────┐
 Lua ───► │ net.*  (lua_net.c)                        │
          ├─────────────────────────────────────────┤
          │ socket (UDP) │ dhcp │ dns                 │
          ├──────────────┴──────┴────────────────────┤
          │ icmp │ udp                                │
          ├──────┴────────────────────────────────────┤
          │ ip (IPv4)          │ arp                   │
          ├────────────────────┴──────────────────────┤
          │ ether                                      │
          ├────────────────────────────────────────────┤
          │ netif (MAC/IP/mask/gw)                      │
          ├────────────────────────────────────────────┤
          │ e1000 (PCI + MMIO + RX/TX rings)            │
          └────────────────────────────────────────────┘
```

All ring 0, one address space (no user boundary → no copies). **Only the BSP
touches the NIC and stack state**, so there is **no cross-core locking**. Lua
worker cores (M9) do not get sockets in v1; if they ever do, they'll post work to
the BSP, not touch the ring directly.

### 1.4 Sockets: blocking-**with-timeout** over polling, not threads

A UDP socket is a bound port + a bounded RX queue. `recvfrom(timeout_ms)` is:

```c
uint64_t deadline = ktime_ms() + timeout_ms;
while (queue_empty(sock)) {
    net_poll();
    if (ktime_ms() >= deadline) return TIMEOUT;
}
```

No scheduler integration, no blocking threads — the same shape `ata.c` uses.

### 1.5 Transport = QEMU user-mode (SLIRP); NIC = Intel **e1000**

`-nic user,model=e1000` gives a userspace virtual network with a **built-in DHCP
server, DNS forwarder, and gateway** — **no root, no host bridge, works in CI and
the sandbox**. Default topology:

| Role            | Address     |
| --------------- | ----------- |
| Guest (via DHCP)| `10.0.2.15` |
| Gateway         | `10.0.2.2`  |
| DNS forwarder   | `10.0.2.3`  |
| DHCP server     | `10.0.2.2`  |

**e1000** (Intel 82540EM, PCI `8086:100E`) is the best-documented hobby NIC and
reuses the PCI-scan + BAR-map pattern already in `gfx.c`. Alternatives considered:
**rtl8139** (simpler, less representative), **virtio-net** (faster, needs
virtqueue infra — a later option).

> [!caution] SLIRP quirk: raw **ICMP echo** to the virtual gateway can be silently
> dropped unless the host permits unprivileged ping (`net.ipv4.ping_group_range`).
> Treat **UDP/DNS round-trip as the authoritative connectivity test** in CI; keep
> `net.ping` as a convenience that may be flaky under SLIRP.

---

## 2. Packet buffers (`pbuf`)

An mbuf-style buffer with **headroom** so each layer prepends its header without
copying payload.

```c
// include/net/pbuf.h
#define PBUF_SIZE     2048          // one RX slot; >= 1514 + slack
#define PBUF_HEADROOM 64            // room for eth(14)+ip(20)+udp(8) + align

typedef struct pbuf {
    uint8_t*  head;                 // start of the backing region
    uint8_t*  data;                 // current payload start (moves with push/pull)
    uint32_t  len;                  // current payload length
    uint32_t  cap;                  // total backing capacity
    uintptr_t phys;                 // physical address of head (for TX descriptors)
    struct pbuf* next;              // free-list / queue link
} pbuf;

pbuf* pbuf_alloc(void);             // from a preallocated frame pool
void  pbuf_free(pbuf* p);
void* pbuf_push(pbuf* p, uint32_t n); // grow toward head, returns new data ptr
void* pbuf_pull(pbuf* p, uint32_t n); // shrink from head (consume a header)
```

Buffers come from a **fixed pool** carved out of frames at `net_init()` (e.g. 64
RX + 64 TX + 32 spare = 160 × 2 KiB ≈ 320 KiB). No per-packet `frame_alloc()` on
the hot path. Each pool slot records its `phys` once.

> [!tip] TX build pattern: `pbuf_alloc()`, write payload at `data`, then
> `udp_output` → `ip_output` → `ether_output` each `pbuf_push()` their header and
> fill it. One buffer, zero copies, ends at the Ethernet header ready for the ring.

---

## 3. The e1000 driver

### 3.1 Bring-up sequence

1. **Find it:** `pci_find(0x8086, 0x100E)` → bus/dev/func.
2. **Enable bus mastering** (mandatory — without it the card cannot DMA): set
   bit 2 (`0x04`) of the PCI command register at config offset `0x04`.
3. **Map BAR0** (32-bit memory BAR) at a fixed VA `NICWIN_VA`, **uncacheable**
   (§5.2). Register access is `volatile` 32-bit reads/writes at `bar0 + off`.
4. **Reset:** set `CTRL.RST` (bit 26), poll until it self-clears, small delay.
5. **Read MAC:** from `RAL0`/`RAH0` (QEMU preloads them), or via the EEPROM read
   register `EERD` (words 0–2). Program it back into `RAL0`/`RAH0` with `RAH0.AV`.
6. **Clear the multicast table** `MTA[0..127]` = 0.
7. **RX ring** (§3.3), then **TX ring** (§3.4).
8. **Link up:** `CTRL.SLU` (bit 6), `CTRL.ASDE` (bit 5); optionally poll
   `STATUS.LU` (bit 1).
9. **Interrupts stay masked:** write `IMC = 0xFFFFFFFF` (we poll).

### 3.2 Register map (BAR0 MMIO offsets)

| Reg      | Offset  | Purpose                                   |
| -------- | ------- | ----------------------------------------- |
| `CTRL`   | `0x0000`| device control (RST, SLU, ASDE)           |
| `STATUS` | `0x0008`| link status (LU)                          |
| `EERD`   | `0x0014`| EEPROM read (MAC)                          |
| `ICR`    | `0x00C0`| interrupt cause read                      |
| `IMS`    | `0x00D0`| interrupt mask set                        |
| `IMC`    | `0x00D8`| interrupt mask clear                      |
| `RCTL`   | `0x0100`| receive control                          |
| `TCTL`   | `0x0400`| transmit control                          |
| `TIPG`   | `0x0410`| transmit inter-packet gap                 |
| `RDBAL`  | `0x2800`| RX descriptor base (low 32)               |
| `RDBAH`  | `0x2804`| RX descriptor base (high 32)              |
| `RDLEN`  | `0x2808`| RX ring length in bytes                   |
| `RDH`    | `0x2810`| RX head (card-owned)                      |
| `RDT`    | `0x2818`| RX tail (driver-owned)                    |
| `TDBAL`  | `0x3800`| TX descriptor base (low 32)               |
| `TDBAH`  | `0x3804`| TX descriptor base (high 32)              |
| `TDLEN`  | `0x3808`| TX ring length in bytes                   |
| `TDH`    | `0x3810`| TX head (card-owned)                      |
| `TDT`    | `0x3818`| TX tail (driver-owned)                    |
| `MTA`    | `0x5200`| multicast table array (128 × 32-bit)      |
| `RAL0`   | `0x5400`| receive address low (MAC[0..3])           |
| `RAH0`   | `0x5404`| receive address high (MAC[4..5] + AV)     |

### 3.3 RX ring

Legacy 16-byte descriptor:

```c
typedef struct __attribute__((packed)) {
    uint64_t addr;      // buffer physical address
    uint16_t length;    // bytes written by the card
    uint16_t checksum;
    uint8_t  status;    // bit0 DD (done), bit1 EOP
    uint8_t  errors;
    uint16_t special;
} e1000_rx_desc;
```

Setup: `N_RX = 256` descriptors in one frame (256 × 16 = 4096 B). Allocate 256
`pbuf`s, write each `pbuf->phys` into `desc[i].addr`, clear `status`. Program
`RDBAL/RDBAH` = ring phys, `RDLEN` = 4096, `RDH = 0`, `RDT = N_RX - 1` (all slots
handed to the card). Then:

```
RCTL = EN(1<<1) | BAM(1<<15) | SECRC(1<<26) | BSIZE_2048(00b)   // + UPE/MPE if promiscuous
```

`e1000_rx_poll()`:

```
while desc[rx_cur].status & DD:
    len = desc[rx_cur].length
    hand pbuf(rx_cur) up to ether_input(data, len)   // copy out or swap in a fresh pbuf
    refill desc[rx_cur] (new/again buffer), clear status
    RDT = rx_cur                                       // return slot to card
    rx_cur = (rx_cur + 1) % N_RX
```

### 3.4 TX ring

```c
typedef struct __attribute__((packed)) {
    uint64_t addr;
    uint16_t length;
    uint8_t  cso;       // checksum offset
    uint8_t  cmd;       // bit0 EOP, bit1 IFCS, bit3 RS
    uint8_t  status;    // bit0 DD
    uint8_t  css;
    uint16_t special;
} e1000_tx_desc;
```

`N_TX = 256`. `TDBAL/TDBAH` = ring phys, `TDLEN` = 4096, `TDH = TDT = 0`.

```
TCTL = EN(1<<1) | PSP(1<<3) | (0x0F << 4 /*CT*/) | (0x40 << 12 /*COLD full-duplex*/)
TIPG = 0x0060200A     // standard IEEE 802.3 gap
```

`e1000_tx(pbuf)`:

```
d = &desc[tx_tail]
d.addr = pbuf->phys; d.length = pbuf->len
d.cmd = EOP(1) | IFCS(2) | RS(8)        // card appends CRC (IFCS), reports DD (RS)
d.status = 0
tx_tail = (tx_tail + 1) % N_TX
mfence
TDT = tx_tail                            // doorbell
```

TX completion is reaped lazily (check `status & DD`) or ignored for fire-and-forget
UDP; the ring is large enough that back-pressure is rare at shell speeds.

---

## 4. The stack layers

### 4.1 Endianness + checksum

Network byte order is **big-endian**; x86 is little-endian. Helpers in `net.h`:
`htons/htonl/ntohs/ntohl`. The **Internet checksum** (RFC 1071) is shared by IP,
ICMP, UDP:

```c
uint16_t inet_csum(const void* data, size_t len, uint32_t seed) {
    uint32_t sum = seed;
    const uint16_t* p = data;
    while (len > 1) { sum += *p++; len -= 2; }
    if (len) sum += *(const uint8_t*)p;         // odd tail
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}
```

### 4.2 Ethernet (`ether`)

14-byte header `{dst[6], src[6], ethertype}`. `ether_input` dispatches:
`0x0806` → ARP, `0x0800` → IPv4 (drop others). `ether_output(pbuf, dst_mac,
ethertype)` `pbuf_push(14)` and fills, then `e1000_tx`.

### 4.3 ARP (`arp`)

- **Cache:** small fixed table `{ip, mac, state, expiry}` (state = FREE / PENDING /
  RESOLVED), LRU eviction.
- `arp_resolve(ip)`: RESOLVED → return MAC; else broadcast an ARP request, mark
  PENDING, and **queue the pending pbuf** on that entry (one deep) to flush on
  reply. IP output that can't resolve immediately returns "would-block"; `net_poll`
  retransmits the request on a timer (≈1 s, a few tries).
- **Reply** to any ARP request for our IP (so the gateway/host can reach us).

### 4.4 IPv4 (`ip`)

20-byte header, no options. Build: ver/IHL `0x45`, TTL 64, proto (ICMP 1 / UDP 17),
`inet_csum` over the header, IP-ID counter, DF set, no fragmentation (payloads
kept < MTU). **Routing:** dest on-link (`(dst & mask) == (ip & mask)`) → ARP the
dest; else → ARP the **gateway**. `ip_input` verifies checksum + our-address, then
dispatches to ICMP / UDP. Fragmented packets (MF set or nonzero offset) are dropped
in v1.

### 4.5 ICMP (`icmp`)

Echo **reply** (answer pings: flip to type 0, recompute checksum, `ip_output` back
to source) and echo **request** for `net.ping` (type 8, id+seq, match replies,
measure `ktime_ms` delta).

### 4.6 UDP (`udp`) + sockets (`socket`)

8-byte header `{sport, dport, length, checksum}` with the IPv4 **pseudo-header**
checksum (src, dst, proto=17, udp-length). `udp_input` demuxes by destination port
to a bound socket; unbound → drop (or ICMP port-unreachable, optional).

Socket object:

```c
typedef struct udp_socket {
    uint16_t     local_port;
    pbuf_queue   rx;                 // bounded (drop-oldest on overflow)
    struct udp_socket* next;
} udp_socket;
```

Ephemeral ports allocated from `49152..65535`. `recvfrom` uses the poll-with-
deadline loop from §1.4 and returns `(data, src_ip, src_port)`.

### 4.7 DHCP client (`dhcp`)

State machine over UDP (client `68` → server `67`, broadcast). BOOTP header + magic
cookie `0x63825363` + options:

```
INIT ──DISCOVER(opt53=1)──► SELECTING ──OFFER(53=2)──► REQUESTING
     ──REQUEST(53=3, opt50=yiaddr, opt54=serverid)──► ACK(53=5) ──► BOUND
```

From the ACK we take `yiaddr` (our IP), option 1 (subnet mask), 3 (router/gateway),
6 (DNS), 51 (lease time). On success we program `netif` and gratuitously ARP our
new address. Lease renewal (T1/T2) is **out of scope for v1** — SLIRP leases are
effectively permanent.

### 4.8 DNS (`dns`)

Minimal resolver: build a standard query (one question, type `A`, class `IN`),
send UDP to the DHCP-provided nameserver (`10.0.2.3` under SLIRP) port 53, parse the
first `A` answer (handle name compression pointers `0xC0`). No caching, UDP only,
first answer wins. `net.resolve("host")` → dotted-quad string or `nil`.

---

## 5. Changes to existing modules

### 5.1 PCI helpers (`src/pci.c` / `include/pci.h`)

Today only `pci_read32` / `pci_write32` exist and `gfx.c` open-codes its scan. Add:

```c
bool     pci_find(uint16_t vendor, uint16_t device, uint8_t* bus, uint8_t* dev, uint8_t* fn);
uint32_t pci_bar(uint8_t bus, uint8_t dev, uint8_t fn, int n);   // read + mask
void     pci_enable_bus_master(uint8_t bus, uint8_t dev, uint8_t fn); // set cmd bit 2
```

(These also let `gfx.c` drop its inline scan later.)

### 5.2 Paging: an uncacheable MMIO flag (`src/paging.c` / `include/paging.h`)

`map_page` currently honors only `PAGEF_P | PAGEF_RW | PAGEF_U`. Add
**`PAGEF_UC`** → set `PCD` (bit 4) [+ `PWT`, bit 3] in the PTE so the NIC's
register window is strongly-ordered uncached. Map BAR0 at a dedicated
`NICWIN_VA` (mirroring `gfx.c`'s `FBWIN_VA = 0xffffe00000000000`), e.g.
`0xffffe00010000000`.

> [!danger] Mapping device registers **cacheable** is a classic silent-corruption
> bug: writes to `TDT`/`RDT` sit in the cache and the doorbell never rings. `UC` is
> not optional here.

### 5.3 Boot (`src/kernel.c`)

Call `net_init()` after `ata_init()` and before `shell_run()` (~line 345–447).
"No NIC found" logs a line and returns — networking is optional, boot must not
regress.

### 5.4 Shell idle pump (`src/shell.c` / `console_getch`)

The `shell_read_line` loop (`src/shell.c:117`) blocks on `console_getch()`. Pump
`net_poll()` from that idle path so the stack stays live at the prompt.

### 5.5 Lua registration

Register the `net` library next to `k` / `fs` / `fb` / `pci` (via the shell's lib
table).

### 5.6 Build + QEMU (`Makefile`)

- Add `src/net/*.c` to the object list (globbed like the rest).
- `run` and the test harness: append `-nic user,model=e1000`.
- Optional capture for debugging: `-object filter-dump,id=d,netdev=...,file=net.pcap`.

### 5.7 Tests (`tests/`)

`tests/net-smoke.sh`: boot headless → `net.dhcp()` (expect `10.0.2.15`) →
`net.resolve("example.com")` (non-nil) → UDP round-trip against `10.0.2.3:53` →
print a marker. Wire into `make test` and CI (QEMU already installed there).

---

## 6. The `net.*` Lua API

```lua
-- interface / config
net.dhcp([timeout_ms])            -- autoconfigure; -> ip | nil, err
net.config{ip=, mask=, gw=, dns=} -- static config
net.ip()                          -- current dotted-quad, or nil
net.mac()                         -- "52:54:00:12:34:56"
net.up()                          -- link status boolean

-- diagnostics
net.ping(host [, timeout_ms])     -- -> latency_ms | nil        (flaky under SLIRP)
net.resolve(name [, timeout_ms])  -- DNS A -> "ip" | nil

-- UDP sockets
local s = net.udp()
s:bind(port)                      -- 0 = ephemeral
s:sendto(ip, port, data)
local data, ip, port = s:recvfrom([timeout_ms])
s:close()
```

Example (`build/scripts/net.lua`):

```lua
print("dhcp ->", net.dhcp())
print("ping gw ->", net.ping("10.0.2.2"))
print("example.com ->", net.resolve("example.com"))
local s = net.udp(); s:bind(0)
s:sendto("10.0.2.3", 53, dns_query("juampi.os"))   -- raw DNS demo
print(s:recvfrom(2000))
```

---

## 7. Modules (files) index

> [!note] New tree under `src/net/` + `include/net/`.

| File | Responsibility |
| ---- | -------------- |
| `pbuf.[ch]`     | packet buffer + pool (§2) |
| `e1000.[ch]`    | PCI probe, BAR map, RX/TX rings, `e1000_tx`, `e1000_rx_poll` (§3) |
| `netif.[ch]`    | interface state (MAC/IP/mask/gw), TX/RX glue |
| `ether.[ch]`    | Ethernet framing + dispatch (§4.2) |
| `arp.[ch]`      | ARP cache + resolve (§4.3) |
| `ip.[ch]`       | IPv4 parse/build, checksum, routing (§4.4) |
| `icmp.[ch]`     | echo request/reply (§4.5) |
| `udp.[ch]`      | UDP + port demux (§4.6) |
| `socket.[ch]`   | UDP sockets + queues (§4.6) |
| `dhcp.[ch]`     | DHCP client (§4.7) |
| `dns.[ch]`      | A-record resolver (§4.8) |
| `net.[ch]`      | `net_init`, `net_poll`, byte-order + checksum |
| `lua_net.c`     | `net.*` bindings (§6) |

**Edited:** `pci.c`/`pci.h`, `paging.c`/`paging.h`, `kernel.c`, `shell.c`,
`Makefile`, Lua lib registration, `tests/net-smoke.sh`.

---

## 8. Phased plan

| Phase | Deliverable | Verify |
| ----- | ----------- | ------ |
| **0 ✅** | e1000 up: PCI probe, BAR map, RX/TX rings, raw TX/RX | **done** — card up, MAC/link, DMA rings working |
| **1 ✅** | Ethernet + ARP + IPv4 + ICMP | **done** — `net.ping("10.0.2.2")` replies (~0.1–0.9 ms), answers pings too; `tests/net-smoke.sh` in `make test` |
| **2** | UDP + sockets + DHCP + DNS *(v1 ships here)* | `net-smoke.sh`: DHCP `10.0.2.15`, DNS resolves, UDP round-trip; added to CI |
| **3** | TCP (handshake, 1 connection, RTO, active connect) — one-shot HTTP GET | *scoped separately; ≈ size of 0–2 combined* |
| **4** | Optional: interrupt-driven RX, virtio-net, `net.stat()` introspection | — |

---

## 9. Risks & gotchas

- [ ] **Bus mastering** must be enabled or the card silently never DMAs (§3.1).
- [ ] **MMIO must be `UC`** or doorbell writes get cached and lost (§5.2).
- [ ] **Endianness** everywhere at the wire boundary — wrap every multi-byte
      wire field in `hton*/ntoh*`.
- [ ] **DMA barriers**: `mfence` before writing `TDT`/`RDT`; descriptor stores must
      land before the doorbell.
- [ ] **SLIRP ICMP** may be dropped without unprivileged-ping host config — rely on
      UDP/DNS for the CI liveness check (§1.5).
- [ ] **No fragmentation / no TCP** in v1 — keep UDP payloads < MTU (1472 B for
      IPv4/UDP over 1500 MTU).
- [ ] **BSP-only**: never call the stack from a Lua worker core.

---

## 10. Open decisions

1. **v1 scope** — recommend **Phases 0–2** (raw → ping → UDP/DHCP/DNS); TCP as a
   separate effort.
2. **NIC** — **e1000** (recommended) vs rtl8139 (simpler) vs virtio-net (faster).
3. **Poll vs interrupt** — start **polled** (recommended); revisit only if latency
   matters.

---

## 11. Mapping to real hardware

> [!abstract] The point
> QEMU's `e1000` is **not** a QEMU invention — it is a register-accurate emulation
> of a real chip, the **Intel 82540EM** (gigabit PCI NIC, ~2002, "PRO/1000 MT").
> The register map in §3.2 *is* Intel's datasheet. So `e1000.c` is a **real NIC
> driver**: boot this OS on a machine with an 82540EM-family card and it works
> unchanged. The emulation *is* the hardware interface.

### 11.1 The ladder of "realness"

How much of the stack survives contact with a physical machine:

| Part | On real hardware |
| ---- | ---------------- |
| Ethernet / ARP / IP / ICMP / UDP (§4) | **Identical.** Just RFC byte layouts — nothing hardware-specific. |
| PCI config access (`0xCF8`/`0xCFC`) | **Real.** A genuine x86 CPU mechanism on every PC. |
| BAR / MMIO register block (§3.2) | **Real.** The card really exposes those registers in the physical address space. |
| DMA + descriptor rings (§3.3–3.4) | **Real.** A physical card really DMAs to/from RAM through those exact ring structures. |
| MAC address | **Differs.** QEMU makes one up (`52:54:00:…`); a real card has it **burned in an EEPROM**, read via `EERD`. |
| The network itself (SLIRP) | **Differs.** Fake gateway/DHCP/DNS → a real switch/router and your real LAN's addresses. |

> [!note] Structural consequence: **only `e1000.c` is hardware-specific.** Swap the
> NIC and you swap that one file — the `netif` seam (§1.3) means Ethernet and up
> never change. Writing `realtek.c` later reuses 100% of the stack.

### 11.2 What actually differs on bare metal

> [!danger] Forgiving in QEMU, **load-bearing on metal**
> - **Uncached MMIO (`PAGEF_UC`, §5.2) + `mfence` barriers (§1.2)** — QEMU may
>   tolerate a cacheable register mapping; real hardware **silently corrupts**
>   (the doorbell write sits in cache, the card never sees it). These are no-ops in
>   emulation but mandatory on metal.

- **Which NIC you have.** The 82540EM is a *specific old* chip. A modern laptop has
  something else (Intel I219/I225, Realtek, Wi-Fi) needing *its own* driver. To run
  `e1000.c` as-is, use a machine with an e1000-family card or add a cheap Intel
  PRO/1000 card. The stack above is unaffected.
- **PCIe vs legacy PCI.** Modern machines are PCI*e*, but it stays
  config-space-compatible, so `0xCF8`/`0xCFC` legacy access still finds the device.
- **The IOMMU (VT-d).** With an IOMMU active, the address the card DMAs to is
  *remapped* — the physical address from `frame_alloc()` is no longer what the card
  pokes, and unmapped DMA faults. QEMU defaults to no IOMMU (raw physical access),
  which is why the simple plan works; on metal you'd program its tables or run with
  it off.
- **Interrupts, eventually.** Polling behaves identically on both, but real traffic
  makes it wasteful — bare-metal networking usually *wants* interrupts, which loops
  back to the deferred LAPIC/IOAPIC routing (Phase 4).
- **Physical reality.** Link negotiation, cable/no-cable, slow power-up, EEPROM
  quirks — QEMU gives a perfect always-connected card; metal makes you wait for
  `STATUS.LU` and handle "unplugged."

### 11.3 Same bridge as graphics

This mirrors the display work in [[x86-64-port]] / the `gfx` driver: QEMU's
`stdvga` emulates the Bochs display interface; the driver finds its BAR, maps it,
and drives mode-set registers — the *approach* (PCI → map BAR → poke registers) is
real, only the *specific device* varies. A real GPU needs a real GPU driver; a real
NIC needs a real NIC driver. Identical skeleton.

**To actually go bare metal:** (a) have an e1000-family card or write a driver for
the NIC present; (b) read the MAC from EEPROM instead of trusting `RAL0/RAH0`;
(c) keep the `UC` mapping + barriers correct (already in the plan); (d) handle the
IOMMU if enabled. Everything else is the same bytes on the same wire.

---

## References

- OSDev — [Intel 8254x/e1000](https://wiki.osdev.org/Intel_8254x),
  [PCI](https://wiki.osdev.org/PCI), [ARP](https://wiki.osdev.org/ARP)
- Intel — *PCI/PCI-X Family of Gigabit Ethernet Controllers SDM* (82540EM register
  reference)
- RFCs — 826 (ARP), 791/792 (IPv4/ICMP), 768 (UDP), 2131 (DHCP), 1035 (DNS),
  1071 (checksum)
- QEMU — [user-mode / SLIRP networking](https://www.qemu.org/docs/master/system/devices/net.html)

See also: [[x86-64-port]] (paging/HHDM/PCI/interrupts foundation), [[lua-shell]]
(the polled-SMP model this reuses), [[Index]].
