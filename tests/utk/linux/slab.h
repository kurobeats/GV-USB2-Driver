/* SPDX-License-Identifier: GPL-2.0-only OR BSD-3-Clause */
#ifndef UTK_LINUX_SLAB_H
#define UTK_LINUX_SLAB_H

#include <stdlib.h>
#include "../utk.h"

#define GFP_KERNEL 0xcc0

static inline void *kmalloc(size_t n, gfp_t flags)
{
	(void)flags;
	return malloc(n);
}

static inline void kfree(void *p) { free(p); }

#endif