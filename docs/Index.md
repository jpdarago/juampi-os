---
title: Index
tags: [moc, index]
---

# juampiOS — design vault

Map of content for the juampiOS design notes. This folder is an
[Obsidian](https://obsidian.md) vault: notes use `[[wikilinks]]`, YAML
frontmatter properties (`tags`, `status`, `milestone`), and callouts. Open the
`docs/` folder as a vault to get the graph view and backlinks.

## Notes

- [[x86-64-port]] — migrating the kernel from 32-bit protected mode to 64-bit
  long mode under Limine. *Status: complete.*
- [[lua-shell]] — booting into a parallel, ring-0 Lua shell with per-core
  interpreters and shared memory. *Status: complete.*
- [[networking]] — TCP/IP stack design: e1000 driver, polled execution model,
  Ethernet/ARP/IPv4/ICMP/UDP, DHCP/DNS, and a `net.*` socket library.
  *Status: planned (M10).*

## By status

| Note            | Status            | Milestone |
| --------------- | ----------------- | --------- |
| [[x86-64-port]] | complete          | —         |
| [[lua-shell]]   | complete          | M8–M9     |
| [[networking]]  | planned           | M10       |

## Conventions

- **Frontmatter** — every note carries `title`, `tags`, and (for feature work)
  `status` + `milestone`.
- **Links** — relate notes with `[[note-name]]` (filename without `.md`).
- **Callouts** — `> [!abstract]` goals, `> [!warning]`/`> [!danger]` gotchas,
  `> [!note]` asides.
