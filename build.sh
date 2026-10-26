#!/usr/bin/env bash
# build.sh — JAI OS top-level build entry
# Usage: ./build.sh <phase>
#   deps    - check build prerequisites are installed
#   module  - build the jai_dev kernel module
#   rootfs  - debootstrap Ubuntu 24.04 minimal + apply overlay  (Phase 1)
#   image   - produce bootable image via mkosi               (Phase 2)
set -euo pipefail

JAI_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$JAI_ROOT/build"
DISTRO_RELEASE=noble   # Ubuntu 24.04 LTS

msg() { printf '\033[1;36m[jai]\033[0m %s\n' "$*"; }
die() { printf '\033[1;31m[jai ERROR]\033[0m %s\n' "$*" >&2; exit 1; }

check_deps() {
	local ok=1
	for cmd in gcc make git debootstrap mkosi ccache; do
		command -v "$cmd" >/dev/null 2>&1 || { ok=0; msg "MISSING: $cmd"; }
	done
	local kdir="/lib/modules/$(uname -r)/build"
	[ -d "$kdir" ] || { ok=0; msg "MISSING: kernel headers for $(uname -r)"; }
	[ "$ok" = 1 ] && msg "all prerequisites present"
	return $ok
}

build_module() {
	msg "building jai_dev kernel module for $(uname -r)"
	make -C "$JAI_ROOT/kernel/jai_dev"
	ls -lh "$JAI_ROOT/kernel/jai_dev/jai_dev.ko" && msg "module built — load with: make -C kernel/jai_dev load"
}

build_rootfs() {
	command -v debootstrap >/dev/null || die "debootstrap not installed"
	mkdir -p "$BUILD_DIR"
	[ -d "$BUILD_DIR/rootfs" ] || {
		msg "debootstrapping Ubuntu $DISTRO_RELEASE minimal (needs sudo)"
		sudo debootstrap --variant=minbase "$DISTRO_RELEASE" "$BUILD_DIR/rootfs" http://archive.ubuntu.com/ubuntu
	}
	msg "applying JAI overlay"
	sudo cp -rv "$JAI_ROOT/rootfs/overlay/." "$BUILD_DIR/rootfs/"
	msg "rootfs ready at $BUILD_DIR/rootfs — chroot with: sudo chroot $BUILD_DIR/rootfs"
}

build_image() {
	command -v mkosi >/dev/null || die "mkosi not installed"
	cd "$JAI_ROOT/image"
	mkosi build
	msg "image output in $JAI_ROOT/image/"
}

case "${1:-}" in
	deps)   check_deps ;;
	module) build_module ;;
	rootfs) build_rootfs ;;
	image)  build_image ;;
	*) sed -n '2,7p' "$0"; exit 1 ;;
esac
