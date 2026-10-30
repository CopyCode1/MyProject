#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "adapters/imu/gt02_imu_zephyr.h"

LOG_MODULE_REGISTER(gt02_imu_zephyr, LOG_LEVEL_INF);

#if !DT_NODE_HAS_STATUS_OKAY(DT_ALIAS(imu0))
#error "GT02 board must provide an enabled imu0 alias"
#endif

/* LSM6DSO register map, kept private to this adapter. */
#define REG_FIFO_CTRL1   0x07
#define REG_FIFO_CTRL2   0x08
#define REG_FIFO_CTRL3   0x09
#define REG_FIFO_CTRL4   0x0a
#define REG_WHO_AM_I     0x0f
#define REG_CTRL1_XL     0x10
#define REG_CTRL2_G      0x11
#define REG_CTRL3_C      0x12
#define REG_CTRL6_C      0x15
#define REG_CTRL9_XL     0x18
#define REG_FIFO_STATUS1 0x3a
#define REG_FIFO_TAG     0x78
#define FIFO_OVERRUN     BIT(6)

static const struct i2c_dt_spec imu = I2C_DT_SPEC_GET(DT_ALIAS(imu0));
static enum gt02_imu_mode current_mode;
static bool initialized;
static bool have_accel;
static bool have_gyro;
static struct gt02_imu_sample pending;

static int write_reg(uint8_t reg, uint8_t value)
{
	return i2c_reg_write_byte_dt(&imu, reg, value);
}

static int update_reg(uint8_t reg, uint8_t mask, uint8_t value)
{
	uint8_t old;
	int ret = i2c_reg_read_byte_dt(&imu, reg, &old);

	return ret < 0 ? ret : write_reg(reg, (old & ~mask) | (value & mask));
}

static int fifo_restart(void)
{
	int ret = update_reg(REG_FIFO_CTRL4, 0x07, 0); /* Bypass clears FIFO. */

	if (ret < 0) {
		return ret;
	}
	have_accel = false;
	have_gyro = false;
	return update_reg(REG_FIFO_CTRL4, 0x07, 0x06); /* Continuous mode. */
}

int gt02_imu_zephyr_set_mode(enum gt02_imu_mode mode)
{
	int ret;

	if (!initialized) {
		return -ENODEV;
	}
	if (mode != GT02_IMU_MODE_WORK && mode != GT02_IMU_MODE_LOW) {
		return -EINVAL;
	}
	if (mode == current_mode) {
		return 0;
	}

	/* Stop FIFO before changing ODR; old and new frames must not mix. */
	ret = update_reg(REG_FIFO_CTRL4, 0x07, 0);
	if (ret < 0) {
		goto failed;
	}
	if (mode == GT02_IMU_MODE_WORK) {
		ret = update_reg(REG_CTRL6_C, BIT(4), 0);
		if (ret < 0) goto failed;
		ret = write_reg(REG_CTRL1_XL, 0x40); /* 104 Hz, +/-2 g. */
		if (ret < 0) goto failed;
		ret = write_reg(REG_CTRL2_G, 0x48); /* 104 Hz, +/-1000 dps. */
		if (ret < 0) goto failed;
		ret = write_reg(REG_FIFO_CTRL3, 0x44); /* Both batched at 104 Hz. */
	} else {
		ret = write_reg(REG_CTRL2_G, 0); /* Gyroscope off. */
		if (ret < 0) goto failed;
		ret = update_reg(REG_CTRL6_C, BIT(4), BIT(4));
		if (ret < 0) goto failed;
		ret = write_reg(REG_CTRL1_XL, 0xb0); /* Legacy 1.6 Hz low power. */
		if (ret < 0) goto failed;
		ret = write_reg(REG_FIFO_CTRL3, 0x01); /* Accel batch at 12.5 Hz. */
	}
	if (ret < 0) goto failed;
	ret = fifo_restart();
	if (ret < 0) goto failed;
	current_mode = mode;
	LOG_INF("IMU mode: %s", mode == GT02_IMU_MODE_WORK ? "104 Hz" : "low power");
	return 0;

failed:
	initialized = false; /* Partial configuration must not be used. */
	have_accel = false;
	have_gyro = false;
	LOG_ERR("IMU mode change failed: %d", ret);
	return ret;
}

int gt02_imu_zephyr_init(void)
{
	uint8_t id, ctrl3;
	int ret;

	initialized = false;
	if (!i2c_is_ready_dt(&imu)) return -ENODEV;
	ret = i2c_reg_read_byte_dt(&imu, REG_WHO_AM_I, &id);
	if (ret < 0) return ret;
	if (id != 0x6c) {
		LOG_ERR("Unexpected IMU WHO_AM_I: 0x%02x", id);
		return -ENODEV;
	}
	ret = write_reg(REG_CTRL3_C, BIT(0)); /* Software reset. */
	if (ret < 0) return ret;
	for (int attempt = 0; attempt < 20; ++attempt) {
		k_sleep(K_MSEC(1));
		ret = i2c_reg_read_byte_dt(&imu, REG_CTRL3_C, &ctrl3);
		if (ret < 0) return ret;
		if ((ctrl3 & BIT(0)) == 0) break;
		if (attempt == 19) return -ETIMEDOUT;
	}
	ret = update_reg(REG_CTRL3_C, BIT(6) | BIT(2), BIT(6) | BIT(2));
	if (ret < 0) return ret; /* BDU and register auto-increment. */
	ret = update_reg(REG_CTRL9_XL, BIT(1), BIT(1)); /* Disable I3C. */
	if (ret < 0) return ret;
	ret = write_reg(REG_FIFO_CTRL1, 200); /* Legacy watermark; no INT pin. */
	if (ret < 0) return ret;
	ret = write_reg(REG_FIFO_CTRL2, 0);
	if (ret < 0) return ret;
	initialized = true;
	current_mode = GT02_IMU_MODE_LOW;
	ret = gt02_imu_zephyr_set_mode(GT02_IMU_MODE_WORK);
	if (ret == 0) LOG_INF("LSM6DSO FIFO ready, WHO_AM_I=0x%02x", id);
	return ret;
}

static int16_t decode_axis(const uint8_t *bytes)
{
	return (int16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8));
}

int gt02_imu_zephyr_read_batch(struct gt02_imu_sample *samples,
				      size_t capacity, size_t *count)
{
	uint8_t status[2], tag, data[6];
	uint16_t entries;
	int ret;

	if (samples == NULL || count == NULL || capacity == 0) return -EINVAL;
	*count = 0;
	if (!initialized) return -ENODEV;
	ret = i2c_burst_read_dt(&imu, REG_FIFO_STATUS1, status, sizeof(status));
	if (ret < 0) goto failed;
	if (status[1] & (FIFO_OVERRUN | BIT(3))) {
		LOG_WRN("IMU FIFO overrun; discarding stale samples");
		ret = fifo_restart();
		if (ret < 0) {
			initialized = false;
			goto failed;
		}
		return -EOVERFLOW;
	}
	entries = status[0] | ((uint16_t)(status[1] & 0x03) << 8);
	for (uint16_t i = 0; i < entries && *count < capacity; ++i) {
		/* Match the legacy FIFO protocol: read tag, then the six data bytes. */
		ret = i2c_reg_read_byte_dt(&imu, REG_FIFO_TAG, &tag);
		if (ret < 0) goto failed;
		ret = i2c_burst_read_dt(&imu, REG_FIFO_TAG + 1, data, sizeof(data));
		if (ret < 0) goto failed;
		switch (tag >> 3) {
		case 0x02: /* Accelerometer. */
			for (int axis = 0; axis < 3; ++axis) {
				int16_t raw = decode_axis(&data[axis * 2]);

				/* Legacy +/-2 g normalization, expressed in mm/s^2. */
				pending.accel_mm_s2[axis] = (int32_t)((int64_t)raw * 19613 / 32768);
			}
			have_accel = true;
			break;
		case 0x01: /* Gyroscope. */
			for (int axis = 0; axis < 3; ++axis) {
				int16_t raw = decode_axis(&data[axis * 2]);

				/* Legacy +/-1000 dps normalization, expressed in mrad/s. */
				pending.gyro_mrad_s[axis] =
					(int32_t)((int64_t)raw * 17453 / 32768);
			}
			have_gyro = true;
			break;
		default:
			have_accel = false;
			have_gyro = false;
			continue;
		}
		if (have_accel && (have_gyro || current_mode == GT02_IMU_MODE_LOW)) {
			if (current_mode == GT02_IMU_MODE_LOW) {
				for (int axis = 0; axis < 3; ++axis) pending.gyro_mrad_s[axis] = 0;
			}
			pending.timestamp_ms = k_uptime_get(); /* Drain time, not FIFO frame time. */
			samples[(*count)++] = pending;
			have_accel = false;
			have_gyro = false;
		}
	}
	return 0;

failed:
	have_accel = false;
	have_gyro = false;
	return ret;
}
