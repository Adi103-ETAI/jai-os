# JAI — Decision Log

Each decision: date, choice, why, alternatives rejected.

## 2026-10-26 — Project named JAI
User chose "JAI" as the assistant/OS name. All files, services, devices,
and configs use jai-* / JAI naming. (Replaces working name "Jarvis".)

## 2026-10-26 — Base: Ubuntu 24.04 (noble) via debootstrap, NOT a git clone
Cloning Ubuntu's repo yields packaging scripts, not a usable OS.
debootstrap gives a real minimal rootfs (~300 MB) we can layer onto.
LTS chosen for 5-year kernel/userland support.

## 2026-10-26 — Brain: opencode serve, NOT Hermes
User decision. opencode already runs as a headless HTTP service on the
build host, supports MCP tools and sessions. Caveat accepted: opencode
is developer-flavored; identity/persona fixed via /etc/jai/IDENTITY.md,
and jai-gateway (Phase 4) provides the OS-assistant front-end.

## 2026-10-26 — Kernel strategy: stock + out-of-tree modules
No kernel forking/rebuilding. First module: jai_dev (misc device /dev/jai,
read=status, write=event→printk). Event ring buffer deferred to Phase 3.
eBPF considered for later introspection needs.

## 2026-10-26 — Test VMs: 1 GB/1 vCPU, never more
october-win11 (4 GB/2 vCPU = whole host) was abandoned half-installed.
Rule: test VMs must never starve the build host.

## 2026-10-26 — Headless only
No display manager, no PipeWire in base (audio arrives with Phase 4
voice stack only). Boot target: multi-user, SSH + Tailscale.

## 2026-10-26 — INCIDENT: bind-mount unmount killed host /dev/pts + /dev/shm
vm/make-disk.sh lazily unmounted the rbind'd /dev subtree inside the
rootfs; the unmount propagated through shared-mount peer groups and
detached the HOST's /dev/pts and /dev/shm. Result: no new pty can be
allocated → every subsequent `sudo` fails (sudo's use_pty needs a pty).
Deadlock: fixing needs root, root needs a pty → clean reboot required.
Rules adopted:
1. NEVER unmount rbind'd subtrees without `mount --make-rslave` first.
2. setup-rootfs.sh now marks bind trees rslave immediately after rbind.
3. make-disk.sh no longer unmounts anything — rsync -x staging makes
   mount state irrelevant.
Mitigation for readers who hit this: `systemctl reboot` (polkit allows
reboot from an active user session — no sudo needed).

## 2026-10-26 — Phase 0 COMPLETE ✅
jai_dev compiled against kali 7.1.5 kernel and loaded successfully.
Verified: /dev/jai read ("JAI is awake. Kernel link established."),
write→kernel log ("jai: event: hello from userland"), open counter.
Makefile ccache quoting bug found and fixed during first build.
Module is boot-scoped (not persisted) — Phase 2 adds modules-load.d
integration so JAI OS loads its own kernel module at boot.
Next: Phase 1 — debootstrap Ubuntu 24.04 minimal rootfs, boot in 1 GB VM.
