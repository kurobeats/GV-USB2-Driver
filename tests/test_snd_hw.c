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

int main(void)
{
	test_min_period_is_fine_enough();
	test_regression_256ms_floor();
	test_period_fits_buffer();
	test_period_bytes_for_one_second();
	printf("test_snd_hw: all tests passed\n");
	return 0;
}
