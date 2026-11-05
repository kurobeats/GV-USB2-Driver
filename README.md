# GV-USB2 Linux Driver

A Linux kernel driver for the **I-O DATA GV-USB2 / GV-USB2/E** USB 2.0 SD video
capture device. It exposes the device as a standard V4L2 capture node and an
ALSA capture card, so anything that speaks V4L2 or ALSA works: ffmpeg, mpv,
OBS, GStreamer, arecord, your own code.

It is a maintained fork of
[Isaac-Lozano/GV-USB2-Driver](https://github.com/Isaac-Lozano/GV-USB2-Driver)
with PAL support, corrected video geometry, hardened error handling and a
host-side test suite.

**What you get**

| | |
|---|---|
| Device | USB ID `04bb:0532`, I-O DATA GV-USB2 / GV-USB2/E |
| Video | 720x486 (NTSC) or 720x576 (PAL), UYVY 4:2:2, interlaced, frame rate follows the input |
| VBI (teletext/closed captions) | 720x516 (NTSC) or 720x616 (PAL) |
| Audio | 48 kHz, 16-bit stereo, capture only |
| Inputs | Composite (RCA) and S-Video (mini-DIN), switchable at runtime |
| Controls | Brightness, contrast, saturation, hue, sharpness, gain, AGC, crop offsets, VBI |
| Hotplug | Safe to unplug while streaming, video or audio |

---

## Requirements

- Linux with kernel headers matching the **running** kernel
- A C compiler and `make`
- The `tw9910` decoder module (`CONFIG_VIDEO_TW9910`). It is normally built by
  distribution kernels and loaded automatically; without it the video module
  refuses to bind rather than expose a capture node that can never start
  (the decoder would stay unprogrammed and every frame would be black)
- Optional but handy: `v4l2-utils` (for `v4l2-ctl`), `alsa-utils` (for
  `arecord`), `ffmpeg`

If **Secure Boot** is enabled the unsigned modules are rejected at load time
with `Key was rejected by service`. Either sign them with a MOK key you enrol
(`mokutil --import`), or use the akmod package in `packaging/`, or turn Secure
Boot off. Check with `mokutil --sb-state`.

## Installation

```sh
./install.sh
```

That builds both modules, installs them under `/lib/modules/$(uname -r)`,
and loads them. Two modules are involved: `gvusb2-video` (V4L2 capture) and
`gvusb2-sound` (ALSA capture).

The sound module is loaded with ALSA card index 7 by default; if that index is
already taken, load it with another one:

```sh
sudo modprobe gvusb2-sound index=9
```

If the modules are already loaded, `install.sh` unloads them first and stops
with an error when something still holds the device — close OBS/`arecord`
first. On a desktop the audio server usually holds the ALSA card even when
nothing is playing:

```sh
sudo systemctl stop alsa-state
systemctl --user stop pipewire pipewire-pulse wireplumber
# ... install/reload, then start them again
```

An RPM/akmod spec that rebuilds the modules after each kernel update lives in
`packaging/gvusb2-kmod.spec`.

### Building for a different kernel

If you build on one machine and run on another, use the shipped helper:

```sh
./build-and-ship.sh --fetch 6.11.4-301.fc41.x86_64   # once, to extract headers
./build-and-ship.sh 6.11.4-301.fc41.x86_64 tetsuo    # build + install over ssh
```

Headers are taken from an extracted `kernel-devel` RPM, so nothing is
installed on either machine. On the far end the modules are staged in
`/usr/local/lib/gvusb2/<kernel>/` and loaded through
`/usr/local/sbin/gvusb2-kmod-install.sh`, which is itself shipped from
`packaging/` on every run. `FORCE=1` (the default here) reloads an already
loaded module and fails loudly if it is in use rather than leaving the old
build running.

Use this rather than copying a module built against your local `/usr/src`:
distribution kernel headers ship a **rewritten config** (Fedora's literally
says *"We are resetting this value to facilitate local builds"*), and a module
built against it fails to load with `struct module section size must match`.

## Quick start

```sh
# find the device (the node number depends on what else is plugged in)
v4l2-ctl --list-devices
ls -l /dev/v4l/by-id/ | grep -i GV-USB2     # stable path, use this in scripts

# watch it live
ffplay -f v4l2 -i /dev/video0 -vf setdar=4/3,setfield=tff

# or record a minute (match -video_size to the standard, see "Frame sizes")
ffmpeg -f v4l2 -video_size 720x486 -i /dev/video0 \
       -f alsa -i hw:CARD=gvusb2,DEV=0 -t 60 out.mkv
```

Prefer the `/dev/v4l/by-id/...-video-index0` path over `/dev/videoN` in
scripts: node numbers shuffle between boots.

## Video

### Inputs

```sh
v4l2-ctl -d /dev/video0 --set-input 0   # Composite (default)
v4l2-ctl -d /dev/video0 --set-input 1   # S-Video
```

### Standards

NTSC is the default. PAL also works:

```sh
v4l2-ctl -d /dev/video0 --set-standard pal
v4l2-ctl -d /dev/video0 --set-standard ntsc
```

The standard cannot be changed while a stream is running — stop capturing
first, otherwise the ioctl returns `EBUSY`.

### Frame sizes

| Standard | Normal | VBI enabled |
|---|---|---|
| NTSC | 720x486 | 720x516 |
| PAL | 720x576 | 720x616 |

VBI capture is 30 lines taller and starts earlier, so it includes the vertical
blanking interval where teletext and closed captions live. The top lines of
the VBI interval are not captured.

```sh
v4l2-ctl -d /dev/video0 --set-ctrl vbi_capture=1
```

### Controls

All controls are standard V4L2. Values persist until the module is reloaded;
applications should set what they need on start.

| Control | Range | Default | Notes |
|---|---|---|---|
| `brightness` | 0–255 | 128 | |
| `contrast` | 0–255 | 105 | |
| `saturation` | 0–255 | 128 | |
| `hue` | 0–255 | 128 | NTSC only |
| `sharpness` | 0–15 | 0 | |
| `gain_automatic` | 0–1 | 1 | AGC on/off |
| `gain` | 0–255 | 0 | only active when `gain_automatic=0` |
| `vertical_start` | 1–4 (1–3 on PAL) | 2 | higher values shift the picture up |
| `horizontal_start` | 0–4 | 4 | higher values shift the picture left |
| `vbi_capture` | 0–1 | 0 | adds the VBI lines |
| `black_pedestal` | 0–1 | 0 | force the 7.5 IRE pedestal on 625/50 input |

```sh
v4l2-ctl -d /dev/video0 -c brightness=140
v4l2-ctl -d /dev/video0 -c gain_automatic=0 -c gain=90
v4l2-ctl -d /dev/video0 -c black_pedestal=1   # 7.5 IRE blacks on PAL
```

**Black level.** By default the pedestal follows the standard: 7.5 IRE for
NTSC (which is what US tape and DVD sources carry) and 0 IRE for 625/50. Some
PAL/625 sources, and Japanese NTSC, sit at 0 IRE; setting `black_pedestal=1`
puts the 7.5 IRE pedestal back. Measured effect on a PAL capture: mean luma
rises from about 32 to about 50 and the share of pixels crushed to black falls
from 46% to 20%. If blacks look crushed, this is the control to try.

`v4l2-ctl --list-ctrls` on the device always prints the authoritative names,
ranges and defaults for your kernel.

### Interlacing

The device delivers interlaced SD video, top field first. Leave it interlaced
in the file (`setfield=tff`, `-flags +ilme+ildct`) and deinterlace at playback
or in your editor — that keeps the most options open for archival.

## Audio

One ALSA capture device, card name `gvusb2`, device 0:

```sh
arecord -D hw:CARD=gvusb2,DEV=0 -f S16_LE -r 48000 -c 2 out.wav
```

Fixed at 48 kHz, 16-bit, stereo — that is what the hardware provides. The
driver advertises a **21 ms minimum period** so media frameworks can timestamp
audio finely; a coarser floor caused periodic audio dropouts in OBS.

## Archival capture

Lossless FFV1 video + FLAC audio, interlaced frames preserved:

```sh
ffmpeg \
  -f v4l2 -framerate 29.97 -video_size 720x486 -rtbufsize 2G -thread_queue_size 1024 -i /dev/video0 \
  -f alsa -ac 2 -rtbufsize 512M -thread_queue_size 1024 -i hw:CARD=gvusb2,DEV=0 \
  -c:v ffv1 -level 3 -threads 4 -coder 1 -context 1 -slices 30 -slicecrc 1 -g 1 \
  -vf setdar=4/3,setfield=tff -flags +ilme+ildct -fflags +genpts+igndts -fps_mode:v passthrough \
  -c:a flac -compression_level 8 \
  OUTPUT_FILE.mkv
```

Notes:

- Compression is done in realtime: allow ~8 GB of free RAM and 4+ CPU cores.
  Recording straight to a spinning disk is not recommended.
- PAL: change to `-framerate 25 -video_size 720x576`.
- VBI: use `-video_size 720x516` (NTSC) or `720x616` (PAL).

## Streaming

See this guide: <https://gist.github.com/scaramangado/4e09031d782cbad8a4446ba101f43ef7>

## How it works (for the curious)

The device is a bridge chip (STK1150) plus a video decoder (TW9910) behind a
USB 2.0 isochronous endpoint:

- Video arrives on an isochronous endpoint in 3072-byte microframe packets,
  four URBs of 256 packets each in flight. Every packet carries a small header
  with a sequence counter; the driver assembles two interleaved fields into one
  interlaced frame and hands it to videobuf2.
- If packets go missing the counter jumps, the driver restarts the field rather
  than writing misaligned data — that is the `counter desync` line in `dmesg`.
- Bus-level errors are not fatal: when the host controller reports a failed
  transfer (`-EIO`/`-EPROTO`) the driver discards that batch and resubmits the
  URBs, so a one-off glitch costs a field or an audio packet instead of
  freezing the stream until you stop and restart capture.
- The TW9910 is configured over the bridge's I2C engine: video standard,
  cropping/active windows, black level and the analog input mux all live there.
- Audio is a second isochronous stream feeding ALSA at a fixed 48 kHz.

The 720x486 (not 480) height is deliberate: the driver returns the whole
received signal including a few lines of blanking. White or black bands at the
picture edge and a few millimetres of flicker at the bottom are normal for
capture devices — I-O DATA documents the same for the Windows driver.

Bandwidth is close to the USB 2.0 ceiling: ~21 MB/s needed against ~24.6 MB/s
theoretically available. Use a port that is not shared with other busy
isochronous devices if you see dropped frames.

## Tests

Two independent suites:

```sh
tests/run-tests.sh        # host unit tests, no hardware or kernel headers
tests/hw-test.sh          # on-device matrix, run on the capture machine
```

The host suite covers the pure logic (frame assembly, TW9910 register plans,
ALSA period arithmetic) and compiles the real driver sources against fake
kernel headers, one lane per area: core/I2C register protocol with a mock
register engine, the video path (URB lifecycle, field assembly, vb2 buffers,
probe/disconnect unwind), the V4L2 control/ioctl surface, and the audio path
(isoc completion, ring arithmetic, ALSA open/close/disconnect). The hardware
suite checks all four geometry modes, frame integrity, frame rate, kernel-log
health, warmed-up steady state and the audio path; it needs the capture device
to itself, so close OBS before running it. Both are expected to be green
before any change is merged.

## Troubleshooting

**`Device or resource busy`** — something already holds the device (OBS, a
browser tab, a stray `arecord`). Close it. `fuser -v /dev/video0` shows who.

**`modprobe -r` says the module is in use** — the video/audio node is still
open. Besides OBS and `arecord`, `pipewire`/`wireplumber` keep the ALSA card
open and `alsa-state` holds the control device; stop them as shown under
*Installation* and reload.

**`insmod: Key was rejected by service`** — Secure Boot, see *Requirements*.

**The video node never appears** (`dmesg | grep gvusb2` shows the probe was
tried) — check that the `tw9910` module is available on this kernel; the
driver fails the probe deliberately when the decoder cannot be registered.

**Dropped frames / audio skips** — almost always USB bandwidth or host load.
Try another port, remove other USB devices, close background applications. The
driver already logs specific causes; check `dmesg | grep gvusb2`.

**Bands at the edge / flicker at the bottom** — by design, see above. Adjust
`vertical_start` / `horizontal_start` to trim, or crop in software.

**No audio** — check `arecord -l` for card `gvusb2`, and make sure the source
device actually has audio connected to the RCA inputs. A silent capture at
about -90 dBFS is a missing source, not a broken driver.

**Video looks wrong after standby or replug** — the driver handles hot-unplug
and rebind; if a stream misbehaves, stop and restart it, or reload:

```sh
sudo modprobe -r gvusb2-video gvusb2-sound
sudo modprobe gvusb2-video
sudo modprobe gvusb2-sound          # add index=N if 7 is taken
```

## Limitations

- **720 pixels wide only.** The hardware can capture narrower widths (the
  vendor software offers 352x240 up to 720x480); this driver does not expose
  that yet.
- **No tuner, no hardware scaling, no hardware deinterlacing.** The device is
  a capture bridge; scaling and deinterlacing belong in software.
- **PAL/SECAM is beyond the vendor spec.** I-O DATA documents NTSC only; it
  works because the decoder chip is multi-standard, and PAL is regularly
  tested, but treat it as an extension rather than a supported mode.
- Audio is capture-only, as the hardware has no output path.

## Licence

Dual licensed BSD-3-Clause / GPL-2.0 — see `LICENSE.bsd` and `LICENSE.gpl`.
Original driver by Isaac Lozano; this fork carries PAL support, geometry fixes
and the test suites.
