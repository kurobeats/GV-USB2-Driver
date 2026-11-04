/* Host-side tests for the gvusb2 frame assembly. Run tests/run-tests.sh. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../gvusb2-assemble.h"

/* 720x486 UYVY NTSC plane, 243 source lines per field. */
#define BPL 1440u
#define LINES_PER_FIELD 243u
#define PLANE (486u * BPL)
#define GUARD 64u

/* Every capture mode the driver serves: lines per field and frame height. */
static const struct {
	unsigned int lines;
	unsigned int height;
} geometries[] = {
	{ 243, 486 }, /* NTSC 720x486 */
	{ 288, 576 }, /* PAL 720x576 */
	{ 258, 516 }, /* NTSC VBI 720x516 */
	{ 308, 616 }, /* PAL VBI 720x616 */
};

/*
 * Feed `total` bytes of one field through the assembler in `chunk`-sized
 * URBs, copying into a guarded plane so we can prove nothing runs past it.
 */
static int feed_field(struct gvusb2_pos *p, uint32_t total, uint32_t chunk,
		      unsigned char *plane, uint32_t plane_len)
{
	static unsigned char src[16384];
	uint32_t done = 0;

	assert(chunk <= sizeof(src));
	memset(src, 0xa5, sizeof(src));

	while (done < total) {
		uint32_t remaining = total - done > chunk ? chunk : total - done;
		int ret;

		for (;;) {
			unsigned int off = 0, len = 0;

			ret = gvusb2_next_run(p, remaining, BPL, plane_len,
					      &off, &len);
			if (ret <= 0)
				break;

			assert(off + len <= plane_len);
			memcpy(plane + off, src, len);
			gvusb2_advance(p, len, BPL);
			remaining -= len;
		}

		if (ret < 0)
			return -1;

		done += total - done > chunk ? chunk : total - done;
	}

	return 0;
}

/* Feed one source line and copy it into the guarded plane. */
static void feed_line(struct gvusb2_pos *p, unsigned char line_tag,
		      unsigned char *plane, unsigned int plane_len)
{
	static unsigned char line[1440];
	unsigned int off, len;

	assert(BPL <= sizeof(line));
	memset(line, 0xff, sizeof(line));
	line[0] = line_tag;

	assert(gvusb2_next_run(p, BPL, BPL, plane_len, &off, &len) == 1);
	assert(off + len <= plane_len);
	memcpy(plane + off, line, len);
	gvusb2_advance(p, len, BPL);
}

/* A whole 243-line field fits in the plane, no overflow. */
static void test_ntsc_field_fits(void)
{
	static unsigned char plane[PLANE + GUARD];
	struct gvusb2_pos p = { 0, 0, 0 };

	memset(plane, 0, sizeof(plane));
	/* 243 source lines occupy the whole 486-line plane. */
	assert(gvusb2_field_bytes(LINES_PER_FIELD, BPL) == PLANE);
	assert(feed_field(&p, LINES_PER_FIELD * BPL, 16384, plane, PLANE) == 0);
	assert(p.buf_pos <= PLANE);
}

/* The even field starts one plane line in and still fits. */
static void test_even_field_fits(void)
{
	static unsigned char plane[PLANE + GUARD];
	struct gvusb2_pos p;

	memset(plane, 0, sizeof(plane));
	gvusb2_field_start(&p, 1, BPL);
	assert(p.buf_pos == BPL);

	assert(feed_field(&p, LINES_PER_FIELD * BPL, 16384, plane, PLANE) == 0);
}

/* One extra line is exactly the off-by-one the hardware produced. It must be
 * reported as overflow and must not be written past the plane. */
static void test_one_extra_line_overflows(void)
{
	static unsigned char plane[PLANE + GUARD];
	struct gvusb2_pos p = { 0, 0, 0 };
	unsigned int i;

	memset(plane, 0x5a, sizeof(plane));

	assert(feed_field(&p, (LINES_PER_FIELD + 1) * BPL, 16384, plane,
			  PLANE) == -1);
	assert(p.buf_pos == PLANE);

	for (i = PLANE; i < PLANE + GUARD; i++)
		assert(plane[i] == 0x5a);
}

/* Overflow pins buf_pos to the plane end so the next field starts from a
 * known state, and the caller writes nothing. */
static void test_overflow_pins_position(void)
{
	struct gvusb2_pos p = { PLANE - 100, 0, 0 };
	unsigned int off = 0, len = 0;

	assert(gvusb2_next_run(&p, 4096, BPL, PLANE, &off, &len) == -1);
	assert(p.buf_pos == PLANE);
	assert(off == 0 && len == 0);
}

/* A complete frame (both fields) reports the whole plane as written. */
static void test_payload_full_frame(void)
{
	static unsigned char plane[PLANE + GUARD];
	struct gvusb2_pos p = { 0, 0, 0 };

	memset(plane, 0, sizeof(plane));

	/* field A fills the even plane lines from offset 0 */
	assert(feed_field(&p, LINES_PER_FIELD * BPL, 16384, plane, PLANE) == 0);
	/* field B starts one plane line in, filling the odd lines */
	gvusb2_field_start(&p, 1, BPL);
	assert(feed_field(&p, LINES_PER_FIELD * BPL, 16384, plane, PLANE) == 0);

	assert(gvusb2_payload(&p, PLANE) == PLANE);
}

/* A truncated field must not claim the full plane: the tail would be stale
 * bytes from the previous frame. */
static void test_payload_truncated(void)
{
	static unsigned char plane[PLANE + GUARD];
	struct gvusb2_pos p = { 0, 0, 0 };

	memset(plane, 0, sizeof(plane));

	assert(feed_field(&p, (LINES_PER_FIELD + 1) * BPL, 16384, plane,
			  PLANE) == -1);

	/* 243 lines fit; the 244th overflows, so 243 lines stay valid. */
	assert(gvusb2_payload(&p, PLANE) ==
	       (LINES_PER_FIELD - 1) * 2 * BPL + BPL);
	assert(gvusb2_payload(&p, PLANE) < PLANE);
}

/* A counter desync means the field is misaligned: restarting it must keep the
 * following data inside the plane instead of overrunning it. */
static void test_resync_prevents_overflow(void)
{
	static unsigned char plane[PLANE + GUARD];
	struct gvusb2_pos p = { 0, 0, 0 };

	memset(plane, 0, sizeof(plane));

	/* Part of a field arrives, then a packet is dropped... */
	assert(feed_field(&p, 200 * BPL, 16384, plane, PLANE) == 0);
	/* ...so the driver restarts the field. */
	gvusb2_resync(&p);
	assert(p.buf_pos == 0 && p.line_pos == 0);

	/* The rewritten field fits, with no overflow. */
	assert(feed_field(&p, LINES_PER_FIELD * BPL, 16384, plane, PLANE) == 0);
	assert(gvusb2_payload(&p, PLANE) <= PLANE);
}

/* Chunking must not change the outcome: URB boundaries vary in the wild. */
static void test_chunking_independent(void)
{
	static unsigned char plane_a[PLANE + GUARD], plane_b[PLANE + GUARD];
	struct gvusb2_pos a = { 0, 0, 0 }, b = { 0, 0, 0 };
	uint32_t total = LINES_PER_FIELD * BPL;

	memset(plane_a, 0, sizeof(plane_a));
	memset(plane_b, 0, sizeof(plane_b));

	assert(feed_field(&a, total, 16384, plane_a, PLANE) == 0);
	assert(feed_field(&b, total, 1440, plane_b, PLANE) == 0);

	assert(a.buf_pos == b.buf_pos);
	assert(a.line_pos == b.line_pos);
	assert(memcmp(plane_a, plane_b, PLANE) == 0);
}

/* Each capture mode's plane must be exactly the field_bytes of one field. */
static void test_geometry_planes(void)
{
	unsigned int g;

	for (g = 0; g < sizeof(geometries) / sizeof(geometries[0]); g++) {
		unsigned char *plane;
		struct gvusb2_pos p = { 0, 0, 0 };
		unsigned int plane_len = geometries[g].height * BPL;

		assert(gvusb2_field_bytes(geometries[g].lines, BPL) ==
		       plane_len);

		plane = malloc(plane_len + GUARD);
		assert(plane != NULL);
		memset(plane, 0x77, plane_len + GUARD);

		/* the field fits with room to spare */
		assert(feed_field(&p, geometries[g].lines * BPL, 16384,
				  plane, plane_len) == 0);
		/* one whole extra source line is exactly the off-by-one */
		assert(feed_field(&p, (geometries[g].lines + 1) * BPL, 16384,
				  plane, plane_len) == -1);
		/* written high-water stays at the full plane, guard untouched */
		for (unsigned int i = plane_len; i < plane_len + GUARD; i++)
			assert(plane[i] == 0x77);

		free(plane);
	}
}

/* A full frame fed in the order the driver actually produces it: the tail of
 * the previous frame's second field lands on rows 0,2,4... (buf_pos 0), then
 * the first field of this frame (field_start even) lands on rows 1,3,5....
 * Both halves must coexist and cover the whole plane. */
static void test_full_frame_driver_order(void)
{
	static unsigned char plane[576 * BPL + GUARD];
	struct gvusb2_pos p = { 0, 0, 0 };
	unsigned int plane_len = 576 * BPL;
	unsigned int lines = 288;

	/* previous frame's tail: rows 0,2,4,... tagged with 0x40+i */
	memset(plane, 0xb4, plane_len);
	for (unsigned int i = 0; i < lines; i++)
		feed_line(&p, (unsigned char)(0x40 + i), plane, plane_len);

	/* this frame's first field: rows 1,3,5,... tagged with i */
	gvusb2_field_start(&p, 1, BPL);
	for (unsigned int i = 0; i < lines; i++)
		feed_line(&p, (unsigned char)i, plane, plane_len);

	assert(gvusb2_payload(&p, plane_len) == plane_len);
	for (unsigned int i = 0; i < lines; i++) {
		assert(plane[(2 * i) * BPL] == (unsigned char)(0x40 + i));
		assert(plane[(2 * i + 1) * BPL] == (unsigned char)i);
	}
}

/* Source line k of each field must land on the right plane row: the fields
 * are interleaved, so a shift here doubles every row in the output. */
static void test_interleave_rows(void)
{
	static unsigned char plane[20 * BPL + GUARD];
	struct gvusb2_pos p = { 0, 0, 0 };
	unsigned int lines = 10, plane_len = 20 * BPL;

	memset(plane, 0xb4, sizeof(plane));

	for (unsigned int i = 0; i < lines; i++)
		feed_line(&p, (unsigned char)i, plane, plane_len);
	for (unsigned int i = 0; i < lines; i++)
		assert(plane[(2 * i) * BPL] == (unsigned char)i);

	gvusb2_field_start(&p, 1, BPL);
	for (unsigned int i = 0; i < lines; i++)
		feed_line(&p, (unsigned char)i, plane, plane_len);
	for (unsigned int i = 0; i < lines; i++)
		assert(plane[(2 * i + 1) * BPL] == (unsigned char)i);
	/* even rows kept their first-field content */
	for (unsigned int i = 0; i < lines; i++)
		assert(plane[(2 * i) * BPL] == (unsigned char)i);
}

/* A plane smaller than one line must overflow on the very first run, write
 * nothing, and not touch the guard area. */
static void test_tiny_plane_overflows(void)
{
	static unsigned char plane[100 + GUARD];
	struct gvusb2_pos p = { 0, 0, 0 };
	unsigned int off = 0, len = 0;

	memset(plane, 0x77, sizeof(plane));

	assert(gvusb2_next_run(&p, 200, BPL, 100, &off, &len) == -1);
	assert(p.buf_pos == 100);
	assert(off == 0 && len == 0);
	assert(gvusb2_payload(&p, 100) == 0);
	for (unsigned int i = 100; i < sizeof(plane); i++)
		assert(plane[i] == 0x77);
}

/* A run ending exactly at the plane end is not overflow; one byte more is. */
static void test_exact_fit_boundary(void)
{
	struct gvusb2_pos p = { PLANE - BPL, 0, 0 };
	unsigned int off, len;

	assert(gvusb2_next_run(&p, BPL, BPL, PLANE, &off, &len) == 1);
	assert(off == PLANE - BPL && len == BPL);
	gvusb2_advance(&p, len, BPL);

	assert(gvusb2_next_run(&p, 1, BPL, PLANE, &off, &len) == -1);
	assert(p.buf_pos == PLANE);

	/* an empty URB is not an error: it just advances nothing */
	assert(gvusb2_next_run(&p, 0, BPL, PLANE, &off, &len) == 0);
}

int main(void)
{
	test_geometry_planes();
	test_full_frame_driver_order();
	test_interleave_rows();
	test_tiny_plane_overflows();
	test_exact_fit_boundary();
	test_ntsc_field_fits();
	test_even_field_fits();
	test_one_extra_line_overflows();
	test_overflow_pins_position();
	test_payload_full_frame();
	test_payload_truncated();
	test_resync_prevents_overflow();
	test_chunking_independent();
	printf("test_assemble: all tests passed\n");
	return 0;
}
