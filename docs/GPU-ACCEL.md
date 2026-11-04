# Tier-1 GPU acceleration — the Vega iGPU unlock (2026-11-04)

What happened: tier-1 (qwen3:0.6b) now runs **100% on the GPU** — the
integrated Radeon inside the host's Athlon Silver 3050U — via Ollama's
Vulkan backend. Verified live: `ollama ps` → PROCESSOR `100% GPU`,
926 MB resident, answers still correct.

## The hardware, in simple words

    ┌───────────────────────────────────────────┐
    │  AMD Athlon Silver 3050U (Picasso/Raven2) │
    │                                           │
    │   ┌────────┐      ┌───────────────┐       │
    │   │  CPU   │      │  Vega iGPU    │       │
    │   │  cores │      │  (gfx90c)     │       │
    │   └───┬────┘      └───────┬───────┘       │
    │       └──── SHARED RAM ────┘               │
    └───────────────────────────────────────────┘

- The GPU is not a separate card; it lives inside the CPU package and
  shares system RAM (an "APU"). Host is bare metal (`systemd-detect-virt`
  = none) and headless (blank display, SSH-only). Headless is ideal:
  nothing else competes for the GPU.
- Shared RAM means: GPU offload moves the MATH off the CPU cores, but
  both sip from the same memory pipe. Expect CPU relief, not 2x speed.

## Why ROCm was never an option

Ollama's official ROCm support table (checked against current docs):
Radeon RX 9000/7000/6000 series, Radeon PRO, Instinct, Ryzen AI APUs
(gfx1150/gfx1151). Picasso/Raven2 = gfx90c — **not on the list, and not
worth hacking** (HSA_OVERRIDE_GFX_VERSION=9.0.0 would map to gfx900
kernels never tuned for this iGPU; unstable, unsupported). ROCm: closed.

## The door that worked: Vulkan

Ollama ships a Vulkan backend, enabled by default in current builds.
Vulkan reaches AMD GPUs through Mesa's RADV driver — and RADV supports
every GCN chip, including Raven2 iGPUs. Ollama's own docs say it:
"Additional AMD GPU support is provided by the Vulkan Library."

## The two red herrings (so future-us doesn't chase them again)

1. `vulkaninfo` said "Permission denied on /dev/dri/renderD128" and fell
   back to `llvmpipe` (a fake CPU-GPU). Cause: it ran over SSH as user
   `kali`, and SSH sessions get no console-seat ACL; kali also lacked the
   `render` group. Cosmetic only — the *service* user's view is what
   counts, and the ollama service saw the GPU all along.
2. "could not connect to ollama server" right after `systemctl restart`
   — the server was still waking up; `ollama ps` one second later
   connected fine. A race, not a crash.

## The golden clue

`journalctl -u ollama`:

    dropping integrated GPU; to enable, set OLLAMA_IGPU_ENABLE=1
    id=0 library=Vulkan description="AMD Radeon Graphics (RADV RAVEN2)"
    pci_id=0000:02:00.0

Ollama FOUND the GPU and deliberately skipped it: iGPUs are parked by
default. One environment variable releases the handbrake.

## The fix (reproducible on any machine)

    # 1. Vulkan userspace for AMD (usually already present)
    sudo apt install -y mesa-vulkan-drivers vulkan-tools

    # 2. Service user needs the render/video groups
    sudo usermod -aG render,video ollama

    # 3. Un-park the iGPU (drop-in — survives reboots)
    sudo tee /etc/systemd/system/ollama.service.d/igpu.conf >/dev/null <<'EOF'
    [Service]
    Environment=OLLAMA_IGPU_ENABLE=1
    EOF
    sudo systemctl daemon-reload && sudo systemctl restart ollama

    # 4. Verify
    ollama run qwen3:0.6b "hi" && ollama ps    # PROCESSOR: 100% GPU

Rollback (if a future Ollama version misbehaves on Vulkan):

    sudo rm /etc/systemd/system/ollama.service.d/igpu.conf
    sudo systemctl daemon-reload && sudo systemctl restart ollama

(or keep the file and set `OLLAMA_VULKAN=0` — CPU fallback is instant).

## Measured (2026-11-04, host with VM running)

| Metric              | Before     | After              |
|---------------------|------------|--------------------|
| ollama ps PROCESSOR | 100% CPU   | **100% GPU**       |
| Model resident      | 1.0 GB     | 926 MB             |
| keep_alive          | 24h        | 24h (unchanged)    |
| Host load / idle    | —          | 0.63 / 87.5% idle  |
| Swap used           | 0          | 0                  |

## QEMU audit (same session — the VM is NOT the CPU hog)

Running command: `qemu-system-x86_64 -accel kvm -cpu host -m 1024 -smp 1
-kernel ... -drive ...,if=virtio -netdev user,hostfwd=tcp::2222-:22
-device e1000e ... -nographic`

- ✅ `-accel kvm` — hardware acceleration ON (no TCG emulation disaster)
- ✅ `-cpu host`, virtio disk
- ⚠️ optional: `-smp 2` (VM currently 1 vCPU), `virtio-net-pci` instead
  of `e1000e` (less CPU per packet)
- QEMU idles ~3% CPU. The SSH lag of the llama.cpp era is gone.

## Known TODO before Phase 6b (gate → tier-1 from inside the VM)

Ollama currently binds `127.0.0.1:11434` (OLLAMA_HOST default) — the VM
CANNOT reach it yet. Expose it on all interfaces (one more drop-in):

    sudo tee /etc/systemd/system/ollama.service.d/host.conf >/dev/null <<'EOF'
    [Service]
    Environment=OLLAMA_HOST=0.0.0.0:11434
    EOF
    sudo systemctl daemon-reload && sudo systemctl restart ollama

VM-side URL becomes `http://10.0.2.2:11434` (slirp gateway = host).
The static severity table stays the fallback — tier-1 is an accelerator,
never a dependency.
