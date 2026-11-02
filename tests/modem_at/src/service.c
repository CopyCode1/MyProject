/** @file
 * @brief 运行真实最小服务；默认接真实Lierda/AT，另用同步回调后端验证能力隔离。
 */
#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/ztest.h>

#include "services/network/gt02_network_service.h"
#include "adapters/modem/gt02_modem_lierda.h"

K_MSGQ_DEFINE(fake_rx, sizeof(uint8_t), 1024, 1);
static atomic_t power_count;
static atomic_t powered;
static atomic_t tx_count;
static gt02_modem_event_cb_t inline_callback;
static void *inline_context;

/* 模拟原始串口回复，仍由真实SDK解析器重组文本。 */
static void inject_text(const char *text)
{
	for (size_t i = 0; i < strlen(text); ++i) {
		zassert_ok(k_msgq_put(&fake_rx, &text[i], K_NO_WAIT));
	}
}

/* 核对服务已经发布正确阶段；不从回调或假硬件直接修改服务状态。 */
static void expect_phase(enum gt02_network_state expected)
{
	struct gt02_network_status snapshot;

	zassert_ok(gt02_network_service_get_status(&snapshot));
	zassert_equal(snapshot.state, expected);
}

/* 只允许上电一次，按场景模拟错误；不访问真实GPIO，也不要求服务调用其他电源方法。 */
static int fake_power_on(void)
{
	expect_phase(GT02_NETWORK_POWERING_ON);
	zassert_equal(atomic_inc(&power_count), 0);
	if (CONFIG_AT_TEST_SERVICE_CASE == 4) {
		return -EIO;
	}
	atomic_set(&powered, 1);
	inject_text("RDY\r\n");
	return 0;
}

/* 接收在上电前返回-EPIPE，之后按字节读取，保留真实初始未上电语义。 */
static int fake_read(uint8_t *buffer, size_t capacity, size_t *received, k_timeout_t timeout)
{
	zassert_true(capacity > 0);
	*received = 0;
	if (!atomic_get(&powered)) {
		return -EPIPE;
	}
	if (k_msgq_get(&fake_rx, buffer, timeout) != 0) {
		return -EAGAIN;
	}
	*received = 1;
	return 0;
}

/* 限定服务仅触发AT/ATE0/CPIN，任何多余指令、自动重试或厂家流程错序都失败。 */
static int fake_write(const uint8_t *buffer, size_t length, size_t *written)
{
	static const char *const expected[] = { "AT", "ATE0", "AT+CPIN?" };
	unsigned int index = (unsigned int)atomic_inc(&tx_count);

	zassert_true(index < ARRAY_SIZE(expected));
	expect_phase(index == 2 ? GT02_NETWORK_QUERYING_SIM : GT02_NETWORK_PREPARING);
	zassert_equal(length, strlen(expected[index]) + 2);
	zassert_mem_equal(buffer, expected[index], length - 2);
	zassert_mem_equal(buffer + length - 2, "\r\n", 2);
	*written = length;
	if (CONFIG_AT_TEST_SERVICE_CASE == 2 && index == 0) {
		inject_text("ERROR\r\n");
	} else if (CONFIG_AT_TEST_SERVICE_CASE == 3 && index == 2) {
		/* 不回复SIM，真实AT核心最终报告超时。 */
	} else if (index == 2) {
		inject_text(CONFIG_AT_TEST_SERVICE_CASE == 1 ? "+CPIN: SIM PIN\r\nOK\r\n" :
			    "+CPIN: READY\r\nOK\r\n");
	} else {
		inject_text("OK\r\n");
	}
	return 0;
}

/* 替代厂家只注册公共回调，验证服务不要求Lierda实现或AT命令ID。 */
static int inline_init(gt02_modem_event_cb_t callback, void *context)
{
	inline_callback = callback;
	inline_context = context;
	return 0;
}

/* 故意在prepare返回前交付完成事件，确保服务没有依赖“返回之后才回调”的假设。 */
static int inline_prepare(void)
{
	const struct gt02_modem_event event = { .type = GT02_MODEM_EVENT_PREPARE_DONE };

	expect_phase(GT02_NETWORK_PREPARING);
	inline_callback(&event, inline_context);
	return 0;
}

/* 同步交付栈上的查询事件，服务必须复制事件而不能保留指针。 */
static int inline_query(enum gt02_modem_query query)
{
	const struct gt02_modem_event event = {
		.type = GT02_MODEM_EVENT_QUERY_DONE, .query = query, .sim = GT02_MODEM_SIM_READY,
	};

	zassert_equal(query, GT02_MODEM_QUERY_SIM);
	expect_phase(GT02_NETWORK_QUERYING_SIM);
	inline_callback(&event, inline_context);
	return 0;
}

/* 每个场景使用一次完整开机，符合服务无重启/恢复接口的首版约定。 */
ZTEST(gt02_service, test_one_shot_service)
{
	static const struct gt02_at_io_ops io = { .read = fake_read, .write = fake_write };
	static const struct gt02_at_config at = { .profile = &gt02_lierda_at_profile, .io = &io };
	static const struct gt02_modem_power_ops power = { .power_on = fake_power_on };
	static const struct gt02_modem_ops immediate = {
		.init = inline_init, .prepare = inline_prepare, .query = inline_query,
	};
	struct gt02_network_config provided = {
		.power = &power,
		.modem = CONFIG_AT_TEST_SERVICE_CASE == 5 ? &immediate : &gt02_lierda_ops,
		.startup_delay_ms = 0,
	};
	struct gt02_network_status snapshot;

	zassert_equal(gt02_network_service_get_status(NULL), -EINVAL);
	expect_phase(GT02_NETWORK_STOPPED);
	zassert_equal(gt02_network_service_start(NULL), -EINVAL);
	if (CONFIG_AT_TEST_SERVICE_CASE != 5) {
		zassert_ok(gt02_at_init(&at));
	}
	zassert_ok(gt02_network_service_start(&provided));
	zassert_equal(gt02_network_service_start(&provided), -EALREADY);
	/* 配置本身只在调用期间借用，原对象改变不得影响服务存储的配置。 */
	provided.modem = NULL;
	int64_t deadline = k_uptime_get() + 8000;

	do {
		zassert_ok(gt02_network_service_get_status(&snapshot));
		if (snapshot.state >= GT02_NETWORK_SIM_READY) {
			break;
		}
		k_sleep(K_MSEC(10));
	} while (k_uptime_get() < deadline);
	int mode = CONFIG_AT_TEST_SERVICE_CASE;

	if (mode == 0 || mode == 5) {
		zassert_equal(snapshot.state, GT02_NETWORK_SIM_READY);
		zassert_equal(snapshot.sim, GT02_MODEM_SIM_READY);
		zassert_equal(snapshot.error, 0);
	} else if (mode == 1) {
		zassert_equal(snapshot.state, GT02_NETWORK_SIM_NOT_READY);
		zassert_equal(snapshot.sim, GT02_MODEM_SIM_PIN_REQUIRED);
		zassert_equal(snapshot.error, 0);
	} else {
		zassert_equal(snapshot.state, GT02_NETWORK_FAILED);
		zassert_equal(snapshot.sim, GT02_MODEM_SIM_UNKNOWN);
		zassert_equal(snapshot.error, mode == 3 ? -ETIMEDOUT : -EIO);
	}
	k_sleep(K_MSEC(50));
	zassert_equal(atomic_get(&power_count), 1);
	zassert_equal(atomic_get(&tx_count), mode == 2 ? 1 : (mode == 4 || mode == 5 ? 0 : 3));
}

ZTEST_SUITE(gt02_service, NULL, NULL, NULL, NULL, NULL);
