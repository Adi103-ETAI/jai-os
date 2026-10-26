# JAI — Architecture

JAI ("Jay") is a persistent OS-level assistant built **on top of** Ubuntu,
not a kernel fork. Borrow the base, own the layers above.

## Layer map

```
┌───────────────────────────────────────────────┐
│ JAI LAYER (ours — all custom code)            │
│  jai-gateway   : voice/IO front-end (Phase 4) │
│  jai-agent     : opencode serve, the brain    │
│  jai-mcp       : custom MCP tool servers      │
│  kernel/jai_dev: /dev/jai kernel interface    │
├───────────────────────────────────────────────┤
│ BASE (borrowed)                               │
│  Ubuntu 24.04 minimal userland (debootstrap)  │
│  systemd, apt, glibc                          │
├───────────────────────────────────────────────┤
│ KERNEL (borrowed)                             │
│  Stock Ubuntu linux-image-generic             │
│  + our out-of-tree modules only               │
└───────────────────────────────────────────────┘
```

## Runtime flow

```
voice/CLI/API ──► jai-gateway (systemd)
                      │ wake word, STT, intent
                      ▼
              jai-agent = opencode serve (HTTP API)
                      │
        ┌─────────────┼──────────────┐
        ▼             ▼              ▼
   jai-mcp        kernel link     NAS / Tailscale
   tool servers   /dev/jai        (memory, devices)
```

## Build flow

debootstrap minimal rootfs → copy overlay/ → chroot: install agent +
services → mkosi → bootable image → test in 1 GB KVM VM → real hardware.

## Hardware reality (build/test host)

AMD Athlon Silver 3050U: 2 cores/3.2 GHz, 9.7 GB RAM, 56 GB free disk.
Everything here is integration work — no kernel compiles, no heavy code
builds — so this box is sufficient. Full kernel rebuilds (only if we ever
patch kernel internals) would take 1.5–2.5 h and are explicitly out of scope.

## Phase plan

| Phase | Goal | Status |
|---|---|---|
| 0 | Toolchain + first kernel module | in progress |
| 1 | Rootfs boots in test VM | next |
| 2 | Overlay + agent baked into image | planned |
| 3 | /dev/jai event ring + agent integration | planned |
| 4 | Voice I/O + remote access | planned |
| 5 | First real-hardware boot | planned |
