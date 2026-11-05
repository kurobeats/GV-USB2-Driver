/* Host-side tests for the gvusb2 core/i2c lane: the USB vendor register
 * protocol in gvusb2-core.c, the I2C bus register map in gvusb2-i2c.c, and the
 * GPIO macros in gvusb2-regs.h. The real gvusb2-core.c/gvusb2-i2c.c are
 * compiled against the fake kernel headers in utk/ (see run-tests.sh).
 *
 * The mock USB device below emulates the register file plus the embedded I2C
 * engine: command register 0x0200 (0x60 = read trigger, 0x05 = write trigger),
 * status flags 0x0201 (0x01 read done, 0x04 write done), bus address 0x0203
 * ((addr << 1) | read-bit), write subaddr 0x0204, write data 0x0205, read
 * subaddr 0x0208, read data 0x0209. The mock encodes the same protocol the
 * driver is assumed to implement; it is self-consistency plus address-math
 * arbitration, not a datasheet cross-check.
 */

#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/usb.h>
#include <linux/i2c.h>
#include <media/videobuf2-v4l2.h>

#include "../gvusb2.h"
#include "../gvusb2-vid.h"
#include "../gvusb2-regs.h"

/* Included for static access to the i2c bus helpers under test. */
#include "../gvusb2-i2c.c"

#define UTK_NREGS 0x1000

static u8 utk_regs[UTK_NREGS];
static u8 utk_chipmem[256];
static int utk_fail_usb_read;
static int utk_fail_usb_write;
static int utk_usb_read_short;
static int utk_i2c_stuck;
static int utk_add_count;
static int utk_del_count;

static void utk_on_alarm(int sig)
{
	(void)sig;
	fprintf(stderr, "test_core: FAILED (test hung busy-waiting)\n");
	_exit(1);
}

static void utk_reset(void)
{
	memset(utk_regs, 0, sizeof(utk_regs));
	memset(utk_chipmem, 0, sizeof(utk_chipmem));
	utk_fail_usb_read = 0;
	utk_fail_usb_write = 0;
	utk_usb_read_short = 0;
	utk_i2c_stuck = 0;
	utk_add_count = 0;
	utk_del_count = 0;
}

int usb_control_msg(struct usb_device *udev, unsigned int pipe, u8 request,
	u8 requesttype, u16 value, u16 index, void *data, u16 size, int timeout)
{
	(void)udev;
	(void)pipe;
	(void)request;
	(void)timeout;

	if (requesttype & USB_DIR_IN) {
		if (utk_fail_usb_read)
			return -EIO;
		if (index >= UTK_NREGS)
			return -EIO;
		if (utk_usb_read_short)
			return 0; /* acknowledged but 0 bytes transferred */
		if (size)
			*(u8 *)data = utk_regs[index];
		return size;
	}

	if (utk_fail_usb_write)
		return -EIO;
	if (index >= UTK_NREGS)
		return -EIO;
	utk_regs[index] = (u8)value;

	if (index == 0x0200) {
		switch (value) {
		case 0x60: /* read trigger */
			utk_regs[0x0209] = utk_chipmem[utk_regs[0x0208]];
			if (!utk_i2c_stuck)
				utk_regs[0x0201] |= 0x01;
			break;
		case 0x05: /* write trigger */
			utk_chipmem[utk_regs[0x0204]] = utk_regs[0x0205];
			if (!utk_i2c_stuck)
				utk_regs[0x0201] |= 0x04;
			break;
		default:
			break;
		}
	}
	return 0;
}

int i2c_add_adapter(struct i2c_adapter *adap)
{
	(void)adap;
	utk_add_count++;
	return 0;
}

void i2c_del_adapter(struct i2c_adapter *adap)
{
	(void)adap;
	utk_del_count++;
}

static struct gvusb2_vid *utk_vid(void)
{
	static struct usb_device udev;
	static struct usb_interface intf;
	static struct gvusb2_vid vid;

	memset(&vid, 0, sizeof(vid));
	vid.gv.udev = &udev;
	vid.intf = &intf;
	return &vid;
}

/* gvusb2-regs.h: GPIO macros must be distinct single bits that fit a u8. */
static void test_gpio_macros(void)
{
	const u8 vals[] = {
		GVUSB2_GPIO_SDA_RESET,
		GVUSB2_GPIO_SOUND_ENABLE,
		GVUSB2_GPIO_VDEC_RESET,
	};
	size_t i, j;

	for (i = 0; i < sizeof(vals) / sizeof(vals[0]); i++) {
		assert(vals[i] != 0);
		assert(vals[i] <= 0xff);
		assert((vals[i] & (vals[i] - 1)) == 0);
		for (j = i + 1; j < sizeof(vals) / sizeof(vals[0]); j++)
			assert(vals[i] != vals[j]);
	}
}

/* gvusb2_write_reg/gvusb2_read_reg round-trip through the vendor protocol. */
static void test_read_write_roundtrip(void)
{
	struct gvusb2_dev dev;
	u8 v = 99;
	int ret;

	utk_reset();
	dev.udev = NULL;

	ret = gvusb2_write_reg(&dev, 0x0500, 0x5a);
	assert(ret == 0);
	ret = gvusb2_read_reg(&dev, 0x0500, &v);
	assert(ret == 0);
	assert(v == 0x5a);
}

/* BUG: gvusb2_snd_reset_adc ignored every register-op error (TODO: return
 * errors), so probe's ret < 0 check was dead code and a dead bus still
 * "successfully" brought up the ADC. */
static void test_reset_adc_error(void)
{
	struct gvusb2_dev dev;
	int ret;

	utk_reset();
	dev.udev = NULL;

	utk_fail_usb_write = 1;
	ret = gvusb2_snd_reset_adc(&dev);
	assert(ret < 0);

	utk_reset();
	dev.udev = NULL;
	utk_fail_usb_read = 1;
	ret = gvusb2_snd_reset_adc(&dev);
	assert(ret < 0);

	/* success path: GPIO + AC97/I2S registers all written, first op's
	 * mask/value visible in the mock register file */
	utk_reset();
	dev.udev = NULL;
	ret = gvusb2_snd_reset_adc(&dev);
	assert(ret == 0);
	assert(utk_regs[0x0002] == 0x30);
	assert(utk_regs[0x0000] == 0x10);
	assert(utk_regs[0x0500] == 0x00);
	assert(utk_regs[0x050c] == 0x01);
}

/* gvusb2_set_reg_mask must read-modify-write only the masked bits. */
static void test_set_reg_mask_rmw(void)
{
	struct gvusb2_dev dev;
	int ret;

	utk_reset();
	dev.udev = NULL;
	utk_regs[0x0007] = 0xc5;

	ret = gvusb2_set_reg_mask(&dev, 0x0007, 0x30, 0x10);
	assert(ret == 0);
	assert(utk_regs[0x0007] == 0xd5); /* (0xc5 & ~0x30) | 0x10 */
}

/* BUG: gvusb2_set_reg_mask checked the read's stale `ret` after the write, so
 * a failed write was silently reported as success. */
static void test_set_reg_mask_write_error(void)
{
	struct gvusb2_dev dev;
	int ret;

	utk_reset();
	dev.udev = NULL;
	utk_fail_usb_write = 1;

	ret = gvusb2_set_reg_mask(&dev, 0x0007, 0x30, 0x10);
	assert(ret < 0);
}

/* BUG: gvusb2_read_reg only checked ret < 0, so an acknowledged read that
 * transferred 0 bytes was treated as success and `*value` was filled from an
 * uninitialized byte. */
static void test_read_reg_short_transfer(void)
{
	struct gvusb2_dev dev;
	u8 v;
	int ret;

	utk_reset();
	dev.udev = NULL;
	utk_usb_read_short = 1;

	v = 0xaa;
	ret = gvusb2_read_reg(&dev, 0x0500, &v);
	assert(ret == -EIO);
	assert(v == 0x00); /* no uninitialized byte may leak through */
}

/* I2C bus register map: address math (addr << 1, read-bit set for reads),
 * subaddress/data registers and trigger commands per direction. */
static void test_i2c_regmap_table(void)
{
	struct gvusb2_vid *dev;
	const u8 addrs[] = { 0x00, 0x0f, 0x44, 0x7f };
	u8 v;
	size_t k;
	int ret;

	for (k = 0; k < sizeof(addrs) / sizeof(addrs[0]); k++) {
		utk_reset();
		dev = utk_vid();

		ret = gvusb2_i2c_write_reg(dev, addrs[k], 0x1c, 0x42);
		assert(ret == 0);
		assert(utk_regs[0x0203] == (u8)(addrs[k] << 1));
		assert(utk_regs[0x0204] == 0x1c);
		assert(utk_regs[0x0205] == 0x42);
		assert(utk_regs[0x0200] == 0x05);
		assert(utk_chipmem[0x1c] == 0x42);

		utk_chipmem[0x07] = 0x99;
		v = 0;
		ret = gvusb2_i2c_read_reg(dev, addrs[k], 0x07, &v);
		assert(ret == 0);
		assert(utk_regs[0x0203] == (u8)((addrs[k] << 1) | 1));
		assert(utk_regs[0x0208] == 0x07);
		assert(utk_regs[0x0200] == 0x60);
		assert(v == 0x99);
	}
}

/* BUG: gvusb2_i2c_busy_wait never incremented its attempt counter, so a
 * never-ready bus hung the driver instead of timing out. */
static void test_i2c_busy_wait_stuck(void)
{
	struct gvusb2_vid *dev;
	int ret;

	utk_reset();
	dev = utk_vid();
	utk_i2c_stuck = 1;

	ret = gvusb2_i2c_read_reg(dev, 0x44, 0x1c, &(u8){ 0 });
	assert(ret == -ETIMEDOUT);

	/* A stuck transaction must be aborted, as the vendor driver does, or the
	 * engine stays wedged and every later access sees a stale completion. */
	assert(utk_regs[0x0200] == 0x80);
}

/* BUG: the busy wait also ignored register read errors; a failed USB status
 * read spun forever rather than reporting the I/O error. */
static void test_i2c_busy_wait_read_error(void)
{
	struct gvusb2_vid *dev;
	int ret;

	utk_reset();
	dev = utk_vid();
	utk_fail_usb_read = 1;

	ret = gvusb2_i2c_read_reg(dev, 0x44, 0x1c, &(u8){ 0 });
	assert(ret == -EIO);
}

/* The adapter algorithm must serve a combined write-then-read transaction and
 * a two-byte write via the same regmap. */
static void test_i2c_xfer_shapes(void)
{
	struct gvusb2_vid *dev;
	struct i2c_adapter *adap;
	struct i2c_msg msgs[2];
	u8 subaddr, data, v;
	int ret;

	utk_reset();
	dev = utk_vid();
	assert(gvusb2_i2c_register(dev) == 0);
	adap = &dev->adap;

	/* combined: write subaddr 0x1c, read back chip value */
	utk_chipmem[0x1c] = 0x3c;
	subaddr = 0x1c;
	v = 0;
	msgs[0].addr = 0x44;
	msgs[0].flags = 0;
	msgs[0].len = 1;
	msgs[0].buf = &subaddr;
	msgs[1].addr = 0x44;
	msgs[1].flags = I2C_M_RD;
	msgs[1].len = 1;
	msgs[1].buf = &v;
	ret = adap->algo->master_xfer(adap, msgs, 2);
	assert(ret == 2);
	assert(v == 0x3c);

	/* plain two-byte write */
	subaddr = 0x09;
	data = 0x77;
	msgs[0].addr = 0x44;
	msgs[0].flags = 0;
	msgs[0].len = 2;
	msgs[0].buf = (u8[]){ subaddr, data };
	ret = adap->algo->master_xfer(adap, msgs, 1);
	assert(ret == 1);
	assert(utk_chipmem[0x09] == 0x77);
}

/* Unsupported transaction shapes must be reported, not guessed. */
static void test_i2c_xfer_unsupported(void)
{
	struct gvusb2_vid *dev;
	struct i2c_adapter *adap;
	struct i2c_msg msgs[2];
	u8 subaddr, v;
	int ret;

	utk_reset();
	dev = utk_vid();
	assert(gvusb2_i2c_register(dev) == 0);
	adap = &dev->adap;

	/* three-byte write: no such op */
	subaddr = 0x1c;
	msgs[0].addr = 0x44;
	msgs[0].flags = 0;
	msgs[0].len = 3;
	msgs[0].buf = (u8[]){ subaddr, 1, 2 };
	ret = adap->algo->master_xfer(adap, msgs, 1);
	assert(ret == -EOPNOTSUPP);

	/* zero-length write */
	msgs[0].len = 0;
	ret = adap->algo->master_xfer(adap, msgs, 1);
	assert(ret == -EOPNOTSUPP);

	/* write subaddr + read pair with mismatched addresses */
	v = 0;
	msgs[0].len = 1;
	msgs[0].addr = 0x44;
	msgs[0].buf = &subaddr;
	msgs[1].addr = 0x45;
	msgs[1].flags = I2C_M_RD;
	msgs[1].len = 1;
	msgs[1].buf = &v;
	ret = adap->algo->master_xfer(adap, msgs, 2);
	assert(ret == -EOPNOTSUPP);
}

/* gvusb2_i2c_register must configure clock divider and alternate-i2c off. */
static void test_i2c_register_config(void)
{
	struct gvusb2_vid *dev;
	int ret;

	utk_reset();
	dev = utk_vid();

	ret = gvusb2_i2c_register(dev);
	assert(ret == 0);
	assert(utk_regs[0x0202] == 0x08); /* clock divider */
	assert(utk_regs[0x02ff] == 0x00); /* alternate i2c disabled */
	assert(strcmp(dev->adap.name, "gvusb2") == 0);
	assert(dev->i2c_client.addr == 0x44);
	assert((dev->adap.algo->functionality(&dev->adap) &
		I2C_FUNC_SMBUS_BYTE_DATA) != 0);
}

/* BUG: i2c_register ignored the return of the clock-divider and
 * alternate-i2c config writes, so a failed USB write left a registered but
 * misconfigured adapter and still reported success. */
static void test_i2c_register_write_failure(void)
{
	struct gvusb2_vid *dev;
	int ret;

	utk_reset();
	dev = utk_vid();
	utk_fail_usb_write = 1;

	ret = gvusb2_i2c_register(dev);
	assert(ret < 0);
	assert(utk_add_count == 1);
	assert(utk_del_count == 1); /* adapter must not stay registered */
}

int main(void)
{
	signal(SIGALRM, utk_on_alarm);
	alarm(30);

	test_gpio_macros();
	test_read_write_roundtrip();
	test_read_reg_short_transfer();
	test_set_reg_mask_rmw();
	test_set_reg_mask_write_error();
	test_i2c_regmap_table();
	test_i2c_busy_wait_stuck();
	test_i2c_busy_wait_read_error();
	test_i2c_xfer_shapes();
	test_i2c_xfer_unsupported();
	test_i2c_register_config();
	test_i2c_register_write_failure();
	test_reset_adc_error();

	printf("test_core: all tests passed\n");
	return 0;
}