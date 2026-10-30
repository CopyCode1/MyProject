/**
 * @file
 * @brief 临时验证4G上下电循环和UART原始URC接收，不实现AT协议。
 */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include "adapters/modem/gt02_modem_zephyr.h"
#include "tests/bringup/gt02_modem_hw_test.h"

LOG_MODULE_REGISTER(gt02_modem_test, LOG_LEVEL_INF);

#define CYCLE_INTERVAL_MS 10000
#define RX_LINE_SIZE 256
#define TEST_STACK_SIZE 2048

/* 测试状态仅标记测试阶段，不表示注册、TCP或模组内部状态。 */
enum test_phase {
	TEST_IDLE,       /* 尚未调用测试启动入口。 */
	TEST_STARTING,   /* 正在启动UART及执行硬件上电步骤。 */
	TEST_ONLINE,     /* 上电接口已成功，等待下次定时切电。 */
	TEST_STOPPING,   /* 主动硬件断电及串口收尾。 */
	TEST_FAILED,     /* 接口失败，停止测试且不自动恢复。 */
};

/* 只由接收线程持有，避免UART分片或两个启动周期拼成一条假URC。 */
struct rx_line {
	char text[RX_LINE_SIZE + 1]; /* 日志副本，额外预留NUL，不修改原始字节。 */
	size_t used;               /* 当前副本有效字符数，转义后可能多于原始字节数。 */
	bool fragmented;           /* 当前长行是否已输出过前面的片段。 */
};

static atomic_t phase;
static atomic_t rx_failure;
static atomic_t rx_session; /* 只区分本测试主动发起的硬件周期。 */

/* 输出行尾或指定原因的未完成文本；不把不完整内容伪装成完整URC。 */
static void flush_rx_line(struct rx_line *line, const char *partial_reason)
{
	if (line->used != 0) {
		line->text[line->used] = '\0';
		if (partial_reason != NULL) {
			LOG_WRN("4G RX [partial, %s]: %s", partial_reason, line->text);
		} else if (line->fragmented) {
			LOG_INF("4G RX [last part]: %s", line->text);
		} else {
			LOG_INF("4G RX: %s", line->text);
		}
	}
	line->used = 0;
	line->fragmented = false;
}

/* 跨read拼接完整文本行；非文本字节转义显示，长行分段显示且不静默截断。 */
static void print_rx_bytes(struct rx_line *line, const uint8_t *bytes, size_t count)
{
	static const char hex[] = "0123456789ABCDEF";

	for (size_t i = 0; i < count; ++i) {
		uint8_t byte = bytes[i];
		bool printable = byte >= 0x20 && byte <= 0x7e;
		size_t width = printable ? 1 : 4;

		if (byte == '\r' || byte == '\n') {
			flush_rx_line(line, NULL);
			continue;
		}
		if (line->used + width > RX_LINE_SIZE) {
			line->text[line->used] = '\0';
			LOG_INF("4G RX [part]: %s", line->text);
			line->used = 0;
			line->fragmented = true;
		}
		if (printable) {
			line->text[line->used++] = (char)byte;
		} else {
			line->text[line->used++] = '\\';
			line->text[line->used++] = 'x';
			line->text[line->used++] = hex[byte >> 4];
			line->text[line->used++] = hex[byte & 0x0f];
		}
	}
}

/* 唯一RX消费者，持续接收，包括power_on尚未返回时；不在UART ISR中打印。 */
static void rx_worker(void *p1, void *p2, void *p3)
{
	struct rx_line line = {0};
	uint8_t bytes[64];
	atomic_val_t session = atomic_get(&rx_session);

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	while (atomic_get(&phase) != TEST_FAILED) {
		size_t received;
		enum test_phase read_phase;
		int ret;

		if (session != atomic_get(&rx_session)) {
			flush_rx_line(&line, "power cycle");
			session = atomic_get(&rx_session);
		}
		read_phase = (enum test_phase)atomic_get(&phase);
		ret = gt02_modem_hw_read(bytes, sizeof(bytes), &received, K_MSEC(100));
		if (session != atomic_get(&rx_session)) {
			/* 切电边界可能同时取到旧字节，丢弃本批，不拼进下次启动文本。 */
			flush_rx_line(&line, "power cycle");
			session = atomic_get(&rx_session);
			continue;
		}
		if (ret == 0) {
			print_rx_bytes(&line, bytes, received);
		} else if (ret != -EAGAIN) {
			flush_rx_line(&line, "read interrupted");
			if (ret != -EPIPE && read_phase == TEST_ONLINE &&
			    atomic_get(&phase) == TEST_ONLINE) {
				LOG_ERR("4G RX failed: %d; power-cycle test will stop", ret);
				atomic_set(&rx_failure, ret);
				return;
			}
			/* 未上电/主动下电属于预期状态；启动期错误由power_on统一报告。 */
			k_sleep(K_MSEC(20));
		}
	}
	flush_rx_line(&line, "test stopped");
}

/* 执行首次上电及10秒周期；只拥有上下电动作，不占用main或系统工作队列。 */
static void power_worker(void *p1, void *p2, void *p3)
{
	uint32_t round = 0;
	int64_t deadline;
	int ret;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	LOG_WRN("TEMP 4G test: restart every 10000 ms after initial power-on; raw RX only, no AT/TCP");
	LOG_INF("4G initial power_on begin");
	ret = gt02_modem_hw_power_on();
	LOG_INF("4G initial power_on result=%d", ret);
	if (ret < 0) {
		goto failed;
	}
	atomic_set(&phase, TEST_ONLINE);
	deadline = k_uptime_get() + CYCLE_INTERVAL_MS;
	for (;;) {
		int64_t remaining = deadline - k_uptime_get();

		if (remaining > 0) {
			k_sleep(K_MSEC(remaining));
		}
		ret = (int)atomic_get(&rx_failure);
		if (ret < 0) {
			goto failed;
		}
		++round;
		atomic_set(&phase, TEST_STOPPING);
		atomic_inc(&rx_session);
		LOG_INF("4G cycle=%u power_off begin", (unsigned int)round);
		ret = gt02_modem_hw_power_off();
		LOG_INF("4G cycle=%u power_off result=%d", (unsigned int)round, ret);
		if (ret < 0) {
			goto failed;
		}
		atomic_set(&phase, TEST_STARTING);
		LOG_INF("4G cycle=%u power_on begin", (unsigned int)round);
		ret = gt02_modem_hw_power_on();
		LOG_INF("4G cycle=%u power_on result=%d", (unsigned int)round, ret);
		if (ret < 0) {
			goto failed;
		}
		atomic_set(&phase, TEST_ONLINE);
		deadline += CYCLE_INTERVAL_MS;
		if (deadline <= k_uptime_get()) {
			/* 驱动异常耗时后不追赶补发多次断电，避免形成快速重启。 */
			LOG_WRN("4G cycle overran 10 s; next cycle starts after another 10 s");
			deadline = k_uptime_get() + CYCLE_INTERVAL_MS;
		}
	}
failed:
	atomic_set(&phase, TEST_FAILED);
	LOG_ERR("4G hardware test stopped: error=%d; no automatic recovery; main continues", ret);
}

K_THREAD_DEFINE(modem_test_rx_thread, TEST_STACK_SIZE, rx_worker,
		NULL, NULL, NULL, 6, 0, SYS_FOREVER_MS);
K_THREAD_DEFINE(modem_test_power_thread, TEST_STACK_SIZE, power_worker,
		NULL, NULL, NULL, 7, 0, SYS_FOREVER_MS);

/* 唯一公共测试入口，只启动两个上下文；失败后也不允许重复启动同一线程。 */
int gt02_modem_hw_test_start(void)
{
	if (!atomic_cas(&phase, TEST_IDLE, TEST_STARTING)) {
		return -EALREADY;
	}
	k_thread_start(modem_test_rx_thread);
	k_thread_start(modem_test_power_thread);
	return 0;
}
