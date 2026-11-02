#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "app/gt02_app.h"
#include "adapters/indicator/gt02_status_led_zephyr.h"
#include "adapters/storage/gt02_flash_zephyr.h"
#include "services/imu/gt02_imu_service.h"
#if defined(CONFIG_GT02_MODEM_HW)
#include "adapters/modem/gt02_modem_zephyr.h"
#endif
#if defined(CONFIG_GT02_MODEM_HW_TEST)
#include "tests/bringup/gt02_modem_hw_test.h"
#endif
#if defined(CONFIG_GT02_MODEM_AT_TEST)
#include "tests/bringup/gt02_modem_at_test.h"
#endif
#if defined(CONFIG_GT02_NETWORK_SERVICE)
#include "services/network/gt02_network_service.h"
#endif
#if defined(CONFIG_GT02_MODEM_AT_TEST) || defined(CONFIG_GT02_NETWORK_SERVICE)
#include "adapters/modem/gt02_modem_lierda.h"
#endif
#if defined(CONFIG_GT02_FLASH_SELF_TEST)
#include "tests/bringup/gt02_flash_selftest.h"
#endif

LOG_MODULE_REGISTER(gt02_app, LOG_LEVEL_INF);

#define IMU_POLL_INTERVAL_MS 100
#define HEARTBEAT_INTERVAL_MS 1000

#if defined(CONFIG_GT02_MODEM_AT_TEST) || defined(CONFIG_GT02_NETWORK_SERVICE)
/* 装配层是唯一知道当前选用Lierda协议及Zephyr硬件实现的位置。 */
static const struct gt02_at_config modem_at_config = {
	.profile = &gt02_lierda_at_profile,
	.io = &gt02_modem_zephyr_io,
};
#endif

#if defined(CONFIG_GT02_NETWORK_SERVICE)
/* 启动等待沿用当前GT02/NT26-KCN验证值，服务本身不知道具体厂家或引脚。 */
static const struct gt02_network_config network_config = {
	.power = &gt02_modem_zephyr_power,
	.modem = &gt02_lierda_ops,
	.startup_delay_ms = 2000,
};
#endif

/* 装配外设及4G控制入口；服务负责一次上电/SIM流程，主线程继续运行LED等功能。 */
int gt02_app_run(void)
{
	uint32_t counter = 0;
	uint32_t samples_since_log = 0;
	int64_t next_heartbeat_ms;
	bool led_on = true;
	bool imu_ready;
	int ret;

	/* LED初始化不依赖调试器或日志接收端；成功后立即点亮。 */
	ret = gt02_status_led_init();
	if (ret < 0) {
		LOG_ERR("Heartbeat LED initialization failed: %d", ret);
		return ret;
	}
	LOG_INF("GT02 start: LED heartbeat + IMU FIFO + external Flash read-only probe");
	/* Flash检查失败不阻止六轴和LED运行；不重试、不擦写旧数据。 */
	ret = gt02_flash_zephyr_probe();
	if (ret < 0) {
		LOG_WRN("External Flash probe failed: %d; continuing without storage", ret);
	}
#if defined(CONFIG_GT02_FLASH_SELF_TEST)
	else {
		/* 已授权测试区的单次破坏性测试；IMU尚未启动，不会堆积FIFO。 */
		ret = gt02_flash_selftest_run();
		if (ret < 0) {
			LOG_ERR("External Flash self-test failed: %d; IMU/LED test will continue", ret);
		}
	}
#endif

	/* 单一线程拥有IMU服务；工作模式默认104 Hz，硬件FIFO持续采样。 */
	ret = gt02_imu_service_init();
	imu_ready = (ret == 0);
	if (!imu_ready) {
		LOG_ERR("IMU initialization failed: %d; LED heartbeat remains active", ret);
	} else {
		LOG_INF("LSM6DSO FIFO polling started: 104 Hz, poll=%d ms",
			IMU_POLL_INTERVAL_MS);
	}
#if defined(CONFIG_GT02_MODEM_HW)
	/* 准备控制脚；新旧验证入口互斥，各自拥有唯一的UART接收路径。 */
	ret = gt02_modem_zephyr_power.init();
	if (ret < 0) {
		LOG_WRN("4G hardware preparation failed: %d; other peripherals continue", ret);
	} else {
		LOG_INF("4G hardware prepared: supply inactive, UART not started");
#if defined(CONFIG_GT02_MODEM_HW_TEST)
		ret = gt02_modem_hw_test_start();
		if (ret < 0) {
			LOG_WRN("4G hardware test start failed: %d; other peripherals continue", ret);
		}
#endif
#if defined(CONFIG_GT02_MODEM_AT_TEST)
		ret = gt02_modem_at_test_start(&modem_at_config, &gt02_modem_zephyr_power);
		if (ret < 0) {
			LOG_WRN("4G AT test start failed: %d; other peripherals continue", ret);
		}
#endif
#if defined(CONFIG_GT02_NETWORK_SERVICE)
		/* 先建立唯一AT接收路径，再启动服务线程上电；厂家选择仅出现在装配层。 */
		ret = gt02_at_init(&modem_at_config);
		if (ret == 0) {
			ret = gt02_network_service_start(&network_config);
		}
		if (ret < 0) {
			LOG_WRN("4G service start failed: %d; other peripherals continue", ret);
		}
#endif
	}
#endif
	LOG_INF("counter=%u, LED=ON", (unsigned int)counter);
	/* 必须在第一次比较之前赋值，避免原来未初始化计时变量的问题。 */
	next_heartbeat_ms = k_uptime_get() + HEARTBEAT_INTERVAL_MS;

	while (true) {
		// if (imu_ready) {
		// 	size_t processed;

		// 	ret = gt02_imu_service_poll(&processed);
		// 	if (ret == -EOVERFLOW) {
		// 		/* FIFO溢出会丢弃旧数据；下一轮继续，不假装数据连续。 */
		// 		samples_since_log = 0;
		// 		LOG_WRN("IMU FIFO data lost; discard this interval and poll again");
		// 	} else if (ret < 0) {
		// 		/* 不无限刷错误或重试；保留LED和串口以便定位。 */
		// 		LOG_ERR("IMU polling failed: %d; sampling stopped until reboot", ret);
		// 		imu_ready = false;
		// 	} else {
		// 		samples_since_log += (uint32_t)processed;
		// 	}
		// }
		if (k_uptime_get() >= next_heartbeat_ms) {
			ret = gt02_status_led_toggle();
			if (ret < 0) {
				LOG_ERR("Heartbeat LED toggle failed: %d", ret);
				return ret;
			}
			led_on = !led_on;
			// ++counter;
			// LOG_INF("counter=%u, LED=%s", (unsigned int)counter,
			// 	led_on ? "ON" : "OFF");
			// if (imu_ready) {
			// 	struct gt02_imu_sample sample;

			// 	ret = gt02_imu_service_get_latest(&sample);
			// 	if (ret == 0) {
			// 		/* 只打印最新帧；算法与上传后续通过服务接口接入。 */
			// 		LOG_INF("IMU samples=%u, accel[mm/s2]=(%d,%d,%d), gyro[mrad/s]=(%d,%d,%d)",
			// 			(unsigned int)samples_since_log,
			// 			(int)sample.accel_mm_s2[0], (int)sample.accel_mm_s2[1],
			// 			(int)sample.accel_mm_s2[2], (int)sample.gyro_mrad_s[0],
			// 			(int)sample.gyro_mrad_s[1], (int)sample.gyro_mrad_s[2]);
			// 	} else if (ret == -EAGAIN) {
			// 		LOG_WRN("IMU has no valid sample yet");
			// 	} else {
			// 		LOG_ERR("IMU snapshot failed: %d", ret);
			// 		imu_ready = false;
			// 	}
			// }
			samples_since_log = 0;
			next_heartbeat_ms = k_uptime_get() + HEARTBEAT_INTERVAL_MS;
		}
		/* 约100 ms排空FIFO，LED/数据日志仍约每秒更新；不使用忙等。 */
		k_sleep(K_MSEC(IMU_POLL_INTERVAL_MS));
	}
}
