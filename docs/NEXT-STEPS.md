# JAI — Next Steps

Do these in order. Each step lists exact commands. Where `sudo` appears,
run it in your tmux session (`tmux attach -t jai`).

## STEP 1 — Pack the disk image (Phase 1 finish)
```bash
cd ~/jai-os/vm
sudo ./make-disk.sh        # now unmounts /sys /dev first — fixed
```
Expected: "DONE — boot with ./boot-vm.sh", ~30 seconds.

## STEP 2 — Set the jai user's password (for VM login)
```bash
cd ~/jai-os
sudo chroot build/rootfs passwd jai
```
Remember this password — you'll use it to SSH into the VM.

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

## STEP 5 — Bake the agent layer (Phase 2)
### 5a. opencode into the rootfs
```bash
cd ~/jai-os
curl -fsSL https://opencode.ai/install | \
  PREFIX=build/rootfs/usr/local bash
sudo chroot build/rootfs chmod +x /usr/local/bin/opencode 2>/dev/null || \
  sudo chmod +x build/rootfs/usr/local/bin/opencode
```
(If the installer misbehaves, download the release binary manually and
place it at `build/rootfs/usr/local/bin/opencode`.)

### 5b. JAI's brain config
```bash
sudo mkdir -p build/rootfs/etc/jai
sudo cp agent/opencode.json  build/rootfs/etc/jai/
sudo cp agent/IDENTITY.md    build/rootfs/etc/jai/
sudo chroot build/rootfs mkdir -p /var/lib/jai
sudo chroot build/rootfs chown -R jai:jai /var/lib/jai
```

### 5c. Kernel module for the VM's Ubuntu kernel
The module we built targets Kali's kernel. Rebuild it inside the chroot:
```bash
sudo mount -t proc proc build/rootfs/proc
sudo mount --rbind /sys build/rootfs/sys
sudo mount --rbind /dev build/rootfs/dev
sudo chroot build/rootfs bash -c '
  apt-get install -y linux-headers-6.8.0-147-generic build-essential
'
sudo cp -r kernel/jai_dev build/rootfs/root/
sudo chroot build/rootfs make -C /root/jai_dev
sudo chroot build/rootfs bash -c '
  echo jai_dev > /etc/modules-load.d/jai.conf
  cp /root/jai_dev/jai_dev.ko /lib/modules/6.8.0-147-generic/extra/ || true
  depmod -a 6.8.0-147-generic
'
# unmount again before next make-disk
sudo umount -R build/rootfs/dev build/rootfs/sys build/rootfs/proc
```

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
`rootfs/overlay/`, `kernel/`, and `agent/`. The Ubuntu base is disposable —
`rm -rf build/` and rebuild anytime.
