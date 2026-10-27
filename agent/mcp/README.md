# jai-mcp — MCP servers for the JAI agent

Custom tool servers exposed to the opencode core. Wired via
`agent/opencode.json` → `mcp` → spawned by opencode over stdio.

| Server | Status | Purpose |
|---|---|---|
| jai-system | ✅ **implemented** (`jai-system.py`) | /dev/jai link, dmesg, service health, sysinfo, /var/lib/jai-memory |
| jai-nas | ⬜ Phase 4+ | file operations on NAS mounts |
| jai-home | ⬜ Phase 4+ | Tailscale device control |

## jai-system — the agent↔kernel/OS bridge

**Zero dependencies by design** (raw MCP = JSON-RPC 2.0 over stdio, hand
implemented) so it runs in a debootstrap minbase rootfs with nothing but
`python3`. Deployed to `/usr/local/jai/mcp/jai-system.py` by
`scripts/phase2.sh`.

Tools exposed to the agent:

| Tool | What it does |
|---|---|
| `jai_kernel_status` | read `/dev/jai` → proves the kernel link |
| `jai_kernel_event` | write ≤63-byte event to `/dev/jai` → kernel log `jai: event: …` |
| `jai_dmesg` | kernel log tail (`journalctl -k`, needs `systemd-journal` group) |
| `jai_services` | active/enabled state of jai-agent, jai-gateway, jai-link |
| `jai_sysinfo` | kernel, uptime, load, memory, root disk |
| `jai_memory_read` / `jai_memory_write` | persistent memory in `/var/lib/jai-memory/<key>` |

Protocol notes: stdout is the protocol channel (logs → stderr only);
tool failures are returned as `isError` results — the server never crashes
the agent session. Boot-time kernel handshake is `jai-link.service`
(overlay), the runtime path is this MCP server.
