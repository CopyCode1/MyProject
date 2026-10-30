#ifndef GT02_IMU_H_
#define GT02_IMU_H_

#include <stdint.h>

/* 单位和坐标系属于接口契约；当前轴顺序仍是传感器原始坐标系。 */
struct gt02_imu_sample {
	int64_t timestamp_ms;         /* Zephyr 单调运行时间，单位 ms。 */
	int32_t accel_mm_s2[3];       /* x、y、z；单位 mm/s²。 */
	int32_t gyro_mrad_s[3];       /* x、y、z；单位 mrad/s。 */
};

#endif /* GT02_IMU_H_ */
