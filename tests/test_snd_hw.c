/* Host-side tests for the gvusb2-snd period constraints. See tests/run-tests.sh. */
#include <assert.h>
#include <stdio.h>

#include "../gvusb2-snd-hw.h"

/* libobs snaps audio timestamps that stray more than 70 ms from expected and
 * pads with silence. Keep the advertised floor comfortably below that so the
 * framework can pick fine periods. */
static void test_min_period_is_fine_enough(void)
{
	unsigned int us = gvusb2_period_us(GVUSB2_SND_PERIOD_BYTES_MIN);

	printf("  min period: %u bytes = %u us\n", GVUSB2_SND_PERIOD_BYTES_MIN, us);
	assert(us <= 25000);
}

/* The 256 ms floor (0xc000) is what caused the dropouts: keep it from coming
 * back by accident. */
static void test_regression_256ms_floor(void)
{
	assert(gvusb2_period_us(0xc000) > 250000);
	assert(gvusb2_period_us(GVUSB2_SND_PERIOD_BYTES_MIN) <
	       gvusb2_period_us(0xc000));
}

/* Sanity: a period must fit inside the buffer, and the buffer must hold
 * enough periods to avoid overrunning on a scheduling hiccup. */
static void test_period_fits_buffer(void)
{
	assert(GVUSB2_SND_PERIOD_BYTES_MIN <= GVUSB2_SND_BUFFER_BYTES_MAX);
	assert(GVUSB2_SND_PERIOD_BYTES_MIN <= GVUSB2_SND_PERIOD_BYTES_MAX);
	assert(GVUSB2_SND_BUFFER_BYTES_MAX / GVUSB2_SND_PERIOD_BYTES_MIN >= 4);
}

/* One second of audio, expressed in period bytes, must not be absurd. */
static void test_period_bytes_for_one_second(void)
{
	unsigned int bps = GVUSB2_SND_RATE * GVUSB2_SND_CHANNELS *
			   GVUSB2_SND_SAMPLE_BYTES;

	assert(bps == 192000);
	assert(gvusb2_period_us(bps) == 1000000);
}

/* The completion path credits hw_ptr in frames but consumes the DMA ring in
 * bytes; a packet whose actual_length is not a multiple of the frame size
 * would desync the two permanently. gvusb2_packet_bytes() truncates the tail
 * instead, and is identity for every well-formed length. */
static void test_packet_length_is_frame_aligned(void)
{
	const unsigned int frame = GVUSB2_SND_FRAME_BYTES;

	assert(frame == 4);
	assert(gvusb2_packet_bytes(0) == 0);
	assert(gvusb2_packet_bytes(frame) == frame);
	assert(gvusb2_packet_bytes(24) == 24);	/* 6 frames: typical 48 kHz microframe */
	assert(gvusb2_packet_bytes(25) == 24);	/* stray byte truncated */
	assert(gvusb2_packet_bytes(255) == 252);
	assert(gvusb2_packet_bytes(256) == 256);	/* wMaxPacketSize */
}

/* Boundary contract of the hw-constraint table in gvusb2-snd.c, at the fixed
 * 48000 Hz S16_LE stereo stream. Pins the exact 4096-byte floor (21 ms, do
 * not raise: hardware-verified granularity for libobs timestamps), the
 * 128 KiB ceiling, and frame alignment of both period bounds. */
static void test_hw_constraint_boundaries(void)
{
	assert(GVUSB2_SND_PERIOD_BYTES_MIN == 4096);
	assert(GVUSB2_SND_PERIOD_BYTES_MAX == 128 * 1024);
	assert(GVUSB2_SND_BUFFER_BYTES_MAX == 128 * 1024);
	assert(GVUSB2_SND_RATE == 48000);
	assert(GVUSB2_SND_CHANNELS == 2);
	assert(GVUSB2_SND_SAMPLE_BYTES == 2);

	/* floor: 1024 frames = 21333 us at 48 kHz (integer-truncated) */
	assert(gvusb2_period_us(GVUSB2_SND_PERIOD_BYTES_MIN) == 21333);
	/* ceiling: 128 KiB = 682666 us at 192000 B/s (integer-truncated) */
	assert(gvusb2_period_us(GVUSB2_SND_PERIOD_BYTES_MAX) == 682666);

	/* at the floor the buffer holds exactly periods_max (32, see
	 * gvusb2-snd.c); a finer floor or bigger buffer would break that cap */
	assert(GVUSB2_SND_BUFFER_BYTES_MAX / GVUSB2_SND_PERIOD_BYTES_MIN == 32);
	assert(GVUSB2_SND_PERIOD_BYTES_MIN % GVUSB2_SND_FRAME_BYTES == 0);
	assert(GVUSB2_SND_PERIOD_BYTES_MAX % GVUSB2_SND_FRAME_BYTES == 0);
}

int main(void)
{
	test_min_period_is_fine_enough();
	test_regression_256ms_floor();
	test_period_fits_buffer();
	test_period_bytes_for_one_second();
	test_packet_length_is_frame_aligned();
	test_hw_constraint_boundaries();
	printf("test_snd_hw: all tests passed\n");
	return 0;
}
