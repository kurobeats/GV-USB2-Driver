/* SPDX-License-Identifier: GPL-2.0-only OR BSD-3-Clause */
/*
 * ALSA capture period constraints for gvusb2-snd, kept in a header so
 * tests/test_snd_hw.c can assert the advertised granularity on the host.
 *
 * Why it matters: period size is the granularity at which application
 * timestamps arrive. libobs snaps audio timestamps that deviate by more than
 * 70 ms, padding with silence, so a 256 ms floor produced periodic ~82 ms
 * jumps and audible dropouts in OBS recordings.
 */
#ifndef GVUSB2_SND_HW_H
#define GVUSB2_SND_HW_H

#define GVUSB2_SND_RATE		48000
#define GVUSB2_SND_CHANNELS	2
#define GVUSB2_SND_SAMPLE_BYTES	2	/* S16_LE */
#define GVUSB2_SND_FRAME_BYTES	(GVUSB2_SND_CHANNELS * GVUSB2_SND_SAMPLE_BYTES)

/* Largest period the driver accepts; unchanged from the original limits. */
#define GVUSB2_SND_PERIOD_BYTES_MAX	(128 * 1024)
#define GVUSB2_SND_BUFFER_BYTES_MAX	(128 * 1024)

/* Smallest period. Must stay fine enough for media frameworks to timestamp
 * audio without tripping libobs' smoothing; see gvusb2_period_us(). */
#define GVUSB2_SND_PERIOD_BYTES_MIN	4096

/* Duration of one period, in microseconds. */
static inline unsigned int gvusb2_period_us(unsigned int bytes)
{
	unsigned int bytes_per_sec = GVUSB2_SND_RATE * GVUSB2_SND_CHANNELS *
				     GVUSB2_SND_SAMPLE_BYTES;

	return (unsigned int)(((unsigned long long)bytes * 1000000ULL) /
			      bytes_per_sec);
}

/* An isoc packet length rounded down to whole frames. hw_ptr/avail advance in
 * frames while the DMA ring is consumed in bytes, so an endpoint length that
 * is not a multiple of GVUSB2_SND_FRAME_BYTES would desync the two
 * permanently; the packet tail is dropped instead. */
static inline unsigned int gvusb2_packet_bytes(unsigned int bytes)
{
	return bytes & ~(unsigned int)(GVUSB2_SND_FRAME_BYTES - 1);
}

#endif /* GVUSB2_SND_HW_H */
