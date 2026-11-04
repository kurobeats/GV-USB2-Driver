/* SPDX-License-Identifier: GPL-2.0-only OR BSD-3-Clause */
/*
 * Frame assembly bookkeeping for the gvusb2 video path.
 *
 * Freestanding (kernel and host): tests/test_assemble.c exercises it without
 * kernel headers. The interleaved-field layout is where frames were silent
 * lost.
 */
#ifndef GVUSB2_ASSEMBLE_H
#define GVUSB2_ASSEMBLE_H

struct gvusb2_pos {
	unsigned int buf_pos;
	unsigned int line_pos;
};

/*
 * Next contiguous run to copy out of one URB.
 *
 * Returns 1 and fills offset/len, 0 when the URB is exhausted, or -1 on
 * overflow. On overflow buf_pos is pinned to the end of the plane so the
 * field is still submitted: returning early without that left the field
 * unsubmitted, which dropped whole frames.
 */
static inline int gvusb2_next_run(struct gvusb2_pos *p, unsigned int remaining,
				  unsigned int bytes_per_line, unsigned int buffer_len,
				  unsigned int *offset, unsigned int *len)
{
	unsigned int left_in_line, run;

	if (remaining == 0)
		return 0;

	left_in_line = bytes_per_line - p->line_pos;
	run = remaining < left_in_line ? remaining : left_in_line;

	if (p->buf_pos + run > buffer_len) {
		p->buf_pos = buffer_len;
		return -1;
	}

	*offset = p->buf_pos;
	*len = run;
	return 1;
}

/* Advance past `run` copied bytes. Each source line occupies two plane
 * lines, because the two fields of a frame are interleaved. */
static inline void gvusb2_advance(struct gvusb2_pos *p, unsigned int run,
				 unsigned int bytes_per_line)
{
	p->buf_pos += run;
	p->line_pos += run;

	if (p->line_pos == bytes_per_line) {
		p->buf_pos += bytes_per_line;
		p->line_pos = 0;
	}
}

/* The even field starts one plane line in; the odd field continues from
 * wherever the even field stopped. */
static inline void gvusb2_field_start(struct gvusb2_pos *p, int even,
				      unsigned int bytes_per_line)
{
	p->buf_pos = even ? bytes_per_line : 0;
	p->line_pos = 0;
}

/* Plane bytes a field of `lines` source lines occupies. */
static inline unsigned int gvusb2_field_bytes(unsigned int lines, unsigned int bytes_per_line)
{
	return lines * 2 * bytes_per_line;
}

#endif /* GVUSB2_ASSEMBLE_H */
