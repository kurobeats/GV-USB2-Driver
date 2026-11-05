/* SPDX-License-Identifier: GPL-2.0-only OR BSD-3-Clause */
/*
 * Implementation of the fake kernel/USB/ALSA entry points gvusb2-snd.c calls,
 * plus the few harness helpers tests/test_snd.c drives the driver with.
 *
 * This is a .c file #included by tests/test_snd.c (one translation unit, like
 * the driver itself) so the counters live in one place and stay visible to the
 * assertions. tests/run-tests.sh compiles only test_snd.c.
 */
#include <assert.h>

#include "utk_snd.h"
#include "sound/pcm.h"
#include "linux/vmalloc.h"

#include "../../gvusb2.h"

struct utk_snd_state {
	/* URBs: the tests are the host controller, so submits only count. */
	int urb_alloc_calls;
	int urb_free_calls;
	int urb_alloc_fail_at;	/* fail usb_alloc_urb() from this call on */
	int submit_calls;
	int kill_calls;
	int submit_fail;	/* make every usb_submit_urb() fail */
	struct urb *last_submitted;
	gfp_t last_submit_flags;

	/* ALSA card/PCM plumbing. */
	int card_new_calls;
	int card_free_calls;
	int card_register_calls;
	int pcm_new_calls;
	int pcm_playback_count;
	int pcm_capture_count;
	int period_elapsed_calls;
	struct snd_pcm_substream *period_substream;

	/* Transfer buffers: freed by kfree() in gvusb2_snd_free_isoc(). */
	int kfree_calls;

	/* usb_set_interface recording. */
	unsigned int set_interface_calls;
	unsigned int set_interface_ifnum;
	unsigned int set_interface_alt;

	/* DMA ring allocation. */
	int vmalloc_calls;
	int vfree_calls;
	unsigned long last_vmalloc_size;
	int vmalloc_fail;

	/* gvusb2 core stubs (gvusb2-core.c is not compiled in this lane). */
	int gvusb2_init_calls;
	int gvusb2_free_calls;
	int gvusb2_init_fail;
	int reset_adc_calls;
};

static struct utk_snd_state utk;

/* Canary bytes kept past the end of every fake DMA ring. */
#define UTK_RING_PAD 16
#define UTK_RING_CANARY 0xa5

static void utk_reset(void)
{
	memset(&utk, 0, sizeof(utk));
}

/*****************************************************************************
 * USB
 ****************************************************************************/

struct urb *usb_alloc_urb(int iso_packets, gfp_t mem_flags)
{
	struct urb *urb;

	(void)mem_flags;
	utk.urb_alloc_calls++;
	if (utk.urb_alloc_fail_at && utk.urb_alloc_calls >= utk.urb_alloc_fail_at)
		return NULL;

	urb = calloc(1, sizeof(*urb));
	if (urb == NULL)
		return NULL;
	urb->iso_frame_desc = calloc(iso_packets ? iso_packets : 1,
				     sizeof(*urb->iso_frame_desc));
	if (urb->iso_frame_desc == NULL) {
		free(urb);
		return NULL;
	}
	return urb;
}

void usb_free_urb(struct urb *urb)
{
	if (urb == NULL)
		return;
	utk.urb_free_calls++;
	free(urb->iso_frame_desc);
	free(urb);
}

int usb_submit_urb(struct urb *urb, gfp_t mem_flags)
{
	utk.submit_calls++;
	utk.last_submitted = urb;
	utk.last_submit_flags = mem_flags;

	/* -ENODEV is what a disconnected or unconfigured device returns. */
	return utk.submit_fail ? -ENODEV : 0;
}

void usb_kill_urb(struct urb *urb)
{
	(void)urb;
	utk.kill_calls++;
}

int usb_set_interface(struct usb_device *dev, int ifnum, int alternate)
{
	(void)dev;
	utk.set_interface_calls++;
	utk.set_interface_ifnum = (unsigned int)ifnum;
	utk.set_interface_alt = (unsigned int)alternate;
	return 0;
}

/*****************************************************************************
 * gvusb2 core (real one lives in gvusb2-core.c, which this lane does not
 * compile: the snd lane only needs the interface the probe path calls)
 ****************************************************************************/

int gvusb2_init(struct gvusb2_dev *dev, struct usb_device *udev)
{
	utk.gvusb2_init_calls++;
	dev->udev = udev;
	return utk.gvusb2_init_fail ? -ENODEV : 0;
}

int gvusb2_free(struct gvusb2_dev *dev)
{
	(void)dev;
	utk.gvusb2_free_calls++;
	return 0;
}

int gvusb2_snd_reset_adc(struct gvusb2_dev *dev)
{
	(void)dev;
	utk.reset_adc_calls++;
	return 0;
}

/*****************************************************************************
 * vmalloc / slab
 ****************************************************************************/

void *vmalloc(unsigned long size)
{
	utk.vmalloc_calls++;
	utk.last_vmalloc_size = size;
	if (utk.vmalloc_fail)
		return NULL;
	return calloc(1, size);
}

void vfree(const void *addr)
{
	utk.vfree_calls++;
	free((void *)addr);
}

/* gvusb2_snd_free_isoc() frees each urb's transfer buffer exactly once. */
void kfree(const void *ptr)
{
	utk.kfree_calls++;
	free((void *)ptr);
}

struct page *vmalloc_to_page(const void *addr)
{
	static struct page page;

	(void)addr;
	return &page;
}

/*****************************************************************************
 * ALSA
 ****************************************************************************/

int snd_card_new(struct device *parent, int idx, const char *xid,
		struct module *module, int extra_size, struct snd_card **card_ret)
{
	struct snd_card *card;

	(void)parent;
	(void)idx;
	(void)xid;
	(void)module;
	utk.card_new_calls++;

	card = calloc(1, sizeof(*card) + extra_size);
	if (card == NULL)
		return -ENOMEM;
	*card_ret = card;
	return 0;
}

int snd_card_free(struct snd_card *card)
{
	utk.card_free_calls++;
	free(card);
	return 0;
}

/* The kernel defers until the last file is closed; the fake releases at once
 * because no test keeps a card file open. */
int snd_card_free_when_closed(struct snd_card *card)
{
	return snd_card_free(card);
}

int snd_card_register(struct snd_card *card)
{
	(void)card;
	utk.card_register_calls++;
	return 0;
}

int snd_device_new(struct snd_card *card, int type, void *device_data,
		const struct snd_device_ops *ops)
{
	(void)card;
	(void)type;
	(void)device_data;
	(void)ops;
	return 0;
}

int snd_pcm_new(struct snd_card *card, const char *id, int device,
		int playback_count, int capture_count, struct snd_pcm **rpcm)
{
	struct snd_pcm *pcm;

	utk.pcm_new_calls++;
	utk.pcm_playback_count = playback_count;
	utk.pcm_capture_count = capture_count;

	pcm = calloc(1, sizeof(*pcm));
	if (pcm == NULL)
		return -ENOMEM;
	pcm->card = card;
	pcm->device = device;
	strncpy(pcm->id, id, sizeof(pcm->id) - 1);
	*rpcm = pcm;
	return 0;
}

void snd_pcm_set_ops(struct snd_pcm *pcm, int direction,
		const struct snd_pcm_ops *ops)
{
	pcm->ops[direction] = ops;
}

void snd_pcm_period_elapsed(struct snd_pcm_substream *substream)
{
	utk.period_elapsed_calls++;
	utk.period_substream = substream;
}

int snd_pcm_lib_ioctl(struct snd_pcm_substream *substream, unsigned int cmd,
		void *arg)
{
	(void)substream;
	(void)cmd;
	(void)arg;
	return 0;
}

/*****************************************************************************
 * Harness helpers
 ****************************************************************************/

/* An ALSA runtime for a 48000 Hz S16_LE stereo stream, with a DMA ring of
 * dma_bytes and canary bytes past its end. frame_bits is what the kernel's
 * bytes_to_frames() divides by: 2 channels * 16 bits. */
static struct snd_pcm_runtime *utk_runtime(unsigned int buffer_frames,
					   unsigned int period_frames,
					   unsigned int dma_bytes)
{
	struct snd_pcm_runtime *runtime = calloc(1, sizeof(*runtime));

	assert(runtime != NULL);
	runtime->rate = 48000;
	runtime->channels = 2;
	runtime->sample_bits = 16;
	runtime->frame_bits = 32;
	runtime->buffer_size = buffer_frames;
	runtime->period_size = period_frames;
	runtime->dma_bytes = dma_bytes;
	runtime->dma_area = calloc(1, dma_bytes + UTK_RING_PAD);
	assert(runtime->dma_area != NULL);
	memset(runtime->dma_area + dma_bytes, UTK_RING_CANARY, UTK_RING_PAD);

	return runtime;
}

/* Stands in for snd_pcm_attach_substream() + snd_pcm_open(): ALSA hands the
 * driver's pcm->private_data to the substream, installs the ops registered
 * with snd_pcm_set_ops() and a fresh runtime, then calls ->open. */
static int utk_capture_open(struct gvusb2_snd *dev,
			    struct snd_pcm_substream *sub,
			    struct snd_pcm_runtime *runtime)
{
	memset(sub, 0, sizeof(*sub));
	sub->stream = SNDRV_PCM_STREAM_CAPTURE;
	sub->pcm = dev->pcm;
	sub->private_data = dev->pcm->private_data;
	sub->ops = dev->pcm->ops[SNDRV_PCM_STREAM_CAPTURE];
	sub->runtime = runtime;

	return sub->ops->open(sub);
}
