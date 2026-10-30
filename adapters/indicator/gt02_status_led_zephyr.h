#ifndef GT02_STATUS_LED_ZEPHYR_H_
#define GT02_STATUS_LED_ZEPHYR_H_

/* 配置设备树led0为输出并点亮；失败返回负错误码。 */
int gt02_status_led_init(void);

/* 翻转已初始化的LED；调用方负责周期，不在这里休眠或创建线程。 */
int gt02_status_led_toggle(void);

#endif /* GT02_STATUS_LED_ZEPHYR_H_ */
