/* SPDX-License-Identifier: GPL-2.0-only OR BSD-3-Clause */
/*
 * Fake ALSA card core for the gvusb2-snd host tests: just the card/device
 * lifetime calls gvusb2_snd_alsa_init() and gvusb2_snd_alsa_free() make, plus
 * the card fields the driver fills in.
 *
 * What is NOT modelled here (and so is not tested): the card/control/stream
 * registration machinery. snd_card_register() only counts a call; nothing
 * becomes visible to userspace.
 */
#ifndef UTK_SND_SOUND_CORE_H
#define UTK_SND_SOUND_CORE_H

#include "../utk_snd.h"

struct snd_card;
struct snd_device;

struct snd_device_ops {
	int (*dev_free)(struct snd_device *dev);
};

struct snd_device {
	struct snd_card *card;
	int type;
	void *device_data;
	const struct snd_device_ops *ops;
};

struct snd_card {
	int number;
	char id[16];
	char driver[16];
	char shortname[32];
	char longname[80];
	char mixername[80];
	char components[128];
	struct module *module;
	void *private_data;
};

#define SNDRV_DEV_LOWLEVEL	0x02

int snd_card_new(struct device *parent, int idx, const char *xid,
		struct module *module, int extra_size, struct snd_card **card_ret);
int snd_card_free(struct snd_card *card);
int snd_card_free_when_closed(struct snd_card *card);
int snd_card_register(struct snd_card *card);
int snd_device_new(struct snd_card *card, int type, void *device_data,
		const struct snd_device_ops *ops);

#endif /* UTK_SND_SOUND_CORE_H */
