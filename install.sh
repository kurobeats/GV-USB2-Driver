# Unload drivers if they are already loaded (refuses politely if busy)
cd "$(dirname "$0")"
grep -q "gvusb2_video" /proc/modules && { sudo modprobe -r gvusb2-video \
	|| { echo "gvusb2-video is busy; unload its users (or reboot) and retry"; exit 1; }; }
grep -q "gvusb2_sound" /proc/modules && { sudo modprobe -r gvusb2-sound \
	|| { echo "gvusb2-sound is busy; stop its users (or reboot) and retry"; exit 1; }; }
make clean && make && \
    sudo mkdir -p /lib/modules/$(uname -r)/kernel/drivers/media/usb/gvusb2/ && \
    zstd *.ko && \
    sudo mv -vt /lib/modules/$(uname -r)/kernel/drivers/media/usb/gvusb2/ *.ko.zst && \
    sudo depmod $(uname -r) && \
    sudo modprobe -v gvusb2-video && \
    sudo modprobe -v gvusb2-sound index=7
