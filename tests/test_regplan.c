/* Host-side tests for the TW9910 register plan. Run tests/run-tests.sh. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../tw9910-regplan.h"

/* Decode a 10-bit crop value from its CROP_HI field and _LO register.
 * TW9910 CROP_HI: [7:6] VDELAY_HI, [5:4] VACTIVE_HI, [3:2] HDELAY_HI,
 * [1:0] HACTIVE_HI. */
static unsigned int decode_hi_lo(const struct tw9910_reg *plan, int n,
				 unsigned char reg, unsigned int shift)
{
	unsigned char lo = 0, hi = 0;
	int i;

	for (i = 0; i < n; i++) {
		if (plan[i].reg == 0x07)
			hi = (plan[i].val >> shift) & 0x3;
		if (plan[i].reg == reg)
			lo = plan[i].val;
	}
	return (hi << 8) | lo;
}

static int find_reg(const struct tw9910_reg *plan, int n, unsigned char reg)
{
	int i;

	for (i = 0; i < n; i++)
		if (plan[i].reg == reg)
			return i;
	return -1;
}

static void expect_plan(int pal, int vbi,
			const struct tw9910_reg *want, int want_n)
{
	struct tw9910_reg plan[8];
	int n, i, j;

	n = tw9910_regplan(pal, vbi, plan, 8);
	assert(n == want_n);

	for (i = 0; i < n; i++) {
		assert(plan[i].reg == want[i].reg);
		assert(plan[i].val == want[i].val);
	}

	/* No register may appear twice: that is exactly the bug where the
	 * mainline tw9910 subdev rewrote CROP_HI/VDELAY_LO/VACTIVE_LO after the
	 * fork, leaving the geometry stuck on PAL. */
	for (i = 0; i < n; i++)
		for (j = i + 1; j < n; j++)
			assert(plan[i].reg != plan[j].reg);
}

static void test_pal_normal(void)
{
	const struct tw9910_reg want[] = {
		{ 0x1c, 0x97 }, /* SDT: PAL */
		{ 0x0b, 0xd2 }, /* HACTIVE */
		{ 0x0a, 0x0d }, /* HDELAY */
		{ 0x08, 0x17 }, /* VDELAY */
		{ 0x07, 0x12 }, /* CROP_HI */
		{ 0x09, 0x22 }, /* VACTIVE */
	};

	expect_plan(1, 0, want, 6);
}

static void test_ntsc_normal(void)
{
	const struct tw9910_reg want[] = {
		{ 0x1c, 0x17 }, /* SDT: NTSC */
		{ 0x0b, 0xd2 }, /* HACTIVE */
		{ 0x0a, 0x10 }, /* HDELAY */
		{ 0x08, 0x13 }, /* VDELAY */
		{ 0x07, 0x02 }, /* CROP_HI */
		{ 0x09, 0xf4 }, /* VACTIVE */
	};

	expect_plan(0, 0, want, 6);
}

static void test_pal_vbi(void)
{
	const struct tw9910_reg want[] = {
		{ 0x1c, 0x97 },
		{ 0x0b, 0xd2 },
		{ 0x0a, 0x0d },
		{ 0x08, 0x02 },
		{ 0x07, 0x12 },
		{ 0x09, 0x37 },
	};

	expect_plan(1, 1, want, 6);
}

static void test_ntsc_vbi(void)
{
	const struct tw9910_reg want[] = {
		{ 0x1c, 0x17 },
		{ 0x0b, 0xd2 },
		{ 0x0a, 0x10 },
		{ 0x08, 0x02 },
		{ 0x07, 0x12 },
		{ 0x09, 0x05 },
	};

	expect_plan(0, 1, want, 6);
}

/* The standard-selection register must actually switch: the whole reason the
 * geometry appeared stuck is that PAL and NTSC ended up agreeing. */
static void test_sdt_differs_by_standard(void)
{
	struct tw9910_reg pal[8], ntsc[8];
	int i;

	assert(tw9910_regplan(1, 0, pal, 8) == 6);
	assert(tw9910_regplan(0, 0, ntsc, 8) == 6);

	for (i = 0; i < 6; i++)
		if (pal[i].reg == 0x1c)
			assert(pal[i].val != ntsc[i].val);
}

/* A too-small destination must be reported, not silently truncated. */
static void test_plan_overflow(void)
{
	struct tw9910_reg plan[5];

	assert(tw9910_regplan(0, 0, plan, 5) == -1);
}

/* Black level is a read-modify-write: only the IRE bit (4) and bit 3 may
 * change, every other clamp bit must survive the write. Bit 4 set is the 0 IRE
 * setting and bit 4 clear the 7.5 IRE one, as measured on hardware. */
static void test_black_level_rmw(void)
{
	static const unsigned char curs[] = { 0x00, 0xff, 0xdd, 0x1f };

	for (unsigned int i = 0; i < sizeof(curs); i++) {
		unsigned char seven = tw9910_black_level(curs[i], 0);
		unsigned char zero = tw9910_black_level(curs[i], 1);

		/* bits outside the two the vendor touches are preserved */
		assert((seven & ~0x18) == (curs[i] & ~0x18));
		assert((zero & ~0x18) == (curs[i] & ~0x18));
		assert(seven & 0x08);      /* bit 3 set either way */
		assert(zero & 0x08);
		assert(!(seven & 0x10));   /* 7.5 IRE: bit 4 clear */
		assert(zero & 0x10);       /* 0 IRE:   bit 4 set  */
	}
}

/* The plan writes the registers in settle order: standard selection first,
 * then horizontal window, then the vertical geometry. (Black level, 0x0c, is
 * a read-modify-write applied outside the plan.) */
static void test_plan_order(void)
{
	static const unsigned char regs[] = {
		0x1c, 0x0b, 0x0a, 0x08, 0x07, 0x09
	};
	int pal, vbi;

	for (pal = 0; pal <= 1; pal++)
		for (vbi = 0; vbi <= 1; vbi++) {
			struct tw9910_reg plan[8];
			int n = tw9910_regplan(pal, vbi, plan, 8);

			assert(n == 6);
			for (int i = 0; i < 6; i++)
				assert(plan[i].reg == regs[i]);
			/* exact fit: a plan of 6 must fit in max 6 */
			assert(tw9910_regplan(pal, vbi, plan, 6) == 6);
		}
}

/* The horizontal active window is the same in all four modes: 720 + 2
 * pixels of headroom, held in the HACTIVE_HI/HACTIVE_LO pair. */
static void test_hactive_pair(void)
{
	for (int pal = 0; pal <= 1; pal++)
		for (int vbi = 0; vbi <= 1; vbi++) {
			struct tw9910_reg plan[8];
			int n = tw9910_regplan(pal, vbi, plan, 8);

			assert(n == 6);
			assert(decode_hi_lo(plan, n, 0x0b, 0) == 0x2d2);
			/* the horizontal delay must lie inside the window */
			assert(decode_hi_lo(plan, n, 0x0a, 2) < 0x2d2);
		}
}

/* The VBI crop must strictly contain the non-VBI crop of the same standard
 * (it starts earlier and runs longer), and PAL's active region is taller
 * than NTSC's. A plan that violates these captures fewer lines than the
 * buffer expects and the tail of every frame goes stale. */
static void test_vertical_geometry_monotonic(void)
{
	struct tw9910_reg plans[4][8];
	int idx;

	for (int i = 0; i < 4; i++) {
		idx = tw9910_regplan(i / 2, i % 2, plans[i], 8);
		assert(idx == 6);
	}

	for (int std = 0; std < 2; std++) {
		unsigned int vact_n = decode_hi_lo(plans[std * 2], 6, 0x09, 4);
		unsigned int vact_v = decode_hi_lo(plans[std * 2 + 1], 6, 0x09, 4);
		unsigned int vdly_n = decode_hi_lo(plans[std * 2], 6, 0x08, 6);
		unsigned int vdly_v = decode_hi_lo(plans[std * 2 + 1], 6, 0x08, 6);

		assert(vact_v > vact_n);
		assert(vdly_v <= vdly_n);
	}
	/* PAL captures a taller active region than NTSC in the same mode */
	assert(decode_hi_lo(plans[2], 6, 0x09, 4) >
	       decode_hi_lo(plans[0], 6, 0x09, 4));
	assert(decode_hi_lo(plans[3], 6, 0x09, 4) >
	       decode_hi_lo(plans[1], 6, 0x09, 4));
	assert(find_reg(plans[0], 6, 0x09) == 5); /* VACTIVE last, hi/lo pair contiguous */
}

int main(void)
{
	test_pal_normal();
	test_ntsc_normal();
	test_pal_vbi();
	test_ntsc_vbi();
	test_sdt_differs_by_standard();
	test_plan_overflow();
	test_black_level_rmw();
	test_plan_order();
	test_hactive_pair();
	test_vertical_geometry_monotonic();
	printf("test_regplan: all tests passed\n");
	return 0;
}
