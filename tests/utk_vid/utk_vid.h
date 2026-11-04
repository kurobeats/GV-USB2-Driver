/* SPDX-License-Identifier: GPL-2.0-only OR BSD-3-Clause */
/*
 * Lane-private fakes for tests/test_vid.c (the gvusb2-vid.c lane). Only the
 * vid lane writes here.
 *
 * tests/test_vid.c #includes ../gvusb2-vid.c so the static video path is
 * reachable, which means everything that file references must exist before it
 * does. Two of the shared tests/utk headers cannot be reused as-is:
 *
 *  - <linux/usb.h>: tests/utk/linux/usb.h has no struct urb, no endpoint
 *    descriptors and no isochronous entry points, and the lane needs an urb
 *    whose transfer buffers and iso_frame_desc[] the test can inspect.
 *  - <media/videobuf2-v4l2.h>: its struct vb2_v4l2_buffer is an empty shell,
 *    while the video path reads vb2_buf/sequence/field and sets a payload.
 *  - <linux/slab.h>: its kfree() cannot be accounted, and the lane has to
 *    count frees to prove the partial-allocation unwind releases everything.
 *
 * Their include guards are pre-set below so the shared versions are skipped;
 * the fakes replace exactly those two headers and nothing else. Everything the
 * driver uses from core.c/gvusb2-i2c.c/gvusb2-v4l2.c is faked here as well,
 * because run-tests.sh compiles this lane's test file alone.
 *
 * Nothing here sleeps or touches hardware: usleep_range() only records its
 * argument, so the driver's 10-20 ms decoder settle costs nothing on the host.
 */
#ifndef UTK_VID_H
#define UTK_VID_H

#define UTK_LINUX_USB_H			/* replaced by the fake USB layer below */
#define UTK_MEDIA_VIDEOBUF2_V4L2_H	/* replaced by the fake vb2 types below */
#define UTK_LINUX_SLAB_H		/* replaced by the counting allocator below */

/* The fake struct usb_interface keeps the kernel's `unsigned num_altsetting`,
 * and probe compares it against an int loop counter. The kernel builds without
 * -Wextra so that mix is silent there; this host build has -Werror. */
#pragma GCC diagnostic ignored "-Wsign-compare"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "../utk/utk.h"
#include <linux/i2c.h>
#include <media/videobuf2-core.h>	/* enum vb2_buffer_state, struct vb2_queue */
#include <media/v4l2-device.h>
#include <media/v4l2-ctrls.h>

#define UTKVID_MAX_TRACE 512

/* ------------------------------------------------------------------ */
/* fake <linux/usb.h>                                                  */
/* ------------------------------------------------------------------ */

struct usb_device { int unused; };

struct usb_endpoint_descriptor {
	u8 bLength;
	u8 bDescriptorType;
	u8 bEndpointAddress;
	u8 bmAttributes;
	u16 wMaxPacketSize;
	u8 bInterval;
};

struct usb_host_endpoint {
	struct usb_endpoint_descriptor desc;
};

struct usb_interface_descriptor {
	u8 bNumEndpoints;
};

struct usb_host_interface {
	struct usb_interface_descriptor desc;
	struct usb_host_endpoint *endpoint;
};

struct usb_interface {
	struct device dev;
	unsigned int num_altsetting;
	struct usb_host_interface *altsetting;
	void *intfdata;
};

struct usb_device_id {
	u16 match_flags;
	u16 idVendor;
	u16 idProduct;
};

#define USB_DEVICE(vend, prod) \
	.match_flags = 0x0001 | 0x0002, .idVendor = (vend), .idProduct = (prod)

struct usb_driver {
	const char *name;
	int (*probe)(struct usb_interface *intf, const struct usb_device_id *id);
	void (*disconnect)(struct usb_interface *intf);
	const struct usb_device_id *id_table;
};

struct usb_iso_packet_descriptor {
	unsigned int offset;
	unsigned int length;
	unsigned int actual_length;
	int status;
};

struct urb {
	struct usb_device *dev;
	struct usb_host_endpoint *ep;
	unsigned int pipe;
	int status;
	unsigned int transfer_flags;
	void *transfer_buffer;
	u32 transfer_buffer_length;
	int start_frame;
	int number_of_packets;
	int interval;
	void *context;
	void (*complete)(struct urb *urb);
	struct usb_iso_packet_descriptor iso_frame_desc[];
};

#define URB_ISO_ASAP		0x0002
#define USB_ENDPOINT_XFER_ISOC	0x01

#define usb_endpoint_dir_in(e)		(((e)->bEndpointAddress & 0x80) != 0)
#define usb_endpoint_xfer_isoc(e) \
	(((e)->bmAttributes & 0x03) == USB_ENDPOINT_XFER_ISOC)

#define MODULE_DESCRIPTION(x)
#define MODULE_AUTHOR(x)
#define MODULE_LICENSE(x)
#define MODULE_DEVICE_TABLE(type, name)

/* Registering the driver with the fake USB core only pins its address, so a
 * test can check that probe/disconnect/id_table are actually wired up. */
#define module_usb_driver(drv) \
	struct usb_driver *const utkvid_usb_driver = &(drv)

/* ------------------------------------------------------------------ */
/* fake <media/videobuf2-v4l2.h>                                       */
/* ------------------------------------------------------------------ */

struct vb2_plane {
	unsigned int length;	/* negotiated plane size */
	unsigned int bytesused;	/* what vb2_set_plane_payload() reported */
	void *vaddr;		/* where vb2_plane_vaddr() points */
};

struct vb2_buffer {
	struct vb2_plane planes[1];
	unsigned long long timestamp;
};

struct vb2_v4l2_buffer {
	struct vb2_buffer vb2_buf;
	unsigned int sequence;
	unsigned int field;
};

#define V4L2_FIELD_INTERLACED	4

/* ------------------------------------------------------------------ */
/* kernel primitives utk.h does not cover                              */
/* ------------------------------------------------------------------ */

#define container_of(ptr, type, member) \
	((type *)(void *)((char *)(ptr) - offsetof(type, member)))
#define ARRAY_SIZE(a)		(sizeof(a) / sizeof((a)[0]))

static inline void utkvid_list_init(struct list_head *h)
{
	h->next = h;
	h->prev = h;
}
#define INIT_LIST_HEAD(h)	utkvid_list_init(h)

static inline int utkvid_list_empty(const struct list_head *h)
{
	return h->next == h;
}
#define list_empty(h)		utkvid_list_empty(h)

#define list_first_entry(head, type, member) \
	container_of((head)->next, type, member)

static inline void list_add_tail(struct list_head *n, struct list_head *h)
{
	n->prev = h->prev;
	n->next = h;
	h->prev->next = n;
	h->prev = n;
}

static inline void list_del(struct list_head *e)
{
	e->prev->next = e->next;
	e->next->prev = e->prev;
}

#define spin_lock_irqsave(lock, flags) \
	do { (void)(lock); (flags) = 0; } while (0)
#define spin_unlock_irqrestore(lock, flags) \
	do { (void)(lock); (void)(flags); } while (0)

#define mutex_init(m)		((void)(m))
#define mutex_lock(m)		((void)(m))
#define mutex_unlock(m)		((void)(m))
#define mutex_destroy(m)	((void)(m))

#define USEC_PER_MSEC		1000

/* fake <linux/slab.h> */
#define GFP_KERNEL		0xcc0
#define GFP_ATOMIC		0x20

/* ------------------------------------------------------------------ */
/* fake state and helpers                                              */
/* ------------------------------------------------------------------ */

struct utkvid_fake {
	/* fake chip register file, enough for every register gvusb2-vid.c
	 * writes: the STK1150 tables live below 0x0400. */
	u8 regs[0x1000];
	unsigned int writes;
	u16 wreg[UTKVID_MAX_TRACE];
	u8 wval[UTKVID_MAX_TRACE];
	int reg_io_err;

	/* allocation accounting */
	unsigned int urb_alloc_calls;
	unsigned int urb_alloc_fail_at;	/* 1-based call that returns NULL */
	unsigned int urb_free_calls;
	struct urb *urb_freed[UTKVID_MAX_TRACE];
	unsigned int kzalloc_calls;
	unsigned int kzalloc_fail_at;
	size_t kzalloc_size[UTKVID_MAX_TRACE];
	void *kzalloc_ptr[UTKVID_MAX_TRACE];
	unsigned int kfree_calls;
	void *kfree_ptr[UTKVID_MAX_TRACE];

	/* urb submission */
	unsigned int submit_calls;
	struct urb *submitted[UTKVID_MAX_TRACE];
	unsigned int submit_fail_at;
	int submit_errno;
	unsigned int kill_calls;
	struct urb *killed[UTKVID_MAX_TRACE];
	unsigned int kills_at_first_free;
	unsigned int set_interface_calls;
	unsigned int set_interface_alt;
	int set_interface_err;

	/* video bookkeeping */
	unsigned int done_calls;
	struct vb2_buffer *done_buf[UTKVID_MAX_TRACE];
	enum vb2_buffer_state done_state[UTKVID_MAX_TRACE];
	unsigned int warn_count;
	unsigned int usleep_calls;
	unsigned int usleep_min;
	unsigned int usleep_max;
	int width;		/* what get_resolution() reports */
	unsigned long long ns;

	/* cross-lane entry points */
	unsigned int gv_init_calls;
	unsigned int gv_free_calls;
	int gv_init_err;
	unsigned int i2c_register_calls;
	unsigned int i2c_unregister_calls;
	unsigned int vb2_setup_calls;
	unsigned int v4l2_register_calls;
	unsigned int v4l2_unregister_calls;
	unsigned int video_register_calls;
	unsigned int clear_queue_calls;
	unsigned int clear_queue_state;
	unsigned int ctrl_handler_free_calls;
	unsigned int v4l2_device_unregister_calls;
	unsigned int v4l2_device_disconnect_calls;
	unsigned int v4l2_device_put_calls;
	int i2c_register_err;
	int vb2_setup_err;
	int v4l2_register_err;
	int video_register_err;

	struct usb_device udev;
};

static struct utkvid_fake utkvid;

#define dev_warn_ratelimited(dev, fmt, ...) \
	((void)(dev), utkvid.warn_count++)

static inline int utkvid_warn_on(int cond)
{
	if (cond)
		utkvid.warn_count++;
	return cond;
}
#define WARN_ON(cond)		utkvid_warn_on(!!(cond))

static inline unsigned long long ktime_get_ns(void)
{
	return utkvid.ns++;
}

static inline void usleep_range(unsigned int min, unsigned int max)
{
	utkvid.usleep_calls++;
	utkvid.usleep_min = min;
	utkvid.usleep_max = max;
}

static inline void *kzalloc(size_t size, gfp_t flags)
{
	unsigned int n = utkvid.kzalloc_calls++;
	void *p;

	(void)flags;
	if (utkvid.kzalloc_fail_at == n + 1)
		p = NULL;
	else
		p = calloc(1, size);
	if (n < UTKVID_MAX_TRACE) {
		utkvid.kzalloc_size[n] = size;
		utkvid.kzalloc_ptr[n] = p;
	}
	return p;
}

static inline void kfree(void *p)
{
	if (utkvid.kfree_calls < UTKVID_MAX_TRACE)
		utkvid.kfree_ptr[utkvid.kfree_calls] = p;
	utkvid.kfree_calls++;
	free(p);
}

static inline struct urb *usb_alloc_urb(int iso_packets, gfp_t flags)
{
	unsigned int n = utkvid.urb_alloc_calls++;
	struct urb *urb;

	(void)flags;
	if (utkvid.urb_alloc_fail_at == n + 1)
		return NULL;
	urb = calloc(1, sizeof(*urb) +
		(size_t)iso_packets * sizeof(urb->iso_frame_desc[0]));
	return urb;
}

static inline void usb_free_urb(struct urb *urb)
{
	if (utkvid.urb_free_calls == 0)
		utkvid.kills_at_first_free = utkvid.kill_calls;
	if (utkvid.urb_free_calls < UTKVID_MAX_TRACE)
		utkvid.urb_freed[utkvid.urb_free_calls] = urb;
	utkvid.urb_free_calls++;
	free(urb);
}

static inline int usb_submit_urb(struct urb *urb, gfp_t flags)
{
	unsigned int n = utkvid.submit_calls++;

	(void)flags;
	if (n < UTKVID_MAX_TRACE)
		utkvid.submitted[n] = urb;
	if (utkvid.submit_fail_at == n + 1)
		return utkvid.submit_errno ? utkvid.submit_errno : -EPIPE;
	return 0;
}

static inline void usb_kill_urb(struct urb *urb)
{
	if (utkvid.kill_calls < UTKVID_MAX_TRACE)
		utkvid.killed[utkvid.kill_calls] = urb;
	utkvid.kill_calls++;
}

/* The endpoint number is encoded in the pipe so a test can see which one the
 * driver asked for; bit 0x8000 marks a receive pipe. */
static inline unsigned int usb_rcvisocpipe(struct usb_device *udev, int ep)
{
	(void)udev;
	return 0x8000u | (unsigned int)ep;
}

static inline int usb_set_interface(struct usb_device *udev, int ifnum, int alt)
{
	(void)udev;
	(void)ifnum;
	utkvid.set_interface_calls++;
	utkvid.set_interface_alt = (unsigned int)alt;
	return utkvid.set_interface_err;
}

static inline struct usb_device *interface_to_usbdev(struct usb_interface *intf)
{
	(void)intf;
	return &utkvid.udev;
}

static inline void usb_set_intfdata(struct usb_interface *intf, void *data)
{
	intf->intfdata = data;
}

static inline void *usb_get_intfdata(struct usb_interface *intf)
{
	return intf->intfdata;
}

static inline void *vb2_plane_vaddr(struct vb2_buffer *vb, unsigned int i)
{
	return vb->planes[i].vaddr;
}

static inline void vb2_set_plane_payload(struct vb2_buffer *vb, unsigned int i,
	unsigned long len)
{
	vb->planes[i].bytesused = (unsigned int)len;
}

static inline void vb2_buffer_done(struct vb2_buffer *vb,
	enum vb2_buffer_state state)
{
	unsigned int n = utkvid.done_calls++;

	if (n < UTKVID_MAX_TRACE) {
		utkvid.done_buf[n] = vb;
		utkvid.done_state[n] = state;
	}
}

/* ------------------------------------------------------------------ */
/* the driver's own headers, then the entry points other lanes own     */
/* ------------------------------------------------------------------ */

#include "../../gvusb2.h"
#include "../../gvusb2-vid.h"

static inline void video_unregister_device(struct video_device *vdev)
{
	(void)vdev;
}

static inline void v4l2_device_disconnect(struct v4l2_device *v4l2_dev)
{
	(void)v4l2_dev;
	utkvid.v4l2_device_disconnect_calls++;
}

static inline void v4l2_device_put(struct v4l2_device *v4l2_dev)
{
	(void)v4l2_dev;
	utkvid.v4l2_device_put_calls++;
}

static inline void v4l2_device_unregister(struct v4l2_device *v4l2_dev)
{
	(void)v4l2_dev;
	utkvid.v4l2_device_unregister_calls++;
}

static inline void v4l2_ctrl_handler_free(struct v4l2_ctrl_handler *h)
{
	(void)h;
	utkvid.ctrl_handler_free_calls++;
}

int gvusb2_read_reg(struct gvusb2_dev *dev, u16 reg, u8 *value)
{
	(void)dev;
	if (utkvid.reg_io_err)
		return utkvid.reg_io_err;
	if (reg >= ARRAY_SIZE(utkvid.regs))
		return -EIO;
	*value = utkvid.regs[reg];
	return 0;
}

int gvusb2_write_reg(struct gvusb2_dev *dev, u16 reg, u8 value)
{
	(void)dev;
	if (utkvid.reg_io_err)
		return utkvid.reg_io_err;
	if (reg >= ARRAY_SIZE(utkvid.regs))
		return -EIO;
	utkvid.regs[reg] = value;
	if (utkvid.writes < UTKVID_MAX_TRACE) {
		utkvid.wreg[utkvid.writes] = reg;
		utkvid.wval[utkvid.writes] = value;
	}
	utkvid.writes++;
	return 0;
}

int gvusb2_set_reg_mask(struct gvusb2_dev *dev, u16 reg, u8 mask, u8 value)
{
	u8 v = 0;
	int ret;

	ret = gvusb2_read_reg(dev, reg, &v);
	if (ret < 0)
		return ret;
	return gvusb2_write_reg(dev, reg, (v & ~mask) | value);
}

int gvusb2_init(struct gvusb2_dev *dev, struct usb_device *udev)
{
	utkvid.gv_init_calls++;
	dev->udev = udev;
	return utkvid.gv_init_err;
}

int gvusb2_free(struct gvusb2_dev *dev)
{
	(void)dev;
	utkvid.gv_free_calls++;
	return 0;
}

int gvusb2_i2c_register(struct gvusb2_vid *dev)
{
	(void)dev;
	utkvid.i2c_register_calls++;
	return utkvid.i2c_register_err;
}

int gvusb2_i2c_unregister(struct gvusb2_vid *dev)
{
	(void)dev;
	utkvid.i2c_unregister_calls++;
	return 0;
}

int gvusb2_vb2_setup(struct gvusb2_vid *dev)
{
	(void)dev;
	utkvid.vb2_setup_calls++;
	return utkvid.vb2_setup_err;
}

int gvusb2_v4l2_register(struct gvusb2_vid *dev)
{
	(void)dev;
	utkvid.v4l2_register_calls++;
	return utkvid.v4l2_register_err;
}

void gvusb2_v4l2_unregister(struct gvusb2_vid *dev)
{
	(void)dev;
	utkvid.v4l2_unregister_calls++;
}

int gvusb2_video_register(struct gvusb2_vid *dev)
{
	(void)dev;
	utkvid.video_register_calls++;
	return utkvid.video_register_err;
}

void gvusb2_vid_clear_queue(struct gvusb2_vid *dev,
	enum vb2_buffer_state state)
{
	(void)dev;
	utkvid.clear_queue_calls++;
	utkvid.clear_queue_state = (unsigned int)state;
}

/* Owned by gvusb2-v4l2.c: the width is a fixed 720 for every standard, which
 * is the only value the video path uses (bytes per line = width * 2). */
void get_resolution(struct gvusb2_vid *dev, int *width, int *height)
{
	(void)dev;
	if (width != NULL)
		*width = utkvid.width;
	if (height != NULL)
		*height = 0;
}

#endif /* UTK_VID_H */
