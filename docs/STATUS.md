# JAI — Status

Last updated: 2026-10-26 (first build session)

Legend: ✅ done · 🟡 partial/designed but not baked in · ⬜ not started

## Phase 0 — Playground + first kernel code ✅ COMPLETE
- ✅ Toolchain installed (gcc, make, clang, ccache, kernel headers)
- ✅ `kernel/jai_dev/` — real kernel module, misc device `/dev/jai`
  - read → "JAI is awake. Kernel link established."
  - write → userland events land in kernel log (`jai: event: ...`)
- ✅ Compiled (`jai_dev.ko`) against Kali 7.1.5 kernel, **loaded and verified live**
  (kernel log evidence captured; see DECISIONS.md)
- ✅ Git history started; pushed to GitHub

## Phase 1 — Rootfs boots in test VM 🟡 ~90%
- ✅ `debootstrap` Ubuntu 24.04 (noble) minimal rootfs — 1.5 GB at `build/rootfs/`
- ✅ `setup-rootfs.sh` transformed it into JAI OS:
  - apt sources (noble + updates + security) written
  - package set installed — **including Ubuntu kernel `linux-image-6.8.0-147-generic`**
  - hostname `jai`, `jai` user created (uid 1000, in sudo group)
  - `/var/lib/jai-memory` created
  - `jai-agent.service` **enabled via systemctl**
- ✅ `vm/make-disk.sh` + `vm/boot-vm.sh` written (fixed live-mount race)
- ⬜ Disk image built (`make-disk.sh` re-run pending after the unmount fix)
- ⬜ First VM boot + SSH login not yet done

## Phase 2 — Agent layer baked in ⬜ (the layer below is DESIGNED, not installed)
- 🟡 **Agent layer status:**
  - ✅ `jai-agent.service` written + enabled (runs `opencode serve` as daemon)
  - ✅ `agent/IDENTITY.md` persona + `agent/opencode.json` permissions written
  - ❌ `opencode` binary NOT inside the rootfs yet → service fails harmlessly
    in the VM until Phase 2
  - ❌ config not yet copied into rootfs (`/etc/jai/`)
- 🟡 `jai_dev` module: compiled for **Kali** kernel — VM runs **Ubuntu 6.8**,
  so the module must be rebuilt *inside the chroot* against Ubuntu headers
  before it can load there (needs `linux-headers-6.8.0-147-generic` in chroot)
- ⬜ `modules-load.d/jai.conf` so JAI OS loads its module at boot
- ⬜ mkosi bootable image (optional — direct-kernel boot works for testing)

## Phase 3 — Kernel event system ⬜
- ⬜ `/dev/jai` event ring buffer (replaces printk)
- ⬜ `jai-system` MCP server: service health, dmesg, /dev/jai link

## Phase 4 — Voice + remote access ⬜
- ⬜ jai-gateway: openWakeWord + whisper.cpp (STT) + Piper (TTS), event-driven
- ⬜ opencode HTTP API integration
- ⬜ Tailscale baked into image, remote voice sessions

## Phase 5 — First real-hardware boot ⬜
- ⬜ Boot JAI OS image on actual hardware
