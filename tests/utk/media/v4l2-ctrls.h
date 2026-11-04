/* SPDX-License-Identifier: GPL-2.0-only OR BSD-3-Clause */
#ifndef UTK_MEDIA_V4L2_CTRLS_H
#define UTK_MEDIA_V4L2_CTRLS_H

#include <linux/module.h>

struct v4l2_ctrl_handler { int unused; };
struct v4l2_ctrl;
typedef u64 v4l2_std_id;
#define V4L2_CID_USER_BASE 0x00980000

#endif