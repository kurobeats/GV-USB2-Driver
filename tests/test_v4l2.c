/* Host-side tests for the v4l2 lane: format negotiation, standard handling,
 * the TW9910 crop write sequence and the black-level read-modify-write in
 * gvusb2-v4l2.c. No hardware, no module load, no root.
 *
 * The driver source is compiled into this translation unit (it is #include'd
 * below) so the static ioctl handlers and gvusb2_s_ctrl are reachable. The
 * kernel media stack it needs is faked in utk_v4l2/v4l2-fakes.h; the V4L2
 * constants come from the real UAPI header so the boundaries probed here are
 * the ABI's, not guesses.
 *
 * Fakes record an ordered event log: every i2c byte read/write, every
 * gvusb2_write_reg, every subdev s_stream call and every vb2_buffer_done. The
 * tests assert on that log, so what is pinned is the sequence the chips
 * actually see.
 */

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>

#include <v4l2-fakes.h>
#include <utk.h>

/* utk.h's strscpy takes char*, but every call site in this driver passes a
 * u8[...] (cap->driver, i->name). Bridge the signedness so the shared fake
 * still compiles under -Wall. */
static inline size_t utk_strscpy_u(unsigned char *dst, const char *src,
	size_t n)
{
	return strscpy((char *)dst, src, n);
}
#define strscpy(dst, src, n) utk_strscpy_u((unsigned char *)(dst), (src), (n))

#include "../gvusb2.h"
#include "../gvusb2-vid.h"
#include "../tw9910-regplan.h"

#include "../gvusb2-v4l2.c"

/*****************************************************************************
 * Fake kernel state and event log
 ****************************************************************************/

#define UTK_ANY INT_MIN
#define UTK_MAX_EV 512

enum utk_ev_kind {
	EV_I2C_READ,
	EV_I2C_WRITE,
	EV_REG_WRITE,
	EV_REG_MASK,
	EV_SSTREAM,
	EV_VB2_DONE,
	EV_URBS,		/* a: 1 = submit, 0 = cancel */
};

struct utk_ev {
	enum utk_ev_kind kind;
	int a;
	int b;
	int mask;	/* EV_REG_MASK only: the read-modify-write mask */
};

static struct utk_ev utk_ev[UTK_MAX_EV];
static int utk_nev;

/* TW9910 register file, as seen through the fake i2c bus. */
static u8 utk_chip[256];
static int utk_fail_i2c_read;
static int utk_fail_i2c_write;
static int utk_submit_ret;
static int utk_submit_calls;
static int utk_cancel_calls;
int utk_mutex_locks;
int utk_mutex_unlocks;
static int utk_mutex_fail;
static char utk_warn[160];
static struct v4l2_subdev utk_subdev;

static void utk_log(enum utk_ev_kind kind, int a, int b)
{
	if (utk_nev < UTK_MAX_EV) {
		utk_ev[utk_nev].kind = kind;
		utk_ev[utk_nev].a = a;
		utk_ev[utk_nev].b = b;
	}
	utk_nev++;
}

/* Index of the first event at or after `from` matching the pattern, or -1. */
static int utk_find(int from, enum utk_ev_kind kind, int a, int b)
{
	int i;

	for (i = from; i < utk_nev && i < UTK_MAX_EV; i++)
		if (utk_ev[i].kind == kind &&
		    (a == UTK_ANY || utk_ev[i].a == a) &&
		    (b == UTK_ANY || utk_ev[i].b == b))
			return i;
	return -1;
}

static int utk_count(enum utk_ev_kind kind, int a, int b)
{
	int i, n = 0;

	for (i = utk_find(0, kind, a, b); i >= 0;
	     i = utk_find(i + 1, kind, a, b))
		n++;
	return n;
}

/* The i2c traffic on its own, in order: enough to pin which registers the
 * TW9910 receives. */
struct utk_i2c_ev {
	int read;
	int reg;
	int val;
};

static int utk_i2c_events(struct utk_i2c_ev *out, int max)
{
	int i, n = 0;

	for (i = 0; i < utk_nev && i < UTK_MAX_EV; i++) {
		if (utk_ev[i].kind != EV_I2C_READ &&
		    utk_ev[i].kind != EV_I2C_WRITE)
			continue;
		if (n < max) {
			out[n].read = utk_ev[i].kind == EV_I2C_READ;
			out[n].reg = utk_ev[i].a;
			out[n].val = utk_ev[i].b;
		}
		n++;
	}
	return n;
}

/* Last value written to `reg`, or -1. */
static int utk_i2c_written(int reg)
{
	int i, v = -1;

	for (i = 0; i < utk_nev && i < UTK_MAX_EV; i++)
		if (utk_ev[i].kind == EV_I2C_WRITE && utk_ev[i].a == reg)
			v = utk_ev[i].b;
	return v;
}

/*****************************************************************************
 * Fake kernel entry points
 ****************************************************************************/

void utk_dev_warn(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(utk_warn, sizeof(utk_warn), fmt, ap);
	va_end(ap);
}

void mutex_init(struct mutex *m)
{
	(void)m;
}

int mutex_lock_interruptible(struct mutex *m)
{
	(void)m;
	if (utk_mutex_fail)
		return -EINTR;
	utk_mutex_locks++;
	return 0;
}

void mutex_unlock(struct mutex *m)
{
	(void)m;
	utk_mutex_unlocks++;
}

s32 i2c_smbus_read_byte_data(struct i2c_client *client, u8 command)
{
	(void)client;
	if (utk_fail_i2c_read) {
		utk_log(EV_I2C_READ, command, -1);
		return -EIO;
	}
	utk_log(EV_I2C_READ, command, utk_chip[command]);
	return utk_chip[command];
}

s32 i2c_smbus_write_byte_data(struct i2c_client *client, u8 command, u8 value)
{
	(void)client;
	utk_log(EV_I2C_WRITE, command, value);
	if (utk_fail_i2c_write)
		return -EIO;
	utk_chip[command] = value;
	return 0;
}

int gvusb2_write_reg(struct gvusb2_dev *dev, u16 reg, u8 value)
{
	(void)dev;
	utk_log(EV_REG_WRITE, reg, value);
	return 0;
}

int gvusb2_set_reg_mask(struct gvusb2_dev *dev, u16 reg, u8 mask, u8 value)
{
	(void)dev;
	utk_log(EV_REG_MASK, reg, value);
	if (utk_nev <= UTK_MAX_EV)
		utk_ev[utk_nev - 1].mask = mask;
	return 0;
}

int gvusb2_vid_submit_urbs(struct gvusb2_vid *dev)
{
	(void)dev;
	utk_submit_calls++;
	utk_log(EV_URBS, 1, utk_submit_ret);
	return utk_submit_ret;
}

void gvusb2_vid_cancel_urbs(struct gvusb2_vid *dev)
{
	(void)dev;
	utk_cancel_calls++;
	utk_log(EV_URBS, 0, 0);
}

void gvusb2_release(struct v4l2_device *v4l2_dev)
{
	(void)v4l2_dev;
}

int usb_make_path(struct usb_device *udev, unsigned char *buf, size_t size)
{
	(void)udev;
	strscpy(buf, "usb-fake", size);
	return 0;
}

void utk_s_stream(int on)
{
	utk_log(EV_SSTREAM, on, 0);
	utk_subdev.stream_on = on;
}

/* Handed out from one pool that handler_init resets: the tests drive a single
 * device at a time, and a fresh handler must not inherit (or run out of) the
 * previous device's controls. */
static struct v4l2_ctrl utk_ctrl_pool[16];
static int utk_ctrl_next;

int v4l2_ctrl_handler_init(struct v4l2_ctrl_handler *hdl, unsigned int n)
{
	(void)n;
	memset(hdl, 0, sizeof(*hdl));
	memset(utk_ctrl_pool, 0, sizeof(utk_ctrl_pool));
	utk_ctrl_next = 0;
	return 0;
}

void v4l2_ctrl_handler_setup(struct v4l2_ctrl_handler *hdl)
{
	(void)hdl;
}

void v4l2_ctrl_handler_free(struct v4l2_ctrl_handler *hdl)
{
	(void)hdl;
}

struct v4l2_ctrl *v4l2_ctrl_new_std(struct v4l2_ctrl_handler *hdl,
	const struct v4l2_ctrl_ops *ops, u32 id, s64 min, s64 max, u64 step,
	s64 def)
{
	struct v4l2_ctrl *c;

	if (utk_ctrl_next >= (int)ARRAY_SIZE(utk_ctrl_pool))
		return NULL;
	c = &utk_ctrl_pool[utk_ctrl_next++];
	memset(c, 0, sizeof(*c));
	c->ops = ops;
	c->handler = hdl;
	c->id = id;
	c->minimum = min;
	c->maximum = max;
	c->step = step;
	c->default_value = def;
	c->val = def;
	if (hdl->n_ctrls < (int)ARRAY_SIZE(hdl->ctrls))
		hdl->ctrls[hdl->n_ctrls++] = c;
	return c;
}

struct v4l2_ctrl *v4l2_ctrl_new_custom(struct v4l2_ctrl_handler *hdl,
	const struct v4l2_ctrl_config *cfg, void *priv)
{
	struct v4l2_ctrl *c;

	(void)priv;
	c = v4l2_ctrl_new_std(hdl, cfg->ops, cfg->id, cfg->min, cfg->max,
		cfg->step, cfg->def);
	if (c) {
		c->name = cfg->name;
		c->flags = cfg->flags;
	}
	return c;
}

s32 v4l2_ctrl_g_ctrl(struct v4l2_ctrl *ctrl)
{
	return ctrl->val;
}

/* Real behaviour: clamp the current value into the new range, then apply. */
int v4l2_ctrl_modify_range(struct v4l2_ctrl *ctrl, s64 min, s64 max, u64 step,
	s64 def)
{
	(void)def;
	ctrl->minimum = min;
	ctrl->maximum = max;
	ctrl->step = step;
	if (ctrl->val < min)
		ctrl->val = min;
	if (ctrl->val > max)
		ctrl->val = max;
	return 0;
}

int v4l2_ctrl_log_status(struct file *file, void *priv)
{
	(void)file;
	(void)priv;
	return 0;
}

int v4l2_ctrl_subscribe_event(struct v4l2_fh *fh,
	const struct v4l2_event_subscription *sub)
{
	(void)fh;
	(void)sub;
	return 0;
}

int v4l2_event_unsubscribe(struct v4l2_fh *fh,
	const struct v4l2_event_subscription *sub)
{
	(void)fh;
	(void)sub;
	return 0;
}

int v4l2_device_register(struct device *dev, struct v4l2_device *v4l2_dev)
{
	(void)dev;
	v4l2_dev->dev = dev;
	return 0;
}

void v4l2_device_unregister(struct v4l2_device *v4l2_dev)
{
	(void)v4l2_dev;
}

struct v4l2_subdev *v4l2_i2c_new_subdev_board(struct v4l2_device *v4l2_dev,
	struct i2c_adapter *adapter, struct i2c_board_info *info,
	const unsigned short *probe_addrs)
{
	(void)adapter;
	(void)info;
	(void)probe_addrs;
	utk_subdev.v4l2_dev = v4l2_dev;
	return &utk_subdev;
}

int vb2_queue_init(struct vb2_queue *q)
{
	(void)q;
	return 0;
}

int vb2_is_busy(struct vb2_queue *q)
{
	return q->busy;
}

void vb2_buffer_done(struct vb2_buffer *vb, enum vb2_buffer_state state)
{
	utk_log(EV_VB2_DONE, state, vb->index);
}

const struct vb2_mem_ops vb2_vmalloc_memops;

/* The device the static ioctl handlers resolve from `file`; the tests call
 * them directly, so file is NULL and the device comes from here. */
static struct gvusb2_vid *utk_current;

void *video_drvdata(struct file *file)
{
	(void)file;
	return utk_current;
}

void video_set_drvdata(struct video_device *vdev, void *data)
{
	vdev->drvdata = data;
}

int video_register_device(struct video_device *vdev,
	enum vfl_devnode_type type, int nr)
{
	(void)vdev;
	(void)type;
	(void)nr;
	return 0;
}

void video_device_release_empty(struct video_device *vdev)
{
	(void)vdev;
}

int v4l2_fh_open(struct file *file)
{
	(void)file;
	return 0;
}

int vb2_fop_release(struct file *file)
{
	(void)file;
	return 0;
}

ssize_t vb2_fop_read(struct file *file, char *buf, size_t count, loff_t *ppos)
{
	(void)file;
	(void)buf;
	(void)count;
	(void)ppos;
	return 0;
}

unsigned int vb2_fop_poll(struct file *file, void *wait)
{
	(void)file;
	(void)wait;
	return 0;
}

int vb2_fop_mmap(struct file *file, void *vma)
{
	(void)file;
	(void)vma;
	return 0;
}

long video_ioctl2(struct file *file, unsigned int cmd, unsigned long arg)
{
	(void)file;
	(void)cmd;
	(void)arg;
	return 0;
}

int vb2_ioctl_reqbufs(struct file *file, void *priv,
	struct v4l2_requestbuffers *p)
{
	(void)file;
	(void)priv;
	(void)p;
	return 0;
}

int vb2_ioctl_querybuf(struct file *file, void *priv, struct v4l2_buffer *p)
{
	return vb2_ioctl_reqbufs(file, priv, (struct v4l2_requestbuffers *)p);
}

int vb2_ioctl_qbuf(struct file *file, void *priv, struct v4l2_buffer *p)
{
	return vb2_ioctl_querybuf(file, priv, p);
}

int vb2_ioctl_dqbuf(struct file *file, void *priv, struct v4l2_buffer *p)
{
	return vb2_ioctl_querybuf(file, priv, p);
}

int vb2_ioctl_create_bufs(struct file *file, void *priv,
	struct v4l2_create_buffers *p)
{
	(void)file;
	(void)priv;
	(void)p;
	return 0;
}

int vb2_ioctl_streamon(struct file *file, void *priv, enum v4l2_buf_type i)
{
	(void)file;
	(void)priv;
	(void)i;
	return 0;
}

int vb2_ioctl_streamoff(struct file *file, void *priv, enum v4l2_buf_type i)
{
	return vb2_ioctl_streamon(file, priv, i);
}

int vb2_ioctl_expbuf(struct file *file, void *priv,
	struct v4l2_exportbuffer *p)
{
	(void)file;
	(void)priv;
	(void)p;
	return 0;
}

/*****************************************************************************
 * Harness
 ****************************************************************************/

/* Device with hand-built controls: the tests drive one decision at a time
 * instead of going through gvusb2_v4l2_register. */
struct utk_device {
	struct gvusb2_vid vid;
	struct v4l2_ctrl scratch;	/* carrier for one-off control calls */
	struct v4l2_ctrl vbi_capture;
	struct v4l2_ctrl black_pedestal;
	struct v4l2_ctrl vertical_start;
	struct v4l2_ctrl horizontal_start;
};

static struct utk_device utk_d;

static void utk_reset(void)
{
	utk_nev = 0;
	memset(utk_chip, 0, sizeof(utk_chip));
	utk_fail_i2c_read = 0;
	utk_fail_i2c_write = 0;
	utk_submit_ret = 0;
	utk_submit_calls = 0;
	utk_cancel_calls = 0;
	utk_mutex_locks = 0;
	utk_mutex_unlocks = 0;
	utk_mutex_fail = 0;
	utk_warn[0] = '\0';
	memset(&utk_subdev, 0, sizeof(utk_subdev));

	memset(&utk_d, 0, sizeof(utk_d));
	utk_d.vbi_capture.id = GVUSB2_CID_VBI_CAPTURE;
	utk_d.black_pedestal.id = GVUSB2_CID_BLACK_PEDESTAL;
	utk_d.vertical_start.id = GVUSB2_CID_VERTICAL_START;
	utk_d.vertical_start.val = 2;
	utk_d.horizontal_start.id = GVUSB2_CID_HORIZONTAL_START;
	utk_d.horizontal_start.val = 4;
	utk_d.vid.vbi_capture = &utk_d.vbi_capture;
	utk_d.vid.black_pedestal = &utk_d.black_pedestal;
	utk_d.vid.vertical_start = &utk_d.vertical_start;
	utk_d.vid.horizontal_start = &utk_d.horizontal_start;
	utk_d.vid.standard = V4L2_STD_NTSC_M;
	utk_d.vid.vb2q.drv_priv = &utk_d.vid;
	INIT_LIST_HEAD(&utk_d.vid.buf_list);
	utk_current = &utk_d.vid;
}

static struct gvusb2_vid *utk_vid(void)
{
	return &utk_d.vid;
}

static void utk_set_vbi(int on)
{
	utk_d.vbi_capture.val = on;
}

static void utk_set_pedestal(int on)
{
	utk_d.black_pedestal.val = on;
}

/* Run a control the way v4l2 core would: through the driver's own s_ctrl. */
static int utk_ctrl(struct v4l2_ctrl *ctrl, u32 id, int val)
{
	ctrl->id = id;
	ctrl->val = val;
	ctrl->handler = &utk_d.vid.ctrl_handler;
	return gvusb2_ctrl_ops.s_ctrl(ctrl);
}

/*****************************************************************************
 * get_resolution: the geometry that everything else is derived from
 ****************************************************************************/

/* The four documented geometries, selected by standard and the VBI control. */
static void test_resolution_geometries(void)
{
	struct gvusb2_vid *dev = utk_vid();
	int w, h;

	utk_reset();
	w = h = 0;
	get_resolution(dev, &w, &h);
	assert(w == 720);
	assert(h == 486);	/* NTSC_M default, no VBI */

	utk_set_vbi(1);
	get_resolution(dev, &w, &h);
	assert(w == 720);
	assert(h == 516);	/* NTSC + VBI */

	utk_set_vbi(0);
	dev->standard = V4L2_STD_PAL;
	get_resolution(dev, &w, &h);
	assert(w == 720);
	assert(h == 576);

	utk_set_vbi(1);
	get_resolution(dev, &w, &h);
	assert(w == 720);
	assert(h == 616);

	/* The VBI control is tested for "> 0", not for "!= 0": a negative
	 * value reads as off, anything positive as on. Neither may produce a
	 * third geometry. */
	dev->standard = V4L2_STD_NTSC_M;
	utk_set_vbi(-1);
	get_resolution(dev, &w, &h);
	assert(h == 486);
	utk_set_vbi(2);
	get_resolution(dev, &w, &h);
	assert(h == 516);
}

/* The width/height out-parameters are optional. */
static void test_resolution_null_outputs(void)
{
	struct gvusb2_vid *dev = utk_vid();
	int w, h;

	utk_reset();
	w = h = -1;
	get_resolution(dev, NULL, &h);
	assert(h == 486);
	w = h = -1;
	get_resolution(dev, &w, NULL);
	assert(w == 720);
	get_resolution(dev, NULL, NULL); /* must not crash */
}

/* 625/50 is a bitmask, not a single standard: PAL_M and PAL_60 are 525/60
 * (NTSC geometry) despite the name, PAL_N and SECAM are 625/50. */
static void test_resolution_standard_classification(void)
{
	static const struct {
		v4l2_std_id std;
		int height;
		int height_vbi;
	} cases[] = {
		{ V4L2_STD_NTSC_M, 486, 516 },
		{ V4L2_STD_NTSC, 486, 516 },
		{ V4L2_STD_PAL_M, 486, 516 },	/* 525/60 */
		{ V4L2_STD_PAL_60, 486, 516 },	/* 525/60 */
		{ V4L2_STD_PAL, 576, 616 },
		{ V4L2_STD_PAL_N, 576, 616 },	/* 625/50 */
		{ V4L2_STD_PAL_Nc, 576, 616 },	/* 625/50 */
		{ V4L2_STD_SECAM, 576, 616 },
		{ V4L2_STD_UNKNOWN, 486, 516 },	/* no standard set yet */
	};
	struct gvusb2_vid *dev = utk_vid();
	size_t i;
	int h;

	utk_reset();
	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		dev->standard = cases[i].std;
		utk_set_vbi(0);
		h = 0;
		get_resolution(dev, NULL, &h);
		assert(h == cases[i].height);
		utk_set_vbi(1);
		get_resolution(dev, NULL, &h);
		assert(h == cases[i].height_vbi);
	}
}

/*****************************************************************************
 * Format negotiation
 ****************************************************************************/

/* try_fmt normalises to the one supported format, whatever was requested.
 * The requested values below are the interesting boundaries for a driver that
 * claims a single fixed geometry: nothing may be clamped, rounded or passed
 * through. */
static void test_try_fmt_normalises(void)
{
	static const unsigned int probe[][2] = {
		{ 0, 0 }, { 1, 1 }, { 2, 2 },
		{ 719, 483 }, { 720, 486 }, { 721, 487 },
		{ 1440, 972 }, { 0xffff, 0xffff },
	};
	struct v4l2_format f;
	size_t i;

	utk_reset();
	for (i = 0; i < ARRAY_SIZE(probe); i++) {
		memset(&f, 0, sizeof(f));
		f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		f.fmt.pix.width = probe[i][0];
		f.fmt.pix.height = probe[i][1];
		f.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
		f.fmt.pix.field = V4L2_FIELD_ANY;

		assert(gvusb2_vidioc_try_fmt_vid_cap(NULL, NULL, &f) == 0);

		assert(f.type == V4L2_BUF_TYPE_VIDEO_CAPTURE);
		assert(f.fmt.pix.width == 720);
		assert(f.fmt.pix.height == 486);
		assert(f.fmt.pix.field == V4L2_FIELD_INTERLACED);
		assert(f.fmt.pix.pixelformat == V4L2_PIX_FMT_UYVY);
		assert(f.fmt.pix.colorspace == V4L2_COLORSPACE_SMPTE170M);
		/* UYVY: 2 bytes per pixel, so bytesperline is the pixel width
		 * doubled and sizeimage is exactly one full frame. */
		assert(f.fmt.pix.bytesperline == 1440);
		assert(f.fmt.pix.sizeimage == 720u * 486u * 2u);
		assert(f.fmt.pix.sizeimage ==
		       f.fmt.pix.bytesperline * f.fmt.pix.height);
	}
}

/* The same normalisation must follow the standard and the VBI control. */
static void test_try_fmt_follows_geometry(void)
{
	static const struct {
		v4l2_std_id std;
		int vbi;
		unsigned int height;
	} cases[] = {
		{ V4L2_STD_NTSC_M, 0, 486 },
		{ V4L2_STD_NTSC_M, 1, 516 },
		{ V4L2_STD_PAL, 0, 576 },
		{ V4L2_STD_PAL, 1, 616 },
	};
	struct gvusb2_vid *dev = utk_vid();
	struct v4l2_format f;
	size_t i;

	utk_reset();
	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		dev->standard = cases[i].std;
		utk_set_vbi(cases[i].vbi);

		memset(&f, 0, sizeof(f));
		f.fmt.pix.width = 1;
		f.fmt.pix.height = 1;
		assert(gvusb2_vidioc_try_fmt_vid_cap(NULL, NULL, &f) == 0);
		assert(f.fmt.pix.height == cases[i].height);
		assert(f.fmt.pix.bytesperline == 1440);
		assert(f.fmt.pix.sizeimage == 1440u * cases[i].height);
		assert(f.fmt.pix.sizeimage ==
		       720u * cases[i].height * 2u);
	}
}

/* g_fmt reports the same thing try_fmt negotiates, for every geometry. */
static void test_g_fmt_matches_try_fmt(void)
{
	static const struct {
		v4l2_std_id std;
		int vbi;
	} cases[] = {
		{ V4L2_STD_NTSC_M, 0 }, { V4L2_STD_NTSC_M, 1 },
		{ V4L2_STD_PAL, 0 }, { V4L2_STD_PAL, 1 },
	};
	struct gvusb2_vid *dev = utk_vid();
	struct v4l2_format g, t;
	size_t i;

	utk_reset();
	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		dev->standard = cases[i].std;
		utk_set_vbi(cases[i].vbi);

		memset(&g, 0, sizeof(g));
		memset(&t, 0, sizeof(t));
		assert(gvusb2_vidioc_g_fmt_vid_cap(NULL, NULL, &g) == 0);
		assert(gvusb2_vidioc_try_fmt_vid_cap(NULL, NULL, &t) == 0);
		assert(g.fmt.pix.width == t.fmt.pix.width);
		assert(g.fmt.pix.height == t.fmt.pix.height);
		assert(g.fmt.pix.pixelformat == t.fmt.pix.pixelformat);
		assert(g.fmt.pix.field == t.fmt.pix.field);
		assert(g.fmt.pix.bytesperline == t.fmt.pix.bytesperline);
		assert(g.fmt.pix.sizeimage == t.fmt.pix.sizeimage);
		assert(g.fmt.pix.colorspace == t.fmt.pix.colorspace);
	}
}

/* An unsupported fourcc is coerced to UYVY rather than rejected: the driver
 * has exactly one format and says so by returning the adjusted one. */
static void test_unsupported_fourcc_is_coerced(void)
{
	static const unsigned int fourccs[] = {
		0, V4L2_PIX_FMT_RGB24, V4L2_PIX_FMT_YUYV, V4L2_PIX_FMT_MJPEG,
		V4L2_PIX_FMT_NV12, 0xffffffff,
	};
	struct v4l2_format f;
	size_t i;

	utk_reset();
	for (i = 0; i < ARRAY_SIZE(fourccs); i++) {
		memset(&f, 0, sizeof(f));
		f.fmt.pix.pixelformat = fourccs[i];
		assert(gvusb2_vidioc_try_fmt_vid_cap(NULL, NULL, &f) == 0);
		assert(f.fmt.pix.pixelformat == V4L2_PIX_FMT_UYVY);

		memset(&f, 0, sizeof(f));
		f.fmt.pix.pixelformat = fourccs[i];
		assert(gvusb2_vidioc_s_fmt_vid_cap(NULL, NULL, &f) == 0);
		assert(f.fmt.pix.pixelformat == V4L2_PIX_FMT_UYVY);
		assert(f.fmt.pix.width == 720);
		assert(f.fmt.pix.height == 486);
	}
}

/* enum_fmt advertises exactly one format, and it is the format negotiation
 * actually produces. */
static void test_enum_fmt(void)
{
	struct v4l2_fmtdesc f;
	struct v4l2_format neg;

	utk_reset();

	memset(&f, 0, sizeof(f));
	f.index = 0;
	assert(gvusb2_vidioc_enum_fmt_vid_cap(NULL, NULL, &f) == 0);
	assert(f.pixelformat == V4L2_PIX_FMT_UYVY);

	memset(&neg, 0, sizeof(neg));
	assert(gvusb2_vidioc_try_fmt_vid_cap(NULL, NULL, &neg) == 0);
	assert(f.pixelformat == neg.fmt.pix.pixelformat);

	memset(&f, 0, sizeof(f));
	f.index = 1;
	assert(gvusb2_vidioc_enum_fmt_vid_cap(NULL, NULL, &f) == -EINVAL);

	memset(&f, 0, sizeof(f));
	f.index = 2;
	assert(gvusb2_vidioc_enum_fmt_vid_cap(NULL, NULL, &f) == -EINVAL);
}

/* s_fmt refuses to renegotiate while the queue is busy, and must not have
 * touched the caller's struct in that case. try_fmt must still work: it is
 * the call userspace makes to find out what it can ask for. */
static void test_s_fmt_busy(void)
{
	struct gvusb2_vid *dev = utk_vid();
	struct v4l2_format f;

	utk_reset();
	dev->vb2q.busy = 1;

	memset(&f, 0, sizeof(f));
	f.fmt.pix.width = 640;
	f.fmt.pix.height = 480;
	assert(gvusb2_vidioc_s_fmt_vid_cap(NULL, NULL, &f) == -EBUSY);
	assert(f.fmt.pix.width == 640);
	assert(f.fmt.pix.height == 480);

	memset(&f, 0, sizeof(f));
	assert(gvusb2_vidioc_try_fmt_vid_cap(NULL, NULL, &f) == 0);
	assert(f.fmt.pix.width == 720);
	assert(f.fmt.pix.height == 486);

	/* g_fmt is not blocked either. */
	memset(&f, 0, sizeof(f));
	assert(gvusb2_vidioc_g_fmt_vid_cap(NULL, NULL, &f) == 0);
	assert(f.fmt.pix.width == 720);
}

/*****************************************************************************
 * vb2 queue_setup: the buffer size arithmetic
 ****************************************************************************/

static int utk_queue_setup(unsigned int *nbuffers,
	unsigned int *nplanes, unsigned int sizes[])
{
	struct gvusb2_vid *dev = utk_vid();

	return gvusb2_vb2_queue_setup(&dev->vb2q, nbuffers, nplanes, sizes,
		NULL);
}

/* Querying: one plane, frame-sized, at least four buffers. */
static void test_queue_setup_query(void)
{
	unsigned int nb, np, sizes[2];

	utk_reset();
	nb = 0;
	np = 0;
	sizes[0] = sizes[1] = 0;
	assert(utk_queue_setup(&nb, &np, sizes) == 0);
	assert(np == 1);
	assert(nb == 4);	/* minimum four buffers */
	assert(sizes[0] == 720u * 486u * 2u);

	/* Values around the floor. */
	nb = 1;
	np = 0;
	assert(utk_queue_setup(&nb, &np, sizes) == 0);
	assert(nb == 4);
	nb = 3;
	np = 0;
	assert(utk_queue_setup(&nb, &np, sizes) == 0);
	assert(nb == 4);
	nb = 4;
	np = 0;
	assert(utk_queue_setup(&nb, &np, sizes) == 0);
	assert(nb == 4);
	/* No upper clamp: ask for more, get more. */
	nb = 5;
	np = 0;
	assert(utk_queue_setup(&nb, &np, sizes) == 0);
	assert(nb == 5);
	nb = 64;
	np = 0;
	assert(utk_queue_setup(&nb, &np, sizes) == 0);
	assert(nb == 64);

	/* The size follows the geometry. */
	utk_vid()->standard = V4L2_STD_PAL;
	utk_set_vbi(1);
	nb = 4;
	np = 0;
	assert(utk_queue_setup(&nb, &np, sizes) == 0);
	assert(sizes[0] == 720u * 616u * 2u);
}

/* Configuring: one plane only, and the buffer the caller offers must be big
 * enough. */
static void test_queue_setup_configure(void)
{
	unsigned int nb, np, sizes[2];
	unsigned int frame = 720u * 486u * 2u;

	utk_reset();

	np = 1;
	nb = 4;
	sizes[0] = frame;
	assert(utk_queue_setup(&nb, &np, sizes) == 0);
	assert(np == 1);

	/* One byte short is not enough. */
	np = 1;
	sizes[0] = frame - 1;
	assert(utk_queue_setup(&nb, &np, sizes) == -EINVAL);

	np = 1;
	sizes[0] = 0;
	assert(utk_queue_setup(&nb, &np, sizes) == -EINVAL);

	/* A larger buffer is accepted and left alone (the caller may have
	 * aligned it). */
	np = 1;
	sizes[0] = frame + 4096;
	assert(utk_queue_setup(&nb, &np, sizes) == 0);
	assert(sizes[0] == frame + 4096);

	/* Sliced VBI or multiplanar callers are not supported. */
	np = 2;
	sizes[0] = frame;
	sizes[1] = frame;
	assert(utk_queue_setup(&nb, &np, sizes) == -EINVAL);

	np = 3;
	assert(utk_queue_setup(&nb, &np, sizes) == -EINVAL);

	/* The buffer floor applies on this path too. */
	np = 1;
	nb = 0;
	sizes[0] = frame;
	assert(utk_queue_setup(&nb, &np, sizes) == 0);
	assert(nb == 4);
}

/*****************************************************************************
 * set_tw9910_cropping: what the decoder is actually told
 ****************************************************************************/

/* The full i2c write sequence for each standard/geometry, as the TW9910 needs
 * it. Hard-coded per case rather than recomputed, so a swapped PAL/NTSC or a
 * swapped VBI flag cannot cancel itself out. */
static void test_cropping_write_sequence(void)
{
	static const struct {
		const char *name;
		v4l2_std_id std;
		int vbi;
		int pal;	/* expected 625/50 classification */
		unsigned char blc;	/* pedestal value written to 0x0c */
		unsigned char plan[6][2];	/* reg, val */
	} cases[] = {
		{
			"NTSC", V4L2_STD_NTSC_M, 0, 0, 0x08,
			{ { 0x1c, 0x17 }, { 0x0b, 0xd2 }, { 0x0a, 0x10 },
			  { 0x08, 0x13 }, { 0x07, 0x02 }, { 0x09, 0xf4 } },
		},
		{
			"NTSC+VBI", V4L2_STD_NTSC_M, 1, 0, 0x08,
			{ { 0x1c, 0x17 }, { 0x0b, 0xd2 }, { 0x0a, 0x10 },
			  { 0x08, 0x02 }, { 0x07, 0x12 }, { 0x09, 0x05 } },
		},
		{
			"PAL", V4L2_STD_PAL, 0, 1, 0x18,
			{ { 0x1c, 0x97 }, { 0x0b, 0xd2 }, { 0x0a, 0x0d },
			  { 0x08, 0x17 }, { 0x07, 0x12 }, { 0x09, 0x22 } },
		},
		{
			"PAL+VBI", V4L2_STD_PAL, 1, 1, 0x18,
			{ { 0x1c, 0x97 }, { 0x0b, 0xd2 }, { 0x0a, 0x0d },
			  { 0x08, 0x02 }, { 0x07, 0x12 }, { 0x09, 0x37 } },
		},
	};
	struct utk_i2c_ev ev[16];
	struct gvusb2_vid *dev = utk_vid();
	size_t c;
	int i, n;

	for (c = 0; c < ARRAY_SIZE(cases); c++) {
		utk_reset();
		dev = utk_vid();
		/* Both callers in the driver set the standard first; the black
		 * level is resolved from dev->standard, the plan from the
		 * argument. */
		dev->standard = cases[c].std;
		set_tw9910_cropping(dev, cases[c].std, cases[c].vbi);

		n = utk_i2c_events(ev, ARRAY_SIZE(ev));

		/* Black level first: one read of 0x0c, then the modified
		 * value back. Everything else follows the standard plan. */
		assert(n == 8);		/* 2 black-level + 6 plan writes */
		assert(ev[0].read);
		assert(ev[0].reg == 0x0c);
		assert(!ev[1].read);
		assert(ev[1].reg == 0x0c);
		assert(ev[1].val == cases[c].blc);

		for (i = 0; i < 6; i++) {
			assert(!ev[2 + i].read);
			assert(ev[2 + i].reg == cases[c].plan[i][0]);
			assert(ev[2 + i].val == cases[c].plan[i][1]);
		}

		/* The driver's own log message must agree with the plan it
		 * just wrote. */
		if (cases[c].pal)
			assert(strstr(utk_warn, "PAL") != NULL);
		else
			assert(strstr(utk_warn, "ntsc") != NULL);
	}

	/* A decoder write that fails is not a reason to abandon the rest: the
	 * fork owns the TW9910 and the whole plan is still sent. */
	utk_reset();
	dev = utk_vid();
	utk_fail_i2c_write = 1;
	set_tw9910_cropping(dev, V4L2_STD_PAL, 0);
	assert(utk_count(EV_I2C_WRITE, UTK_ANY, UTK_ANY) == 7);
	assert(utk_i2c_written(0x09) == 0x22);
}

/* The VBI argument is a count/flag, not a boolean: only > 0 means VBI. */
static void test_cropping_vbi_boundary(void)
{
	struct utk_i2c_ev ev[16];
	struct gvusb2_vid *dev = utk_vid();
	int n;

	utk_reset();
	set_tw9910_cropping(dev, V4L2_STD_NTSC_M, -1);
	n = utk_i2c_events(ev, ARRAY_SIZE(ev));
	assert(n == 8);
	/* No VBI: VDELAY 0x08 = 0x13, VACTIVE 0x09 = 0xf4. */
	assert(ev[5].reg == 0x08 && ev[5].val == 0x13);
	assert(ev[7].reg == 0x09 && ev[7].val == 0xf4);

	utk_reset();
	dev = utk_vid();
	set_tw9910_cropping(dev, V4L2_STD_NTSC_M, 2);
	n = utk_i2c_events(ev, ARRAY_SIZE(ev));
	assert(n == 8);
	assert(ev[5].reg == 0x08 && ev[5].val == 0x02);
	assert(ev[7].reg == 0x09 && ev[7].val == 0x05);
}

/* 625/50 classification for the crop path: PAL_M/PAL_60 are 525/60. */
static void test_cropping_standard_classification(void)
{
	static const struct {
		v4l2_std_id std;
		unsigned char sdt;	/* 0x1c: standard selection */
	} cases[] = {
		{ V4L2_STD_NTSC_M, 0x17 },
		{ V4L2_STD_PAL_M, 0x17 },
		{ V4L2_STD_PAL_60, 0x17 },
		{ V4L2_STD_UNKNOWN, 0x17 },
		{ V4L2_STD_PAL, 0x97 },
		{ V4L2_STD_PAL_N, 0x97 },
		{ V4L2_STD_SECAM, 0x97 },
	};
	struct gvusb2_vid *dev;
	size_t i;

	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		utk_reset();
		dev = utk_vid();
		dev->standard = cases[i].std;
		set_tw9910_cropping(dev, cases[i].std, 0);
		assert(utk_i2c_written(0x1c) == cases[i].sdt);
	}
}

/*****************************************************************************
 * Black level: read-modify-write, pedestal override, failing read
 ****************************************************************************/

/* Only bit 4 (pedestal) and bit 3 are decided here; the clamp's other bits
 * must survive the RMW. */
static void test_black_level_rmw(void)
{
	static const struct {
		v4l2_std_id std;
		int pedestal;
		unsigned char cur;	/* chip value read from 0x0c */
		unsigned char want;	/* value written back */
	} cases[] = {
		/* 7.5 IRE for NTSC by default... */
		{ V4L2_STD_NTSC_M, 0, 0x00, 0x08 },
		{ V4L2_STD_NTSC_M, 0, 0xff, 0xef },
		/* ...0 IRE for 625/50... */
		{ V4L2_STD_PAL, 0, 0x00, 0x18 },
		{ V4L2_STD_PAL, 0, 0xff, 0xff },
		/* ...and the other bits are preserved. */
		{ V4L2_STD_NTSC_M, 0, 0x52, 0x4a },
		{ V4L2_STD_PAL, 0, 0x52, 0x5a },
		/* The control forces the pedestal back on for any standard. */
		{ V4L2_STD_PAL, 1, 0x52, 0x4a },
		{ V4L2_STD_PAL, 1, 0x00, 0x08 },
		{ V4L2_STD_NTSC_M, 1, 0x52, 0x4a },
	};
	struct gvusb2_vid *dev = utk_vid();
	size_t i;

	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		utk_reset();
		dev = utk_vid();
		dev->standard = cases[i].std;
		utk_set_pedestal(cases[i].pedestal);
		utk_chip[0x0c] = cases[i].cur;

		set_tw9910_cropping(dev, cases[i].std, 0);

		assert(utk_i2c_written(0x0c) == cases[i].want);
		/* Bits 3 and 4 are the pedestal; bits 0-2 and 5-7 belong to
		 * the clamp and must survive the read-modify-write. */
		assert((cases[i].want & 0xe7) == (cases[i].cur & 0xe7));
	}
}

/* A failed read must leave the level alone: no write at all. The rest of the
 * crop still has to be programmed, or a flicker in the i2c bus would leave
 * the decoder on the previous standard. */
static void test_black_level_read_failure(void)
{
	struct utk_i2c_ev ev[16];
	struct gvusb2_vid *dev = utk_vid();
	int n, i;

	utk_reset();
	dev = utk_vid();
	dev->standard = V4L2_STD_PAL;
	utk_chip[0x0c] = 0x18;
	utk_fail_i2c_read = 1;

	set_tw9910_cropping(dev, V4L2_STD_PAL, 0);

	n = utk_i2c_events(ev, ARRAY_SIZE(ev));
	assert(n == 7);	/* one failed read + the six plan writes */
	assert(ev[0].read);
	assert(ev[0].reg == 0x0c);
	for (i = 0; i < n; i++)
		assert(!(ev[i].reg == 0x0c && !ev[i].read));
	assert(utk_i2c_written(0x0c) == -1);
	assert(utk_chip[0x0c] == 0x18);	/* untouched */
	assert(utk_i2c_written(0x1c) == 0x97);	/* plan still applied */
}

/* The pedestal control path: gvusb2_s_ctrl must re-apply the level without
 * touching the crop. */
static void test_black_pedestal_control(void)
{
	struct utk_i2c_ev ev[16];
	struct gvusb2_vid *dev = utk_vid();
	int n;

	utk_reset();
	dev = utk_vid();
	dev->standard = V4L2_STD_PAL;
	utk_chip[0x0c] = 0x52;

	/* The level is resolved from the control on the device, so the call
	 * has to go through the real one. */
	assert(utk_ctrl(dev->black_pedestal, GVUSB2_CID_BLACK_PEDESTAL, 1)
	       == 0);

	n = utk_i2c_events(ev, ARRAY_SIZE(ev));
	assert(n == 2);		/* read 0x0c, write 0x0c */
	assert(ev[0].read && ev[0].reg == 0x0c);
	assert(!ev[1].read && ev[1].reg == 0x0c && ev[1].val == 0x4a);

	/* Clearing it again restores the standard's 0 IRE for 625/50. */
	assert(utk_ctrl(dev->black_pedestal, GVUSB2_CID_BLACK_PEDESTAL, 0)
	       == 0);
	assert(utk_i2c_written(0x0c) == 0x5a);
}

/*****************************************************************************
 * gvusb2_s_ctrl: the register values the controls produce
 ****************************************************************************/

/* Brightness and hue are offset-binary: the control's centre is chip 0. */
static void test_ctrl_brightness_hue(void)
{
	static const int vals[] = { 0, 1, 127, 128, 255 };
	size_t i;

	for (i = 0; i < ARRAY_SIZE(vals); i++) {
		utk_reset();
		assert(utk_ctrl(&utk_d.scratch, V4L2_CID_BRIGHTNESS,
				vals[i]) == 0);
		assert(utk_i2c_written(0x10) ==
		       ((vals[i] + 0x80) & 0xff));
		assert(utk_count(EV_I2C_WRITE, 0x10, UTK_ANY) == 1);

		utk_reset();
		assert(utk_ctrl(&utk_d.scratch, V4L2_CID_HUE,
				vals[i]) == 0);
		assert(utk_i2c_written(0x15) ==
		       ((vals[i] + 0x80) & 0xff));
	}
}

/* Saturation, contrast and gain write their own registers; sharpness is
 * masked into the chip's 4-bit field with the fixed 0x50 base. */
static void test_ctrl_direct_registers(void)
{
	static const struct {
		u32 id;
		int reg;
		int val;
		u8 want;
	} cases[] = {
		{ V4L2_CID_CONTRAST, 0x11, 0, 0x00 },
		{ V4L2_CID_CONTRAST, 0x11, 255, 0xff },
		{ V4L2_CID_GAIN, 0x22, 7, 0x07 },
		{ V4L2_CID_SHARPNESS, 0x12, 0, 0x50 },
		{ V4L2_CID_SHARPNESS, 0x12, 15, 0x5f },
		{ V4L2_CID_SHARPNESS, 0x12, 16, 0x50 },	/* masked out */
		{ V4L2_CID_SHARPNESS, 0x12, 0xff, 0x5f },	/* masked out */
	};
	size_t i;

	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		utk_reset();
		assert(utk_ctrl(&utk_d.scratch, cases[i].id,
				cases[i].val) == 0);
		assert(utk_i2c_written(cases[i].reg) == cases[i].want);
		assert(utk_count(EV_I2C_WRITE, cases[i].reg, UTK_ANY) == 1);
	}

	/* Saturation goes to both colour-difference registers. */
	utk_reset();
	assert(utk_ctrl(&utk_d.scratch, V4L2_CID_SATURATION, 0x3c)
	       == 0);
	assert(utk_i2c_written(0x13) == 0x3c);
	assert(utk_i2c_written(0x14) == 0x3c);
	assert(utk_nev == 2);
}

/* Autogain is a read-modify-write of one bit in register 0x06. */
static void test_ctrl_autogain_rmw(void)
{
	int val;

	for (val = 0; val <= 1; val++) {
		utk_reset();
		utk_chip[0x06] = 0x52;	/* bit 4 clear, others set */

		assert(utk_ctrl(&utk_d.scratch, V4L2_CID_AUTOGAIN,
				val) == 0);
		/* Every bit but bit 4 is preserved... */
		assert((utk_i2c_written(0x06) & 0xef) == 0x42);
		/* ...and the two settings differ in that bit only. */
		assert(utk_i2c_written(0x06) ==
		       (val ? 0x42 : 0x52));
	}
}

/* BUG: a failed read left `reg_06` holding the errno truncated to u8 and the
 * driver wrote that garbage to the chip's mode register. A failed read must
 * leave the register alone and report the error. */
static void test_ctrl_autogain_read_failure(void)
{
	int ret;

	utk_reset();
	utk_chip[0x06] = 0x52;
	utk_fail_i2c_read = 1;

	ret = utk_ctrl(&utk_d.scratch, V4L2_CID_AUTOGAIN, 1);

	assert(utk_count(EV_I2C_WRITE, 0x06, UTK_ANY) == 0);
	assert(utk_chip[0x06] == 0x52);
	assert(ret < 0);
}

/* An unknown control id is not an error and touches no hardware. */
static void test_ctrl_unknown_id(void)
{
	utk_reset();
	assert(utk_ctrl(&utk_d.scratch, V4L2_CID_ROTATE, 1) == 0);
	assert(utk_nev == 0);
}

/* VBI capture toggles the vertical-start control's grabbed flag and
 * reprograms the STK1150 crop. */
static void test_vbi_control_flags(void)
{
	struct gvusb2_vid *dev = utk_vid();

	utk_reset();
	dev = utk_vid();
	dev->standard = V4L2_STD_PAL;

	assert(utk_ctrl(dev->vbi_capture, GVUSB2_CID_VBI_CAPTURE, 1) == 0);
	assert(dev->vertical_start->flags & V4L2_CTRL_FLAG_GRABBED);
	/* VBI mode programs the full-frame crop: 0x37 lines per field. */
	assert(utk_count(EV_REG_WRITE, 0x0116, 0x37) == 1);

	assert(utk_ctrl(dev->vbi_capture, GVUSB2_CID_VBI_CAPTURE, 0) == 0);
	assert(!(dev->vertical_start->flags & V4L2_CTRL_FLAG_GRABBED));
	/* Back to the vertical_start-controlled crop: 2 + 0x20. */
	assert(utk_count(EV_REG_WRITE, 0x0116, 0x22) == 1);
}

/*****************************************************************************
 * set_stk1150_cropping: the vendor-chip window arithmetic
 ****************************************************************************/

static void test_stk1150_cropping(void)
{
	struct gvusb2_vid *dev = utk_vid();

	/* NTSC, no VBI: the window starts at vertical_start and the count is
	 * vertical_start + 0xf3 with no high byte. */
	utk_reset();
	dev = utk_vid();
	set_stk1150_cropping(dev, V4L2_STD_NTSC_M, 0, 4, 2);
	assert(utk_count(EV_REG_WRITE, 0x0112, 2) == 1);
	assert(utk_count(EV_REG_WRITE, 0x0113, 0) == 1);
	assert(utk_count(EV_REG_WRITE, 0x0116, 0xf5) == 1);
	assert(utk_count(EV_REG_WRITE, 0x0117, 0x00) == 1);
	assert(utk_count(EV_REG_WRITE, 0x0110, 4) == 1);
	assert(utk_count(EV_REG_WRITE, 0x0111, 0) == 1);
	assert(utk_count(EV_REG_WRITE, 0x0114, 0xa4) == 1);
	assert(utk_count(EV_REG_WRITE, 0x0115, 0x05) == 1);
	assert(utk_nev == 8);

	/* PAL, no VBI: count is vertical_start + 0x20 and the high byte is
	 * set. */
	utk_reset();
	dev = utk_vid();
	set_stk1150_cropping(dev, V4L2_STD_PAL, 0, 0, 3);
	assert(utk_count(EV_REG_WRITE, 0x0112, 3) == 1);
	assert(utk_count(EV_REG_WRITE, 0x0116, 0x23) == 1);
	assert(utk_count(EV_REG_WRITE, 0x0117, 0x01) == 1);
	assert(utk_count(EV_REG_WRITE, 0x0110, 0) == 1);
	assert(utk_count(EV_REG_WRITE, 0x0114, 0xa0) == 1);

	/* VBI: the vertical controls are ignored and the line counts are the
	 * fixed full-frame ones. */
	utk_reset();
	dev = utk_vid();
	set_stk1150_cropping(dev, V4L2_STD_NTSC_M, 1, 1, 4);
	assert(utk_count(EV_REG_WRITE, 0x0112, 3) == 1);
	assert(utk_count(EV_REG_WRITE, 0x0116, 0x05) == 1);
	assert(utk_count(EV_REG_WRITE, 0x0117, 0x01) == 1);

	utk_reset();
	dev = utk_vid();
	set_stk1150_cropping(dev, V4L2_STD_PAL, 1, 1, 4);
	assert(utk_count(EV_REG_WRITE, 0x0116, 0x37) == 1);
	assert(utk_count(EV_REG_WRITE, 0x0117, 0x01) == 1);
	assert(utk_nev == 8);
}

/* The vertical/horizontal start controls rewrite the same window. */
static void test_start_controls(void)
{
	struct gvusb2_vid *dev = utk_vid();

	utk_reset();
	dev = utk_vid();
	dev->standard = V4L2_STD_NTSC_M;

	assert(utk_ctrl(dev->vertical_start, GVUSB2_CID_VERTICAL_START, 4)
	       == 0);
	assert(utk_count(EV_REG_WRITE, 0x0112, 4) == 1);
	assert(utk_count(EV_REG_WRITE, 0x0116, 0xf7) == 1);	/* 4 + 0xf3 */
	assert(utk_count(EV_REG_WRITE, 0x0117, 0x00) == 1);

	utk_reset();
	dev = utk_vid();
	dev->standard = V4L2_STD_PAL;
	assert(utk_ctrl(dev->vertical_start, GVUSB2_CID_VERTICAL_START, 1)
	       == 0);
	assert(utk_count(EV_REG_WRITE, 0x0116, 0x21) == 1);	/* 1 + 0x20 */
	assert(utk_count(EV_REG_WRITE, 0x0117, 0x01) == 1);

	utk_reset();
	dev = utk_vid();
	assert(utk_ctrl(dev->horizontal_start, GVUSB2_CID_HORIZONTAL_START, 0)
	       == 0);
	assert(utk_count(EV_REG_WRITE, 0x0110, 0) == 1);
	assert(utk_count(EV_REG_WRITE, 0x0114, 0xa0) == 1);
	assert(utk_count(EV_REG_WRITE, 0x0115, 0x05) == 1);
}

/*****************************************************************************
 * Standard handling (s_std)
 ****************************************************************************/

/* Selecting the standard already in use must not touch either chip: this is
 * the path a repeated VIDIOC_S_STD takes. */
static void test_s_std_same_standard(void)
{
	struct gvusb2_vid *dev = utk_vid();

	utk_reset();
	dev = utk_vid();
	dev->standard = V4L2_STD_NTSC_M;

	assert(gvusb2_vidioc_s_std(NULL, NULL, V4L2_STD_NTSC_M) == 0);
	assert(utk_nev == 0);
	assert(dev->standard == V4L2_STD_NTSC_M);
}

/* A real standard change reprograms both chips and narrows the vertical
 * start range to what the new field can hold. */
static void test_s_std_pal(void)
{
	struct gvusb2_vid *dev = utk_vid();

	utk_reset();
	dev = utk_vid();
	dev->standard = V4L2_STD_NTSC_M;
	utk_set_vbi(1);

	assert(gvusb2_vidioc_s_std(NULL, NULL, V4L2_STD_PAL) == 0);
	assert(dev->standard == V4L2_STD_PAL);

	/* TW9910 standard selection, then the full-frame crop both callers
	 * resolve from the VBI control. */
	assert(utk_i2c_written(0x1c) == 0x97);
	assert(utk_i2c_written(0x09) == 0x37);	/* PAL + VBI VACTIVE */
	/* STK1150 crop: VBI fixes the line counts (0x37 lines, high byte
	 * set) and the horizontal window still comes from the controls
	 * (horizontal_start 4 -> 0xa4). */
	assert(utk_count(EV_REG_WRITE, 0x0112, 3) == 1);
	assert(utk_count(EV_REG_WRITE, 0x0116, 0x37) == 1);
	assert(utk_count(EV_REG_WRITE, 0x0117, 0x01) == 1);
	assert(utk_count(EV_REG_WRITE, 0x0114, 0xa4) == 1);
	/* PAL holds fewer lines above active video: 1..3, not 1..4. */
	assert(dev->vertical_start->minimum == 1);
	assert(dev->vertical_start->maximum == 3);

	/* And back to NTSC widens it again. */
	utk_reset();
	dev = utk_vid();
	dev->standard = V4L2_STD_PAL;
	assert(gvusb2_vidioc_s_std(NULL, NULL, V4L2_STD_NTSC_M) == 0);
	assert(dev->vertical_start->maximum == 4);
	assert(utk_i2c_written(0x1c) == 0x17);
}

/* A vertical start that PAL cannot reach is clamped by the range change, and
 * the crop must then use the clamped value. */
static void test_s_std_clamps_vertical_start(void)
{
	struct gvusb2_vid *dev = utk_vid();

	utk_reset();
	dev = utk_vid();
	dev->standard = V4L2_STD_NTSC_M;
	dev->vertical_start->val = 4;

	assert(gvusb2_vidioc_s_std(NULL, NULL, V4L2_STD_PAL) == 0);
	assert(dev->vertical_start->val == 3);
	assert(utk_count(EV_REG_WRITE, 0x0116, 0x23) == 1);	/* 3 + 0x20 */
}

/* While buffers are queued the standard cannot change. */
static void test_s_std_busy(void)
{
	struct gvusb2_vid *dev = utk_vid();

	utk_reset();
	dev = utk_vid();
	dev->standard = V4L2_STD_NTSC_M;
	dev->vb2q.busy = 1;

	assert(gvusb2_vidioc_s_std(NULL, NULL, V4L2_STD_PAL) == -EBUSY);
	assert(dev->standard == V4L2_STD_NTSC_M);
	assert(utk_nev == 0);
}

/* g_std/querystd report what is set and what the device can do. */
static void test_g_std_querystd(void)
{
	struct gvusb2_vid *dev = utk_vid();
	v4l2_std_id std = 0;

	utk_reset();
	dev = utk_vid();
	dev->standard = V4L2_STD_PAL;

	assert(gvusb2_vidioc_g_std(NULL, NULL, &std) == 0);
	assert(std == V4L2_STD_PAL);

	dev->vdev.tvnorms = V4L2_STD_525_60 | V4L2_STD_625_50;
	std = 0;
	assert(gvusb2_vidioc_querystd(NULL, NULL, &std) == 0);
	assert(std == (V4L2_STD_525_60 | V4L2_STD_625_50));
}

/*****************************************************************************
 * Input selection
 ****************************************************************************/

/* Only the two documented inputs exist; the register write preserves the
 * unrelated bits of the chip's input register. */
static void test_input_selection(void)
{
	struct gvusb2_vid *dev = utk_vid();
	struct v4l2_input in;
	unsigned int i;

	utk_reset();
	dev = utk_vid();

	memset(&in, 0, sizeof(in));
	in.index = GVUSB2_INPUT_COMPOSITE;
	assert(gvusb2_vidioc_enum_input(NULL, NULL, &in) == 0);
	assert(strcmp((char *)in.name, "Composite") == 0);
	assert(in.type == V4L2_INPUT_TYPE_CAMERA);

	memset(&in, 0, sizeof(in));
	in.index = GVUSB2_INPUT_SVIDEO;
	assert(gvusb2_vidioc_enum_input(NULL, NULL, &in) == 0);
	assert(strcmp((char *)in.name, "S-Video") == 0);

	memset(&in, 0, sizeof(in));
	in.index = 2;
	assert(gvusb2_vidioc_enum_input(NULL, NULL, &in) == -EINVAL);

	/* S-Video: bits 2 and 4, everything else preserved. */
	utk_chip[0x02] = 0xff;
	assert(gvusb2_vidioc_s_input(NULL, NULL, GVUSB2_INPUT_SVIDEO) == 0);
	assert(utk_i2c_written(0x02) == ((0xff & 0xc3) | 0x14));
	assert(dev->input_num == GVUSB2_INPUT_SVIDEO);

	utk_chip[0x02] = 0xff;
	assert(gvusb2_vidioc_s_input(NULL, NULL, GVUSB2_INPUT_COMPOSITE) == 0);
	assert(utk_i2c_written(0x02) == (0xff & 0xc3));
	assert(dev->input_num == GVUSB2_INPUT_COMPOSITE);

	i = 99;
	assert(gvusb2_vidioc_g_input(NULL, NULL, &i) == 0);
	assert(i == GVUSB2_INPUT_COMPOSITE);

	assert(gvusb2_vidioc_s_input(NULL, NULL, 2) == -EINVAL);
	assert(dev->input_num == GVUSB2_INPUT_COMPOSITE);
}

/* A failed read must not be turned into a register write. */
static void test_input_read_failure(void)
{
	struct gvusb2_vid *dev = utk_vid();
	int ret;

	utk_reset();
	dev = utk_vid();
	utk_fail_i2c_read = 1;

	ret = gvusb2_vidioc_s_input(NULL, NULL, GVUSB2_INPUT_SVIDEO);
	assert(ret == -EIO);
	assert(utk_count(EV_I2C_WRITE, 0x02, UTK_ANY) == 0);
	assert(dev->input_num == 0);
}

/*****************************************************************************
 * Streaming: the crop is (re)programmed on every start
 ****************************************************************************/

static void utk_queue_buf(struct gvusb2_vb *b, struct gvusb2_vid *dev)
{
	memset(b, 0, sizeof(*b));
	b->vb.vb2_buf.vb2_queue = &dev->vb2q;
	gvusb2_vb2_buf_queue(&b->vb.vb2_buf);
}

static void test_start_streaming(void)
{
	struct gvusb2_vb b;
	struct gvusb2_vid *dev = utk_vid();
	int crop_end, sstream, enable, submit;

	utk_reset();
	dev = utk_vid();
	dev->standard = V4L2_STD_PAL;
	dev->sequence = 42;
	/* VBI on: the start must crop for the full frame, not the active
	 * window, so the decoder's VACTIVE register decides it. */
	utk_set_vbi(1);
	utk_queue_buf(&b, dev);

	assert(gvusb2_vb2_start_streaming(&dev->vb2q, 1) == 0);

	assert(dev->sequence == 0);
	assert(utk_submit_calls == 1);
	assert(utk_cancel_calls == 0);
	assert(utk_mutex_locks == 1);
	assert(utk_mutex_unlocks == 1);

	/* The decoder is cropped before the decoder and the capture engine
	 * are started, and the engine is enabled before any URB is
	 * submitted. */
	crop_end = utk_find(0, EV_I2C_WRITE, 0x1c, UTK_ANY);
	sstream = utk_find(0, EV_SSTREAM, 1, UTK_ANY);
	enable = utk_find(0, EV_REG_WRITE, 0x0100, 0xb3);
	submit = utk_find(0, EV_URBS, 1, UTK_ANY);
	assert(crop_end >= 0);
	assert(sstream >= 0);
	assert(enable >= 0);
	assert(submit >= 0);
	assert(crop_end < sstream);
	assert(sstream < enable);
	assert(enable < submit);
	assert(utk_i2c_written(0x1c) == 0x97);	/* PAL, from the standard */
	assert(utk_i2c_written(0x09) == 0x37);	/* PAL + VBI VACTIVE */
	assert(utk_i2c_written(0x0c) >= 0);	/* black level reapplied */
	/* The buffer stays queued and nothing is returned to userspace. */
	assert(!list_empty(&dev->buf_list));
	assert(utk_count(EV_VB2_DONE, UTK_ANY, UTK_ANY) == 0);
}

/* A failed URB submission must undo everything the start did, return the
 * queued buffers as never-started, and drop the lock. */
static void test_start_streaming_failure(void)
{
	struct gvusb2_vb b;
	struct gvusb2_vid *dev = utk_vid();
	int cancel, done, off, on;

	utk_reset();
	dev = utk_vid();
	dev->standard = V4L2_STD_NTSC_M;
	utk_queue_buf(&b, dev);
	utk_submit_ret = -ENOMEM;

	assert(gvusb2_vb2_start_streaming(&dev->vb2q, 1) == -ENOMEM);

	assert(utk_submit_calls == 1);
	assert(utk_cancel_calls == 1);
	assert(utk_mutex_locks == 1);
	assert(utk_mutex_unlocks == 1);

	on = utk_find(0, EV_REG_WRITE, 0x0100, 0xb3);
	cancel = utk_find(0, EV_URBS, 0, UTK_ANY);
	off = utk_find(0, EV_REG_WRITE, 0x0100, 0x33);
	assert(on >= 0);
	assert(cancel >= 0);
	assert(off >= 0);
	/* URBs must be dead before the capture engine is stopped. */
	assert(cancel < off);

	/* The decoder is told to stop as well. */
	assert(utk_find(off, EV_SSTREAM, 0, UTK_ANY) >= 0);

	/* The buffers that never started come back QUEUED, not as errors:
	 * vb2 does not call stop_streaming after a failed start, so returning
	 * them as ERROR would tell userspace a frame was lost. */
	done = utk_find(0, EV_VB2_DONE, UTK_ANY, UTK_ANY);
	assert(done >= 0);
	assert(utk_ev[done].a == VB2_BUF_STATE_QUEUED);
	assert(list_empty(&dev->buf_list));
}

/* Stopping streams hands the in-flight buffers back as errors and resets the
 * capture engine. */
static void test_stop_streaming(void)
{
	struct gvusb2_vb b;
	struct gvusb2_vid *dev = utk_vid();
	int done;

	utk_reset();
	dev = utk_vid();
	utk_queue_buf(&b, dev);

	gvusb2_vb2_stop_streaming(&dev->vb2q);

	assert(utk_cancel_calls == 1);
	assert(utk_mutex_locks == 1);
	assert(utk_mutex_unlocks == 1);
	assert(utk_count(EV_REG_WRITE, 0x0100, 0x33) == 1);
	assert(utk_count(EV_REG_WRITE, 0x0103, 0x00) == 1);
	assert(utk_count(EV_SSTREAM, 0, UTK_ANY) == 1);

	done = utk_find(0, EV_VB2_DONE, UTK_ANY, UTK_ANY);
	assert(done >= 0);
	assert(utk_ev[done].a == VB2_BUF_STATE_ERROR);
	assert(list_empty(&dev->buf_list));
	/* No i2c traffic: stopping must not reprogram the decoder crop. */
	assert(utk_count(EV_I2C_WRITE, UTK_ANY, UTK_ANY) == 0);
}

/* An interrupted lock must not leave the queue half-started, and must not
 * unlock a mutex it never took. */
static void test_start_streaming_lock_failure(void)
{
	struct gvusb2_vid *dev = utk_vid();

	utk_reset();
	dev = utk_vid();
	utk_mutex_fail = 1;

	assert(gvusb2_vb2_start_streaming(&dev->vb2q, 1) == -ERESTARTSYS);
	assert(utk_mutex_locks == 0);
	assert(utk_mutex_unlocks == 0);
	assert(utk_submit_calls == 0);
	assert(utk_nev == 0);
}

/*****************************************************************************
 * vb2 setup and video registration
 ****************************************************************************/

/* gvusb2_vb2_setup() is the contract with vb2: a capture queue of the
 * driver's private buffer type, all three IO modes, the driver's own lock and
 * its ops table. */
static void test_vb2_setup(void)
{
	struct gvusb2_vid *dev = utk_vid();

	utk_reset();
	dev = utk_vid();
	assert(gvusb2_vb2_setup(dev) == 0);

	assert(dev->vb2q.type == V4L2_BUF_TYPE_VIDEO_CAPTURE);
	assert(dev->vb2q.io_modes == (VB2_MMAP | VB2_READ | VB2_USERPTR));
	assert(dev->vb2q.drv_priv == dev);
	assert(dev->vb2q.buf_struct_size == sizeof(struct gvusb2_vb));
	assert(dev->vb2q.ops->queue_setup == gvusb2_vb2_queue_setup);
	assert(dev->vb2q.ops->buf_queue == gvusb2_vb2_buf_queue);
	assert(dev->vb2q.ops->start_streaming == gvusb2_vb2_start_streaming);
	assert(dev->vb2q.ops->stop_streaming == gvusb2_vb2_stop_streaming);
	assert(dev->vb2q.mem_ops == &vb2_vmalloc_memops);
	assert(dev->vb2q.lock == &dev->vb2q_lock);
	assert(dev->vb2q.timestamp_flags ==
	       V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC);
	/* vb2 calls buf_queue against this list; it must start empty */
	assert(list_empty(&dev->buf_list));
}

/* Registration wires the device up and puts the controls at their defaults. */
static void test_v4l2_register(void)
{
	struct gvusb2_vid *dev = utk_vid();

	utk_reset();
	dev = utk_vid();
	assert(gvusb2_v4l2_register(dev) == 0);

	assert(dev->v4l2_dev.ctrl_handler == &dev->ctrl_handler);
	assert(dev->vbi_capture != NULL);
	assert(dev->black_pedestal != NULL);
	assert(dev->vertical_start != NULL);
	assert(dev->horizontal_start != NULL);
	/* Defaults from the driver's tables. */
	assert(dev->vbi_capture->val == 0);
	assert(dev->black_pedestal->val == 0);
	assert(dev->vertical_start->val == 2);
	assert(dev->horizontal_start->val == 4);
	assert(dev->vertical_start->minimum == 1);
	assert(dev->vertical_start->maximum == 4);
	/* STK1150 left in double-word mode: 0x08 in the register, set through a
	 * 0x08 read-modify-write mask. */
	{
		int mask_ev = utk_find(0, EV_REG_MASK, 0x05f0, 0x08);

		assert(mask_ev >= 0);
		assert(utk_ev[mask_ev].mask == 0x08);
	}
	assert(dev->sd_tw9910 == &utk_subdev);
}

/* video_register installs the queue, the lock and the NTSC default, and the
 * standard then decides the geometry. */
static void test_video_register(void)
{
	struct gvusb2_vid *dev = utk_vid();
	struct v4l2_format f;

	utk_reset();
	dev = utk_vid();
	assert(gvusb2_v4l2_register(dev) == 0);
	assert(gvusb2_video_register(dev) == 0);

	assert(dev->standard == V4L2_STD_NTSC_M);
	assert(dev->vdev.queue == &dev->vb2q);
	assert(dev->vdev.lock == &dev->v4l2_lock);
	assert(dev->vdev.v4l2_dev == &dev->v4l2_dev);
	assert(dev->vdev.drvdata == dev);
	assert(dev->vdev.device_caps & V4L2_CAP_VIDEO_CAPTURE);
	assert(dev->vdev.device_caps & V4L2_CAP_STREAMING);
	assert(dev->vdev.device_caps & V4L2_CAP_READWRITE);
	assert((dev->vdev.tvnorms & V4L2_STD_525_60) != 0);
	assert((dev->vdev.tvnorms & V4L2_STD_625_50) != 0);

	memset(&f, 0, sizeof(f));
	assert(gvusb2_vidioc_g_fmt_vid_cap(NULL, NULL, &f) == 0);
	assert(f.fmt.pix.height == 486);
}

int main(void)
{
	test_resolution_geometries();
	test_resolution_null_outputs();
	test_resolution_standard_classification();

	test_try_fmt_normalises();
	test_try_fmt_follows_geometry();
	test_g_fmt_matches_try_fmt();
	test_unsupported_fourcc_is_coerced();
	test_enum_fmt();
	test_s_fmt_busy();

	test_queue_setup_query();
	test_queue_setup_configure();

	test_cropping_write_sequence();
	test_cropping_vbi_boundary();
	test_cropping_standard_classification();

	test_black_level_rmw();
	test_black_level_read_failure();
	test_black_pedestal_control();

	test_ctrl_brightness_hue();
	test_ctrl_direct_registers();
	test_ctrl_autogain_rmw();
	test_ctrl_autogain_read_failure();
	test_ctrl_unknown_id();
	test_vbi_control_flags();

	test_stk1150_cropping();
	test_start_controls();

	test_s_std_same_standard();
	test_s_std_pal();
	test_s_std_clamps_vertical_start();
	test_s_std_busy();
	test_g_std_querystd();

	test_input_selection();
	test_input_read_failure();

	test_start_streaming();
	test_start_streaming_failure();
	test_stop_streaming();
	test_start_streaming_lock_failure();

	test_vb2_setup();
	test_v4l2_register();
	test_video_register();

	printf("test_v4l2: all tests passed\n");
	return 0;
}
