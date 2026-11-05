/* Host-side tests for the gvusb2-vid.c lane: URB allocation/teardown, the
 * isochronous completion path, the frame-assembly bookkeeping it drives, the
 * endpoint scan in probe, and the probe unwind ladder.
 *
 * The real gvusb2-vid.c is #included so its statics are reachable; the USB,
 * vb2 and cross-lane entry points it needs are faked in utk_vid/utk_vid.h.
 * No hardware, no module load, no sleeping: usleep_range() is recorded, not
 * slept through. See utk_vid/utk_vid.h for what is faked and why.
 *
 * Geometry used throughout is the NTSC 720x486 capture mode the driver
 * advertises: 1440 bytes per line of UYVY, 243 source lines per field, and a
 * 699840-byte plane (that is what gvusb2_vb2_queue_setup asks vb2 for). The
 * two fields are interleaved, so one source line occupies two plane lines.
 */

#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "utk_vid/utk_vid.h"
#include "../gvusb2-vid.c"

#define WIDTH		720
#define BPL		(WIDTH * 2)		/* bytes per source line */
#define LINES_PER_FIELD	243
#define PLANE		(WIDTH * 486 * 2)	/* NTSC frame plane */
#define FIELD_BYTES	(LINES_PER_FIELD * 2 * BPL)
#define GUARD		64

static void utk_on_alarm(int sig)
{
	(void)sig;
	fprintf(stderr, "test_vid: FAILED (test hung)\n");
	_exit(1);
}

static void utkvid_reset(void)
{
	memset(&utkvid, 0, sizeof(utkvid));
	utkvid.width = WIDTH;
}

static struct gvusb2_vid *vid_new(void)
{
	struct gvusb2_vid *dev = calloc(1, sizeof(*dev));

	assert(dev != NULL);
	INIT_LIST_HEAD(&dev->buf_list);
	dev->gv.udev = &utkvid.udev;
	return dev;
}

static void vid_free(struct gvusb2_vid *dev)
{
	free(dev);
}

/* A capture buffer whose plane is followed by a poisoned guard region, so a
 * copy that runs past the plane is caught rather than silently tolerated. */
struct utk_plane {
	struct gvusb2_vb *vb;
	unsigned char *base;
	unsigned int len;
};

static void plane_new(struct utk_plane *p, unsigned int len)
{
	p->len = len;
	p->base = malloc(len + GUARD);
	assert(p->base != NULL);
	memset(p->base, 0, len);
	memset(p->base + len, 0x5a, GUARD);

	p->vb = calloc(1, sizeof(*p->vb));
	assert(p->vb != NULL);
	p->vb->vb.vb2_buf.planes[0].length = len;
	p->vb->vb.vb2_buf.planes[0].vaddr = p->base;
}

static void plane_free(struct utk_plane *p)
{
	free(p->base);
	free(p->vb);
}

static void plane_assert_guard(const struct utk_plane *p)
{
	unsigned int i;

	for (i = p->len; i < p->len + GUARD; i++)
		assert(p->base[i] == 0x5a);
}

static unsigned int free_count(void *ptr)
{
	unsigned int i, n = 0;

	for (i = 0; i < utkvid.kfree_calls && i < UTKVID_MAX_TRACE; i++)
		if (utkvid.kfree_ptr[i] == ptr)
			n++;
	return n;
}

static unsigned int alloc_count(void *ptr)
{
	unsigned int i, n = 0;

	for (i = 0; i < utkvid.kzalloc_calls && i < UTKVID_MAX_TRACE; i++)
		if (utkvid.kzalloc_ptr[i] == ptr)
			n++;
	return n;
}

/* The kzalloc'd driver struct: the only allocation of this size in probe. */
static void *probe_dev_alloc(void)
{
	unsigned int i;

	for (i = 0; i < utkvid.kzalloc_calls && i < UTKVID_MAX_TRACE; i++)
		if (utkvid.kzalloc_size[i] == sizeof(struct gvusb2_vid))
			return utkvid.kzalloc_ptr[i];
	return NULL;
}

/* ------------------------------------------------------------------ */
/* URB allocation / teardown                                           */
/* ------------------------------------------------------------------ */

/* Every packet the URB was allocated for must be described inside the buffer
 * it was given: the last descriptor has to end exactly at the end of the
 * transfer buffer, or the HCD writes past the allocation. */
static void test_urb_layout(void)
{
	struct gvusb2_vid *dev = vid_new();
	unsigned int i, j;
	const unsigned int pkt = GVUSB2_MAX_VIDEO_PACKET_SIZE;

	utkvid_reset();
	assert(gvusb2_vid_allocate_urbs(dev) == 0);
	assert(utkvid.urb_alloc_calls == GVUSB2_NUM_URBS);
	assert(utkvid.kzalloc_calls == GVUSB2_NUM_URBS);

	for (i = 0; i < GVUSB2_NUM_URBS; i++) {
		struct urb *urb = dev->urbs[i];

		assert(urb != NULL);
		assert(urb->dev == &utkvid.udev);
		assert((urb->pipe & 0xff) == 0x02);	/* video endpoint */
		assert((urb->pipe & 0x8000) != 0);	/* receive pipe */
		assert(urb->number_of_packets == GVUSB2_NUM_ISOCH_PACKETS);
		assert(urb->interval == 1);
		assert(urb->start_frame == 0);
		assert(urb->transfer_flags == URB_ISO_ASAP);
		assert(urb->complete == gvusb2_vid_isoc_irq);
		assert(urb->context == dev);

		assert(urb->transfer_buffer != NULL);
		assert(urb->transfer_buffer_length ==
			GVUSB2_NUM_ISOCH_PACKETS * pkt);
		assert(utkvid.kzalloc_size[i] == urb->transfer_buffer_length);

		/* kzalloc, not kmalloc: a reused transfer buffer must not leak
		 * the previous stream's bytes into the first field. */
		assert(((u8 *)urb->transfer_buffer)[0] == 0);
		assert(((u8 *)urb->transfer_buffer)[urb->transfer_buffer_length - 1] == 0);

		for (j = 0; j < (unsigned int)urb->number_of_packets; j++) {
			assert(urb->iso_frame_desc[j].offset == j * pkt);
			assert(urb->iso_frame_desc[j].length == pkt);
		}
		assert(urb->iso_frame_desc[urb->number_of_packets - 1].offset +
			urb->iso_frame_desc[urb->number_of_packets - 1].length ==
			urb->transfer_buffer_length);
	}

	/* four independent urbs and buffers */
	for (i = 0; i < GVUSB2_NUM_URBS; i++) {
		for (j = i + 1; j < GVUSB2_NUM_URBS; j++) {
			assert(dev->urbs[i] != dev->urbs[j]);
			assert(dev->urbs[i]->transfer_buffer !=
				dev->urbs[j]->transfer_buffer);
		}
	}

	gvusb2_vid_free_urbs(dev);
	vid_free(dev);
}

/* usb_alloc_urb() failing on call N must leave nothing allocated: no urb in
 * the array, and every earlier urb and its buffer released exactly once. */
static void test_urb_alloc_failure_unwinds(void)
{
	unsigned int n, i;

	for (n = 1; n <= GVUSB2_NUM_URBS; n++) {
		struct gvusb2_vid *dev = vid_new();

		utkvid_reset();
		utkvid.urb_alloc_fail_at = n;
		assert(gvusb2_vid_allocate_urbs(dev) == -ENOMEM);

		assert(utkvid.urb_alloc_calls == n);
		for (i = 0; i < GVUSB2_NUM_URBS; i++)
			assert(dev->urbs[i] == NULL);

		assert(utkvid.urb_free_calls == n - 1);
		assert(utkvid.kzalloc_calls == n - 1);
		for (i = 0; i + 1 < n; i++) {
			assert(utkvid.kzalloc_ptr[i] != NULL);
			assert(free_count(utkvid.kzalloc_ptr[i]) == 1);
		}
		vid_free(dev);
	}
}

/* Same for the transfer buffer: the urb allocated on the failing call must be
 * freed too, not just the ones that already had a buffer. */
static void test_urb_buffer_alloc_failure_unwinds(void)
{
	unsigned int n, i;

	for (n = 1; n <= GVUSB2_NUM_URBS; n++) {
		struct gvusb2_vid *dev = vid_new();

		utkvid_reset();
		utkvid.kzalloc_fail_at = n;
		assert(gvusb2_vid_allocate_urbs(dev) == -ENOMEM);

		assert(utkvid.kzalloc_calls == n);
		assert(utkvid.kzalloc_ptr[n - 1] == NULL);
		assert(utkvid.urb_free_calls == n);
		for (i = 0; i < GVUSB2_NUM_URBS; i++)
			assert(dev->urbs[i] == NULL);
		for (i = 0; i + 1 < n; i++) {
			assert(utkvid.kzalloc_ptr[i] != NULL);
			assert(free_count(utkvid.kzalloc_ptr[i]) == 1);
		}
		vid_free(dev);
	}
}

/* free_urbs() must release buffer and urb and clear the slot, so a second
 * teardown (disconnect frees, release frees again) cannot double free. */
static void test_free_urbs_releases_and_nulls(void)
{
	struct gvusb2_vid *dev = vid_new();
	unsigned int i;

	utkvid_reset();
	assert(gvusb2_vid_allocate_urbs(dev) == 0);
	gvusb2_vid_free_urbs(dev);

	assert(utkvid.kfree_calls == GVUSB2_NUM_URBS);
	assert(utkvid.urb_free_calls == GVUSB2_NUM_URBS);
	for (i = 0; i < GVUSB2_NUM_URBS; i++) {
		assert(dev->urbs[i] == NULL);
		assert(utkvid.kzalloc_ptr[i] != NULL);
		assert(free_count(utkvid.kzalloc_ptr[i]) == 1);
		assert(utkvid.urb_freed[i] != NULL);
	}

	/* idempotent: nothing left to free, nothing freed again */
	gvusb2_vid_free_urbs(dev);
	assert(utkvid.kfree_calls == GVUSB2_NUM_URBS);
	assert(utkvid.urb_free_calls == GVUSB2_NUM_URBS);

	vid_free(dev);
}

/* A device whose URBs were never allocated must tear down silently. */
static void test_free_urbs_without_allocation(void)
{
	struct gvusb2_vid *dev = vid_new();

	utkvid_reset();
	gvusb2_vid_free_urbs(dev);
	assert(utkvid.kfree_calls == 0);
	assert(utkvid.urb_free_calls == 0);

	vid_free(dev);
}

/* ------------------------------------------------------------------ */
/* URB submission / cancellation                                       */
/* ------------------------------------------------------------------ */

static void test_submit_urbs_all(void)
{
	struct gvusb2_vid *dev = vid_new();
	unsigned int i;

	utkvid_reset();
	assert(gvusb2_vid_allocate_urbs(dev) == 0);

	assert(gvusb2_vid_submit_urbs(dev) == 0);
	assert(utkvid.submit_calls == GVUSB2_NUM_URBS);
	for (i = 0; i < GVUSB2_NUM_URBS; i++)
		assert(utkvid.submitted[i] == dev->urbs[i]);

	gvusb2_vid_free_urbs(dev);
	vid_free(dev);
}

/* A failed submit must be reported and stop the loop. The caller
 * (start_streaming) then cancels what did submit, so swallowing the error or
 * carrying on would leave URBs streaming with nobody owning them. */
static void test_submit_urbs_stops_at_first_failure(void)
{
	struct gvusb2_vid *dev = vid_new();

	utkvid_reset();
	assert(gvusb2_vid_allocate_urbs(dev) == 0);
	utkvid.submit_fail_at = 3;
	utkvid.submit_errno = -EPIPE;

	assert(gvusb2_vid_submit_urbs(dev) == -EPIPE);
	assert(utkvid.submit_calls == 3);
	assert(utkvid.submitted[0] == dev->urbs[0]);
	assert(utkvid.submitted[1] == dev->urbs[1]);
	assert(utkvid.submitted[2] == dev->urbs[2]);

	/* the unwind the caller performs: every urb is killed, submitted or not */
	gvusb2_vid_cancel_urbs(dev);
	assert(utkvid.kill_calls == GVUSB2_NUM_URBS);
	assert(utkvid.killed[3] == dev->urbs[3]);

	gvusb2_vid_free_urbs(dev);
	vid_free(dev);
}

static void test_cancel_urbs_kills_each(void)
{
	struct gvusb2_vid *dev = vid_new();
	unsigned int i;

	utkvid_reset();
	assert(gvusb2_vid_allocate_urbs(dev) == 0);

	gvusb2_vid_cancel_urbs(dev);
	assert(utkvid.kill_calls == GVUSB2_NUM_URBS);
	for (i = 0; i < GVUSB2_NUM_URBS; i++)
		assert(utkvid.killed[i] == dev->urbs[i]);

	gvusb2_vid_free_urbs(dev);
	vid_free(dev);
}

/* ------------------------------------------------------------------ */
/* buffer queue                                                        */
/* ------------------------------------------------------------------ */

/* Buffers are handed out in the order userspace queued them, and a popped
 * buffer leaves the list (a buffer handed out twice would be written by two
 * fields at once). */
static void test_next_buffer_fifo(void)
{
	struct gvusb2_vid *dev = vid_new();
	struct gvusb2_vb a, b;

	utkvid_reset();
	memset(&a, 0, sizeof(a));
	memset(&b, 0, sizeof(b));

	assert(gvusb2_vid_next_buffer(dev) == NULL);

	list_add_tail(&a.list, &dev->buf_list);
	list_add_tail(&b.list, &dev->buf_list);

	assert(gvusb2_vid_next_buffer(dev) == &a);
	assert(!list_empty(&dev->buf_list));
	assert(gvusb2_vid_next_buffer(dev) == &b);
	assert(list_empty(&dev->buf_list));
	assert(gvusb2_vid_next_buffer(dev) == NULL);

	vid_free(dev);
}

/* ------------------------------------------------------------------ */
/* frame assembly (the part vid.c adds on top of gvusb2-assemble.h)    */
/* ------------------------------------------------------------------ */

/* Source lines land on every other plane line, and the position carries over
 * from one URB to the next. */
static void test_copy_video_interleaves_and_carries_state(void)
{
	struct gvusb2_vid *dev = vid_new();
	struct utk_plane p;
	u8 src[4096];
	unsigned int i;

	utkvid_reset();
	plane_new(&p, PLANE);
	dev->current_buf = p.vb;

	for (i = 0; i < sizeof(src); i++)
		src[i] = (u8)(i * 7 + 1);

	gvusb2_vid_copy_video(dev, src, BPL);
	assert(p.vb->written == BPL);
	assert(p.vb->buf_pos == 2 * BPL);	/* the pair's second line is skipped */
	assert(p.vb->line_pos == 0);
	assert(utkvid.warn_count == 0);

	gvusb2_vid_copy_video(dev, src, 1000);
	assert(p.vb->written == 2 * BPL + 1000);
	assert(p.vb->buf_pos == 2 * BPL + 1000);
	assert(p.vb->line_pos == 1000);

	/* line 0 of the pair at offset 0, line 1 at 2*BPL */
	assert(memcmp(p.base, src, BPL) == 0);
	assert(memcmp(p.base + 2 * BPL, src, 1000) == 0);
	assert(p.base[BPL] == 0);	/* the skipped line stays untouched */
	plane_assert_guard(&p);

	plane_free(&p);
	vid_free(dev);
}

/* A field that does not fit is truncated at the plane boundary: the guard
 * region proves nothing is written past the buffer vb2 handed over. */
static void test_copy_video_truncates_at_plane_end(void)
{
	struct gvusb2_vid *dev = vid_new();
	struct utk_plane p;
	unsigned char src[PLANE + 4096];

	utkvid_reset();
	plane_new(&p, PLANE);
	dev->current_buf = p.vb;
	memset(src, 0x3c, sizeof(src));

	/* the whole plane in one go: 243 lines fit, the 244th would overrun */
	gvusb2_vid_copy_video(dev, src, sizeof(src));

	assert(utkvid.warn_count == 1);
	/* 243 lines fitted; the last plane line belongs to the even field */
	assert(p.vb->written == (LINES_PER_FIELD - 1) * 2 * BPL + BPL);
	assert(p.vb->written <= PLANE);
	assert(p.vb->buf_pos == PLANE);
	plane_assert_guard(&p);

	/* once pinned at the end, a later URB cannot write anything at all */
	gvusb2_vid_copy_video(dev, src, BPL);
	assert(utkvid.warn_count == 2);
	assert(p.vb->buf_pos == PLANE);
	plane_assert_guard(&p);

	plane_free(&p);
	vid_free(dev);
}

/* Payload reported to vb2 is the bytes written, clamped to the plane. */
static void test_submit_video_buffer_payload(void)
{
	struct gvusb2_vid *dev = vid_new();
	struct utk_plane p;
	struct gvusb2_vb next;

	utkvid_reset();
	memset(&next, 0, sizeof(next));
	plane_new(&p, PLANE);
	dev->current_buf = p.vb;
	list_add_tail(&next.list, &dev->buf_list);

	p.vb->written = 1234;
	p.vb->vb.sequence = 0;
	gvusb2_vid_submit_video_buffer(dev);
	assert(utkvid.done_calls == 1);
	assert(utkvid.done_buf[0] == &p.vb->vb.vb2_buf);
	assert(utkvid.done_state[0] == VB2_BUF_STATE_DONE);
	assert(p.vb->vb.vb2_buf.planes[0].bytesused == 1234);
	assert(p.vb->vb.sequence == 0);
	assert(p.vb->vb.field == V4L2_FIELD_INTERLACED);
	assert(dev->sequence == 1);
	/* the next queued buffer takes over */
	assert(dev->current_buf == &next);

	/* a field that overran reports the plane, never more */
	dev->current_buf = p.vb;
	p.vb->written = PLANE + 500;
	gvusb2_vid_submit_video_buffer(dev);
	assert(p.vb->vb.vb2_buf.planes[0].bytesused == PLANE);
	assert(p.vb->vb.sequence == 1);
	assert(dev->sequence == 2);
	/* queue is empty now */
	assert(dev->current_buf == NULL);

	plane_free(&p);
	vid_free(dev);
}

/* A frame needs both fields: the odd field marker submits the buffer that was
 * being filled, the even marker re-bases the position to the second line and
 * arms the next submit. */
static void test_status_packet_pairs_fields(void)
{
	struct gvusb2_vid *dev = vid_new();
	struct utk_plane p1, p2;

	utkvid_reset();
	plane_new(&p1, PLANE);
	plane_new(&p2, PLANE);

	/* no buffer at all yet: nothing to assemble into, nothing to submit */
	gvusb2_vid_process_status_packet(dev, 0x00);
	assert(dev->current_buf == NULL);
	assert(utkvid.done_calls == 0);

	list_add_tail(&p2.vb->list, &dev->buf_list);
	dev->current_buf = p1.vb;

	/* even field: re-base to the second plane line, arm, no submit */
	gvusb2_vid_process_status_packet(dev, 0x00);
	assert(p1.vb->field == 1);
	assert(p1.vb->buf_pos == BPL);
	assert(p1.vb->line_pos == 0);
	assert(utkvid.done_calls == 0);

	/* odd field: the buffer being filled is complete, submit it */
	gvusb2_vid_process_status_packet(dev, 0x40);
	assert(utkvid.done_calls == 1);
	assert(utkvid.done_buf[0] == &p1.vb->vb.vb2_buf);
	assert(dev->current_buf == p2.vb);

	/* the fresh buffer starts its own field pair */
	gvusb2_vid_process_status_packet(dev, 0x00);
	assert(p2.vb->field == 1);
	assert(p2.vb->buf_pos == BPL);
	assert(utkvid.done_calls == 1);

	gvusb2_vid_process_status_packet(dev, 0x40);
	assert(utkvid.done_calls == 2);
	assert(utkvid.done_buf[1] == &p2.vb->vb.vb2_buf);
	assert(dev->current_buf == NULL);

	plane_free(&p1);
	plane_free(&p2);
	vid_free(dev);
}

/* A second even-field marker with no odd marker in between (the odd packet was
 * lost) has to flush the buffer, or it would keep collecting into a plane whose
 * second field is already full. */
static void test_status_packet_flushes_on_second_even_marker(void)
{
	struct gvusb2_vid *dev = vid_new();
	struct utk_plane p;

	utkvid_reset();
	plane_new(&p, PLANE);
	dev->current_buf = p.vb;

	gvusb2_vid_process_status_packet(dev, 0x00);
	assert(p.vb->field == 1);
	assert(utkvid.done_calls == 0);

	gvusb2_vid_process_status_packet(dev, 0x00);
	assert(utkvid.done_calls == 1);
	assert(utkvid.done_buf[0] == &p.vb->vb.vb2_buf);
	assert(dev->current_buf == NULL);

	plane_free(&p);
	vid_free(dev);
}

/* ------------------------------------------------------------------ */
/* packet handling                                                     */
/* ------------------------------------------------------------------ */

/* Anything shorter than the 4-byte packet header is a runt, not data. */
static void test_data_short_packet_ignored(void)
{
	struct gvusb2_vid *dev = vid_new();
	struct utk_plane p;
	u8 pkt[8];

	utkvid_reset();
	plane_new(&p, PLANE);
	dev->current_buf = p.vb;
	memset(pkt, 0, sizeof(pkt));
	pkt[0] = 0x80 | 0x40;	/* a status packet, still too short */

	dev->counter = 5;
	gvusb2_vid_process_data(dev, pkt, 3);
	assert(dev->counter == 5);
	assert(p.vb->written == 0);
	assert(utkvid.done_calls == 0);

	plane_free(&p);
	vid_free(dev);
}

/* A dropped isoc packet desyncs the counter; the field is restarted from the
 * top of the plane so the rest cannot walk past its end. */
static void test_data_desync_restarts_field(void)
{
	struct gvusb2_vid *dev = vid_new();
	struct utk_plane p;
	u8 pkt[64];

	utkvid_reset();
	plane_new(&p, PLANE);
	dev->current_buf = p.vb;
	p.vb->buf_pos = 4 * BPL + 100;
	p.vb->line_pos = 100;
	p.vb->written = 9 * BPL;

	memset(pkt, 0x11, sizeof(pkt));
	pkt[0] = 0x00;		/* data packet */
	pkt[1] = 9;		/* expected 7: the packet before this one was lost */

	dev->counter = 7;
	gvusb2_vid_process_data(dev, pkt, sizeof(pkt));

	assert(p.vb->buf_pos == 60);	/* restarted at the top of the plane */
	assert(p.vb->line_pos == 60);
	assert(p.vb->written >= 9 * BPL);	/* the high-water mark is kept */
	assert(p.base[0] == 0x11);
	assert(dev->counter == 10);

	plane_free(&p);
	vid_free(dev);
}

/* The packet counter is 6 bits: 63 wraps to 0, it never reaches 64. */
static void test_data_counter_wraps(void)
{
	struct gvusb2_vid *dev = vid_new();
	struct utk_plane p;
	u8 pkt[64];

	utkvid_reset();
	plane_new(&p, PLANE);
	dev->current_buf = p.vb;
	memset(pkt, 0x22, sizeof(pkt));
	pkt[0] = 0x00;
	pkt[1] = 63;

	dev->counter = 63;
	gvusb2_vid_process_data(dev, pkt, sizeof(pkt));
	assert(dev->counter == 0);

	/* a status packet advances the counter the same way */
	dev->counter = 63;
	pkt[0] = 0x80 | 0x40;
	pkt[1] = 0;
	gvusb2_vid_process_data(dev, pkt, sizeof(pkt));
	assert(dev->counter == 0);

	plane_free(&p);
	vid_free(dev);
}

/* ------------------------------------------------------------------ */
/* isochronous completion                                              */
/* ------------------------------------------------------------------ */

static struct urb *isoc_new(int packets, u8 **transfer_buffer)
{
	struct urb *urb = usb_alloc_urb(packets, GFP_KERNEL);

	assert(urb != NULL);
	*transfer_buffer = calloc(1, (size_t)packets * GVUSB2_MAX_VIDEO_PACKET_SIZE);
	assert(*transfer_buffer != NULL);
	urb->transfer_buffer = *transfer_buffer;
	urb->transfer_buffer_length = packets * GVUSB2_MAX_VIDEO_PACKET_SIZE;
	urb->number_of_packets = packets;
	return urb;
}

static void isoc_free(struct urb *urb, u8 *transfer_buffer)
{
	free(transfer_buffer);
	usb_free_urb(urb);
}

/* A killed or shutdown URB must not be processed or resubmitted: that is what
 * makes cancel_urbs() final. */
static void test_isoc_irq_ignores_cancelled_urbs(void)
{
	static const int statuses[] = {
		-ENOENT, -ECONNRESET, -ESHUTDOWN,
	};
	struct gvusb2_vid *dev = vid_new();
	struct utk_plane p;
	unsigned int k;

	for (k = 0; k < ARRAY_SIZE(statuses); k++) {
		u8 *tbuf;
		struct urb *urb = isoc_new(2, &tbuf);

		utkvid_reset();
		plane_new(&p, PLANE);
		dev->current_buf = p.vb;
		dev->counter = 3;
		urb->status = statuses[k];
		urb->context = dev;

		gvusb2_vid_isoc_irq(urb);

		assert(utkvid.submit_calls == 0);
		assert(dev->counter == 3);
		assert(p.vb->written == 0);
		assert(utkvid.done_calls == 0);

		plane_free(&p);
		isoc_free(urb, tbuf);
	}
	vid_free(dev);
}

/* A transient bus error is not the end of the stream. The HCD reports -EPROTO
 * or -EXDEV on an isochronous endpoint when it could not transfer the packets,
 * and only the shutdown codes above mean "stop". Dropping the URB here instead
 * of resubmitting it leaves the capture frozen with no error reported to
 * userspace: nothing else ever submits those URBs again. */
static void test_isoc_irq_resubmits_after_bus_error(void)
{
	static const int statuses[] = { -EPROTO, -EXDEV, -ETIME };
	struct gvusb2_vid *dev = vid_new();
	struct utk_plane p;
	unsigned int k;

	for (k = 0; k < ARRAY_SIZE(statuses); k++) {
		u8 *tbuf;
		struct urb *urb = isoc_new(2, &tbuf);

		utkvid_reset();
		plane_new(&p, PLANE);
		dev->current_buf = p.vb;
		dev->counter = 3;
		urb->context = dev;
		urb->status = statuses[k];
		urb->iso_frame_desc[0].actual_length = 64;
		urb->iso_frame_desc[0].status = 0;
		tbuf[0] = 0x00;
		tbuf[1] = dev->counter;
		memset(tbuf + 4, 0x77, 60);

		gvusb2_vid_isoc_irq(urb);

		assert(utkvid.submit_calls == 1);
		assert(utkvid.submitted[0] == urb);
		/* the packets of an errored URB are not trusted, but the next
		 * round starts clean */
		assert(p.vb->written == 0);
		assert(dev->counter == 3);
		assert(urb->iso_frame_desc[0].status == 0);
		assert(urb->iso_frame_desc[0].actual_length == 0);

		plane_free(&p);
		isoc_free(urb, tbuf);
	}
	vid_free(dev);
}

/* The completion handler consumes the packets, clears the descriptors for the
 * next round and resubmits the same URB. A packet the HCD flagged is skipped. */
static void test_isoc_irq_processes_then_resubmits(void)
{
	struct gvusb2_vid *dev = vid_new();
	struct utk_plane p;
	u8 *tbuf;
	struct urb *urb = isoc_new(2, &tbuf);
	const unsigned int pktlen = GVUSB2_MAX_VIDEO_PACKET_SIZE;
	u8 *first = tbuf;
	u8 *second = tbuf + pktlen;

	utkvid_reset();
	plane_new(&p, PLANE);
	dev->current_buf = p.vb;
	urb->context = dev;

	/* packet 0 failed, packet 1 is a data packet with a 60-byte payload */
	first[0] = 0x00;
	first[1] = dev->counter;
	memset(first + 4, 0xee, 64);
	second[0] = 0x00;
	second[1] = dev->counter;
	memset(second + 4, 0x5c, 64);
	urb->iso_frame_desc[0].offset = 0;
	urb->iso_frame_desc[0].length = pktlen;
	urb->iso_frame_desc[0].actual_length = pktlen;
	urb->iso_frame_desc[0].status = -EPROTO;
	urb->iso_frame_desc[1].offset = pktlen;
	urb->iso_frame_desc[1].length = pktlen;
	urb->iso_frame_desc[1].actual_length = 64;
	urb->iso_frame_desc[1].status = 0;
	urb->status = 0;

	gvusb2_vid_isoc_irq(urb);

	/* only the second packet's payload was copied, at its own offset */
	assert(p.vb->written == 60);
	assert(p.base[0] == 0x5c);
	assert(dev->counter == 1);

	/* descriptors are cleared and the urb goes back on the wire */
	assert(urb->iso_frame_desc[0].status == 0);
	assert(urb->iso_frame_desc[0].actual_length == 0);
	assert(urb->iso_frame_desc[1].status == 0);
	assert(urb->iso_frame_desc[1].actual_length == 0);
	assert(utkvid.submit_calls == 1);
	assert(utkvid.submitted[0] == urb);

	plane_free(&p);
	isoc_free(urb, tbuf);
	vid_free(dev);
}

/* ------------------------------------------------------------------ */
/* endpoint scan                                                       */
/* ------------------------------------------------------------------ */

static void ep_set(struct usb_endpoint_descriptor *ep, u8 addr, u8 attrs,
	u16 maxpkt)
{
	memset(ep, 0, sizeof(*ep));
	ep->bEndpointAddress = addr;
	ep->bmAttributes = attrs;
	ep->wMaxPacketSize = maxpkt;
}

static void ep_video(struct usb_endpoint_descriptor *ep)
{
	ep_set(ep, 0x82, USB_ENDPOINT_XFER_ISOC, 0x1400);
}

struct utk_interface {
	struct usb_interface intf;
	struct usb_host_interface alt[2];
	struct usb_host_endpoint ep[2][2];
};

static void interface_setup(struct utk_interface *fi, unsigned int alts,
	unsigned int eps)
{
	unsigned int i;

	memset(fi, 0, sizeof(*fi));
	fi->intf.num_altsetting = alts;
	fi->intf.altsetting = fi->alt;
	for (i = 0; i < alts; i++) {
		fi->alt[i].desc.bNumEndpoints = (u8)eps;
		fi->alt[i].endpoint = fi->ep[i];
	}
}

/* Only endpoint 0x82, IN, isochronous, wMaxPacketSize 0x1400 is the video
 * stream (0x1400 = three 1024-byte transactions = the 0xc00 the driver reads
 * per packet). Everything else must be rejected, including a single step
 * either side of the packet-size boundary. */
static void test_check_altsetting_rules(void)
{
	struct utk_interface fi;
	struct usb_endpoint_descriptor *found;

	utkvid_reset();

	interface_setup(&fi, 1, 1);
	ep_video(&fi.ep[0][0].desc);
	gvusb2_vid_check_altsetting(&fi.intf, 0, &found);
	assert(found == &fi.ep[0][0].desc);

	interface_setup(&fi, 2, 2);
	ep_video(&fi.ep[1][1].desc);	/* last endpoint of the last altsetting */
	gvusb2_vid_check_altsetting(&fi.intf, 1, &found);
	assert(found == &fi.ep[1][1].desc);

	interface_setup(&fi, 1, 1);
	ep_set(&fi.ep[0][0].desc, 0x81, USB_ENDPOINT_XFER_ISOC, 0x1400);
	gvusb2_vid_check_altsetting(&fi.intf, 0, &found);
	assert(found == NULL);		/* wrong endpoint address */

	ep_set(&fi.ep[0][0].desc, 0x02, USB_ENDPOINT_XFER_ISOC, 0x1400);
	gvusb2_vid_check_altsetting(&fi.intf, 0, &found);
	assert(found == NULL);		/* OUT, not IN */

	ep_set(&fi.ep[0][0].desc, 0x82, 0x02, 0x1400);
	gvusb2_vid_check_altsetting(&fi.intf, 0, &found);
	assert(found == NULL);		/* bulk */

	ep_set(&fi.ep[0][0].desc, 0x82, 0x03, 0x1400);
	gvusb2_vid_check_altsetting(&fi.intf, 0, &found);
	assert(found == NULL);		/* interrupt */

	ep_set(&fi.ep[0][0].desc, 0x82, USB_ENDPOINT_XFER_ISOC, 0x400);
	gvusb2_vid_check_altsetting(&fi.intf, 0, &found);
	assert(found == NULL);		/* single 1024-byte transaction: too slow */

	ep_set(&fi.ep[0][0].desc, 0x82, USB_ENDPOINT_XFER_ISOC, 0x13ff);
	gvusb2_vid_check_altsetting(&fi.intf, 0, &found);
	assert(found == NULL);

	ep_set(&fi.ep[0][0].desc, 0x82, USB_ENDPOINT_XFER_ISOC, 0x1401);
	gvusb2_vid_check_altsetting(&fi.intf, 0, &found);
	assert(found == NULL);

	interface_setup(&fi, 1, 0);
	gvusb2_vid_check_altsetting(&fi.intf, 0, &found);
	assert(found == NULL);		/* interface without endpoints */
}

/* ------------------------------------------------------------------ */
/* probe                                                               */
/* ------------------------------------------------------------------ */

/* An interface without the video endpoint is not ours; nothing may be
 * allocated or configured before that decision. */
static void test_probe_rejects_other_interfaces(void)
{
	struct utk_interface fi;
	int ret;

	utkvid_reset();
	interface_setup(&fi, 2, 1);
	ep_set(&fi.ep[0][0].desc, 0x81, USB_ENDPOINT_XFER_ISOC, 0x1400);
	ep_set(&fi.ep[1][0].desc, 0x02, 0x02, 0x0200);

	ret = gvusb2_vid_probe(&fi.intf, NULL);
	assert(ret == -ENODEV);
	assert(utkvid.kzalloc_calls == 0);
	assert(utkvid.urb_alloc_calls == 0);
	assert(utkvid.gv_init_calls == 0);
	assert(utkvid.set_interface_calls == 0);
	assert(utkvid.writes == 0);
	assert(fi.intf.intfdata == NULL);
}

/* Every failure after the URBs exist has to release them, the core handle and
 * the driver struct; and only the pieces actually registered are undone. */
static void test_probe_unwind_when_i2c_register_fails(void)
{
	struct utk_interface fi;
	void *devp;
	int ret;

	utkvid_reset();
	interface_setup(&fi, 1, 1);
	ep_video(&fi.ep[0][0].desc);
	utkvid.i2c_register_err = -EIO;

	ret = gvusb2_vid_probe(&fi.intf, NULL);
	assert(ret == -EIO);
	assert(utkvid.urb_alloc_calls == GVUSB2_NUM_URBS);
	assert(utkvid.urb_free_calls == GVUSB2_NUM_URBS);
	assert(utkvid.i2c_unregister_calls == 0);
	assert(utkvid.v4l2_unregister_calls == 0);
	assert(utkvid.gv_free_calls == 1);
	assert(fi.intf.intfdata == NULL);
	devp = probe_dev_alloc();
	assert(devp != NULL);
	assert(alloc_count(devp) == 1);
	assert(free_count(devp) == 1);
}

static void test_probe_unwind_when_vb2_setup_fails(void)
{
	struct utk_interface fi;

	utkvid_reset();
	interface_setup(&fi, 1, 1);
	ep_video(&fi.ep[0][0].desc);
	utkvid.vb2_setup_err = -ENOMEM;

	assert(gvusb2_vid_probe(&fi.intf, NULL) == -ENOMEM);
	assert(utkvid.i2c_register_calls == 1);
	assert(utkvid.i2c_unregister_calls == 1);
	assert(utkvid.v4l2_register_calls == 0);
	assert(utkvid.v4l2_unregister_calls == 0);
	assert(utkvid.urb_free_calls == GVUSB2_NUM_URBS);
	assert(utkvid.gv_free_calls == 1);
	assert(free_count(probe_dev_alloc()) == 1);
}

static void test_probe_unwind_when_video_register_fails(void)
{
	struct utk_interface fi;

	utkvid_reset();
	interface_setup(&fi, 1, 1);
	ep_video(&fi.ep[0][0].desc);
	utkvid.video_register_err = -EINVAL;

	assert(gvusb2_vid_probe(&fi.intf, NULL) == -EINVAL);
	assert(utkvid.v4l2_register_calls == 1);
	assert(utkvid.v4l2_unregister_calls == 1);
	assert(utkvid.i2c_unregister_calls == 1);
	assert(utkvid.urb_free_calls == GVUSB2_NUM_URBS);
	assert(utkvid.gv_free_calls == 1);
	assert(free_count(probe_dev_alloc()) == 1);
}

static void test_probe_unwind_when_v4l2_register_fails(void)
{
	struct utk_interface fi;

	utkvid_reset();
	interface_setup(&fi, 1, 1);
	ep_video(&fi.ep[0][0].desc);
	utkvid.v4l2_register_err = -EIO;

	assert(gvusb2_vid_probe(&fi.intf, NULL) == -EIO);
	assert(utkvid.vb2_setup_calls == 1);
	assert(utkvid.v4l2_unregister_calls == 0);
	assert(utkvid.i2c_unregister_calls == 1);
	assert(utkvid.urb_free_calls == GVUSB2_NUM_URBS);
	assert(utkvid.gv_free_calls == 1);
	assert(free_count(probe_dev_alloc()) == 1);
}

/* The early ladder: each failure before the URBs exist must undo exactly what
 * was done and leave the interface untouched. */
static void test_probe_unwind_before_urbs(void)
{
	struct utk_interface fi;
	void *devp;

	/* no driver struct */
	utkvid_reset();
	interface_setup(&fi, 1, 1);
	ep_video(&fi.ep[0][0].desc);
	utkvid.kzalloc_fail_at = 1;
	assert(gvusb2_vid_probe(&fi.intf, NULL) == -ENOMEM);
	assert(utkvid.gv_init_calls == 0);
	assert(utkvid.urb_alloc_calls == 0);
	assert(fi.intf.intfdata == NULL);

	/* core init failed */
	utkvid_reset();
	utkvid.gv_init_err = -EIO;
	assert(gvusb2_vid_probe(&fi.intf, NULL) == -EIO);
	assert(utkvid.set_interface_calls == 0);
	assert(utkvid.urb_alloc_calls == 0);
	assert(utkvid.gv_free_calls == 0);
	assert(free_count(probe_dev_alloc()) == 1);

	/* SET_INTERFACE failed: the core handle is released, no URBs yet */
	utkvid_reset();
	utkvid.set_interface_err = -EPIPE;
	assert(gvusb2_vid_probe(&fi.intf, NULL) == -EPIPE);
	assert(utkvid.urb_alloc_calls == 0);
	assert(utkvid.gv_free_calls == 1);
	assert(free_count(probe_dev_alloc()) == 1);

	/* the first URB could not be allocated */
	utkvid_reset();
	utkvid.urb_alloc_fail_at = 1;
	assert(gvusb2_vid_probe(&fi.intf, NULL) == -ENOMEM);
	assert(utkvid.i2c_register_calls == 0);
	assert(utkvid.urb_free_calls == 0);
	assert(utkvid.gv_free_calls == 1);
	devp = probe_dev_alloc();
	assert(devp != NULL);
	assert(free_count(devp) == 1);
}

/* The successful probe wires the driver struct to the interface, the endpoint
 * it selected and alternate setting 5 (the isochronous video setting). */
static void test_probe_wires_device(void)
{
	struct utk_interface fi;
	struct gvusb2_vid *dev;

	utkvid_reset();
	interface_setup(&fi, 2, 2);
	ep_set(&fi.ep[0][0].desc, 0x81, USB_ENDPOINT_XFER_ISOC, 0x1400);
	ep_video(&fi.ep[1][1].desc);
	/* the isochronous video setting is enumerated as alternate setting 5
	 * (interface 0); probe must select the scanned setting, not hardcode it */
	fi.alt[1].desc.bAlternateSetting = 5;

	assert(gvusb2_vid_probe(&fi.intf, NULL) == 0);
	assert(utkvid.set_interface_calls == 1);
	assert(utkvid.set_interface_ifnum == 0);
	assert(utkvid.set_interface_alt == 5);
	assert(utkvid.urb_alloc_calls == GVUSB2_NUM_URBS);

	dev = usb_get_intfdata(&fi.intf);
	assert(dev != NULL);
	assert(dev->intf == &fi.intf);
	assert(dev->gv.udev == &utkvid.udev);
	assert(dev->urbs[0] != NULL);
	assert(dev->urbs[0]->dev == &utkvid.udev);
	assert(utkvid.i2c_register_calls == 1);
	assert(utkvid.vb2_setup_calls == 1);
	assert(utkvid.v4l2_register_calls == 1);
	assert(utkvid.video_register_calls == 1);

	gvusb2_vid_free_urbs(dev);
	free(dev);
}

/* ------------------------------------------------------------------ */
/* STK1150 bring-up                                                    */
/* ------------------------------------------------------------------ */

/* The decoder reset is a GPIO on register 0x0000 bit 3: the pin is made an
 * output, driven low, and driven high again ~10 ms later, and every other GPIO
 * bit in that register has to survive the toggle. */
static void test_stk1150_gpio_reset(void)
{
	struct gvusb2_vid *dev = vid_new();
	unsigned int i;
	int first = -1, second = -1, third = -1;

	utkvid_reset();
	/* bit 3 clear, all other GPIO bits set */
	utkvid.regs[0x0000] = 0xf0;

	gvusb2_stk1150_init(dev);

	for (i = 0; i < utkvid.writes && i < UTKVID_MAX_TRACE; i++) {
		if (utkvid.wreg[i] != 0x0000)
			continue;
		if (first < 0)
			first = i;
		else if (second < 0)
			second = i;
		else if (third < 0)
			third = i;
	}
	assert(first >= 0 && second > first && third > second);
	assert(utkvid.regs[0x0002] == 0x08);	/* pin configured as output */
	assert(utkvid.wval[first] == 0xf8);	/* pin high, other bits kept */
	assert(utkvid.wval[second] == 0xf0);	/* reset asserted */
	assert(utkvid.wval[third] == 0xf8);	/* reset released */

	/* the decoder needs the documented settle time between the edges */
	assert(utkvid.usleep_calls == 2);
	assert(utkvid.usleep_min == 10 * USEC_PER_MSEC);
	assert(utkvid.usleep_max == 20 * USEC_PER_MSEC);

	/* bring-up must leave the capture engine stopped and the I2C clock
	 * divider set, whatever the register table contains */
	assert(utkvid.regs[0x0100] == 0x33);
	assert(utkvid.regs[0x0202] == 0x08);

	vid_free(dev);
}

/* ------------------------------------------------------------------ */
/* driver data teardown                                                */
/* ------------------------------------------------------------------ */

/* gvusb2_vid_free() is the end of the road: URBs, core handle, the struct. */
static void test_vid_free_releases_everything(void)
{
	struct gvusb2_vid *dev = vid_new();
	void *self = dev;

	utkvid_reset();
	assert(gvusb2_vid_allocate_urbs(dev) == 0);

	assert(gvusb2_vid_free(dev) == 0);
	assert(utkvid.urb_free_calls == GVUSB2_NUM_URBS);
	assert(utkvid.kfree_calls == GVUSB2_NUM_URBS + 1);
	assert(utkvid.gv_free_calls == 1);
	assert(free_count(self) == 1);
}

/* The v4l2 release callback runs on the last v4l2_device_put(): the i2c adapter
 * must go before the memory holding it, and the handler/device are unregistered
 * exactly once. */
static void test_release_unregisters_then_frees(void)
{
	struct gvusb2_vid *dev = vid_new();
	void *self = dev;

	utkvid_reset();
	assert(gvusb2_vid_allocate_urbs(dev) == 0);

	gvusb2_release(&dev->v4l2_dev);

	/* order: the v4l2 layer (its subdevs reference i2c clients that
	 * i2c_del_adapter would free first) goes down before the adapter */
	assert(utkvid.v4l2_device_unregister_seq < utkvid.i2c_unregister_seq);
	assert(utkvid.ctrl_handler_free_seq < utkvid.i2c_unregister_seq);
	assert(utkvid.i2c_unregister_calls == 1);
	assert(utkvid.ctrl_handler_free_calls == 1);
	assert(utkvid.v4l2_device_unregister_calls == 1);
	assert(utkvid.urb_free_calls == GVUSB2_NUM_URBS);
	assert(utkvid.gv_free_calls == 1);
	assert(free_count(self) == 1);
}

/* Disconnect kills the URBs before releasing them (freeing a submitted URB is a
 * use-after-free in the HCD), returns the queued buffers with an error and lets
 * go of the v4l2 device. */
static void test_disconnect_cancels_before_freeing(void)
{
	struct utk_interface fi;
	struct gvusb2_vid *dev = vid_new();
	void *self = dev;

	utkvid_reset();
	interface_setup(&fi, 1, 1);
	assert(gvusb2_vid_allocate_urbs(dev) == 0);
	usb_set_intfdata(&fi.intf, dev);

	gvusb2_vid_disconnect(&fi.intf);

	assert(usb_get_intfdata(&fi.intf) == NULL);
	assert(utkvid.kill_calls == GVUSB2_NUM_URBS);
	assert(utkvid.kills_at_first_free == GVUSB2_NUM_URBS);
	assert(utkvid.urb_free_calls == GVUSB2_NUM_URBS);
	assert(utkvid.clear_queue_calls == 1);
	assert(utkvid.clear_queue_state == VB2_BUF_STATE_ERROR);
	assert(utkvid.v4l2_device_disconnect_calls == 1);
	assert(utkvid.v4l2_device_put_calls == 1);

	free(self);
}

/* ------------------------------------------------------------------ */
/* driver registration                                                 */
/* ------------------------------------------------------------------ */

/* A wrong or unterminated ID table means the driver never binds or binds to a
 * stranger's device; host-testable because it is pure data. The IDs are the
 * real device's (README: 04bb:0532), spelled out here rather than read back
 * from the macros the table itself is built from. */
static void test_usb_ids_and_driver(void)
{
	assert(gvusb2_id_table[0].idVendor == 0x04bb);
	assert(gvusb2_id_table[0].idProduct == 0x0532);
	assert(gvusb2_id_table[0].match_flags & 0x0001);
	assert(gvusb2_id_table[0].match_flags & 0x0002);
	assert(gvusb2_id_table[1].idVendor == 0);
	assert(gvusb2_id_table[1].idProduct == 0);

	assert(strcmp(utkvid_usb_driver->name, "gvusb2-vid") == 0);
	assert(utkvid_usb_driver->probe == gvusb2_vid_probe);
	assert(utkvid_usb_driver->disconnect == gvusb2_vid_disconnect);
	assert(utkvid_usb_driver->id_table == gvusb2_id_table);
}

int main(void)
{
	signal(SIGALRM, utk_on_alarm);
	alarm(30);

	test_urb_layout();
	test_urb_alloc_failure_unwinds();
	test_urb_buffer_alloc_failure_unwinds();
	test_free_urbs_releases_and_nulls();
	test_free_urbs_without_allocation();
	test_submit_urbs_all();
	test_submit_urbs_stops_at_first_failure();
	test_cancel_urbs_kills_each();
	test_next_buffer_fifo();
	test_copy_video_interleaves_and_carries_state();
	test_copy_video_truncates_at_plane_end();
	test_submit_video_buffer_payload();
	test_status_packet_pairs_fields();
	test_status_packet_flushes_on_second_even_marker();
	test_data_short_packet_ignored();
	test_data_desync_restarts_field();
	test_data_counter_wraps();
	test_isoc_irq_ignores_cancelled_urbs();
	test_isoc_irq_resubmits_after_bus_error();
	test_isoc_irq_processes_then_resubmits();
	test_check_altsetting_rules();
	test_probe_rejects_other_interfaces();
	test_probe_unwind_when_i2c_register_fails();
	test_probe_unwind_when_vb2_setup_fails();
	test_probe_unwind_when_video_register_fails();
	test_probe_unwind_when_v4l2_register_fails();
	test_probe_unwind_before_urbs();
	test_probe_wires_device();
	test_vid_free_releases_everything();
	test_release_unregisters_then_frees();
	test_disconnect_cancels_before_freeing();
	test_stk1150_gpio_reset();
	test_usb_ids_and_driver();

	printf("test_vid: all tests passed\n");
	return 0;
}
