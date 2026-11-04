/* SPDX-License-Identifier: GPL-2.0-only OR BSD-3-Clause */
/*
 * Fake vmalloc for the gvusb2-snd host tests. The DMA ring is plain heap here:
 * the driver only memcpy()s into it and the tests read it back. The call
 * counters let the tests pin that hw_params/hw_free free exactly once.
 */
#ifndef UTK_SND_LINUX_VMALLOC_H
#define UTK_SND_LINUX_VMALLOC_H

#include "../utk_snd.h"

void *vmalloc(unsigned long size);
void vfree(const void *addr);
struct page *vmalloc_to_page(const void *addr);

#endif /* UTK_SND_LINUX_VMALLOC_H */
