/* Host-side tests for the TW9910 register plan. Run tests/run-tests.sh. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../tw9910-regplan.h"

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
		{ 0x0c, 0xcc }, /* CNTRL1: 0 IRE */
		{ 0x0b, 0xd2 }, /* HACTIVE */
		{ 0x0a, 0x0d }, /* HDELAY */
		{ 0x08, 0x17 }, /* VDELAY */
		{ 0x07, 0x12 }, /* CROP_HI */
		{ 0x09, 0x22 }, /* VACTIVE */
	};

	expect_plan(1, 0, want, 7);
}

static void test_ntsc_normal(void)
{
	const struct tw9910_reg want[] = {
		{ 0x1c, 0x17 }, /* SDT: NTSC */
		{ 0x0c, 0xdc }, /* CNTRL1: 7.5 IRE */
		{ 0x0b, 0xd2 }, /* HACTIVE */
		{ 0x0a, 0x10 }, /* HDELAY */
		{ 0x08, 0x13 }, /* VDELAY */
		{ 0x07, 0x02 }, /* CROP_HI */
		{ 0x09, 0xf4 }, /* VACTIVE */
	};

	expect_plan(0, 0, want, 7);
}

static void test_pal_vbi(void)
{
	const struct tw9910_reg want[] = {
		{ 0x1c, 0x97 },
		{ 0x0c, 0xcc },
		{ 0x0b, 0xd2 },
		{ 0x0a, 0x0d },
		{ 0x08, 0x02 },
		{ 0x07, 0x12 },
		{ 0x09, 0x37 },
	};

	expect_plan(1, 1, want, 7);
}

static void test_ntsc_vbi(void)
{
	const struct tw9910_reg want[] = {
		{ 0x1c, 0x17 },
		{ 0x0c, 0xdc },
		{ 0x0b, 0xd2 },
		{ 0x0a, 0x10 },
		{ 0x08, 0x02 },
		{ 0x07, 0x12 },
		{ 0x09, 0x05 },
	};

	expect_plan(0, 1, want, 7);
}

/* The standard-selection register must actually switch: the whole reason the
 * geometry appeared stuck is that PAL and NTSC ended up agreeing. */
static void test_sdt_differs_by_standard(void)
{
	struct tw9910_reg pal[8], ntsc[8];
	int i;

	assert(tw9910_regplan(1, 0, pal, 8) == 7);
	assert(tw9910_regplan(0, 0, ntsc, 8) == 7);

	for (i = 0; i < 7; i++)
		if (pal[i].reg == 0x1c)
			assert(pal[i].val != ntsc[i].val);
}

/* A too-small destination must be reported, not silently truncated. */
static void test_plan_overflow(void)
{
	struct tw9910_reg plan[6];

	assert(tw9910_regplan(0, 0, plan, 6) == -1);
}

int main(void)
{
	test_pal_normal();
	test_ntsc_normal();
	test_pal_vbi();
	test_ntsc_vbi();
	test_sdt_differs_by_standard();
	test_plan_overflow();
	printf("test_regplan: all tests passed\n");
	return 0;
}
