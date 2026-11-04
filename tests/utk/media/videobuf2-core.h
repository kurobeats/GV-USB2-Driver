/* SPDX-License-Identifier: GPL-2.0-only OR BSD-3-Clause */
#ifndef UTK_MEDIA_VIDEOBUF2_CORE_H
#define UTK_MEDIA_VIDEOBUF2_CORE_H

#include <linux/module.h>

struct vb2_queue { int unused; };

enum vb2_buffer_state {
	VB2_BUF_STATE_ERROR = 0,
	VB2_BUF_STATE_QUEUED,
	VB2_BUF_STATE_DONE,
};

#endif