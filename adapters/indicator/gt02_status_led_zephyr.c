#include <errno.h>
#include <stdbool.h>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>

#include "adapters/indicator/gt02_status_led_zephyr.h"

#if !DT_NODE_HAS_STATUS_OKAY(DT_ALIAS(led0))
#error "GT02 heartbeat test requires an enabled led0 alias"
#endif

/* 引脚和有效电平取自设备树；应用不依赖P1.15或低电平点亮。 */
static const struct gpio_dt_spec status_led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static bool initialized;

int gt02_status_led_init(void)
{
	int ret;

	initialized = false;
	if (!gpio_is_ready_dt(&status_led)) {
		return -ENODEV;
	}
	/* ACTIVE是逻辑点亮，GPIO驱动会处理设备树中的GPIO_ACTIVE_LOW。 */
	ret = gpio_pin_configure_dt(&status_led, GPIO_OUTPUT_ACTIVE);
	if (ret == 0) {
		initialized = true;
	}
	return ret;
}

int gt02_status_led_toggle(void)
{
	if (!initialized) {
		return -EACCES;
	}
	return gpio_pin_toggle_dt(&status_led);
}
