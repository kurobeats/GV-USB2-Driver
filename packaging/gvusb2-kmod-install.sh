#!/bin/bash
# Install the gvusb2 modules staged in /usr/local/lib/gvusb2/$(uname -r)/.
# FORCE=1 reloads them even if they are already loaded.
#
# Shipped by build-and-ship.sh; lives here so a deploy never depends on a
# hand-patched file on the target.
set -euo pipefail
KVER=$(uname -r)
SRC=/usr/local/lib/gvusb2/$KVER
DEST=/lib/modules/$KVER/kernel/drivers/media/usb/gvusb2
[ -f "$SRC/gvusb2-video.ko" ] || { echo "no modules staged for $KVER"; exit 1; }
sudo mkdir -p "$DEST"
sudo install -m644 "$SRC"/gvusb2-*.ko "$DEST/"
if [ "${FORCE:-0}" = "1" ] && lsmod | grep -q "^gvusb2"; then
	# Fail loudly: a silent failure here leaves the OLD module running and
	# every later test measures the previous build.
	if ! sudo modprobe -r gvusb2-sound gvusb2-video 2>/dev/null; then
		echo "cannot unload gvusb2 (busy). Stop its users (OBS, arecord," >&2
		echo "wireplumber/pipewire, alsa-state) and retry." >&2
		exit 1
	fi
fi
sudo depmod "$KVER"
sudo modprobe gvusb2-video
sudo modprobe gvusb2-sound
lsmod | grep "^gvusb2" || { echo "modules did not load"; exit 1; }
modinfo -n gvusb2-video
