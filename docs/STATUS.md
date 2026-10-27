# JAI — Status

Last updated: 2026-10-26 (first build session + hardening pass)

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
  - ✅ `scripts/phase2.sh` — one-command bake-in (opencode, /etc/jai, in-chroot
    module build with KDIR override, depmod, modules-load.d)
  - ✅ `scripts/doctor.sh` — build host pre-flight check
  - ✅ overlay additions: `etc/modules-load.d/jai.conf`, `etc/fstab`,
    `etc/systemd/system/jai-link.service` (boot kernel handshake)
  - ✅ `packages.txt` hardened: `systemd` + `systemd-sysv` explicit (minbase
    does not guarantee PID 1), `kmod` for depmod/modprobe
  - ❌ `opencode` binary NOT inside the rootfs yet → run `sudo ./scripts/phase2.sh`
- 🟡 `jai_dev` module: compiled for **Kali** kernel — VM runs **Ubuntu 6.8**,
  so the module must be rebuilt *inside the chroot* against Ubuntu headers
  (`phase2.sh` handles this; inside a chroot `uname -r` is the HOST kernel,
  so the Makefile gets an explicit `KDIR=`)
- ✅ `modules-load.d/jai.conf` so JAI OS loads its module at boot (overlay + phase2.sh)
- ⬜ `phase2.sh` actually RUN on the build host (needs the host)
- ⬜ mkosi bootable image (optional — direct-kernel boot works for testing)

## Phase 3 — Kernel event system 🟡 FIRST CUT DONE
- ✅ **agent↔kernel/OS bridge:** `agent/mcp/jai-system.py` — zero-dependency
  MCP stdio server (raw JSON-RPC 2.0) exposing /dev/jai read/write, dmesg,
  jai-* service health, sysinfo, and /var/lib/jai-memory to the agent;
  wired into `opencode.json` → mcp; deployed by `phase2.sh` 5b+
- ✅ `jai-link.service` — boot-time kernel handshake (cat /dev/jai + boot
  event into kernel log); jai user added to adm/systemd-journal for dmesg
- ⬜ `/dev/jai` event ring buffer in the kernel module (replaces printk)
- ⬜ runtime event subscription (kernel → agent push, not just agent pull)

## Phase 4 — Voice + remote access ⬜
- ⬜ jai-gateway: openWakeWord + whisper.cpp (STT) + Piper (TTS), event-driven
- ⬜ opencode HTTP API integration
- ⬜ Tailscale baked into image, remote voice sessions

## Phase 5 — First real-hardware boot ⬜
- ⬜ Boot JAI OS image on actual hardware
