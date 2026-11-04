/* SPDX-License-Identifier: GPL-2.0-only OR BSD-3-Clause */
#ifndef UTK_LINUX_I2C_H
#define UTK_LINUX_I2C_H

#include <linux/module.h>

struct i2c_adapter;

struct i2c_msg {
	u16 addr;
	u16 flags;
	u16 len;
	u8 *buf;
};

#define I2C_M_RD 0x0001
#define I2C_FUNC_SMBUS_BYTE_DATA 0x00020000

struct i2c_algorithm {
	int (*master_xfer)(struct i2c_adapter *adap, struct i2c_msg *msgs,
		int num);
	u32 (*functionality)(struct i2c_adapter *adap);
};

struct i2c_adapter {
	struct module *owner;
	char name[48];
	struct device dev;
	const struct i2c_algorithm *algo;
	void *algo_data;
};

struct i2c_client {
	char name[24];
	struct i2c_adapter *adapter;
	u16 addr;
};

int i2c_add_adapter(struct i2c_adapter *adap);
void i2c_del_adapter(struct i2c_adapter *adap);

#endif