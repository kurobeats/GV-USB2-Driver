/* SPDX-License-Identifier: GPL-2.0-only OR BSD-3-Clause */
/*
 * Kernel primitives for the gvusb2-snd host tests (tests/test_snd.c).
 *
 * tests/utk/ carries the core lane's fakes and comes first on the include
 * path. The snd lane needs a wider USB and slab surface, so it takes over
 * those two utk/ guards and supplies its own supersets here; utk.h itself
 * (types, struct device, dev_info) is shared unchanged.
 *
 * The spinlock fakes are no-ops: these tests are single-threaded, so they
 * exercise the code around the lock, not the locking itself.
 */
#ifndef UTK_SND_H
#define UTK_SND_H

#define UTK_LINUX_USB_H		/* utk_usb.h supersedes utk/linux/usb.h */
#define UTK_LINUX_SLAB_H	/* kzalloc/calloc supersede utk/linux/slab.h */

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <utk.h>
#include <linux/module.h>

#include "utk_usb.h"

#define GFP_KERNEL	0xcc0
#define GFP_ATOMIC	0x20

static inline void *kmalloc(size_t size, gfp_t flags)
{
	(void)flags;
	return malloc(size);
}

static inline void *kzalloc(size_t size, gfp_t flags)
{
	(void)flags;
	return calloc(1, size);
}

static inline void kfree(const void *ptr)
{
	free((void *)ptr);
}

struct page { int unused; };

static inline void spin_lock_init(spinlock_t *lock)
{
	(void)lock;
}

/* The kernel saves/restores the interrupt state in `flags`. Nothing can
 * interrupt anything here, so the flag is just cleared: macros rather than
 * inline functions because the driver passes `flags` by value and an unused
 * out-parameter would trip -Wuninitialized. */
#define spin_lock_irqsave(lock, flags) \
	do { (void)(lock); (flags) = 0; } while (0)
#define spin_unlock_irqrestore(lock, flags) \
	do { (void)(lock); (void)(flags); } while (0)

#define MODULE_DESCRIPTION(desc)
#define MODULE_AUTHOR(author)
#define MODULE_LICENSE(license)
#define MODULE_PARM_DESC(name, desc)
#define MODULE_DEVICE_TABLE(type, table)

/* The module parameters themselves are not what these tests pin; the fake
 * just keeps the driver's static variables referenced so -Wunused stays
 * quiet. */
#define module_param_named(name, var, type, perm) \
	void *utk_param_##name = &(var)
#define module_param(name, type, perm) void *utk_modparam_##name = &(name)

#define module_usb_driver(driver) void *utk_usb_driver = &(driver)

#endif /* UTK_SND_H */
