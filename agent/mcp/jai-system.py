#!/usr/bin/env python3
# jai-system.py — MCP stdio server: the agent↔kernel/OS bridge (Phase 3, first cut)
#
# Gives the opencode agent (jai-agent) live tools over the machine it lives on:
#
#   jai_kernel_status   read /dev/jai        → proves the kernel link
#   jai_kernel_event    write /dev/jai       → userland event lands in kernel log
#   jai_dmesg           kernel log tail      (via journalctl/dmesg)
#   jai_services        health of jai-* systemd units
#   jai_sysinfo         host/kernel/uptime/mem/disk/load
#   jai_memory_read     long-term memory: /var/lib/jai-memory/<key>
#   jai_memory_write    long-term memory: /var/lib/jai-memory/<key>
#
# ZERO DEPENDENCIES on purpose: this runs inside a debootstrap minbase
# Ubuntu 24.04 (python3 only). The MCP protocol is JSON-RPC 2.0 with
# newline-delimited messages on stdio — implementing the handshake by hand
# avoids pulling pydantic/httpx/etc into a minimal rootfs.
#
# CRITICAL RULE: stdout is the protocol channel. Every log goes to stderr.
#
# Install target (see scripts/phase2.sh): /usr/local/jai/mcp/jai-system.py
# Wired into the agent via /etc/jai/opencode.json → mcp.jai-system

import json
import os
import re
import subprocess
import sys
from datetime import datetime, timezone

SERVER_NAME = "jai-system"
SERVER_VERSION = "0.1.0"
PROTOCOL_VERSION = "2024-11-05"

DEV_JAI = "/dev/jai"
MEMORY_DIR = "/var/lib/jai-memory"
MAX_LINE = 1_000_000          # 1 MB guard per inbound message
MAX_EVENT = 63                # kernel kbuf is 64 bytes incl. NUL (jai_dev.c)
MAX_MEMORY_BYTES = 64 * 1024
KEY_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$")

JAI_UNITS = ["jai-agent.service", "jai-gateway.service", "jai-link.service"]


def log(*a):
    print(*a, file=sys.stderr, flush=True)


# --------------------------------------------------------------- kernel ---
def tool_kernel_status(_: dict) -> str:
    try:
        with open(DEV_JAI, "rb") as f:
            msg = f.read().decode("utf-8", "replace").strip()
    except FileNotFoundError:
        raise RuntimeError(
            "/dev/jai does not exist — jai_dev module not loaded. "
            "Check: systemctl status systemd-modules-load; dmesg | grep jai"
        )
    except PermissionError:
        raise RuntimeError("/dev/jai exists but is not accessible (permissions)")
    except OSError as e:
        raise RuntimeError(f"/dev/jai read failed: {e}")
    return f"KERNEL LINK UP — /dev/jai says: {msg!r}"


def tool_kernel_event(args: dict) -> str:
    text = str(args.get("text", "")).strip()
    if not text:
        raise RuntimeError("need: {\"text\": \"<event, <=63 chars>\"}")
    if len(text.encode("utf-8")) > MAX_EVENT:
        raise RuntimeError(
            f"event too long ({len(text.encode('utf-8'))} bytes) — kernel buffer is {MAX_EVENT} bytes"
        )
    try:
        with open(DEV_JAI, "w") as f:
            f.write(text)
    except FileNotFoundError:
        raise RuntimeError("/dev/jai missing — kernel module not loaded")
    except OSError as e:
        raise RuntimeError(f"/dev/jai write failed: {e}")
    return (
        f"event written to kernel: {text!r}\n"
        "verify with jai_dmesg — kernel log should show: jai: event: "
        + text
    )


def tool_dmesg(args: dict) -> str:
    try:
        n = int(args.get("lines", 20))
    except (TypeError, ValueError):
        n = 20
    n = max(1, min(n, 100))
    # journalctl -k works for non-root users in the systemd-journal group;
    # plain dmesg usually needs CAP_SYSLOG (dmesg_restrict=1 on Ubuntu).
    for cmd in (
        ["journalctl", "-k", "-n", str(n), "--no-pager"],
        ["dmesg", "--color=never"],
    ):
        try:
            out = subprocess.run(
                cmd, capture_output=True, text=True, timeout=10
            )
            if out.returncode == 0 and out.stdout.strip():
                lines = out.stdout.strip().splitlines()
                return f"kernel log (last {len(lines)}):\n" + "\n".join(lines[-n:])
        except (subprocess.TimeoutExpired, FileNotFoundError):
            continue
    raise RuntimeError(
        "cannot read kernel log — is the jai user in the systemd-journal group? "
        "(sudo usermod -aG systemd-journal,adm jai)"
    )


# ------------------------------------------------------------ services ---
def _unit_state(unit: str) -> dict:
    def ctl(*p):
        r = subprocess.run(
            ["systemctl", *p], capture_output=True, text=True, timeout=10
        )
        return r.stdout.strip() if r.returncode == 0 else "unknown"

    return {
        "unit": unit,
        "active": ctl("is-active", unit),
        "enabled": ctl("is-enabled", unit),
    }


def tool_services(_: dict) -> str:
    states = []
    for u in JAI_UNITS:
        s = _unit_state(u)
        states.append(f"{s['unit']:<22} active={s['active']:<9} enabled={s['enabled']}")
    return "JAI systemd units:\n" + "\n".join(states)


# ------------------------------------------------------------- sysinfo ---
def tool_sysinfo(_: dict) -> str:
    uname = os.uname()
    try:
        with open("/proc/uptime") as f:
            up_s = int(float(f.read().split()[0]))
    except (OSError, ValueError):
        up_s = -1
    up = f"{up_s // 86400}d {up_s % 86400 // 3600}h {up_s % 3600 // 60}m" if up_s >= 0 else "?"

    mem = {}
    try:
        with open("/proc/meminfo") as f:
            for line in f:
                k, v = line.split(":", 1)
                if k in ("MemTotal", "MemAvailable"):
                    mem[k] = f"{int(v.strip().split()[0]) // 1024} MB"
    except (OSError, ValueError):
        pass

    disk = "?"
    try:
        st = os.statvfs("/")
        disk = f"{st.f_bavail * st.f_frsize // (1 << 30)} GB free / {(st.f_blocks * st.f_frsize) // (1 << 30)} GB"
    except OSError:
        pass

    try:
        with open("/proc/loadavg") as f:
            load = " ".join(f.read().split()[:3])
    except OSError:
        load = "?"

    mem_s = f"{mem.get('MemAvailable', '?')} available / {mem.get('MemTotal', '?')}"
    return (
        f"host: {uname.nodename} ({uname.sysname} {uname.release})\n"
        f"uptime: {up}\n"
        f"load: {load}\n"
        f"memory: {mem_s}\n"
        f"disk /: {disk}\n"
        f"now (UTC): {datetime.now(timezone.utc).isoformat(timespec='seconds')}"
    )


# -------------------------------------------------------------- memory ---
def _mem_path(key: str) -> str:
    if not KEY_RE.match(key or ""):
        raise RuntimeError("bad key: use [A-Za-z0-9][A-Za-z0-9._-]{0,63} (no paths)")
    return os.path.join(MEMORY_DIR, key)


def tool_memory_read(args: dict) -> str:
    path = _mem_path(str(args.get("key", "")))
    if not os.path.isfile(path):
        raise RuntimeError(f"no memory at key {args.get('key')!r} (nothing written yet)")
    with open(path, "r", errors="replace") as f:
        data = f.read(MAX_MEMORY_BYTES + 1)
    if len(data) > MAX_MEMORY_BYTES:
        data = data[:MAX_MEMORY_BYTES] + "\n…(truncated)"
    return data


def tool_memory_write(args: dict) -> str:
    path = _mem_path(str(args.get("key", "")))
    content = str(args.get("content", ""))
    if len(content.encode("utf-8")) > MAX_MEMORY_BYTES:
        raise RuntimeError(f"content too large — max {MAX_MEMORY_BYTES} bytes")
    try:
        os.makedirs(MEMORY_DIR, exist_ok=True)
        with open(path, "w") as f:
            f.write(content)
    except OSError as e:
        raise RuntimeError(f"memory write failed: {e} (is {MEMORY_DIR} writable by jai?)")
    return f"stored {len(content.encode('utf-8'))} bytes → {path}"


# ---------------------------------------------------------- tool table ---
TOOLS = [
    {
        "name": "jai_kernel_status",
        "description": "Check the kernel link: reads /dev/jai. Drains any pending kernel events first (Phase 3 ring buffer), then returns the status line. Proves the agent is connected to the actual kernel.",
        "inputSchema": {"type": "object", "properties": {}},
    },
    {
        "name": "jai_kernel_event",
        "description": "Write a short event (max 254 bytes) to /dev/jai. It enters the kernel event ring buffer AND the kernel log ('jai: event: <text>').",
        "inputSchema": {
            "type": "object",
            "properties": {"text": {"type": "string", "maxLength": 254}},
            "required": ["text"],
        },
    },
    {
        "name": "jai_dmesg",
        "description": "Tail of the kernel log.",
        "inputSchema": {
            "type": "object",
            "properties": {"lines": {"type": "integer", "minimum": 1, "maximum": 100}},
        },
    },
    {
        "name": "jai_services",
        "description": "Health of the JAI systemd units (jai-agent, jai-gateway, jai-link): active + enabled state.",
        "inputSchema": {"type": "object", "properties": {}},
    },
    {
        "name": "jai_sysinfo",
        "description": "Host summary: kernel, uptime, load, memory, root disk.",
        "inputSchema": {"type": "object", "properties": {}},
    },
    {
        "name": "jai_memory_read",
        "description": "Read long-term memory: /var/lib/jai-memory/<key>.",
        "inputSchema": {
            "type": "object",
            "properties": {"key": {"type": "string", "pattern": "^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$"}},
            "required": ["key"],
        },
    },
    {
        "name": "jai_memory_write",
        "description": "Write long-term memory (max 64 KB): /var/lib/jai-memory/<key>.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "key": {"type": "string", "pattern": "^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$"},
                "content": {"type": "string"},
            },
            "required": ["key", "content"],
        },
    },
]

TOOL_FUNCS = {
    "jai_kernel_status": tool_kernel_status,
    "jai_kernel_event": tool_kernel_event,
    "jai_dmesg": tool_dmesg,
    "jai_services": tool_services,
    "jai_sysinfo": tool_sysinfo,
    "jai_memory_read": tool_memory_read,
    "jai_memory_write": tool_memory_write,
}


# ----------------------------------------------------------- protocol ----
def reply(msg_id, result):
    send({"jsonrpc": "2.0", "id": msg_id, "result": result})


def reply_err(msg_id, code, message):
    send({"jsonrpc": "2.0", "id": msg_id, "error": {"code": code, "message": message}})


def send(obj):
    sys.stdout.write(json.dumps(obj, separators=(",", ":")) + "\n")
    sys.stdout.flush()


def handle(req: dict):
    method = req.get("method")
    msg_id = req.get("id")  # None = notification → never respond

    if method == "initialize":
        reply(msg_id, {
            "protocolVersion": PROTOCOL_VERSION,
            "capabilities": {"tools": {}},
            "serverInfo": {"name": SERVER_NAME, "version": SERVER_VERSION},
        })
    elif method == "notifications/initialized":
        pass  # notification: no response
    elif method == "ping":
        reply(msg_id, {})
    elif method == "tools/list":
        reply(msg_id, {"tools": TOOLS})
    elif method == "tools/call":
        params = req.get("params") or {}
        name = params.get("name")
        args = params.get("arguments") or {}
        fn = TOOL_FUNCS.get(name)
        if fn is None:
            reply(msg_id, {
                "content": [{"type": "text", "text": f"unknown tool: {name}"}],
                "isError": True,
            })
            return
        try:
            text = fn(args)
            reply(msg_id, {"content": [{"type": "text", "text": text}], "isError": False})
        except Exception as e:  # tool errors are RESULTS, never crashes
            log(f"tool {name} failed: {e}")
            reply(msg_id, {
                "content": [{"type": "text", "text": f"error: {e}"}],
                "isError": True,
            })
    elif msg_id is not None:
        reply_err(msg_id, -32601, f"method not found: {method}")


def main():
    log(f"{SERVER_NAME} v{SERVER_VERSION} — agent↔kernel bridge online (pid {os.getpid()})")
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        if len(line) > MAX_LINE:
            log("oversized message dropped")
            continue
        try:
            req = json.loads(line)
        except json.JSONDecodeError as e:
            send({"jsonrpc": "2.0", "id": None,
                  "error": {"code": -32700, "message": f"parse error: {e}"}})
            continue
        try:
            handle(req)
        except Exception as e:  # never let the loop die
            log(f"handler crashed: {e}")
            msg_id = req.get("id") if isinstance(req, dict) else None
            if msg_id is not None:
                reply_err(msg_id, -32603, "internal error")
    log("stdin closed — bridge down")


if __name__ == "__main__":
    main()
