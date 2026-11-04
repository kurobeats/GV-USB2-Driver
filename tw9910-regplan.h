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
 * Fill `plan` (up to `max` entries) with the register writes needed for the
 * requested standard. Returns the number of entries, or -1 if the plan does
 * not fit. `pal` selects PAL (non-zero) or NTSC (zero); `vbi` selects the
 * full-frame (VBI) crop.
 */
static inline int tw9910_regplan(int pal, int vbi, struct tw9910_reg *plan,
				 int max)
{
	struct tw9910_reg *p = plan;

	if (max < 7)
		return -1;

	/* Standard selection first. */
	*p++ = (struct tw9910_reg){ 0x1c, pal ? TW9910_SDT_PAL : TW9910_SDT_NTSC };
	/* Input black level: 0 IRE (PAL) or 7.5 IRE (NTSC). */
	*p++ = (struct tw9910_reg){ 0x0c, pal ? 0xcc : 0xdc };
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
