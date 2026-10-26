# jai-mcp — MCP servers for the JAI agent

Custom tool servers exposed to the opencode core. Planned set:

| Server | Purpose |
|---|---|
| jai-system | /dev/jai link, service health, dmesg tail |
| jai-nas | file operations on NAS mounts |
| jai-home | Tailscale device control (Phase 4+) |

Each server is a small stdio MCP program (Python, 3.14) in this directory.
Phase 2+.
