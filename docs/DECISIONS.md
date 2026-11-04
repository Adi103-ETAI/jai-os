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

## 2026-10-27 — Phase 3 step 1: /dev/jai event ring buffer (module v0.2.0)
Design choices (module rebuilt in-chroot by phase2.sh; no host compile
was possible, so the code favors fully-understood primitives):
- Hand-rolled 4 KiB ring (mutex + head/tail/count) instead of kernel
  kfifo: kfifo's DECLARE/DEFINE/INIT macro variants are easy to get
  subtly wrong across kernel versions and could not be compile-tested
  here. ~40 lines we fully understand beat a macro we can't verify.
- read() = DRAIN (pipe-like): pending events first; when empty, a status
  line ("JAI is awake..." + events/opens stats) is served via *ppos so
  readers terminate. This keeps `cat /dev/jai` and the agent's
  jai_kernel_status MCP tool meaningful at ANY moment (before/after
  events, boot handshake included).
- printk mirror KEPT on write: the kernel log stays a durable event
  journal (journalctl -k); the ring is in-memory and evicts oldest bytes
  on overflow (newest survive; acceptable for a 1 GB VM appliance).
- write() cap raised 64 → 254 bytes; trailing newline enforced so cat
  output stays line-oriented; whole payload reported consumed (matches
  v0.1.0 behavior).
Remaining Phase 3: kernel→agent PUSH (poll / netlink / uevent) — today
the agent can only pull.

## 2026-10-27 — Incident #3: the appliance had NO network stack (EAI_AGAIN)
First-conversation attempt in the VM died with
`Error: getaddrinfo EAI_AGAIN opencode.ai`. Diagnosis chain: the initial
systemd-resolved fix failed with "Unit systemd-resolved.service not found"
— which WAS the diagnosis. Root cause: debootstrap --variant=minbase ships
no network layer whatsoever:
- no `iproute2` (no `ip`), no DHCP client, no `systemd-resolved`
- `/etc/resolv.conf` = whatever debootstrap copied from the BUILD HOST
  (systemd-resolved stub 127.0.0.53 — nothing listens on it inside the VM)
- `systemd-networkd` present (ships in the `systemd` package) but never
  enabled → NIC up with NO IP, NO route; glibc getaddrinfo → EAI_AGAIN.
Lesson (same family as the SIGILL incident): build-host behavior is NOT
target behavior — the chroot always had network (host kernel netns), the
VM does not.
Adopted design (baked 2026-10-27):
- `rootfs/overlay/etc/systemd/network/20-jai.network`: networkd DHCP (QEMU
  slirp server 10.0.2.2 → 10.0.2.15/24), `UseDNS=no`.
- networkd enablement in BOTH setup-rootfs.sh and phase2.sh 5-net
  (systemctl enable, raw-symlink fallback), binary existence guarded.
- `/etc/resolv.conf`: STATIC appliance DNS (1.1.1.1 8.8.8.8 9.9.9.9,
  `JAI_VM_DNS` env override) written AFTER all chroot apt usage; phase2
  copies the HOST resolv.conf in at start so build-time apt always works.
  Rationale: no systemd-resolved in the appliance → glibc reads the file
  directly; slirp's 10.0.2.3 forwarder is unreliable (EAI_AGAIN) so public
  resolvers are reached straight through slirp NAT (UDP/53 out is NATed).
- `iproute2` added to packages.txt (diagnostics only — networkd does not
  need it).
- NO networkd-wait-online: boot must never hang on networking; the
  appliance comes up and the agent simply has no internet if DHCP fails.
Hardware phase (Phase 5) TODO: revisit DNS for real upstreams (install
systemd-resolved or a DHCP hook); revisit the "free models reach
opencode.ai through NAT" assumption for air-gapped deployments.
Verification probes inside the VM: `getent hosts`, `curl -sI` — NEVER
ping (slirp ICMP needs host ping_group_range and lies).

## 2026-11-02 — Brain model: OpenCode Zen free tier, device-flow only
`"model": "opencode/mimo-v2.6-flash-free"` (live-VM verified: MCP tools
called through the hands, one-shot reports work). Field findings, now
policy:
- v2.2.0 keeps credentials in `~/.local/share/opencode/opencode*.db`
  (SQLite). There is NO auth.json in v2 — "Connected ✓" with no file is
  EXPECTED, not an error.
- Service-account API keys carry NO free quota: login succeeds, the first
  call returns "Free usage limit reached". The personal DEVICE FLOW
  (console account) does have quota. Headless-safe: the flow prints a
  URL + code, no local callback needed.
- `"autoupdate": false` — the appliance pins its brain. Unattended
  self-updates on a headless appliance are unattended breakage (v1→v2
  config schema drift already bit once). Upgrades happen via
  `scripts/vm-upgrade-brain.sh` only: db snapshot → upgrade → smoke test
  → rollback path printed on failure. Schema migrations are one-way,
  hence the snapshot.
- No restarts needed after a brain swap: opencode is stateless-per-call
  (spawned per `jai` / per MCP invocation, exits when done). Only
  jai-eventd is a resident JAI process.
- The `jai` wrapper ships via overlay (`rootfs/overlay/usr/local/bin/jai`)
  with a per-call override: `JAI_MODEL=<provider/model> jai "..."`.
- OpenRouter provider block kept in opencode.json as a fallback catalog
  (`limit.output: 4096` sidesteps the credit pre-flight on $0).

## 2026-11-02 — Reflex arc: drain-poll daemon, NOT a blocking reader
`jai_dev.c` v0.2.0 semantics: /dev/jai NEVER blocks — an empty ring
returns the status frame (then EOF). A classic blocking reader is
impossible, so `jai-eventd` is a drain-poller: open → drain → close →
sleep 500 ms. Chosen over netlink/uevent push (STEP 7's other
candidates): zero kernel changes, survives module reloads, and appliance
event rates make 2 Hz polling effectively real-time.
The status frame's kernel-side counter ("events: N total") is the
overflow detector: accepted-minus-captured > 0 between polls means the
4 KiB ring overwrote oldest bytes → a `{"gap":K}` record lands in
`/var/lib/jai/events.jsonl`. Telemetry may have holes; the brain is TOLD
(never silently truncated). Accepted v0.2.0 protocol limits: an event
beginning "JAI is awake." is indistinguishable from a status frame (don't
write those); embedded newlines in event text make gap accounting
approximate. Real framing = jai_dev v0.3, only if ever needed.
All JSONL is built with json-c (house rule — the file faces untrusted
event text); jai_events reads only the last 64 KB tail (bounded work).

## 2026-11-02 — Heartbeat: % specifier trap + beat script with event delta
systemd parses % in ExecStart as a unit specifier: `date "+%F %T"` in
jai-heartbeat.service never worked right (it needs %%). The logic moved
to `rootfs/overlay/usr/local/bin/jai-heartbeat-beat` — a plain shell file
with no escaping games — which now also records the kernel-event delta
per beat (`events_delta=` from events.jsonl line counts, logrotate-safe
via copytruncate detection). Rule of thumb: if an ExecStart needs date
formatting, it wants to be a script.

## 2026-11-02 — Ops layer: doctor, update, upgrade, provision (Phase 3d)
- `scripts/vm-doctor.sh`: one-shot VM self-test (kernel link round-trip,
  hands, reflex arc, timers, brain, sudoers). Run after every change.
- `scripts/vm-update.sh`: refresh the whole JAI layer from repo main.
  Hard rules after the truncated-config incident: download to temp FIRST,
  compile C BEFORE installing, .bak every replaced file, validate sudoers
  before it lands. Replaces all ad-hoc "curl one file into /etc" moves.
- `provision.sh`: one-command rebuild chaining the existing pipeline
  (doctor → rootfs → setup-rootfs → phase2 → make-disk; `--fresh` wipes
  build/). The Ubuntu base is disposable; credentials never are — they
  stay out of images by design (in-VM device-flow login re-run).
- Kernel build artifacts untracked (.gitignore existed; the .ko/.o/.cmd
  files were committed before it and kept shipping noise).

## D8 — Warm brain (Phase 6a)
what: jai-brain.service (systemd, User=jai) keeps opencode serve warm on
127.0.0.1:4096; wrapper v2 connects to it, falls back to --standalone cold.
why: 45.6 s cold thoughts kill interactivity; host evidence shows resident
service = 434 MB, ~0.4% CPU. Golden rule: warm != stateful (kill-safe).
impact: unit + wrapper v2 in rootfs/overlay; doctor gains brain checks.
Refs: Phase 6a; ARCHITECTURE-V2.md §3

## D9 — Gate before brain (Phase 6b)
what: jai-gate (C) enforces wake policy: Class 0 ignore / 1 note / 2 wake,
with budget file, dedup, serialization.
why: quota death observed live; brain must never be woken by accident.
impact: 4b decoder becomes prerequisite; 4c wake becomes safe by design.
Refs: Phase 6b; ARCHITECTURE-V2.md §4

## D10 — Tier-1 quick judge (small local model)
what: Qwen3-0.6B GGUF via llama.cpp on Kali host classifies/summarizes for
free (~1 s); cloud brain only for real judgment.
why: triage must not burn quota; system must degrade gracefully if quota dies.
impact: new optional dependency over socket; static severity table as fallback.
Refs: TIER1-MODEL.md

## D11 — Product pivot
what: JAI OS grows toward shippable product: .deb packages (M1), installer
(M2), config TUI+API (M3), web/desktop UI off-box (M4), signed repo (M5).
why: owner wants an installable OS + app, not a personal VM setup.
impact: packaging/ layout, dkms module, VM stays light; UI never runs in VM.
Refs: ROADMAP-V2.md milestones

## D12 — Ollama replaces llama.cpp for tier-1 serving
what: tier-1 model served by Ollama (systemd service, qwen3:0.6b,
keep_alive 24h) instead of a hand-built llama-server.
why: llama.cpp build broke and its CPU appetite dented SSH; Ollama gives
a managed service, one-command model install, and automatic GPU backend
discovery — with an OpenAI-compatible endpoint, so the gate's JSON
contract is unchanged (only URL/port move, 8081 -> 11434).
impact: TIER1-MODEL.md rewritten; gate code unchanged except URL/port.
Refs: TIER1-MODEL.md; GPU-ACCEL.md

## D13 — Tier-1 GPU offload via Vulkan iGPU (not ROCm)
what: /etc/systemd/system/ollama.service.d/igpu.conf sets
OLLAMA_IGPU_ENABLE=1; ollama service user in render+video groups;
verified `ollama ps` PROCESSOR = 100% GPU (RADV RAVEN2), 926 MB resident.
why: host has no NVIDIA (no CUDA) and ROCm does not support Picasso/
Raven2 (gfx90c) — Ollama's Vulkan backend + Mesa RADV support every GCN
chip. Ollama parks iGPUs by default; the journal line "dropping
integrated GPU; to enable, set OLLAMA_IGPU_ENABLE=1" was the golden
clue. Purpose: move model math off the CPU cores so SSH, eventd, and
QEMU stay snappy (headless bare-metal APU, 2 cores only).
impact: measured host 87.5% idle with VM up; drop-in survives reboots;
rollback = delete the file. vulkaninfo llvmpipe over SSH documented as
a red herring. VM-side access still TODO (OLLAMA_HOST=0.0.0.0).
Refs: GPU-ACCEL.md; ARCHITECTURE-V2.md §5

## D14 — Hardware-boundless: adapt + degrade, never require
what: JAI OS must run on ANY hardware. A boot-time capability probe
(jai-capabilities, roadmap 2b) measures cores/RAM/GPU/disk/network ->
/etc/jai/capabilities.json -> the profile picks itself (minimal /
balanced / full). Every feature must scale down and fall back; nothing
may REQUIRE a powerful machine. Reference pattern (already proven with
tier-1): GPU if present -> CPU if not -> static rules if unreachable.
why: the workshop laptop (2 cores, no dedicated GPU) must never become
the product's ceiling. Earlier docs read as if weak hardware were a
design constraint — it is only a testing constraint. Owner's direction:
"JAI OS should run on any hardware, boundless to hardware."
impact: VISION.md is the north star (Law 1); roadmap gains 2b; all
future components must state their fallback chain.
Refs: VISION.md; GPU-ACCEL.md; ROADMAP-V2.md step 2b

## D15 — JAI OS is a desktop OS (product definition)
what: the product is a real desktop operating system (Ubuntu base +
desktop environment) with the AI woven into the OS: an assistant that
watches, explains in plain words, fixes what is safe, and helps control
the machine — through permission gates. The user can always type, click,
and use the computer normally.
why: owner's product definition: "JAI OS should be an actual desktop OS
like Windows, Ubuntu or macOS; AI should control or help the OS, that
doesn't mean the user can't type; make things simpler for the user."
impact: docs reframed — README leads with the product; VISION.md +
CODE-MAP.md added (what/how/what-code/architecture); M4 rewritten as
on-box desktop integration; the headless VM remains the DEV BENCH only.
Refs: VISION.md; CODE-MAP.md; ROADMAP-V2.md M4
