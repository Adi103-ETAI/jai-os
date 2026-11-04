# jai-capabilities — the boot-time hardware probe (roadmap 2b)

> Spec BEFORE code. This document is the contract: the JSON schema, the
> ladder thresholds, the accel chain. The C code must obey this file.
> Law 1 (VISION.md): **measure the machine, adapt, degrade gracefully,
> never require.** This probe is Law 1 made executable.

## What it is, in plain words

At every boot, a tiny C program looks at the machine — CPU, RAM, GPU,
disk, network, display — and writes ONE truth file:

    /etc/jai/capabilities.json

Everything AI-related in JAI OS reads that file instead of guessing.
The profile (minimal / balanced / full) picks ITSELF from measured facts.
No human configures hardware. That is the whole point.

    BOOT
     │
     ▼
    jai-capabilities (oneshot, < 1 s, read-only probing)
     │ writes
     ▼
    /etc/jai/capabilities.json
     │
     ├──► brain/wrapper   which local model size, cloud on/off
     ├──► jai-gate (6b)   budget defaults per profile
     ├──► phase2.sh       bake the RIGHT model at build time
     ├──► vm-doctor.sh    sanity: file valid, profile sane
     └──► M4 desktop      DE picker (later), voice (FULL only)

## The two axes — measure independently, never conflate

Axis 1 answers "how much computer?" (RAM + cores only).
Axis 2 answers "where does heavy math run?" (GPU probe).
A 16 GB headless server is FULL-compute with NO GPU — both facts are
true at once and must survive independently.

### Axis 1 — the ladder (profile)

| Profile  | Threshold (measured)                  | Face (VISION ladder)                    |
|----------|----------------------------------------|-----------------------------------------|
| FULL     | ram_mb >= 15360 AND cores >= 4         | desktop UI, bigger local models, voice  |
| BALANCED | ram_mb >= 7600 AND cores >= 2          | light desktop UI, small local model     |
| MINIMAL  | everything else                        | headless/light, rules + tiny model      |

- Thresholds are nominal-RAM minus margin (MemTotal undercounts: a
  "16 GB" box reports ~15360; "8 GB" reports ~7600). One rule, no fudge.
- profile = the HIGHEST rung whose ALL conditions pass.
- ram_mb < 1900 → profile = minimal AND `below_floor: true` — still
  boots, kernel reflexes always on (they cost almost zero), AI = rules.
- Kernel reflexes (jai_dev, eventd) run on EVERY profile. Never gated.

### Axis 2 — the accel chain (GPU → CPU → rules)

| accel   | When                                                    |
|---------|----------------------------------------------------------|
| cuda    | NVIDIA driver + nvidia-smi present                       |
| rocm    | ROCm stack present on a SUPPORTED chip (per Ollama list) |
| vulkan  | Mesa Vulkan present AND a real device (NOT llvmpipe)     |
| cpu     | nothing usable found — tier-1 still runs, on CPU         |

- **llvmpipe is explicitly rejected.** It is a CPU pretending to be a
  GPU (the red herring from GPU day — vulkaninfo over SSH reported it).
  If the only "device" is llvmpipe → accel = cpu. This check is NOT
  optional.
- accel = cpu is a NORMAL state, not an error. The fallback chain is
  the reference pattern: **GPU if present → CPU if not → static rules
  if the model is unreachable.** Nothing in JAI OS may be weaker.
- v1 does not run a render test. Detecting libraries + a non-llvmpipe
  device is enough; the REAL proof is tier-1's own fallback chain.

## Probe table — what gets measured, and from where

| Fact        | Source (exact)                                   | Missing → |
|-------------|---------------------------------------------------|-----------|
| arch        | uname(2) machine                                  | "unknown" |
| virt        | systemd-detect-virt --value                       | "none"    |
| cores       | sysconf(_SC_NPROCESSORS_ONLN) (nproc)             | 0 → fail  |
| cpu_model   | /proc/cpuinfo first "model name"                  | ""        |
| ram_mb      | /proc/meminfo MemTotal kB / 1024                  | 0 → fail  |
| swap_mb     | /proc/meminfo SwapTotal kB / 1024                 | 0         |
| gpu.present | /sys/class/drm/card* with a display vendor        | false     |
| gpu.vendor  | device/vendor: 0x1002 amd · 0x10de nvidia · 0x8086 intel | —  |
| gpu.model   | device/product_name if present, else pci id       | ""        |
| gpu.vram_mb | device/mem_info_vram_total (amdgpu) bytes / 1MiB  | 0         |
| gpu.driver  | readlink device/driver → amdgpu/i915/nouveau/...  | ""        |
| gpu.kind    | heuristic v1: vram_mb >= 4096 → discrete, else integrated; note says heuristic | "integrated" |
| accel       | per chain above (checks: nvidia-smi, rocm-smi, libvulkan + non-llvmpipe device) | "cpu" |
| disk_root   | statvfs("/") → total_gb, free_gb                  | 0s        |
| net_links   | count of /sys/class/net/* excluding lo that are UP| 0         |
| net_online  | 1 s timeout probe (DNS resolve or TCP 1.1.1.1:44) | false     |
| display     | X socket (/tmp/.X11-unix) or Wayland socket exists| "headless"| 

Rules that never bend:
- Probing is READ-ONLY. Writes exactly one file (with --write).
- Never blocks boot: every probe has a source-or-default; total budget
  < 1 s (the only timed one is net_online, capped at 1 s).
- No new hard dependencies: C99 + json-c (already a jai-mcp dep) +
  libc. nvidia-smi / rocm-smi / vulkaninfo are OPTIONAL — checked with
  access(), never required.

## capabilities.json — schema v1

Stable contract. Adding keys later is allowed (consumers must ignore
unknown keys). Removing/renaming keys = schema bump.

```json
{
  "schema": 1,
  "generated_at": "2026-11-04T09:30:00Z",
  "arch": "x86_64",
  "virt": "none",
  "cpu":  { "cores": 2, "model": "AMD Athlon Silver 3050U with Radeon Graphics" },
  "ram_mb": 9928,
  "swap_mb": 0,
  "gpu": {
    "present": true,
    "kind": "integrated",
    "vendor": "amd",
    "model": "Radeon Vega Series / RAVEN2",
    "vram_mb": 512,
    "driver": "amdgpu"
  },
  "accel": "vulkan",
  "disk_root": { "total_gb": 20, "free_gb": 12 },
  "net": { "links": 1, "online": true },
  "display": "headless",
  "profile": "balanced",
  "below_floor": false,
  "notes": [
    "2 cores + 9.7 GB -> balanced profile",
    "Vulkan-capable AMD iGPU found -> tier-1 runs on GPU (Vulkan/RADV)",
    "iGPU shares system RAM: offload moves MATH off the cores, not memory",
    "headless -> no desktop UI this boot"
  ]
}
```

- `notes[]` is a REQUIRED field and MUST be in plain words — Law
  "explains in plain words" applies even to JSON. `jq -r '.notes[]'`
  must read like a human explaining the machine.
- vram note: iGPUs share system RAM; vram_mb is the carve-out only
  (workshop Vega: 512 MB carve-out — tier-1 still fits, 926 MB resident
  is mostly GTT/shared — see GPU-ACCEL.md).

## Service design

```ini
# /etc/systemd/system/jai-capabilities.service
[Unit]
Description=JAI OS — measure the machine, write capabilities.json
After=local-fs.target
Before=multi-user.target jai-eventd.service

[Service]
Type=oneshot
ExecStart=/usr/local/bin/jai-capabilities --write
RemainAfterExit=yes
```

CLI contract:
- `jai-capabilities`            → print JSON to stdout (human/doctor)
- `jai-capabilities --write`    → also write /etc/jai/capabilities.json
                                   (0644, atomic: tmp+rename)
- `jai-capabilities --profile`  → print just the profile (for scripts)

Idempotent: same machine → same output (modulo generated_at). Runs
EVERY boot (cheap); a stale file from a previous boot is simply
overwritten. Profile changes require reboot in v1 (honest limit).

## Consumers (who reads what)

| Consumer           | Reads              | Decides                                   |
|--------------------|--------------------|-------------------------------------------|
| wrapper / brain    | profile, accel     | local model size (0.6b default; 1.7b only on FULL+vulkan/cuda), cloud tier on/off |
| jai-gate (6b)      | profile            | budget defaults (minimal → tightest)      |
| phase2.sh          | full file at bake  | which model to pre-pull into the image    |
| doctor / vm-doctor | full file          | sanity: parses as JSON, profile sane, accel consistent with gpu.present |
| M4 desktop (later) | profile, display   | which DE; voice only on FULL + desktop    |

## Honest limits (v1)

- Boot-time only — no hotplug re-probe (USB GPU, RAM changed live).
- Profile switch = reboot (fine: hardware rarely changes under us).
- gpu.kind is a coarse heuristic (vram >= 4096 MB → discrete).
- accel detection checks presence of libraries + a non-llvmpipe device;
  it does not render a test frame (tier-1's fallback chain is the proof).
- net_online is a reachability probe, not a quality score.

## Acceptance criteria (done-when for roadmap 2b)

1. Builds in the VM with the same recipe as jai-mcp (gcc + libjson-c-dev).
2. Boots: /etc/jai/capabilities.json exists, `jq .` parses, boot cost < 1 s.
3. Workshop bare metal → profile=balanced, accel=vulkan, gpu.kind=integrated,
   notes in plain words. (2 cores + 9.7 GB → BALANCED per ladder.)
4. Dev VM (1 vCPU, 1 GB) → profile=minimal, accel=cpu, virt=kvm.
5. GPU-less machine (any box without amdgpu/nvidia) → accel=cpu, no crash,
   no non-zero exit.
6. Re-run is idempotent (diff = timestamp only).
7. CODE-MAP.md + STATUS.md gain the component rows (lands WITH the code).

## Refs

- VISION.md — Law 1, the ladder, the fallback-chain reference pattern
- GPU-ACCEL.md — the llvmpipe red herring; Vega iGPU facts (gfx90c, RADV)
- ROADMAP-V2.md — row 2b (this file is its spec)
- GPU day 2026-11-04 — measured numbers used in examples
