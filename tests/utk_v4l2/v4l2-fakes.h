/* SPDX-License-Identifier: GPL-2.0-only OR BSD-3-Clause */
/*
 * Lane-private fake kernel media stack for the v4l2 lane
 * (tests/test_v4l2.c, wired in tests/run-tests.sh with
 * `-Iutk -Iutk_v4l2`).
 *
 * The shared fakes in tests/utk/ were written for the core/i2c lane and are
 * deliberately empty (struct video_device is `{ int unused; }` there). This
 * lane needs those structs to have the members gvusb2-v4l2.c pokes at, and
 * -Iutk is searched first, so the shared header would win on name. Defining
 * its include guard first turns the shared body into a no-op and lets the
 * lane-private definitions below stand. Nothing in tests/utk/ is modified.
 *
 * Only kernel-internal headers are faked. The V4L2 *UAPI* header is the real
 * one: the constants this lane's tests probe (V4L2_STD_625_50, the pixel
 * formats, the geometry limits) have to be the ABI values, not guesses.
 *
 * The fake function bodies live in tests/test_v4l2.c; they are extern here so
 * that unused-fake warnings cannot fire and so that each fake can hold the
 * state the assertions read (an ordered event log).
 */
#ifndef UTK_V4L2_FAKES_H
#define UTK_V4L2_FAKES_H

#define UTK_MEDIA_V4L2_DEVICE_H
#define UTK_MEDIA_V4L2_CTRLS_H
#define UTK_MEDIA_VIDEOBUF2_CORE_H
#define UTK_MEDIA_VIDEOBUF2_V4L2_H

#include <stddef.h>
#include <sys/types.h>
#include <time.h>			/* struct timespec, for v4l2_event */
#include <linux/module.h>	/* utk.h: u8/u32, strscpy, list_head, mutex */
#include <linux/videodev2.h>	/* real UAPI */
#include <linux/i2c.h>		/* utk fake: struct i2c_client */
#include <linux/usb.h>		/* utk fake: struct usb_device */

typedef long long loff_t;

struct file;
struct v4l2_fh;
struct v4l2_ctrl;

#ifndef ERESTARTSYS
#define ERESTARTSYS 512
#endif

#define container_of(ptr, type, member) \
	((type *)(void *)((char *)(ptr) - offsetof(type, member)))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

/* ---- kernel/device bits ------------------------------------------------ */

#define dev_warn(dev, fmt, ...) utk_dev_warn(fmt, ##__VA_ARGS__)
void utk_dev_warn(const char *fmt, ...);

/* ---- list_head --------------------------------------------------------- */

#define INIT_LIST_HEAD(ptr) do { \
	(ptr)->next = (ptr); (ptr)->prev = (ptr); \
} while (0)

#define list_empty(head) ((head)->next == (head))

#define list_first_entry(ptr, type, member) \
	container_of((ptr)->next, type, member)

static inline void list_add_tail(struct list_head *n, struct list_head *head)
{
	n->prev = head->prev;
	n->next = head;
	head->prev->next = n;
	head->prev = n;
}

static inline void list_del(struct list_head *entry)
{
	entry->prev->next = entry->next;
	entry->next->prev = entry->prev;
	entry->next = entry->prev = entry;
}

/* ---- mutex / spinlock: single-threaded host, only the count matters ----- */

extern int utk_mutex_locks;
extern int utk_mutex_unlocks;

void mutex_init(struct mutex *m);
int mutex_lock_interruptible(struct mutex *m);
void mutex_lock(struct mutex *m);
void mutex_unlock(struct mutex *m);

#define spin_lock_irqsave(lock, flags) do { (flags) = 0; (void)(lock); } while (0)
#define spin_unlock_irqrestore(lock, flags) do { (void)(lock); (void)(flags); } while (0)

/* ---- media/v4l2-ctrls.h ----------------------------------------------- */

struct v4l2_ctrl_handler;

struct v4l2_ctrl_ops {
	int (*s_ctrl)(struct v4l2_ctrl *ctrl);
};

struct v4l2_ctrl {
	const struct v4l2_ctrl_ops *ops;
	struct v4l2_ctrl_handler *handler;
	u32 id;
	s64 minimum;
	s64 maximum;
	u64 step;
	s64 default_value;
	s32 val;
	u32 flags;
	const char *name;
};

struct v4l2_ctrl_config {
	const struct v4l2_ctrl_ops *ops;
	u32 id;
	const char *name;
	u32 type;
	u32 flags;
	s64 min;
	s64 max;
	u64 step;
	s64 def;
};

struct v4l2_ctrl_handler {
	const struct v4l2_ctrl_ops *ops;
	struct v4l2_ctrl *ctrls[16];
	int n_ctrls;
	int error;
};

int v4l2_ctrl_handler_init(struct v4l2_ctrl_handler *hdl, unsigned int n);
void v4l2_ctrl_handler_setup(struct v4l2_ctrl_handler *hdl);
void v4l2_ctrl_handler_free(struct v4l2_ctrl_handler *hdl);
struct v4l2_ctrl *v4l2_ctrl_new_std(struct v4l2_ctrl_handler *hdl,
	const struct v4l2_ctrl_ops *ops, u32 id, s64 min, s64 max, u64 step,
	s64 def);
struct v4l2_ctrl *v4l2_ctrl_new_custom(struct v4l2_ctrl_handler *hdl,
	const struct v4l2_ctrl_config *cfg, void *priv);
s32 v4l2_ctrl_g_ctrl(struct v4l2_ctrl *ctrl);
int v4l2_ctrl_modify_range(struct v4l2_ctrl *ctrl, s64 min, s64 max,
	u64 step, s64 def);
int v4l2_ctrl_log_status(struct file *file, void *priv);
int v4l2_ctrl_subscribe_event(struct v4l2_fh *fh,
	const struct v4l2_event_subscription *sub);
int v4l2_event_unsubscribe(struct v4l2_fh *fh,
	const struct v4l2_event_subscription *sub);

/* ---- media/v4l2-dev.h: kernel-internal, absent from the UAPI ---------- */

enum vfl_devnode_type {
	VFL_TYPE_VIDEO = 0,
	VFL_TYPE_VBI,
	VFL_TYPE_RADIO,
	VFL_TYPE_SUBDEV,
	VFL_TYPE_SDR,
	VFL_TYPE_TOUCH,
};

enum vfl_dir {
	VFL_DIR_RX = 0,
	VFL_DIR_TX,
	VFL_DIR_M2M,
};

struct v4l2_fh;

struct v4l2_file_operations {
	struct module *owner;
	int (*open)(struct file *file);
	int (*release)(struct file *file);
	ssize_t (*read)(struct file *file, char *buf, size_t count,
		loff_t *ppos);
	unsigned int (*poll)(struct file *file, void *wait);
	int (*mmap)(struct file *file, void *vma);
	long (*unlocked_ioctl)(struct file *file, unsigned int cmd,
		unsigned long arg);
};

struct v4l2_ioctl_ops {
	int (*vidioc_querycap)(struct file *file, void *priv,
		struct v4l2_capability *cap);
	int (*vidioc_enum_input)(struct file *file, void *priv,
		struct v4l2_input *in);
	int (*vidioc_g_input)(struct file *file, void *priv, unsigned int *i);
	int (*vidioc_s_input)(struct file *file, void *priv, unsigned int i);
	int (*vidioc_querystd)(struct file *file, void *priv,
		v4l2_std_id *std);
	int (*vidioc_g_std)(struct file *file, void *priv, v4l2_std_id *std);
	int (*vidioc_s_std)(struct file *file, void *priv, v4l2_std_id std);
	int (*vidioc_enum_fmt_vid_cap)(struct file *file, void *priv,
		struct v4l2_fmtdesc *f);
	int (*vidioc_g_fmt_vid_cap)(struct file *file, void *priv,
		struct v4l2_format *f);
	int (*vidioc_s_fmt_vid_cap)(struct file *file, void *priv,
		struct v4l2_format *f);
	int (*vidioc_try_fmt_vid_cap)(struct file *file, void *priv,
		struct v4l2_format *f);
	int (*vidioc_reqbufs)(struct file *file, void *priv,
		struct v4l2_requestbuffers *p);
	int (*vidioc_querybuf)(struct file *file, void *priv,
		struct v4l2_buffer *p);
	int (*vidioc_qbuf)(struct file *file, void *priv, struct v4l2_buffer *p);
	int (*vidioc_dqbuf)(struct file *file, void *priv, struct v4l2_buffer *p);
	int (*vidioc_create_bufs)(struct file *file, void *priv,
		struct v4l2_create_buffers *p);
	int (*vidioc_streamon)(struct file *file, void *priv,
		enum v4l2_buf_type i);
	int (*vidioc_streamoff)(struct file *file, void *priv,
		enum v4l2_buf_type i);
	int (*vidioc_expbuf)(struct file *file, void *priv,
		struct v4l2_exportbuffer *p);
	int (*vidioc_log_status)(struct file *file, void *priv);
	int (*vidioc_subscribe_event)(struct v4l2_fh *fh,
		const struct v4l2_event_subscription *sub);
	int (*vidioc_unsubscribe_event)(struct v4l2_fh *fh,
		const struct v4l2_event_subscription *sub);
};

/* ---- media/v4l2-device.h ---------------------------------------------- */

struct v4l2_device;
struct v4l2_subdev {
	struct v4l2_device *v4l2_dev;
	int stream_on;		/* last s_stream argument seen */
};

struct v4l2_device {
	struct device *dev;
	void (*release)(struct v4l2_device *v4l2_dev);
	struct v4l2_ctrl_handler *ctrl_handler;
};

struct video_device {
	const char *name;
	u32 device_caps;
	u64 tvnorms;
	enum vfl_dir vfl_dir;
	const struct v4l2_file_operations *fops;
	const struct v4l2_ioctl_ops *ioctl_ops;
	void (*release)(struct video_device *vdev);

	struct v4l2_device *v4l2_dev;
	struct vb2_queue *queue;
	struct mutex *lock;
	void *drvdata;
};

void *video_drvdata(struct file *file);
void video_set_drvdata(struct video_device *vdev, void *data);
int video_register_device(struct video_device *vdev,
	enum vfl_devnode_type type, int nr);
void video_device_release_empty(struct video_device *vdev);

/* Only ever called as v4l2_device_call_all(dev, 0, video, s_stream, on) here. */
#define v4l2_device_call_all(v4l2_dev, pad, o, f, on) utk_s_stream(on)
void utk_s_stream(int on);

int v4l2_device_register(struct device *dev, struct v4l2_device *v4l2_dev);
void v4l2_device_unregister(struct v4l2_device *v4l2_dev);

struct i2c_board_info {
	char type[20];
	unsigned short addr;
	void *platform_data;
};

struct tw9910_video_info {
	unsigned int buswidth;
	unsigned int mpout;
};

#define TW9910_MPO_FIELD 0

struct v4l2_subdev *v4l2_i2c_new_subdev_board(struct v4l2_device *v4l2_dev,
	struct i2c_adapter *adapter, struct i2c_board_info *info,
	const unsigned short *probe_addrs);

/* ---- media/videobuf2-core.h ------------------------------------------- */

#define VB2_MMAP	0x00000001
#define VB2_READ	0x00000002
#define VB2_USERPTR	0x00000004

enum vb2_buffer_state {
	VB2_BUF_STATE_ERROR = 0,
	VB2_BUF_STATE_QUEUED,
	VB2_BUF_STATE_ACTIVE,
	VB2_BUF_STATE_DONE,
};

struct vb2_mem_ops { int unused; };

struct vb2_buffer {
	struct vb2_queue *vb2_queue;
	unsigned int index;
	unsigned int num_planes;
};

struct vb2_ops {
	int (*queue_setup)(struct vb2_queue *q, unsigned int *nbuffers,
		unsigned int *nplanes, unsigned int sizes[],
		struct device *alloc_devs[]);
	void (*buf_queue)(struct vb2_buffer *vb);
	int (*start_streaming)(struct vb2_queue *q, unsigned int count);
	void (*stop_streaming)(struct vb2_queue *q);
};

struct vb2_queue {
	enum v4l2_buf_type type;
	unsigned int io_modes;
	void *drv_priv;
	unsigned int buf_struct_size;
	const struct vb2_ops *ops;
	const struct vb2_mem_ops *mem_ops;
	struct mutex *lock;
	u32 timestamp_flags;
	int busy;
};

#define vb2_get_drv_priv(q) ((q)->drv_priv)

int vb2_queue_init(struct vb2_queue *q);
int vb2_is_busy(struct vb2_queue *q);
void vb2_buffer_done(struct vb2_buffer *vb, enum vb2_buffer_state state);
extern const struct vb2_mem_ops vb2_vmalloc_memops;

/* ---- media/videobuf2-v4l2.h ------------------------------------------- */

struct vb2_v4l2_buffer {
	struct vb2_buffer vb2_buf;
	u32 flags;
	u32 field;
};

#define to_vb2_v4l2_buffer(vb) \
	container_of(vb, struct vb2_v4l2_buffer, vb2_buf)

/* vb2 fops/ioctl helpers: referenced by the driver's ops tables only, so the
 * signatures just have to be plausible. */
int v4l2_fh_open(struct file *file);
int vb2_fop_release(struct file *file);
ssize_t vb2_fop_read(struct file *file, char *buf, size_t count,
	loff_t *ppos);
unsigned int vb2_fop_poll(struct file *file, void *wait);
int vb2_fop_mmap(struct file *file, void *vma);
long video_ioctl2(struct file *file, unsigned int cmd, unsigned long arg);

int vb2_ioctl_reqbufs(struct file *file, void *priv,
	struct v4l2_requestbuffers *p);
int vb2_ioctl_querybuf(struct file *file, void *priv, struct v4l2_buffer *p);
int vb2_ioctl_qbuf(struct file *file, void *priv, struct v4l2_buffer *p);
int vb2_ioctl_dqbuf(struct file *file, void *priv, struct v4l2_buffer *p);
int vb2_ioctl_create_bufs(struct file *file, void *priv,
	struct v4l2_create_buffers *p);
int vb2_ioctl_streamon(struct file *file, void *priv,
	enum v4l2_buf_type i);
int vb2_ioctl_streamoff(struct file *file, void *priv,
	enum v4l2_buf_type i);
int vb2_ioctl_expbuf(struct file *file, void *priv,
	struct v4l2_exportbuffer *p);

/* ---- kernel helpers the driver calls that utk/ does not declare ------- */

int usb_make_path(struct usb_device *udev, unsigned char *buf, size_t size);
s32 i2c_smbus_read_byte_data(struct i2c_client *client, u8 command);
s32 i2c_smbus_write_byte_data(struct i2c_client *client, u8 command, u8 value);

#endif /* UTK_V4L2_FAKES_H */
