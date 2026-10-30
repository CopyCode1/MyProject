#ifndef GT02_FLASH_SELFTEST_H_
#define GT02_FLASH_SELFTEST_H_

/*
 * 破坏性板上验证，不是产品存储接口。
 * 仅在启动线程中、Flash只读身份检查成功后、IMU启动前调用。
 * 每次启动最多尝试一次；返回0表示完整通过，负errno表示失败。
 * 批准区域的旧数据不会备份或恢复；失败时保留现场，不追加擦除。
 */
int gt02_flash_selftest_run(void);

#endif /* GT02_FLASH_SELFTEST_H_ */
