/*
 * Copyright (c) 2018 Diego Sueiro
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT silabs_gecko_i2c

#include <errno.h>
#include <drivers/i2c.h>
#include <sys/util.h>
#include <em_cmu.h>
#include <em_i2c.h>
#include <em_gpio.h>
#include <soc.h>

#define LOG_LEVEL CONFIG_I2C_LOG_LEVEL
#include <logging/log.h>
LOG_MODULE_REGISTER(i2c_gecko);

#include "i2c-priv.h"

#define DEV_CFG(dev) \
	((struct i2c_gecko_config * const)(dev)->config->config_info)
#define DEV_DATA(dev) \
	((struct i2c_gecko_data * const)(dev)->driver_data)
#define DEV_BASE(dev) \
	((I2C_TypeDef *)(DEV_CFG(dev))->base)

struct i2c_gecko_config {
	I2C_TypeDef *base;
	CMU_Clock_TypeDef clock;
	I2C_Init_TypeDef i2cInit;
	u32_t bitrate;
	struct soc_gpio_pin pin_sda;
	struct soc_gpio_pin pin_scl;
#ifdef CONFIG_SOC_GECKO_HAS_INDIVIDUAL_PIN_LOCATION
	u8_t loc_sda;
	u8_t loc_scl;
#else
	u8_t loc;
#endif
};

struct i2c_gecko_data {
	struct k_sem device_sync_sem;
	u32_t dev_config;
};

void i2c_gecko_config_pins(struct device *dev,
			   const struct soc_gpio_pin *pin_sda,
			   const struct soc_gpio_pin *pin_scl)
{
	I2C_TypeDef *base = DEV_BASE(dev);
	struct i2c_gecko_config *config = DEV_CFG(dev);

	soc_gpio_configure(pin_scl);
	soc_gpio_configure(pin_sda);

#ifdef CONFIG_SOC_GECKO_HAS_INDIVIDUAL_PIN_LOCATION
	base->ROUTEPEN = I2C_ROUTEPEN_SDAPEN | I2C_ROUTEPEN_SCLPEN;
	base->ROUTELOC0 = (config->loc_sda << _I2C_ROUTELOC0_SDALOC_SHIFT) |
			  (config->loc_scl << _I2C_ROUTELOC0_SCLLOC_SHIFT);
#else
	base->ROUTE = I2C_ROUTE_SDAPEN | I2C_ROUTE_SCLPEN | (config->loc << 8);
#endif
}

static int i2c_gecko_configure(struct device *dev, u32_t dev_config_raw)
{
	I2C_TypeDef *base = DEV_BASE(dev);
	struct i2c_gecko_config *config = DEV_CFG(dev);
	struct i2c_gecko_data *data = DEV_DATA(dev);
	u32_t baudrate;

	if (!(I2C_MODE_MASTER & dev_config_raw)) {
		return -EINVAL;
	}

	switch (I2C_SPEED_GET(dev_config_raw)) {
	case I2C_SPEED_STANDARD:
		baudrate = KHZ(100);
		break;
	case I2C_SPEED_FAST:
		baudrate = KHZ(400);
		break;
	case I2C_SPEED_FAST_PLUS:
		baudrate = MHZ(1);
		break;
	default:
		return -EINVAL;
	}

	data->dev_config = dev_config_raw;
	config->i2cInit.freq = baudrate;

	I2C_Init(base, &config->i2cInit);

	return 0;
}

static int i2c_gecko_transfer(struct device *dev, struct i2c_msg *msgs,
			      u8_t num_msgs, u16_t addr)
{
	I2C_TypeDef *base = DEV_BASE(dev);
	struct i2c_gecko_data *data = DEV_DATA(dev);
	I2C_TransferSeq_TypeDef seq;
	I2C_TransferReturn_TypeDef ret = -EIO;
	u32_t timeout = 300000U;

	if (!num_msgs) {
		return 0;
	}

	seq.addr = addr << 1;

	do {
		seq.buf[0].data = msgs->buf;
		seq.buf[0].len	= msgs->len;

		if ((msgs->flags & I2C_MSG_RW_MASK) == I2C_MSG_READ) {
			seq.flags = I2C_FLAG_READ;
		} else {
			seq.flags = I2C_FLAG_WRITE;
			if (num_msgs > 1) {
				/* Next message */
				msgs++;
				num_msgs--;
				if ((msgs->flags & I2C_MSG_RW_MASK)
				    == I2C_MSG_READ) {
					seq.flags = I2C_FLAG_WRITE_READ;
				} else {
					seq.flags = I2C_FLAG_WRITE_WRITE;
				}
				seq.buf[1].data = msgs->buf;
				seq.buf[1].len	= msgs->len;
			}
		}

		if (data->dev_config & I2C_ADDR_10_BITS) {
			seq.flags |= I2C_FLAG_10BIT_ADDR;
		}

		/* Do a polled transfer */
		ret = I2C_TransferInit(base, &seq);
		while (ret == i2cTransferInProgress && timeout--) {
			ret = I2C_Transfer(base);
		}

		if (ret != i2cTransferDone) {
			goto finish;
		}

		/* Next message */
		msgs++;
		num_msgs--;
	} while (num_msgs);

finish:
	if (ret != i2cTransferDone) {
		ret = -EIO;
	}
	return ret;
}

static int i2c_gecko_init(struct device *dev)
{
	struct i2c_gecko_config *config = DEV_CFG(dev);
	u32_t bitrate_cfg;
	int error;

	CMU_ClockEnable(config->clock, true);

	i2c_gecko_config_pins(dev, &config->pin_sda, &config->pin_scl);

	bitrate_cfg = i2c_map_dt_bitrate(config->bitrate);

	error = i2c_gecko_configure(dev, I2C_MODE_MASTER | bitrate_cfg);
	if (error) {
		return error;
	}

	return 0;
}

static const struct i2c_driver_api i2c_gecko_driver_api = {
	.configure = i2c_gecko_configure,
	.transfer = i2c_gecko_transfer,
};

#if DT_HAS_NODE_STATUS_OKAY(DT_DRV_INST(0))

#define PIN_I2C_0_SDA {DT_INST_PROP_BY_IDX(0, location_sda, 1), \
		DT_INST_PROP_BY_IDX(0, location_sda, 2), gpioModeWiredAnd, 1}
#define PIN_I2C_0_SCL {DT_INST_PROP_BY_IDX(0, location_scl, 1), \
		DT_INST_PROP_BY_IDX(0, location_scl, 2), gpioModeWiredAnd, 1}

static struct i2c_gecko_config i2c_gecko_config_0 = {
	.base = (I2C_TypeDef *)DT_INST_REG_ADDR(0),
	.clock = cmuClock_I2C0,
	.i2cInit = I2C_INIT_DEFAULT,
	.pin_sda = PIN_I2C_0_SDA,
	.pin_scl = PIN_I2C_0_SCL,
#ifdef CONFIG_SOC_GECKO_HAS_INDIVIDUAL_PIN_LOCATION
	.loc_sda = DT_INST_PROP_BY_IDX(0, location_sda, 0),
	.loc_scl = DT_INST_PROP_BY_IDX(0, location_scl, 0),
#else
#if DT_INST_PROP_BY_IDX(0, location_sda, 0) \
	!= DT_INST_PROP_BY_IDX(0, location_scl, 0)
#error I2C_0 DTS location-* properties must have identical value
#endif
	.loc = DT_INST_PROP_BY_IDX(0, location_scl, 0),
#endif
	.bitrate = DT_INST_PROP(0, clock_frequency),
};

static struct i2c_gecko_data i2c_gecko_data_0;

DEVICE_AND_API_INIT(i2c_gecko_0, DT_INST_LABEL(0),
		    &i2c_gecko_init, &i2c_gecko_data_0, &i2c_gecko_config_0,
		    POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE,
		    &i2c_gecko_driver_api);
#endif /* DT_HAS_NODE_STATUS_OKAY(DT_DRV_INST(0)) */

#if DT_HAS_NODE_STATUS_OKAY(DT_DRV_INST(1))

#define PIN_I2C_1_SDA {DT_INST_PROP_BY_IDX(1, location_sda, 1), \
		DT_INST_PROP_BY_IDX(1, location_sda, 2), gpioModeWiredAnd, 1}
#define PIN_I2C_1_SCL {DT_INST_PROP_BY_IDX(1, location_scl, 1), \
		DT_INST_PROP_BY_IDX(1, location_scl, 2), gpioModeWiredAnd, 1}

static struct i2c_gecko_config i2c_gecko_config_1 = {
	.base = (I2C_TypeDef *)DT_INST_REG_ADDR(1),
	.clock = cmuClock_I2C1,
	.i2cInit = I2C_INIT_DEFAULT,
	.pin_sda = PIN_I2C_1_SDA,
	.pin_scl = PIN_I2C_1_SCL,
#ifdef CONFIG_SOC_GECKO_HAS_INDIVIDUAL_PIN_LOCATION
	.loc_sda = DT_INST_PROP_BY_IDX(1, location_sda, 0),
	.loc_scl = DT_INST_PROP_BY_IDX(1, location_scl, 0),
#else
#if DT_INST_PROP_BY_IDX(1, location_sda, 0) \
	!= DT_INST_PROP_BY_IDX(1, location_scl, 0)
#error I2C_1 DTS location-* properties must have identical value
#endif
	.loc = DT_INST_PROP_BY_IDX(1, location_scl, 0),
#endif
	.bitrate = DT_INST_PROP(1, clock_frequency),
};

static struct i2c_gecko_data i2c_gecko_data_1;

DEVICE_AND_API_INIT(i2c_gecko_1, DT_INST_LABEL(1),
		    &i2c_gecko_init, &i2c_gecko_data_1, &i2c_gecko_config_1,
		    POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE,
		    &i2c_gecko_driver_api);
#endif /* DT_HAS_NODE_STATUS_OKAY(DT_DRV_INST(1)) */
