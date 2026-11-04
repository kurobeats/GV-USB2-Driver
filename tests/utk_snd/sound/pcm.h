/* SPDX-License-Identifier: GPL-2.0-only OR BSD-3-Clause */
/*
 * Fake ALSA PCM layer for the gvusb2-snd host tests.
 *
 * Scope: the structs and flags gvusb2-snd.c touches, and bytes_to_frames()/
 * frames_to_bytes() copied verbatim from the kernel's sound/pcm.h (they are
 * plain arithmetic). The kernel's constraint solver and the substream state
 * machine (snd_pcm_hw_params(), attach/detach, xrun handling) are NOT
 * modelled: the tests assert the runtime->hw table the driver advertises at
 * open, not what ALSA would compute from it. See utk_fakes.c for the harness
 * helpers that stand in for attach.
 */
#ifndef UTK_SND_SOUND_PCM_H
#define UTK_SND_SOUND_PCM_H

#include "core.h"

#include <sys/types.h>	/* ssize_t, as in the kernel header's users */

typedef unsigned long snd_pcm_uframes_t;
typedef long snd_pcm_sframes_t;

struct snd_pcm_substream;
struct snd_pcm_runtime;
struct snd_pcm_hw_params;

#define SNDRV_PCM_STREAM_PLAYBACK	0
#define SNDRV_PCM_STREAM_CAPTURE	1

/* values from include/uapi/sound/asound.h and include/sound/pcm.h */
#define SNDRV_PCM_INFO_MMAP		0x00000001
#define SNDRV_PCM_INFO_MMAP_VALID	0x00000002
#define SNDRV_PCM_INFO_INTERLEAVED	0x00000100
#define SNDRV_PCM_INFO_BLOCK_TRANSFER	0x00010000
#define SNDRV_PCM_INFO_RESUME		0x00040000
#define SNDRV_PCM_INFO_PAUSE		0x00080000

#define SNDRV_PCM_RATE_44100		(1U << 6)
#define SNDRV_PCM_RATE_48000		(1U << 7)

/* SNDRV_PCM_FORMAT_S16_LE == 2 */
#define SNDRV_PCM_FMTBIT_S16_LE		(1ULL << 2)

#define SNDRV_PCM_TRIGGER_STOP		0
#define SNDRV_PCM_TRIGGER_START		1
#define SNDRV_PCM_TRIGGER_PAUSE_PUSH	2
#define SNDRV_PCM_TRIGGER_PAUSE_RELEASE	3
#define SNDRV_PCM_TRIGGER_SUSPEND	4
#define SNDRV_PCM_TRIGGER_RESUME	5

struct snd_pcm_hardware {
	unsigned int info;
	u64 formats;
	u32 subformats;
	unsigned int rates;
	unsigned int rate_min;
	unsigned int rate_max;
	unsigned int channels_min;
	unsigned int channels_max;
	size_t buffer_bytes_max;
	size_t period_bytes_min;
	size_t period_bytes_max;
	unsigned int periods_min;
	unsigned int periods_max;
	size_t fifo_size;
};

struct snd_pcm_ops {
	int (*open)(struct snd_pcm_substream *substream);
	int (*close)(struct snd_pcm_substream *substream);
	int (*ioctl)(struct snd_pcm_substream *substream, unsigned int cmd,
		void *arg);
	int (*hw_params)(struct snd_pcm_substream *substream,
		struct snd_pcm_hw_params *params);
	int (*hw_free)(struct snd_pcm_substream *substream);
	int (*prepare)(struct snd_pcm_substream *substream);
	int (*trigger)(struct snd_pcm_substream *substream, int cmd);
	snd_pcm_uframes_t (*pointer)(struct snd_pcm_substream *substream);
	struct page *(*page)(struct snd_pcm_substream *substream,
		unsigned long offset);
};

/* Only the fields gvusb2-snd.c reads or writes; the kernel struct has many
 * more. */
struct snd_pcm_runtime {
	unsigned int rate;
	unsigned int channels;
	snd_pcm_uframes_t period_size;
	unsigned int periods;
	snd_pcm_uframes_t buffer_size;
	unsigned int frame_bits;
	unsigned int sample_bits;
	struct snd_pcm_hardware hw;
	unsigned char *dma_area;
	size_t dma_bytes;
};

struct snd_pcm_hw_params {
	unsigned int buffer_bytes;
};

struct snd_pcm_substream {
	struct snd_pcm *pcm;
	void *private_data;	/* copied from pcm->private_data by ALSA */
	int number;
	int stream;
	const struct snd_pcm_ops *ops;
	struct snd_pcm_runtime *runtime;
};

struct snd_pcm {
	struct snd_card *card;
	char id[64];
	int device;
	void *private_data;
	const struct snd_pcm_ops *ops[2];
};

static inline snd_pcm_sframes_t bytes_to_frames(struct snd_pcm_runtime *runtime,
					       ssize_t size)
{
	return size * 8 / runtime->frame_bits;
}

static inline ssize_t frames_to_bytes(struct snd_pcm_runtime *runtime,
				      snd_pcm_sframes_t size)
{
	return size * runtime->frame_bits / 8;
}

#define params_buffer_bytes(params)	((params)->buffer_bytes)

static inline void *snd_pcm_substream_chip(struct snd_pcm_substream *substream)
{
	return substream->private_data;
}

int snd_pcm_new(struct snd_card *card, const char *id, int device,
		int playback_count, int capture_count, struct snd_pcm **rpcm);
void snd_pcm_set_ops(struct snd_pcm *pcm, int direction,
		const struct snd_pcm_ops *ops);
void snd_pcm_period_elapsed(struct snd_pcm_substream *substream);
int snd_pcm_lib_ioctl(struct snd_pcm_substream *substream, unsigned int cmd,
		void *arg);

#endif /* UTK_SND_SOUND_PCM_H */
