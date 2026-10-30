#ifndef GT02_IMU_ZEPHYR_H_
#define GT02_IMU_ZEPHYR_H_

#include <stddef.h>
#include "interfaces/gt02_imu.h"

enum gt02_imu_mode {
	GT02_IMU_MODE_WORK,
	GT02_IMU_MODE_LOW,
};

/* Only this adapter owns the LSM6DSO registers and imu0 I2C address. */
int gt02_imu_zephyr_init(void);
int gt02_imu_zephyr_set_mode(enum gt02_imu_mode mode);
int gt02_imu_zephyr_read_batch(struct gt02_imu_sample *samples,
				      size_t capacity, size_t *count);

#endif
