# jai-mcp - MCP servers for the JAI agent

Custom tool servers exposed to the opencode core. Wired via
`agent/opencode.json` -> `mcp.servers` -> spawned by opencode over stdio.

| Server | Status | Purpose |
|---|---|---|
| **jai-mcp** | **canonical** (`jai-mcp.c`, C) | /dev/jai link + events, dmesg, service health/status/restart, sysinfo, disk usage, /var/lib/jai-memory |
| jai-system | reference (`jai-system.py`, python, disabled in config) | original Phase 3 first cut; kept as the readable spec the C server was ported from |
| jai-nas | Phase 4+ | file operations on NAS mounts |
| jai-home | Phase 4+ | Tailscale device control |

## jai-mcp - the hands (C, canonical since Phase 3b)

Hand-written JSON-RPC 2.0 over stdio in C against `libjson-c`. Same tool
names as jai-system.py (so IDENTITY.md and muscle memory survive) plus
three additions.

**Why C:** zero runtime deps beyond `libjson-c5` (no python needed by the
bridge), one small binary, and the same toolchain that builds the kernel
module. **The trade-off, handled:** C is where memory-safety bugs would
live, and this is the layer that parses model-generated JSON - so every
argument passes a charset allowlist, every buffer is fixed-size, all JSON
is built with json-c (never string-concatenated), subprocesses are
fork+execvp (never popen), and the whole server runs unprivileged.

Deployed to `/usr/bin/jai-mcp` (built INSIDE the chroot by `phase2.sh`
step 5m against the rootfs glibc - build-host != target, the recurring
lesson). Live-VM install without a re-bake: `scripts/vm-jai-mcp.sh`.

### Tools exposed to the agent

| Tool | What it does |
|---|---|
| `jai_kernel_status` | read `/dev/jai` -> drains pending events, or the kernel's status line when the ring is empty |
| `jai_kernel_event` | write <=254 bytes to `/dev/jai` -> ring + kernel log `jai: event: ...`, returns the read-back as proof |
| `jai_events` | tail of `/var/lib/jai/events.jsonl` - recent kernel events captured by jai-eventd (Phase 3c reflex arc); GAP records surface ring-overflow losses |
| `jai_dmesg` | kernel log tail (`journalctl -k`, `dmesg` fallback; needs `systemd-journal` group) |
| `jai_services` | active/enabled state of jai-agent, jai-eventd, jai-gateway, jai-heartbeat, jai-link |
| `jai_sysinfo` | host, kernel, uptime, load, memory, root disk, UTC time |
| `jai_memory_read` / `jai_memory_write` | persistent memory in `/var/lib/jai-memory/<key>` (64 KB cap, strict key charset) |
| `disk_usage` | statvfs on an absolute path - no subprocess |
| `service_status` | `systemctl status` + last 10 journal lines (read-only, any unit) |
| `service_restart` | **the only privileged action**: restarts `jai-*` units via `sudo -n` |

### The double lock (anti-ULTRON principle)

`service_restart` is gated twice, with different keys:

1. **In the server** (`jai-mcp.c`): refuses any unit not matching `jai-*`
   and any unit name with characters outside `[A-Za-z0-9_.@-]`.
2. **In the OS** (`/etc/sudoers.d/jai-mcp`, mode 440):
   `jai ALL=(root) NOPASSWD: /usr/bin/systemctl restart jai-*`

Capabilities live in the body's allowlists, never in the mind - the brain
can only ASK; the C code decides.

### Known limits (documented, by design)

- Under `jai-agent.service`, `NoNewPrivileges=yes` blocks sudo elevation,
  so `service_restart` returns sudo's refusal when run through the
  hardened daemon. Interactive `opencode run` (and the `jai` wrapper) do
  not inherit that flag and work fine. The gateway phase will get a
  proper privileged helper for the daemon path.
- `/dev/jai` access uses whatever permissions the module/node grants the
  `jai` user (same as jai-system.py had - no change).
- Protocol notes: stdout is the protocol channel (logs -> stderr only);
  tool failures are returned as `isError` results - the server never
  crashes the agent session. Boot-time kernel handshake remains
  `jai-link.service` (overlay); the runtime path is this server.
