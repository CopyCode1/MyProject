#include <errno.h>
#include <stdbool.h>

#include "adapters/imu/gt02_imu_zephyr.h"
#include "services/imu/gt02_imu_service.h"

#define IMU_BATCH_CAPACITY 16
#define IMU_MAX_BATCHES   32

/* One caller currently owns polling and this snapshot. */
static struct gt02_imu_sample batch[IMU_BATCH_CAPACITY];
static struct gt02_imu_sample latest_sample;
static bool latest_valid;

int gt02_imu_service_init(void)
{
	latest_valid = false;
	return gt02_imu_zephyr_init();
}

int gt02_imu_service_set_mode(enum gt02_imu_sampling_mode mode)
{
	int ret;

	if (mode != GT02_IMU_SAMPLING_WORK && mode != GT02_IMU_SAMPLING_LOW) {
		return -EINVAL;
	}
	ret = gt02_imu_zephyr_set_mode(mode == GT02_IMU_SAMPLING_WORK ?
				      GT02_IMU_MODE_WORK : GT02_IMU_MODE_LOW);
	latest_valid = false; /* Discard the previous or uncertain mode's snapshot. */
	return ret;
}

int gt02_imu_service_poll(size_t *processed)
{
	size_t count;
	int ret;

	if (processed == NULL) {
		return -EINVAL;
	}
	*processed = 0;
	for (int pass = 0; pass < IMU_MAX_BATCHES; ++pass) {
		ret = gt02_imu_zephyr_read_batch(batch, IMU_BATCH_CAPACITY, &count);
		if (ret < 0) {
			latest_valid = false;
			return ret;
		}
		if (count == 0) {
			return 0;
		}
		latest_sample = batch[count - 1];
		latest_valid = true;
		*processed += count;
	}
	latest_valid = false;
	return -EOVERFLOW; /* Poller could not keep up with producer. */
}

int gt02_imu_service_get_latest(struct gt02_imu_sample *sample)
{
	if (sample == NULL) {
		return -EINVAL;
	}
	if (!latest_valid) {
		return -EAGAIN;
	}
	*sample = latest_sample;
	return 0;
}
