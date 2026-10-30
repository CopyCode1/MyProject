#ifndef GT02_IMU_SERVICE_H_
#define GT02_IMU_SERVICE_H_

#include <stddef.h>
#include "interfaces/gt02_imu.h"

enum gt02_imu_sampling_mode {
	GT02_IMU_SAMPLING_WORK,
	GT02_IMU_SAMPLING_LOW,
};

int gt02_imu_service_init(void);
int gt02_imu_service_set_mode(enum gt02_imu_sampling_mode mode);
/* Call approximately every 100 ms; processed is the number of complete samples drained. */
int gt02_imu_service_poll(size_t *processed);
/* Returns -EAGAIN before the first sample or after a read/mode-change failure. */
int gt02_imu_service_get_latest(struct gt02_imu_sample *sample);

#endif
