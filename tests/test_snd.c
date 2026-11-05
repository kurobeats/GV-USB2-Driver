/* Host-side tests for the gvusb2-snd lane: the isochronous completion path
 * (bytes -> frames -> hw_ptr/avail, DMA-ring placement), the period/buffer
 * constraints advertised at open, and the open/close/trigger state machine.
 *
 * The real gvusb2-snd.c is compiled here against the fake kernel, USB and ALSA
 * surfaces in utk_snd/ and #included for access to its static functions, the
 * same trick as tests/test_core.c. See tests/run-tests.sh for the include
 * paths.
 *
 * No hardware, no URBs are ever submitted to anything: the tests are the host
 * controller, filling in iso_frame_desc[] and calling urb->complete().
 *
 * Not modelled, hence not covered: ALSA's constraint solver and substream
 * state machine (the tests assert the runtime->hw table the driver advertises,
 * not what snd_pcm_hw_params() derives from it), and locking (the fake
 * spinlocks are no-ops on a single thread).
 */

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "utk_snd/utk_snd.h"
#include <sound/core.h>
#include <sound/pcm.h>

/* The kernel builds this driver without -Wsign-compare (it compares int
 * offsets against unsigned frame and descriptor counts), so the host flags
 * must not reject the unmodified source either. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#include "../gvusb2-snd.c"
#pragma GCC diagnostic pop

/* Fake implementations and harness helpers; needs struct gvusb2_snd above. */
#include "utk_snd/utk_fakes.c"

static struct usb_device utk_udev;
static struct usb_interface utk_io_intf;	/* for the ALSA-only tests */
static struct usb_interface utk_probe_intf;	/* for the probe tests */
static struct usb_host_interface utk_probe_alt;
static struct usb_host_endpoint utk_probe_ep;

/* A device as gvusb2_snd_probe() leaves it: card, PCM and URBs in place. */
static struct gvusb2_snd *utk_dev(void)
{
	struct gvusb2_snd *dev = calloc(1, sizeof(*dev));

	assert(dev != NULL);
	dev->intf = &utk_io_intf;
	dev->gv.udev = &utk_udev;
	assert(gvusb2_snd_alsa_init(dev) == 0);
	assert(gvusb2_snd_allocate_urbs(dev) == 0);
	return dev;
}

/* Byte pattern the fake host controller puts into packet `packet`, index `j`.
 * Distinct enough to tell the packets apart for the short lens used below. */
static unsigned char utk_byte(int packet, unsigned int index)
{
	return (unsigned char)(packet * 64 + index);
}

/* Place `n` packets at their transfer-buffer offsets with lens[] delivered
 * bytes and status[] (NULL = all good). */
static void utk_fill_packets(struct urb *urb, int n, const unsigned int *lens,
			     const int *status)
{
	int p;

	urb->number_of_packets = n;
	for (p = 0; p < n; p++) {
		unsigned char *pkt = urb->transfer_buffer +
				     urb->iso_frame_desc[p].offset;
		unsigned int j;

		urb->iso_frame_desc[p].actual_length = lens[p];
		urb->iso_frame_desc[p].status = status ? status[p] : 0;
		for (j = 0; j < lens[p]; j++)
			pkt[j] = utk_byte(p, j);
	}
}

/* Packet `packet`'s bytes [first, first+len) must sit at ring + offset. */
static void utk_expect_packet(const unsigned char *ring, unsigned int offset,
			      int packet, unsigned int first, unsigned int len)
{
	unsigned int j;

	for (j = 0; j < len; j++)
		assert(ring[offset + j] == utk_byte(packet, first + j));
}

static void utk_expect_zero(const unsigned char *ring, unsigned int offset,
			    unsigned int len)
{
	unsigned int j;

	for (j = 0; j < len; j++)
		assert(ring[offset + j] == 0);
}

/* The fake DMA ring's canary: nothing may be written past dma_bytes. */
static void utk_expect_canary(const struct snd_pcm_runtime *runtime)
{
	unsigned int j;

	for (j = 0; j < UTK_RING_PAD; j++)
		assert(runtime->dma_area[runtime->dma_bytes + j] ==
		       UTK_RING_CANARY);
}

/*****************************************************************************
 * Constraints advertised at open
 ****************************************************************************/

/* The hw-constraint table only exists for downstream consumers once
 * capture_open() copies it into runtime->hw: this is the seam that makes the
 * 4096-byte floor (and the fixed rate/format) real. */
static void test_open_installs_constraints(void)
{
	struct snd_pcm_hardware hw;
	struct gvusb2_snd *dev;
	struct snd_pcm_substream sub;
	struct snd_pcm_runtime *runtime;

	utk_reset();
	dev = utk_dev();
	runtime = utk_runtime(1024, 1024, 4096);

	assert(utk_capture_open(dev, &sub, runtime) == 0);
	hw = runtime->hw;

	assert(hw.period_bytes_min == 4096);
	assert(gvusb2_period_us(hw.period_bytes_min) == 21333);
	assert(hw.period_bytes_max == 128 * 1024);
	assert(hw.buffer_bytes_max == 128 * 1024);
	assert(hw.periods_min == 1);
	assert(hw.periods_max == 32);

	/* Both period bounds must be whole frames: hw_ptr advances in frames
	 * while the ring is consumed in bytes. */
	assert(hw.period_bytes_min % GVUSB2_SND_FRAME_BYTES == 0);
	assert(hw.period_bytes_max % GVUSB2_SND_FRAME_BYTES == 0);
	assert(hw.buffer_bytes_max % GVUSB2_SND_FRAME_BYTES == 0);

	/* Fixed format: the whole constraint arithmetic (and
	 * gvusb2_period_us(), which assumes 192000 B/s) is only valid for
	 * 48 kHz S16_LE stereo, so nothing else may be advertised. */
	assert(hw.rate_min == 48000 && hw.rate_max == 48000);
	assert(hw.rates == SNDRV_PCM_RATE_48000);
	assert((hw.rates & SNDRV_PCM_RATE_44100) == 0);
	assert(hw.formats == SNDRV_PCM_FMTBIT_S16_LE);
	assert(hw.channels_min == 2 && hw.channels_max == 2);

	assert((hw.info & SNDRV_PCM_INFO_MMAP) != 0);
	assert((hw.info & SNDRV_PCM_INFO_MMAP_VALID) != 0);
	assert((hw.info & SNDRV_PCM_INFO_INTERLEAVED) != 0);
	assert((hw.info & SNDRV_PCM_INFO_BLOCK_TRANSFER) != 0);
	/* resume/pause are only honest if ->trigger accepts the commands; it
	 * rejects everything but START/STOP. */
	assert((hw.info & SNDRV_PCM_INFO_PAUSE) == 0);
	assert((hw.info & SNDRV_PCM_INFO_RESUME) == 0);

	/* the open claimed the substream and started every URB */
	assert(dev->substream == &sub);
	assert(utk.submit_calls == GVUSB2_NUM_URBS);
}

/* Which period byte counts the advertised table admits. Only the floor/
 * period-cap relation is pinned here: it is what keeps the finest period from
 * silently changing the buffer/period ratio. */
static void test_period_constraint_arithmetic(void)
{
	struct gvusb2_snd *dev;
	struct snd_pcm_substream sub;
	struct snd_pcm_runtime *runtime;
	struct snd_pcm_hardware hw;

	utk_reset();
	dev = utk_dev();
	runtime = utk_runtime(1024, 1024, 4096);
	assert(utk_capture_open(dev, &sub, runtime) == 0);
	hw = runtime->hw;

	/* The floor is admissible with the advertised period cap; a finer floor
	 * would silently change the buffer/period ratio. */
	assert(hw.buffer_bytes_max / hw.period_bytes_min == hw.periods_max);
}

/*****************************************************************************
 * open / close
 ****************************************************************************/

/* Only one capture stream at a time; a rejected open must not disturb the
 * stream that owns the device. */
static void test_second_open_is_busy(void)
{
	struct gvusb2_snd *dev;
	struct snd_pcm_substream first, second;
	struct snd_pcm_runtime *rt_first, *rt_second;

	utk_reset();
	dev = utk_dev();
	rt_first = utk_runtime(1024, 1024, 4096);
	rt_second = utk_runtime(1024, 1024, 4096);

	assert(utk_capture_open(dev, &first, rt_first) == 0);
	assert(utk.submit_calls == GVUSB2_NUM_URBS);

	assert(utk_capture_open(dev, &second, rt_second) == -EBUSY);
	assert(dev->substream == &first);
	/* no URBs submitted for the rejected open, and the rejected substream
	 * did not get half-configured */
	assert(utk.submit_calls == GVUSB2_NUM_URBS);
	assert(rt_second->hw.period_bytes_min == 0);

	/* still busy, not confused into releasing */
	assert(utk_capture_open(dev, &second, rt_second) == -EBUSY);
	assert(dev->substream == &first);

	/* close releases: URBs killed, device reusable */
	assert(gvusb2_snd_capture_close(&first) == 0);
	assert(dev->substream == NULL);
	assert(utk.kill_calls == GVUSB2_NUM_URBS);

	assert(utk_capture_open(dev, &second, rt_second) == 0);
	assert(dev->substream == &second);
	assert(utk.submit_calls == 2 * GVUSB2_NUM_URBS);
	assert(rt_second->hw.period_bytes_min == 4096);
}

/* BUG: gvusb2_snd_submit_isoc() swallowed usb_submit_urb() errors and always
 * returned 0, so a stream whose URBs never started opened successfully and
 * simply never delivered audio. */
static void test_open_unwinds_on_submit_failure(void)
{
	struct gvusb2_snd *dev;
	struct snd_pcm_substream sub;
	struct snd_pcm_runtime *runtime;
	int ret;

	utk_reset();
	dev = utk_dev();
	runtime = utk_runtime(1024, 1024, 4096);

	utk.submit_fail = 1;
	ret = utk_capture_open(dev, &sub, runtime);
	assert(ret < 0);
	/* the open must not leave the substream claimed ... */
	assert(dev->substream == NULL);
	/* ... nor URBs running that ALSA will never close (it does not call
	 * ->close after a failed ->open) */
	assert(utk.kill_calls == GVUSB2_NUM_URBS);

	/* and the device is still openable once the USB side recovers */
	utk.submit_fail = 0;
	assert(utk_capture_open(dev, &sub, runtime) == 0);
	assert(dev->substream == &sub);
}

/* A completion that arrives for an urb killed by close must not write into the
 * runtime ALSA has already torn down, nor resurrect the urb. */
static void test_close_stops_completion(void)
{
	static const unsigned int lens[] = { 24 };
	struct gvusb2_snd *dev;
	struct snd_pcm_substream sub;
	struct snd_pcm_runtime *runtime;
	struct urb *urb;
	int submits;

	utk_reset();
	dev = utk_dev();
	runtime = utk_runtime(1024, 1024, 4096);
	assert(utk_capture_open(dev, &sub, runtime) == 0);

	urb = dev->urbs[0];
	utk_fill_packets(urb, 1, lens, NULL);
	submits = utk.submit_calls;

	assert(gvusb2_snd_capture_close(&sub) == 0);

	urb->complete(urb);

	assert(dev->hw_ptr == 0);	/* pointer not moved */
	assert(dev->dma_offset == 0);	/* ring not consumed */
	assert(dev->avail == 0);
	utk_expect_zero(runtime->dma_area, 0, 24);
	/* BUG: the completion handler resubmitted unconditionally, so a
	 * completion in flight during close left a live urb behind for a stream
	 * that no longer exists. */
	assert(utk.submit_calls == submits);
}

/*****************************************************************************
 * isochronous completion accounting
 ****************************************************************************/

/* The pointer must advance by the frames actually delivered, at 4 bytes per
 * frame. */
static void test_pointer_advances_by_delivered_frames(void)
{
	static const unsigned int lens[] = { 24, 24, 24, 24 };
	struct gvusb2_snd *dev;
	struct snd_pcm_substream sub;
	struct snd_pcm_runtime *runtime;

	utk_reset();
	dev = utk_dev();
	runtime = utk_runtime(1024, 1024, 4096);
	assert(utk_capture_open(dev, &sub, runtime) == 0);

	utk_fill_packets(dev->urbs[0], 4, lens, NULL);
	dev->urbs[0]->complete(dev->urbs[0]);

	/* 4 packets * 24 bytes = 96 bytes = 24 frames */
	assert(dev->hw_ptr == 24);
	assert(gvusb2_snd_pcm_pointer(&sub) == 24);
	assert(dev->dma_offset == 96);
	assert(dev->avail == 24);
	assert(utk.period_elapsed_calls == 0);
	utk_expect_packet(runtime->dma_area, 0, 0, 0, 24);
	utk_expect_packet(runtime->dma_area, 24, 1, 0, 24);
	utk_expect_packet(runtime->dma_area, 48, 2, 0, 24);
	utk_expect_packet(runtime->dma_area, 72, 3, 0, 24);
}

/* Real 48 kHz endpoints vary packet length and the last packet of a stream is
 * short. A length that is not a whole number of frames loses its tail (or the
 * pointer would drift away from the ring forever), and packet n must be read
 * from its own slot in the transfer buffer, not from just after packet n-1. */
static void test_partial_packets_and_stride(void)
{
	static const unsigned int lens[] = { 24, 22, 26, 4 };
	struct gvusb2_snd *dev;
	struct snd_pcm_substream sub;
	struct snd_pcm_runtime *runtime;

	utk_reset();
	dev = utk_dev();
	runtime = utk_runtime(1024, 1024, 4096);
	assert(utk_capture_open(dev, &sub, runtime) == 0);

	utk_fill_packets(dev->urbs[0], 4, lens, NULL);
	dev->urbs[0]->complete(dev->urbs[0]);

	/* delivered: 24 + 20 + 24 + 4 bytes = 72 bytes = 18 frames, so the
	 * pointer and the ring stay in step to the byte */
	assert(dev->dma_offset == 72);
	assert(dev->hw_ptr == 18);
	assert(dev->avail == 18);
	assert(frames_to_bytes(runtime, dev->hw_ptr) == dev->dma_offset);

	utk_expect_packet(runtime->dma_area, 0, 0, 0, 24);
	utk_expect_packet(runtime->dma_area, 24, 1, 0, 20);	/* 2 stray bytes dropped */
	utk_expect_packet(runtime->dma_area, 44, 2, 0, 24);	/* 2 stray bytes dropped */
	utk_expect_packet(runtime->dma_area, 68, 3, 0, 4);
	utk_expect_canary(runtime);
}

/* The ring wraps: a packet that straddles the end is split, and nothing is
 * written past dma_bytes. */
static void test_dma_ring_wrap_mid_packet(void)
{
	static const unsigned int lens[] = { 24, 24, 24 };
	struct gvusb2_snd *dev;
	struct snd_pcm_substream sub;
	struct snd_pcm_runtime *runtime;

	utk_reset();
	dev = utk_dev();
	runtime = utk_runtime(1024, 1024, 64);
	assert(utk_capture_open(dev, &sub, runtime) == 0);

	utk_fill_packets(dev->urbs[0], 3, lens, NULL);
	dev->urbs[0]->complete(dev->urbs[0]);

	assert(dev->dma_offset == 8);	/* 8 bytes of packet 2 at the start */
	assert(dev->hw_ptr == 18);
	utk_expect_packet(runtime->dma_area, 48, 2, 0, 16);	/* 48..63 */
	utk_expect_packet(runtime->dma_area, 0, 2, 16, 8);	/* the split tail */
	utk_expect_packet(runtime->dma_area, 8, 0, 8, 16);	/* not overwritten */
	utk_expect_packet(runtime->dma_area, 24, 1, 0, 24);
	utk_expect_canary(runtime);
}

/* A packet that ends exactly on the ring boundary is not a wrap (dma_offset
 * becomes == dma_bytes); the next packet starts a new lap with a zero-length
 * first half. Off-by-one here would either skip a byte or write out of
 * bounds. */
static void test_dma_ring_exact_boundary(void)
{
	static const unsigned int two[] = { 24, 16 };
	static const unsigned int tail[] = { 4 };
	struct gvusb2_snd *dev;
	struct snd_pcm_substream sub;
	struct snd_pcm_runtime *runtime;

	utk_reset();
	dev = utk_dev();
	runtime = utk_runtime(1024, 1024, 40);
	assert(utk_capture_open(dev, &sub, runtime) == 0);

	utk_fill_packets(dev->urbs[0], 2, two, NULL);
	dev->urbs[0]->complete(dev->urbs[0]);

	assert(dev->dma_offset == 40);	/* exactly the end, no wrap yet */
	utk_expect_packet(runtime->dma_area, 0, 0, 0, 24);
	utk_expect_packet(runtime->dma_area, 24, 1, 0, 16);

	utk_fill_packets(dev->urbs[0], 1, tail, NULL);
	dev->urbs[0]->complete(dev->urbs[0]);

	assert(dev->dma_offset == 4);
	assert(dev->hw_ptr == 11);
	utk_expect_packet(runtime->dma_area, 0, 0, 0, 4);
	utk_expect_packet(runtime->dma_area, 4, 0, 4, 20);	/* still packet 0 */
	utk_expect_packet(runtime->dma_area, 24, 1, 0, 16);
	utk_expect_canary(runtime);
}

/* period_elapsed() must fire once the delivered frames complete the period
 * and not before, and the pointer must wrap at buffer_size rather than run
 * past it or be reset to 0. A completion that over-runs the period consumes
 * exactly one period's credit: the surplus stays in avail. */
static void test_period_boundary_and_pointer_wrap(void)
{
	static unsigned int lens[16];
	struct gvusb2_snd *dev;
	struct snd_pcm_substream sub;
	struct snd_pcm_runtime *runtime;
	int i;

	utk_reset();
	dev = utk_dev();
	runtime = utk_runtime(1024, 1024, 4096);
	assert(utk_capture_open(dev, &sub, runtime) == 0);

	/* 15 packets of 256 bytes (64 frames) + 252 bytes (63 frames) =
	 * 1023 frames: one frame short of the period */
	for (i = 0; i < 15; i++)
		lens[i] = 256;
	lens[15] = 252;
	utk_fill_packets(dev->urbs[0], 16, lens, NULL);
	dev->urbs[0]->complete(dev->urbs[0]);

	assert(dev->hw_ptr == 1023);
	assert(dev->avail == 1023);
	assert(utk.period_elapsed_calls == 0);

	/* 1024 frames complete the period ... */
	utk_fill_packets(dev->urbs[1], 1, (unsigned int[]){ 256 }, NULL);
	dev->urbs[1]->complete(dev->urbs[1]);

	/* 1087 frames arrive: one period is consumed and the surplus 63 frames
	 * stay credited, and the pointer wraps to 63 rather than being zeroed. */
	assert(utk.period_elapsed_calls == 1);
	assert(utk.period_substream == &sub);
	assert(dev->avail == 63);	/* one period consumed, not zeroed */
	assert(dev->hw_ptr == 63);	/* 1087 - 1024, not zeroed */
	assert(gvusb2_snd_pcm_pointer(&sub) == 63);
	assert((size_t)frames_to_bytes(runtime, dev->hw_ptr) <= runtime->dma_bytes);
}

/* A per-packet isoc error drops that packet entirely: it must not advance the
 * pointer and must not consume ring space. */
static void test_isoc_packet_error_skipped(void)
{
	static const unsigned int lens[] = { 24, 24, 24 };
	static const int status[] = { 0, -EPROTO, 0 };
	struct gvusb2_snd *dev;
	struct snd_pcm_substream sub;
	struct snd_pcm_runtime *runtime;

	utk_reset();
	dev = utk_dev();
	runtime = utk_runtime(1024, 1024, 4096);
	assert(utk_capture_open(dev, &sub, runtime) == 0);

	utk_fill_packets(dev->urbs[0], 3, lens, status);
	dev->urbs[0]->complete(dev->urbs[0]);

	assert(dev->hw_ptr == 12);	/* 2 packets delivered */
	assert(dev->dma_offset == 48);
	utk_expect_packet(runtime->dma_area, 0, 0, 0, 24);
	utk_expect_packet(runtime->dma_area, 24, 2, 0, 24);	/* no hole */
	utk_expect_zero(runtime->dma_area, 48, 24);
}

/* Which URB-level statuses drop the buffer, and whether the urb goes back
 * to the host controller. */
static void test_isoc_urb_status_filtering(void)
{
	static const unsigned int lens[] = { 24 };
	static const int retryable[] = { -ECONNRESET, -ENOENT, -ESHUTDOWN };
	static const int other_errors[] = { -EIO, -EPROTO, -ETIMEDOUT };
	struct gvusb2_snd *dev;
	struct snd_pcm_substream sub;
	struct snd_pcm_runtime *runtime;
	struct urb *urb;
	int submits;
	size_t i;

	utk_reset();
	dev = utk_dev();
	runtime = utk_runtime(1024, 1024, 4096);
	assert(utk_capture_open(dev, &sub, runtime) == 0);
	urb = dev->urbs[0];
	submits = utk.submit_calls;

	for (i = 0; i < sizeof(retryable) / sizeof(retryable[0]); i++) {
		utk_fill_packets(urb, 1, lens, NULL);
		urb->status = retryable[i];
		urb->complete(urb);
		assert(dev->hw_ptr == 0);
		assert(utk.submit_calls == submits);	/* killed, not retried */
	}

	/* BUG: a transient HCD error (-EIO/-EPROTO/-ETIMEDOUT) was not retried,
	 * which froze capture forever with no error surfaced to ALSA. The video
	 * lane resubmits these; both lanes now agree. */
	for (i = 0; i < sizeof(other_errors) / sizeof(other_errors[0]); i++) {
		urb->status = other_errors[i];
		utk_fill_packets(urb, 1, lens, NULL);
		urb->complete(urb);
		assert(dev->hw_ptr == 0);	/* not processed */
		assert(urb->iso_frame_desc[0].actual_length == 0);
		assert(urb->iso_frame_desc[0].status == 0);
		assert(utk.submit_calls == submits + 1);
		assert(utk.last_submit_flags == GFP_ATOMIC);
		assert(utk.last_submitted == urb);
		submits = utk.submit_calls;
	}

	/* a good completion is processed, its descriptors cleared, and the urb
	 * handed back with GFP_ATOMIC (completion context) */
	urb->status = 0;
	utk_fill_packets(urb, 1, lens, NULL);
	urb->complete(urb);
	assert(dev->hw_ptr == 6);
	assert(utk.submit_calls == submits + 1);
	assert(utk.last_submit_flags == GFP_ATOMIC);
	assert(utk.last_submitted == urb);
	assert(urb->iso_frame_desc[0].actual_length == 0);
	assert(urb->iso_frame_desc[0].status == 0);
}

/* One completion can deliver many laps of a small buffer (256 packets of
 * 256 bytes = 16384 frames against a 1024-frame minimum buffer): the pointer
 * must land at n % buffer_size, not stop somewhere beyond it after a single
 * subtraction. */
static void test_pointer_multi_lap(void)
{
	static unsigned int lens[64];
	struct gvusb2_snd *dev;
	struct snd_pcm_substream sub;
	struct snd_pcm_runtime *runtime;
	int i;

	utk_reset();
	dev = utk_dev();
	runtime = utk_runtime(1024, 1024, 4096);
	assert(utk_capture_open(dev, &sub, runtime) == 0);

	/* 63 packets of 256 B (64 frames) + 1 packet of 252 B (63 frames) =
	 * 4095 frames: almost four laps of the 1024-frame buffer */
	for (i = 0; i < 63; i++)
		lens[i] = 256;
	lens[63] = 252;
	utk_fill_packets(dev->urbs[0], 64, lens, NULL);
	dev->urbs[0]->complete(dev->urbs[0]);

	assert(dev->hw_ptr == 4095 % 1024);
	assert(gvusb2_snd_pcm_pointer(&sub) == 4095 % 1024);
	assert(gvusb2_snd_pcm_pointer(&sub) < runtime->buffer_size);
}

/* URBs stream from probe->open until hw_params arms the DMA ring: a completion
 * arriving in that window must be dropped whole (accounting untouched, ring
 * untouched), not memcpy'd to dma_area == NULL, and the urb must still be
 * resubmitted so the stream is not lost. */
static void test_completion_before_hw_params_dropped(void)
{
	static const unsigned int lens[] = { 24 };
	struct gvusb2_snd *dev;
	struct snd_pcm_substream sub;
	struct snd_pcm_runtime *runtime;
	struct urb *urb;

	utk_reset();
	dev = utk_dev();
	runtime = utk_runtime(1024, 512, 0);	/* ring not armed: dma_bytes 0 */
	memset(&sub, 0, sizeof(sub));
	sub.runtime = runtime;
	dev->substream = &sub;

	urb = dev->urbs[0];
	utk_fill_packets(urb, 1, lens, NULL);
	urb->complete(urb);

	assert(dev->hw_ptr == 0);
	assert(dev->dma_offset == 0);
	assert(dev->avail == 0);
	/* resubmission still happens, so the stream is not lost */
	assert(utk.submit_calls == 1);
	assert(utk.last_submit_flags == GFP_ATOMIC);

	dev->substream = NULL;
}

/*****************************************************************************
 * trigger
 ****************************************************************************/

/* START must not disturb the accounting (the URBs are already running), STOP
 * resets it for the next stream, and everything else is refused rather than
 * silently treated as START/STOP. */
static void test_trigger_start_stop(void)
{
	static const unsigned int lens[] = { 24 };
	struct gvusb2_snd *dev;
	struct snd_pcm_substream sub;
	struct snd_pcm_runtime *runtime;

	utk_reset();
	dev = utk_dev();
	runtime = utk_runtime(1024, 1024, 4096);
	assert(utk_capture_open(dev, &sub, runtime) == 0);

	utk_fill_packets(dev->urbs[0], 1, lens, NULL);
	dev->urbs[0]->complete(dev->urbs[0]);
	assert(dev->hw_ptr == 6 && dev->dma_offset == 24 && dev->avail == 6);

	assert(gvusb2_snd_pcm_trigger(&sub, SNDRV_PCM_TRIGGER_START) == 0);
	assert(dev->hw_ptr == 6);
	assert(dev->dma_offset == 24);
	assert(dev->avail == 6);

	assert(gvusb2_snd_pcm_trigger(&sub, SNDRV_PCM_TRIGGER_PAUSE_PUSH) == -EINVAL);
	assert(gvusb2_snd_pcm_trigger(&sub, SNDRV_PCM_TRIGGER_SUSPEND) == -EINVAL);
	assert(gvusb2_snd_pcm_trigger(&sub, 0x7fff) == -EINVAL);
	assert(dev->hw_ptr == 6);	/* a refused command changes nothing */

	assert(gvusb2_snd_pcm_trigger(&sub, SNDRV_PCM_TRIGGER_STOP) == 0);
	assert(dev->hw_ptr == 0);
	assert(dev->dma_offset == 0);
	assert(dev->avail == 0);
	assert(gvusb2_snd_pcm_pointer(&sub) == 0);
}

/*****************************************************************************
 * hw_params / hw_free
 ****************************************************************************/

static void test_hw_params_allocates_and_frees(void)
{
	struct snd_pcm_runtime runtime;
	struct snd_pcm_substream sub;
	struct snd_pcm_hw_params params;

	utk_reset();
	memset(&runtime, 0, sizeof(runtime));
	memset(&sub, 0, sizeof(sub));
	sub.runtime = &runtime;

	params.buffer_bytes = 32768;
	assert(gvusb2_snd_hw_params(&sub, &params) == 0);
	assert(utk.vmalloc_calls == 1);
	assert(utk.last_vmalloc_size == 32768);
	assert(runtime.dma_bytes == 32768);
	assert(runtime.dma_area != NULL);

	/* re-arming frees the previous ring instead of leaking it */
	params.buffer_bytes = 16384;
	assert(gvusb2_snd_hw_params(&sub, &params) == 0);
	assert(utk.vfree_calls == 1);
	assert(utk.last_vmalloc_size == 16384);
	assert(runtime.dma_bytes == 16384);

	assert(gvusb2_snd_hw_free(&sub) == 0);
	assert(utk.vfree_calls == 2);
	assert(runtime.dma_bytes == 0);

	/* hw_free is called unconditionally by ALSA; a second call must not
	 * free the ring twice */
	assert(gvusb2_snd_hw_free(&sub) == 0);
	assert(utk.vfree_calls == 2);
}

/* A failed allocation must leave the runtime in a state where the following
 * hw_free() does not free a stale pointer. */
static void test_hw_params_allocation_failure(void)
{
	struct snd_pcm_runtime runtime;
	struct snd_pcm_substream sub;
	struct snd_pcm_hw_params params;

	utk_reset();
	memset(&runtime, 0, sizeof(runtime));
	memset(&sub, 0, sizeof(sub));
	sub.runtime = &runtime;
	params.buffer_bytes = 32768;

	/* a failed allocation leaves no stale pointer for the isoc completion
	 * path (it may arrive between the failed hw_params and the next one) */
	utk_reset();
	memset(&runtime, 0, sizeof(runtime));
	memset(&sub, 0, sizeof(sub));
	sub.runtime = &runtime;
	utk.vmalloc_fail = 1;
	assert(gvusb2_snd_hw_params(&sub, &params) == -ENOMEM);
	assert(runtime.dma_bytes == 0);
	assert(runtime.dma_area == NULL);
	assert(gvusb2_snd_hw_free(&sub) == 0);
	assert(utk.vfree_calls == 0);

	/* and a successful allocation followed by a failed re-arm releases the
	 * first ring exactly once and leaves no dangling pointer */
	utk.vmalloc_fail = 0;
	assert(gvusb2_snd_hw_params(&sub, &params) == 0);
	utk.vmalloc_fail = 1;
	assert(gvusb2_snd_hw_params(&sub, &params) == -ENOMEM);
	assert(utk.vfree_calls == 1);
	assert(runtime.dma_bytes == 0);
	assert(runtime.dma_area == NULL);
	assert(gvusb2_snd_hw_free(&sub) == 0);
	assert(utk.vfree_calls == 1);
}

/*****************************************************************************
 * URB setup / probe
 ****************************************************************************/

/* The URB layout fixes where each packet's bytes land in the transfer buffer
 * and how many bytes the DMA ring may receive per completion. */
static void test_allocate_urbs_layout(void)
{
	struct gvusb2_snd *dev;
	int i, p;

	utk_reset();
	dev = utk_dev();

	assert(utk.urb_alloc_calls == GVUSB2_NUM_URBS);
	for (i = 0; i < GVUSB2_NUM_URBS; i++) {
		struct urb *urb = dev->urbs[i];
		unsigned int offset = 0;
		unsigned int total = 0;

		assert(urb != NULL);
		assert(urb->dev == &utk_udev);
		assert(urb->pipe == usb_rcvisocpipe(&utk_udev, 0x04));
		assert(urb->complete == gvusb2_snd_isoc_irq);
		assert(urb->context == dev);
		assert(urb->interval == 1);
		assert(urb->start_frame == 0);
		assert(urb->transfer_flags == URB_ISO_ASAP);
		assert(urb->number_of_packets == GVUSB2_NUM_ISOCH_PACKETS);
		assert(urb->transfer_buffer != NULL);
		assert(urb->transfer_buffer_length ==
		       GVUSB2_NUM_ISOCH_PACKETS * GVUSB2_MAX_AUDIO_PACKET_SIZE);

		for (p = 0; p < GVUSB2_NUM_ISOCH_PACKETS; p++) {
			assert(urb->iso_frame_desc[p].offset == offset);
			assert(urb->iso_frame_desc[p].length ==
			       GVUSB2_MAX_AUDIO_PACKET_SIZE);
			offset += urb->iso_frame_desc[p].length;
		}
		/* the descriptor slots tile the transfer buffer exactly: the last
		 * packet must not read past it */
		total = offset;
		assert(total == urb->transfer_buffer_length);
	}

	gvusb2_snd_free_isoc(dev);
	assert(utk.urb_free_calls == GVUSB2_NUM_URBS);
	/* every transfer buffer freed, not leaked */
	assert(utk.kfree_calls == GVUSB2_NUM_URBS);
	for (i = 0; i < GVUSB2_NUM_URBS; i++)
		assert(dev->urbs[i] == NULL);
}

/* A partially allocated URB set must be released, not half-kept. */
static void test_allocate_urbs_failure(void)
{
	struct gvusb2_snd dev;
	int i, ret;

	utk_reset();
	memset(&dev, 0, sizeof(dev));
	dev.gv.udev = &utk_udev;
	dev.intf = &utk_io_intf;
	utk.urb_alloc_fail_at = 3;

	ret = gvusb2_snd_allocate_urbs(&dev);
	assert(ret == -ENOMEM);
	assert(utk.urb_alloc_calls == 3);	/* third allocation failed */
	/* the two already allocated are released and the array cleared, so a
	 * later ->disconnect cannot free them again */
	assert(utk.urb_free_calls == 2);
	/* with their transfer buffers */
	assert(utk.kfree_calls == 2);
	for (i = 0; i < GVUSB2_NUM_URBS; i++)
		assert(dev.urbs[i] == NULL);
}

/* Builds a one-altsetting, one-endpoint interface and runs ->probe. */
static int utk_probe(u8 endpoint_address, u8 attributes, u16 max_packet,
		     u8 interface_class)
{
	memset(&utk_probe_ep, 0, sizeof(utk_probe_ep));
	memset(&utk_probe_alt, 0, sizeof(utk_probe_alt));
	memset(&utk_probe_intf, 0, sizeof(utk_probe_intf));

	utk_probe_ep.desc.bEndpointAddress = endpoint_address;
	utk_probe_ep.desc.bmAttributes = attributes;
	utk_probe_ep.desc.wMaxPacketSize = max_packet;

	/* the real device enumerates the audio interface as interface 2,
	 * alternate setting 1; probe must read these, not hardcode them */
	utk_probe_alt.desc.bInterfaceNumber = 2;
	utk_probe_alt.desc.bAlternateSetting = 1;

	utk_probe_alt.desc.bNumEndpoints = 1;
	utk_probe_alt.desc.bInterfaceClass = interface_class;
	utk_probe_alt.endpoint = &utk_probe_ep;

	utk_probe_intf.altsetting = &utk_probe_alt;
	utk_probe_intf.num_altsetting = 1;
	utk_probe_intf.udev = &utk_udev;

	return gvusb2_snd_probe(&utk_probe_intf, NULL);
}

/* The audio interface is identified by one very specific endpoint: IN, isoc,
 * address 0x84 and a 256-byte max packet. Anything else is not ours. */
static void test_probe_endpoint_filter(void)
{
	const u8 isoc = USB_ENDPOINT_XFER_ISOC;

	utk_reset();
	assert(utk_probe(0x04, isoc, 0x100, 0) == -ENODEV);	/* OUT */
	assert(utk.gvusb2_init_calls == 0);
	assert(utk_probe(0x85, isoc, 0x100, 0) == -ENODEV);	/* other address */
	assert(utk_probe(0x84, USB_ENDPOINT_XFER_BULK, 0x100, 0) == -ENODEV);
	assert(utk_probe(0x84, isoc, 0x0ff, 0) == -ENODEV);	/* one byte short */
	assert(utk_probe(0x84, isoc, 0x101, 0) == -ENODEV);	/* one byte over */
	assert(utk.gvusb2_init_calls == 0);
	assert(utk.card_new_calls == 0);

	/* the USB-audio-class interface is explicitly refused even though it
	 * carries a matching endpoint */
	assert(utk_probe(0x84, isoc, 0x100, USB_CLASS_AUDIO) == -ENODEV);
	assert(utk.card_new_calls == 0);
}

/* BUG: disconnect kfree()d the driver data while an open card still hands
 * that pointer back to ops ->close/->pointer/->trigger (UAF on unplug while
 * recording). The data must be released by the card's dev_free callback at
 * real card destruction. */
static void test_disconnect_dev_outlives_open_card(void)
{
	struct gvusb2_snd *dev;
	int kfree_calls_before;

	utk_reset();
	utk.card_in_use = 1;
	dev = utk_dev();
	usb_set_intfdata(&utk_io_intf, dev);

	gvusb2_snd_disconnect(&utk_io_intf);

	assert(usb_get_intfdata(&utk_io_intf) == NULL);
	assert(utk.kill_calls == GVUSB2_NUM_URBS);
	assert(utk.urb_free_calls == GVUSB2_NUM_URBS);
	assert(utk.kfree_calls == GVUSB2_NUM_URBS);	/* transfer buffers */
	/* the card outlives the driver data because a user holds the fd */
	assert(utk.card_deferred == 1);
	assert(utk.card_free_calls == 0);
	assert(utk.dev_free_calls == 0);

	/* userspace closes the capture fd: the card is released and only then
	 * frees the driver data it owns */
	kfree_calls_before = utk.kfree_calls;
	assert(snd_card_free(utk.last_card) == 0);
	assert(utk.card_free_calls == 1);
	assert(utk.dev_free_calls == 1);
	assert(utk.kfree_calls == kfree_calls_before + 1);	/* dev itself */
}

/* With no open stream, disconnect releases the card synchronously, which
 * runs dev_free and frees the driver data in the same call. */
static void test_disconnect_frees_idle_card(void)
{
	struct gvusb2_snd *dev;

	utk_reset();
	dev = utk_dev();
	usb_set_intfdata(&utk_io_intf, dev);

	gvusb2_snd_disconnect(&utk_io_intf);

	assert(utk.card_deferred == 0);
	assert(utk.card_free_calls == 1);
	assert(utk.dev_free_calls == 1);
}

/* A probe failure after a successful alsa_init releases the card once, and
 * the card's dev_free consumes the driver data exactly once (double-free
 * would crash the kernel instead of failing the probe). */
static void test_probe_unwind_card_owned_dev(void)
{
	int ret;

	utk_reset();
	utk.urb_alloc_fail_at = 1;
	ret = utk_probe(0x84, USB_ENDPOINT_XFER_ISOC, 0x100, 0);
	assert(ret == -ENOMEM);
	assert(utk.card_free_calls == 1);
	assert(utk.dev_free_calls == 1);
}

/* The happy path wires card, PCM, ops and URBs in the order the rest of the
 * driver depends on. */
static void test_probe_registers_card(void)
{
	struct gvusb2_snd *dev;
	int ret;

	utk_reset();
	ret = utk_probe(0x84, USB_ENDPOINT_XFER_ISOC, 0x100, 0);
	assert(ret == 0);

	dev = usb_get_intfdata(&utk_probe_intf);
	assert(dev != NULL);
	assert(dev->intf == &utk_probe_intf);

	assert(utk.gvusb2_init_calls == 1);
	assert(utk.reset_adc_calls == 1);	/* ADC reset before the card */
	assert(utk.set_interface_calls == 1);
	assert(utk.set_interface_ifnum == 2);
	assert(utk.set_interface_alt == 1);
	assert(utk.card_new_calls == 1);
	assert(utk.card_register_calls == 1);
	assert(utk.pcm_capture_count == 1);
	assert(utk.pcm_playback_count == 0);
	assert(utk.urb_alloc_calls == GVUSB2_NUM_URBS);

	assert(strcmp(dev->card->shortname, "gvusb2") == 0);
	assert(strcmp(dev->card->longname, "gvusb2") == 0);
	assert(strcmp(dev->card->driver, "gvusb2-snd") == 0);
	assert(strcmp(dev->pcm->id, "analog in") == 0);
	assert(dev->pcm->private_data == dev);
	assert(dev->pcm->ops[SNDRV_PCM_STREAM_CAPTURE]->open ==
	       gvusb2_snd_capture_open);

	/* a freshly probed device is idle: nothing claims the substream and no
	 * audio flows before the first open */
	assert(dev->substream == NULL);
	assert(utk.submit_calls == 0);
	assert(dev->hw_ptr == 0);
	assert(dev->avail == 0);
}

int main(void)
{
	test_open_installs_constraints();
	test_period_constraint_arithmetic();

	test_second_open_is_busy();
	test_open_unwinds_on_submit_failure();
	test_close_stops_completion();

	test_pointer_advances_by_delivered_frames();
	test_partial_packets_and_stride();
	test_dma_ring_wrap_mid_packet();
	test_dma_ring_exact_boundary();
	test_period_boundary_and_pointer_wrap();
	test_isoc_packet_error_skipped();
	test_isoc_urb_status_filtering();
	test_pointer_multi_lap();
	test_completion_before_hw_params_dropped();

	test_trigger_start_stop();

	test_hw_params_allocates_and_frees();
	test_hw_params_allocation_failure();

	test_allocate_urbs_layout();
	test_allocate_urbs_failure();
	test_probe_endpoint_filter();
	test_probe_registers_card();
	test_disconnect_dev_outlives_open_card();
	test_disconnect_frees_idle_card();
	test_probe_unwind_card_owned_dev();

	printf("test_snd: all tests passed\n");
	return 0;
}
