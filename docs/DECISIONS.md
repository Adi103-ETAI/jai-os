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

### Incident escalation (same day)
The damage was worse than first measured: the /sys rbind unmount ALSO
propagated and detached /sys/fs/cgroup (cgroup2). Consequences:
- polkitd cannot restart (systemd cannot create cgroups →
  "Failed to create cgroup /system.slice/polkit.service"), start-limit-hit
- ALL systemd service starts fail → dbus activation dead → polkit dead
- systemctl reboot (and --force --force) impossible: polkit auth +
  cgroup-required shutdown jobs both unavailable → "Need to be root"
Full recovery required physical intervention: hardware power cycle.
Lesson: rbind of /sys is even more dangerous than /dev — cgroupfs is
load-bearing for the entire service manager. Recovery: power button.

## 2026-10-26 — Phase 0 COMPLETE ✅
jai_dev compiled against kali 7.1.5 kernel and loaded successfully.
Verified: /dev/jai read ("JAI is awake. Kernel link established."),
write→kernel log ("jai: event: hello from userland"), open counter.
Makefile ccache quoting bug found and fixed during first build.
Module is boot-scoped (not persisted) — Phase 2 adds modules-load.d
integration so JAI OS loads its own kernel module at boot.
Next: Phase 1 — debootstrap Ubuntu 24.04 minimal rootfs, boot in 1 GB VM.

## 2026-10-27 — Agent core: OpenCode v2 (2.2.0), pinned, installed from npm tarball
User flagged that opencode.ai/install yields 1.19.4 while latest is 2.2.0.
Verified against the real binaries:
- opencode.ai/install REFUSES 2.x by design ("only installs OpenCode 1")
- v2 lives on its own channel: opencode.ai/v2/install → npm @opencode/cli-<target>
- the v2 installer has no PREFIX support → phase2.sh downloads the npm
  tarball directly (pinned via JAI_OPENCODE_VERSION, default 2.2.0)
- v2 config schema: permission{} → permissions[] ({action,resource,effect}),
  bash → shell, mcp.<n>.enabled → mcp.servers.<n>.disabled, theme removed
  (agent/opencode.json rewritten in v2 format; verified loading natively
  via `opencode debug config` — no migration artifacts)
- `serve --service` flag and default port 4096 unchanged → jai-agent.service
  and the Phase 4 gateway target stay valid
- NEW in v2: `serve` prints a one-time server password at startup (stdout →
  systemd journal). Gateway must authenticate; see agent/gateway/README.md.
- CPU baseline: no AVX2 → linux-x64-baseline build (Athlon 3050U has AVX2,
  detection is dynamic either way)

## 2026-10-27 — INCIDENT: opencode SIGILL in VM (first boot) → baseline build is the default
First VM boot succeeded (serial console, jai login, sshd socket) but
`opencode` died instantly with `Illegal instruction`. Root cause chain:
1. phase2.sh probed AVX2 on the BUILD HOST (Athlon Zen 1 → has AVX2)
   and downloaded the x86-64-v3 `linux-x64` build;
2. the binary runs in the VM, where boot-vm.sh (TCG) used the default
   `qemu64` CPU model — no AVX2 → SIGILL.
Lesson: feature-probe the TARGET CPU, never the build host — and for an
OS image the target is "anywhere", so:
- phase2.sh now defaults to `linux-x64-baseline` (override:
  JAI_OPENCODE_TARGET=linux-x64)
- boot-vm.sh sets the CPU model to match the accelerator: `-cpu host`
  under KVM, `-cpu max` under TCG (instead of feature-poor qemu64)

## 2026-10-27 — INCIDENT #2: bare chroot aborts opencode (Bun needs /proc)
Verifying the baked binary with
`sudo chroot build/rootfs /usr/local/bin/opencode --version`
died ~1 ms in: `panic(main thread): abort() called — oh no: Bun has
crashed`. NOT a broken binary, and NOT the old SIGILL:
- opencode v2 is a Bun standalone executable; Bun locates its embedded
  payload via /proc/self/exe.
- A debootstrap rootfs has no /proc until mounted, and phase2.sh unmounts
  /proc,/sys,/dev in its EXIT trap — so any MANUAL chroot after phase2
  is bare → abort. (The same binary passed `--version` on the host.)
Rules adopted:
1. Never hand-chroot the rootfs bare. Use `scripts/chroot-run.sh`
   (mounts /proc,/sys,/dev rbind+rslave, cleans up after itself).
2. phase2.sh now smoke-tests `opencode --version` INSIDE the chroot at
   bake time — a bad bake fails the build instead of crash-looping
   jai-agent at boot.
3. The baked build is stamped to /etc/jai/opencode-build
   (version/target/date) so "which variant is in the image" is always
   answerable.

## 2026-10-27 — VERIFIED: v2 server pins — port 4096 + HTTP Basic password
The re-baked VM boot (15:39 UTC) proved the agent stack end-to-end but
exposed two v2 server facts our docs had wrong:
1. `serve` binds a RANDOM port every boot when unconfigured — the VM
   picked 49374, not the assumed default 4096. The Phase 4 gateway would
   have chased a moving target.
2. The JSON API ALWAYS requires auth, even on 127.0.0.1 (no-auth → 401
   `{"_tag":"UnauthorizedError"}`). The credential is NOT ephemeral:
   v2 keeps service config (incl. a password) in
   ~/.config/opencode/service.json and accepts HTTP Basic with any
   username. Bearer is NOT accepted. (Confirmed empirically against
   2.2.0: Basic → 200, Bearer/query/x-opencode-password → 401.)
Reverse-engineered (bun binary strings + live probes) the managed-service
CLI: `opencode service get|set|unset|start|stop|restart|status`; valid
config keys: disabled | remote | hostname | port | password | cors | env.
Fix baked into phase2.sh (step 5a-pin), using v2's own CLI so the file
format is always version-correct:
- `opencode service set port 4096`
- `opencode service set password ${JAI_SERVER_PASSWORD:-jai}`
- config lands at /var/lib/jai/.config/opencode/service.json, chowned
  1000:1000; jai-agent.service unchanged (`serve --service` reads it).
Dev password "jai" is fine for the VM appliance; Phase 5 (hardware) must
generate a real secret. Gateway must read the password from
service.json at runtime (agent/gateway/README.md).
