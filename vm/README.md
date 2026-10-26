# vm/README.md — test VM plan (Phase 1)

The Kali build host tests JAI OS images in a minimal QEMU VM before
any real hardware boots them. Deliberately tiny — we learned from the
october-win11 lesson not to over-provision.

Spec:
- 1 GB RAM, 1 vCPU  (host has 2 cores; never starve the build host)
- UEFI (OVMF), 8 GB raw disk image in build/
- User-mode networking with hostfwd tcp::2222-:22 for SSH
- No display — serial console + SSH only

Boot a built image:
  qemu-system-x86_64 -m 1024 -smp 1 \
    -bios /usr/share/OVMF/OVMF_CODE.fd \
    -drive file=../image/jarvisos.img,format=raw \
    -netdev user,id=net0,hostfwd=tcp::2222-:22 -device e1000e,netdev=net0 \
    -nographic
