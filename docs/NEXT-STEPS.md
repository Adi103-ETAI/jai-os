# JAI — Next Steps

Do these in order. Each step lists exact commands. Where `sudo` appears,
run it in your tmux session (`tmux attach -t jai`).

## STEP 0 — Pre-flight check
```bash
cd ~/jai-os && ./scripts/doctor.sh
```
Reports toolchain, headers, KVM, disk, and current artifact state.
Fix anything CRITICAL before continuing.

## STEP 1 — Pack the disk image (Phase 1 finish)
```bash
cd ~/jai-os/vm
sudo ./make-disk.sh        # rsync -x staging — immune to any chroot mounts
```
Expected: "DONE — boot with ./boot-vm.sh", ~30 seconds.

## STEP 2 — Set the jai user's password (for VM login)
```bash
cd ~/jai-os
sudo chroot build/rootfs passwd jai
```
Remember this password — you'll use it to SSH into the VM.
TIP (2026-10-27): a bare chroot like this is fine for `passwd`, but NOT
for running Bun-based binaries — opencode aborts there without /proc.
To run or inspect things in the rootfs use:
```bash
sudo ./scripts/chroot-run.sh                            # interactive shell
sudo ./scripts/chroot-run.sh /usr/local/bin/opencode --version
```

## STEP 3 — Boot JAI OS (first boot!)
In tmux:
```bash
cd ~/jai-os/vm && ./boot-vm.sh
```
- Serial console scrolls the Linux 6.8 boot.
- **Known, harmless:** `jai-agent.service` fails/retries — `opencode`
  isn't installed inside the rootfs yet (that's Step 5).
- Ends at a login prompt on host `jai`.

From a second terminal:
```bash
ssh -p 2222 jai@localhost
```
Then inside the VM, verify:
```bash
hostname          # → jai
uname -r          # → 6.8.0-147-generic
systemctl status jai-agent   # → failed (expected, see Step 5)
free -h           # ~1 GB VM
```
Exit QEMU with Ctrl+A then X.

## STEP 4 — Commit + push the milestone
```bash
cd ~/jai-os && git add -A && git commit -m "Phase 1 complete: JAI OS boots in test VM"
git push
```
(If push asks for auth, generate a fresh token on GitHub.)

## STEP 5 — Bake the agent layer (Phase 2) — ONE COMMAND
```bash
cd ~/jai-os && sudo ./scripts/phase2.sh
```
This automates everything below (opencode install, /etc/jai config,
module rebuild inside the chroot, depmod, modules-load.d) with the
incident lessons applied: rbind mounts are immediately `--make-rslave`,
kernel version is detected from the rootfs (never hardcoded), and the
module Makefile gets an explicit `KDIR=` (inside a chroot, `uname -r`
reports the HOST kernel — hardcoding or trusting it silently builds
against the wrong kernel).
It also STAMPS the baked opencode build to `/etc/jai/opencode-build`
(version/target/date) and smoke-tests `opencode --version` inside the
chroot — a bad bake now fails the build immediately instead of
crash-looping `jai-agent` at boot.

<details><summary>Manual equivalent (legacy — kept for reference)</summary>

### 5a. opencode into the rootfs
NOTE: the rootfs is root-owned (created by sudo debootstrap), so the
installer must run as root:
```bash
cd ~/jai-os
curl -fsSL https://opencode.ai/install | \
  sudo env PREFIX=build/rootfs/usr/local bash
```
(If the installer misbehaves, download the release binary manually and
place it at `build/rootfs/usr/local/bin/opencode`.)

### 5b. JAI's brain config
```bash
sudo mkdir -p build/rootfs/etc/jai build/rootfs/var/lib/jai
sudo cp agent/opencode.json  build/rootfs/etc/jai/
sudo cp agent/IDENTITY.md    build/rootfs/etc/jai/
sudo chown -R 1000:1000 build/rootfs/var/lib/jai
```

### 5c. Kernel module for the VM's Ubuntu kernel
The module we built targets the HOST kernel. Rebuild it inside the chroot.
CRITICAL (2026-10-26 incident): rbind mounts MUST be marked rslave
immediately, or unmounts can propagate and kill the host's /dev/pts +
cgroupfs. Also pass KDIR= explicitly — chroot `uname -r` is the host kernel.
```bash
sudo mount -t proc proc build/rootfs/proc
sudo mount --rbind /sys build/rootfs/sys
sudo mount --rbind /dev build/rootfs/dev
sudo mount --make-rslave build/rootfs/sys
sudo mount --make-rslave build/rootfs/dev
KVER=$(basename "$(readlink -f build/rootfs/boot/vmlinuz)" | sed 's/^vmlinuz-//')
sudo chroot build/rootfs bash -c "
  apt-get update && apt-get install -y linux-headers-$KVER build-essential
"
sudo cp -r kernel/jai_dev build/rootfs/root/
sudo chroot build/rootfs make -C /root/jai_dev KDIR=/lib/modules/$KVER/build
sudo chroot build/rootfs bash -c "
  echo jai_dev > /etc/modules-load.d/jai.conf
  cp /root/jai_dev/jai_dev.ko /lib/modules/$KVER/extra/
  depmod -a $KVER
"
sudo umount -R build/rootfs/dev build/rootfs/sys build/rootfs/proc
```

</details>

### 5d. Rebuild image + boot — now with the agent
```bash
cd ~/jai-os/vm && sudo ./make-disk.sh
cd ~/jai-os && sudo chroot build/rootfs passwd jai   # if image was rebuilt
cd ~/jai-os/vm && ./boot-vm.sh
```
Inside the VM this time:
```bash
systemctl status jai-agent   # → active (running)
cat /dev/jai                 # → JAI is awake. Kernel link established.
```

## STEP 6 — Phase 2b: mkosi bootable image (optional now, needed for hardware)
Refine `image/mkosi.conf` and run `./build.sh image`. Defer until the
direct-kernel-boot VM is stable — mkosi is packaging polish, not new function.

## STEP 7 — Phase 3: event ring buffer in the kernel module
Rewrite `jai_write` to append events into a ring buffer instead of printk;
`jai_read` drains events. Then write `agent/mcp/jai-system` (small Python
MCP server) exposing `/dev/jai` + service health to the agent.

## STEP 8 — Phase 4: voice
`agent/gateway/` — openWakeWord → whisper.cpp → Piper, wired to
`http://127.0.0.1:4096` (jai-agent). Bake Tailscale into the image.

## STEP 9 — Phase 5: real hardware
Flash `image/jarvisos.img` (or write a GRUB-less direct-boot via the
host's bootloader), boot, and JAI says hello on real metal.

---
**Golden rule:** everything that makes it "JAI OS" lives in
`rootfs/overlay/`, `kernel/`, `agent/`, and `scripts/`. The Ubuntu base is
disposable — `rm -rf build/` and rebuild anytime.
