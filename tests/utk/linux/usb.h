/* SPDX-License-Identifier: GPL-2.0-only OR BSD-3-Clause */
#ifndef UTK_LINUX_USB_H
#define UTK_LINUX_USB_H

#include "../utk.h"

struct usb_device { int unused; };
struct usb_interface { struct device dev; };
struct usb_device_id { int unused; };

#define USB_DIR_IN      0x80
#define USB_DIR_OUT     0x00
#define USB_TYPE_VENDOR 0x40
#define USB_RECIP_DEVICE 0x00

#define HZ 1000

static inline unsigned int usb_rcvctrlpipe(struct usb_device *udev, int ep)
{
	(void)udev; (void)ep;
	return 0;
}

static inline unsigned int usb_sndctrlpipe(struct usb_device *udev, int ep)
{
	(void)udev; (void)ep;
	return 0;
}

int usb_control_msg(struct usb_device *dev, unsigned int pipe, u8 request,
	u8 requesttype, u16 value, u16 index, void *data, u16 size, int timeout);

#endif