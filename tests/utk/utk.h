/* SPDX-License-Identifier: GPL-2.0-only OR BSD-3-Clause */
/*
 * Minimal fake kernel primitives for host-side testing of the gvusb2 core/i2c
 * lane (tests/test_core.c, wired in tests/run-tests.sh). Only what
 * gvusb2-core.c/gvusb2-i2c.c/gvusb2-vid.h touch is faked; everything else is
 * out of scope.
 */
#ifndef UTK_H
#define UTK_H

#include <stddef.h>
#include <stdio.h>
#include <errno.h>

typedef unsigned char u8, __u8;
typedef unsigned short u16, __u16;
typedef unsigned int u32, __u32;
typedef unsigned long long u64, __u64;
typedef signed char s8;
typedef short s16;
typedef int s32;
typedef long long s64;
typedef u32 gfp_t;

struct list_head { struct list_head *next, *prev; };
struct mutex { int unused; };
typedef struct { int unused; } spinlock_t;
struct device { void *parent; };

#define pr_err(fmt, ...) fprintf(stderr, "gvusb2: " fmt, ##__VA_ARGS__)
#define dev_err(dev, fmt, ...) fprintf(stderr, "gvusb2: " fmt, ##__VA_ARGS__)
#define dev_dbg(dev, fmt, ...) ((void)(dev))
#define dev_info(dev, fmt, ...) ((void)(dev))

static inline size_t strscpy(char *dst, const char *src, size_t n)
{
	size_t i;

	for (i = 0; i + 1 < n && src[i]; i++)
		dst[i] = src[i];
	if (n)
		dst[i] = '\0';
	return i;
}

#endif /* UTK_H */