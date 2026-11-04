/* SPDX-License-Identifier: GPL-2.0-only OR BSD-3-Clause */
/*
 * Fake USB core for the gvusb2-snd host tests: the structs and entry points
 * gvusb2-snd.c needs, and nothing else. Included from utk_snd.h, which takes
 * over tests/utk/linux/usb.h's include guard (that one only covers the core
 * lane's control-message traffic).
 *
 * The URBs are ordinary heap objects and no transfer ever happens: the tests
 * play the role of the host controller by filling in iso_frame_desc[].status/
 * actual_length and calling urb->complete() themselves.
 */
#ifndef UTK_SND_USB_H
#define UTK_SND_USB_H

#include <utk.h>

struct usb_device { int unused; };

struct usb_device_id {
	u16 match_flags;
	u16 idVendor;
	u16 idProduct;
};

/* Unbraced, like the kernel's own USB_DEVICE(): callers wrap it in { }. */
#define USB_DEVICE(vend, prod) \
	.match_flags = 1, .idVendor = (vend), .idProduct = (prod)

struct usb_endpoint_descriptor {
	u8 bLength;
	u8 bDescriptorType;
	u8 bEndpointAddress;
	u8 bmAttributes;
	u16 wMaxPacketSize;
	u8 bInterval;
};

struct usb_interface_descriptor {
	u8 bLength;
	u8 bDescriptorType;
	u8 bInterfaceNumber;
	u8 bAlternateSetting;
	u8 bNumEndpoints;
	u8 bInterfaceClass;
	u8 bInterfaceSubClass;
	u8 bInterfaceProtocol;
	u8 iInterface;
};

struct usb_host_endpoint {
	struct usb_endpoint_descriptor desc;
};

/* The kernel's struct usb_host_interface is what intf->altsetting points at. */
struct usb_host_interface {
	struct usb_interface_descriptor desc;
	struct usb_host_endpoint *endpoint;
	char *extra;
	int extralen;
};

struct usb_interface {
	struct usb_host_interface *altsetting;
	unsigned int num_altsetting;
	/* Real kernel reaches the device through dev.parent; a plain field keeps
	 * interface_to_usbdev() honest without faking the driver core. */
	struct usb_device *udev;
	struct device dev;
	void *devdata;
};

#define USB_DIR_IN		0x80
#define USB_ENDPOINT_DIR_MASK	0x80
#define USB_ENDPOINT_XFERTYPE_MASK	0x03
#define USB_ENDPOINT_XFER_ISOC	0x01
#define USB_ENDPOINT_XFER_BULK	0x02
#define USB_CLASS_AUDIO		0x01

static inline int usb_endpoint_dir_in(const struct usb_endpoint_descriptor *epd)
{
	return (epd->bEndpointAddress & USB_ENDPOINT_DIR_MASK) == USB_DIR_IN;
}

static inline int usb_endpoint_xfer_isoc(const struct usb_endpoint_descriptor *epd)
{
	return (epd->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) ==
	       USB_ENDPOINT_XFER_ISOC;
}

struct usb_iso_packet_descriptor {
	unsigned int offset;
	unsigned int length;
	unsigned int actual_length;
	int status;
};

struct urb {
	struct usb_device *dev;
	unsigned int pipe;
	int status;
	void *transfer_buffer;
	u32 transfer_buffer_length;
	void *context;
	unsigned int start_frame;
	unsigned int number_of_packets;
	unsigned int interval;
	unsigned int transfer_flags;
	void (*complete)(struct urb *urb);
	struct usb_iso_packet_descriptor *iso_frame_desc;
};

#define URB_ISO_ASAP	0x0002

static inline unsigned int usb_rcvisocpipe(struct usb_device *udev, int ep)
{
	(void)udev;
	return 0x80 | (unsigned int)ep;
}

static inline struct usb_device *interface_to_usbdev(struct usb_interface *intf)
{
	return intf->udev;
}

static inline void usb_set_intfdata(struct usb_interface *intf, void *data)
{
	intf->devdata = data;
}

static inline void *usb_get_intfdata(struct usb_interface *intf)
{
	return intf->devdata;
}

struct usb_driver {
	const char *name;
	int (*probe)(struct usb_interface *intf, const struct usb_device_id *id);
	void (*disconnect)(struct usb_interface *intf);
	const struct usb_device_id *id_table;
};

struct urb *usb_alloc_urb(int iso_packets, gfp_t mem_flags);
void usb_free_urb(struct urb *urb);
int usb_submit_urb(struct urb *urb, gfp_t mem_flags);
void usb_kill_urb(struct urb *urb);
int usb_set_interface(struct usb_device *dev, int ifnum, int alternate);

#endif /* UTK_SND_USB_H */
