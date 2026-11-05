#!/bin/bash
# Build gvusb2 against a target kernel's headers and ship the modules.
# Headers come from an extracted kernel-devel RPM, so no packages are
# installed on the build host or the target.
#   ./build-and-ship.sh [kernelver] [host]
#   ./build-and-ship.sh --fetch [kernelver]   (download+extract headers first)
set -euo pipefail

fetch_headers() {
	kver=$1
	mkdir -p "$HOME/dev/kbuild/rpm" "$HOME/dev/kbuild/root"
	dnf download "kernel-devel-$kver" --destdir="$HOME/dev/kbuild/rpm"
	cd "$HOME/dev/kbuild/root"
	rpm2cpio "$HOME/dev/kbuild/rpm/kernel-devel-$kver.x86_64.rpm" | cpio -idmu --quiet
}

if [ "${1:-}" = "--fetch" ]; then
	fetch_headers "${2:?kernel version required}"
	exit 0
fi

KVER=${1:-$(ssh tetsuo uname -r)}
HOST=${2:-tetsuo}
KDIR=$HOME/dev/kbuild/root/usr/src/kernels/$KVER

if [ ! -d "$KDIR" ]; then
	echo "missing headers for $KVER; run: $0 --fetch $KVER" >&2
	exit 1
fi

cd "$(dirname "$0")"
make -C "$KDIR" M="$PWD" modules

# Ship the installer too: the target's copy may be stale or missing.
scp -q packaging/gvusb2-kmod-install.sh "$HOST:/tmp/gvusb2-kmod-install.sh"
ssh "$HOST" "sudo install -m755 /tmp/gvusb2-kmod-install.sh /usr/local/sbin/gvusb2-kmod-install.sh"

ssh "$HOST" "sudo mkdir -p /usr/local/lib/gvusb2/$KVER"
scp -q gvusb2-video.ko gvusb2-sound.ko "$HOST:/tmp/"
ssh "$HOST" "sudo install -m644 /tmp/gvusb2-video.ko /tmp/gvusb2-sound.ko /usr/local/lib/gvusb2/$KVER/ && sudo env FORCE=1 /usr/local/sbin/gvusb2-kmod-install.sh"
echo "shipped $KVER to $HOST"
