/* SPDX-License-Identifier: GPL-2.0-only OR BSD-3-Clause */
/* tests/utk has no <media/v4l2-dev.h>; the vid lane's fake layer pulls in the
 * v4l2 types it needs (video_device, v4l2_device) and stubs the entry points
 * gvusb2-vid.c calls. */
#ifndef UTK_VID_MEDIA_V4L2_DEV_H
#define UTK_VID_MEDIA_V4L2_DEV_H

#include "../utk_vid.h"

#endif
