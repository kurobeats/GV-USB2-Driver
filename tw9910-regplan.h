/* SPDX-License-Identifier: GPL-2.0-only OR BSD-3-Clause */
/*
 * TW9910 register plan for one video standard.
 *
 * Freestanding (kernel and host): tests/test_regplan.c exercises the exact
 * register layout without kernel headers. The fork owns the TW9910; the
 * mainline tw9910 subdev must not also write CROP_HI/VDELAY_LO/VACTIVE_LO or
 * it overwrites these after the fork does (see gvusb2_vidioc_s_std).
 */
#ifndef GVUSB2_TW9910_REGPLAN_H
#define GVUSB2_TW9910_REGPLAN_H

struct tw9910_reg {
	unsigned char reg;
	unsigned char val;
};

/* SDT (0x1c): standard selection; bit 7 selects PAL (625/50) vs NTSC (525/60). */
#define TW9910_SDT_PAL  0x97
#define TW9910_SDT_NTSC 0x17

/*
 * BLC (0x0c) black level, applied as a read-modify-write outside the plan: the
 * vendor driver only touches bit 4 (0 IRE for 625/50, 7.5 IRE for 525/60) and
 * bit 3, leaving the clamp's other bits as the chip set them. Writing a whole
 * constant here clobbered those.
 */
#define TW9910_BLC_KEEP   0xef
#define TW9910_BLC_NTSC   0x08
#define TW9910_BLC_PAL    0x18

static inline unsigned char tw9910_black_level(unsigned char cur, int pal)
{
	return (cur & TW9910_BLC_KEEP) | (pal ? TW9910_BLC_PAL : TW9910_BLC_NTSC);
}

/*
 * Fill `plan` (up to `max` entries) with the register writes needed for the
 * requested standard. Returns the number of entries, or -1 if the plan does
 * not fit. `pal` selects PAL (non-zero) or NTSC (zero); `vbi` selects the
 * full-frame (VBI) crop.
 */
static inline int tw9910_regplan(int pal, int vbi, struct tw9910_reg *plan,
				 int max)
{
	struct tw9910_reg *p = plan;

	if (max < 6)
		return -1;

	/* Standard selection first. */
	*p++ = (struct tw9910_reg){ 0x1c, pal ? TW9910_SDT_PAL : TW9910_SDT_NTSC };
	/* HACTIVE (720) + 2, headroom for horizontal shift. */
	*p++ = (struct tw9910_reg){ 0x0b, 0xd2 };
	/* HDELAY. */
	*p++ = (struct tw9910_reg){ 0x0a, pal ? 0x0d : 0x10 };
	/* VDELAY. */
	*p++ = (struct tw9910_reg){ 0x08, pal ? (vbi ? 0x02 : 0x17)
					     : (vbi ? 0x02 : 0x13) };
	/* CROP_HI. */
	*p++ = (struct tw9910_reg){ 0x07, pal ? 0x12 : (vbi ? 0x12 : 0x02) };
	/* VACTIVE. */
	*p++ = (struct tw9910_reg){ 0x09, pal ? (vbi ? 0x37 : 0x22)
					     : (vbi ? 0x05 : 0xf4) };

	return p - plan;
}

#endif /* GVUSB2_TW9910_REGPLAN_H */
