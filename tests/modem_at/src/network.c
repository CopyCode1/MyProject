/** @file
 * @brief 运行真实周期服务与厂家/AT解析；用假电源和字节流验证跨轮结果与故障边界。
 */
#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/ztest.h>

#include "services/network/gt02_network_service.h"
#include "adapters/modem/gt02_modem_lierda.h"

K_MSGQ_DEFINE(fake_rx, sizeof(uint8_t), 1024, 1);
K_SEM_DEFINE(round_held, 0, 1);
K_SEM_DEFINE(release_round, 0, 1);
static atomic_t power_count;
static atomic_t powered;
static atomic_t tx_count;
static unsigned int round_number; /* 实际UART场景由AT线程独占，替代后端场景由服务线程独占。 */
static unsigned int query_step;
static gt02_modem_event_cb_t inline_callback;
static void *inline_context;

/* 注入原始串口字节，保证真实SDK解析器和厂家回调均参与测试。 */
static void inject_text(const char *text)
{
	for (size_t i = 0; i < strlen(text); ++i) {
		zassert_ok(k_msgq_put(&fake_rx, &text[i], K_NO_WAIT));
	}
}

/* 启动通知可以在上电返回前发生；缺失场景不注入，替代后端同步交付栈事件。 */
static int fake_power_on(void)
{
	zassert_equal(atomic_inc(&power_count), 0);
	atomic_set(&powered, 1);
	if (CONFIG_AT_TEST_NETWORK_CASE == 7) {
		const struct gt02_modem_event event = { .type = GT02_MODEM_EVENT_STARTUP_READY };

		inline_callback(&event, inline_context);
	} else if (CONFIG_AT_TEST_NETWORK_CASE != 4) {
		inject_text(CONFIG_AT_TEST_NETWORK_CASE == 5 ? "RDY\r\nRDY\r\nRDY\r\n" :
			    "RDY\r\n");
		k_sleep(K_MSEC(40));
		zassert_equal(atomic_get(&tx_count), 0, "AT sent before power_on returned");
	}
	return 0;
}

/* 初始未上电返回-EPIPE；上电后每次读一个字节，保留SDK的分片拼行路径。 */
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

/* 严格限制为启动握手与四项只读查询；下一轮首个TX暂停，便于观察尚未发布的临时结果。 */
static int fake_write(const uint8_t *buffer, size_t length, size_t *written)
{
	static const char *const commands[] = { "AT+CPIN?", "AT+CSQ", "AT+CEREG?", "AT+QIACT?" };
	unsigned int index = (unsigned int)atomic_inc(&tx_count);
	const char *expected = index < 2 ? (index == 0 ? "AT" : "ATE0") : commands[query_step];

	zassert_equal(length, strlen(expected) + 2);
	zassert_mem_equal(buffer, expected, length - 2);
	zassert_mem_equal(buffer + length - 2, "\r\n", 2);
	*written = length;
	if (CONFIG_AT_TEST_NETWORK_CASE == 5) {
		inject_text("RDY\r\n");
	}
	if (index < 2) {
		inject_text("OK\r\n");
		return 0;
	}
	if (query_step == 0) {
		++round_number;
		if (CONFIG_AT_TEST_NETWORK_CASE <= 3 && round_number >= 2 && round_number <= 3) {
			k_sem_give(&round_held);
			zassert_ok(k_sem_take(&release_round, K_SECONDS(2)));
		}
		if (CONFIG_AT_TEST_NETWORK_CASE != 6 || index != 2) {
			inject_text("+CPIN: READY\r\nOK\r\n");
		} else {
			/* 第一条SIM故意超时，下轮仍从SIM重新开始。 */
			return 0;
		}
	} else if (query_step == 1) {
		/* 99和0均是可报告的信号值，不能单凭强度未知/弱就拒绝数据就绪。 */
		inject_text(round_number == 1 ? "+CSQ: 99,99\r\nOK\r\n" : "+CSQ: 0,0\r\nOK\r\n");
	} else if (query_step == 2) {
		if (CONFIG_AT_TEST_NETWORK_CASE == 0 && round_number == 2) {
			inject_text("+CEREG: 0,2\r\nOK\r\n");
			query_step = 0;
			return 0;
		}
		inject_text("+CEREG: 0,1\r\nOK\r\n");
	} else if (CONFIG_AT_TEST_NETWORK_CASE == 1 && round_number == 1) {
		inject_text("OK\r\n");
	} else if (CONFIG_AT_TEST_NETWORK_CASE == 2 && round_number == 1) {
		inject_text("+QIACT: 1,1,1,\"0.0.0.0\"\r\nOK\r\n");
	} else if (CONFIG_AT_TEST_NETWORK_CASE == 3 && round_number == 1) {
		inject_text("+QIACT: 1,1,1,\"10.22.0.1\"\r\nERROR\r\n");
	} else {
		inject_text("+QIACT: 1,1,1,\"10.22.0.1\"\r\nOK\r\n");
	}
	query_step = (query_step + 1) % ARRAY_SIZE(commands);
	return 0;
}

/* 替代后端只绑定长期事件出口，验证服务不依赖Lierda或AT实现。 */
static int inline_init(gt02_modem_event_cb_t callback, void *context)
{
	inline_callback = callback;
	inline_context = context;
	return 0;
}

/* 故意在接口返回前完成准备，保证服务先登记期望状态再调用下层。 */
static int inline_prepare(void)
{
	const struct gt02_modem_event event = { .type = GT02_MODEM_EVENT_PREPARE_DONE };

	inline_callback(&event, inline_context);
	return 0;
}

/* 第一轮立即完成，第二轮信号查询不通知，分别触发快照过期和服务等待保护。 */
static int inline_query(enum gt02_modem_query query)
{
	struct gt02_modem_event event = {
		.type = GT02_MODEM_EVENT_QUERY_DONE, .query = query,
		.sim = GT02_MODEM_SIM_READY, .signal_rssi = 99, .registration = 1,
		.data_context = { .active = true, .ipv4 = { 10, 22, 0, 1 } },
	};

	if (query == GT02_MODEM_QUERY_SIM) {
		++round_number;
	}
	if (round_number == 2 && query == GT02_MODEM_QUERY_SIGNAL) {
		return 0;
	}
	inline_callback(&event, inline_context);
	return 0;
}

/* 只等待公开可观察结论，不读取服务内部变量；checked_at区分前后轮次。 */
static struct gt02_network_status wait_status(enum gt02_network_state state, int error,
					    int64_t after_ms)
{
	struct gt02_network_status snapshot;
	int64_t deadline = k_uptime_get() + 5000;

	do {
		zassert_ok(gt02_network_service_get_status(&snapshot));
		if (snapshot.state == state && snapshot.error == error &&
		    snapshot.checked_at_ms >= after_ms) {
			return snapshot;
		}
		k_sleep(K_MSEC(5));
	} while (k_uptime_get() < deadline);
	zassert_unreachable("state=%d error=%d; expected state=%d error=%d",
			    snapshot.state, snapshot.error, state, error);
	return snapshot;
}

/* 单次开机验证周期维护，异常场景不会重新上电或隐式执行APN、激活、TCP命令。 */
ZTEST(gt02_network, test_periodic_readiness)
{
	static const struct gt02_at_io_ops io = { .read = fake_read, .write = fake_write };
	static const struct gt02_at_config at = { .profile = &gt02_lierda_at_profile, .io = &io };
	static const struct gt02_modem_power_ops power = { .power_on = fake_power_on };
	static const struct gt02_modem_ops immediate = {
		.init = inline_init, .prepare = inline_prepare, .query = inline_query,
	};
	struct gt02_network_config config = {
		.power = &power, .modem = CONFIG_AT_TEST_NETWORK_CASE == 7 ? &immediate : &gt02_lierda_ops,
		.startup_timeout_ms = 200, .operation_timeout_ms = CONFIG_AT_TEST_NETWORK_CASE == 7 ? 500 : 3000,
		.retry_interval_ms = 30, .check_interval_ms = 50,
		.result_valid_ms = CONFIG_AT_TEST_NETWORK_CASE == 7 ? 150 : 1000,
	};
	struct gt02_network_status snapshot;
	int mode = CONFIG_AT_TEST_NETWORK_CASE;

	zassert_equal(gt02_network_service_get_status(NULL), -EINVAL);
	zassert_ok(gt02_network_service_get_status(&snapshot));
	zassert_equal(snapshot.state, GT02_NETWORK_STOPPED);
	zassert_equal(gt02_network_service_start(NULL), -EINVAL);
	config.result_valid_ms = config.check_interval_ms;
	zassert_equal(gt02_network_service_start(&config), -EINVAL);
	config.result_valid_ms = mode == 7 ? 150 : 1000;
	if (mode != 7) {
		zassert_ok(gt02_at_init(&at));
	}
	zassert_ok(gt02_network_service_start(&config));
	zassert_equal(gt02_network_service_start(&config), -EALREADY);
	config.modem = NULL; /* 服务必须已复制配置，不能继续借用调用栈。 */

	if (mode == 4) {
		snapshot = wait_status(GT02_NETWORK_FAULT, -ETIMEDOUT, 0);
		int before = atomic_get(&tx_count);

		zassert_equal(before, 0);
		inject_text("RDY\r\n");
		k_sleep(K_MSEC(80));
		zassert_ok(gt02_network_service_get_status(&snapshot));
		zassert_equal(snapshot.state, GT02_NETWORK_FAULT);
		zassert_equal(atomic_get(&tx_count), before);
	} else if (mode == 6) {
		snapshot = wait_status(GT02_NETWORK_NOT_READY, -ETIMEDOUT, 0);
		wait_status(GT02_NETWORK_NOT_READY, -EAGAIN, snapshot.checked_at_ms);
		zassert_equal(atomic_get(&tx_count), 3, "retry sent before old transaction ended");
		inject_text("+CPIN: READY\r\nOK\r\n");
		snapshot = wait_status(GT02_NETWORK_DATA_READY, 0, snapshot.checked_at_ms + 1);
		zassert_true(snapshot.data_context.active);
	} else if (mode == 7) {
		snapshot = wait_status(GT02_NETWORK_DATA_READY, 0, 0);
		wait_status(GT02_NETWORK_NOT_READY, -ESTALE, snapshot.checked_at_ms);
		wait_status(GT02_NETWORK_FAULT, -ETIMEDOUT, 0);
		const struct gt02_modem_event late = {
			.type = GT02_MODEM_EVENT_QUERY_DONE, .query = GT02_MODEM_QUERY_SIGNAL,
		};

		inline_callback(&late, inline_context);
		k_sleep(K_MSEC(60));
		zassert_ok(gt02_network_service_get_status(&snapshot));
		zassert_equal(snapshot.state, GT02_NETWORK_FAULT);
		zassert_equal(atomic_get(&tx_count), 0);
	} else if (mode == 5) {
		snapshot = wait_status(GT02_NETWORK_DATA_READY, 0, 0);
		zassert_equal(snapshot.signal_rssi, 99);
		zassert_true(snapshot.data_context.active);
		zassert_equal(atomic_get(&tx_count), 6);
	} else {
		zassert_ok(k_sem_take(&round_held, K_SECONDS(4)));
		zassert_ok(gt02_network_service_get_status(&snapshot));
		zassert_equal(snapshot.state, mode == 0 ? GT02_NETWORK_DATA_READY : GT02_NETWORK_NOT_READY);
		zassert_equal(snapshot.error, mode == 3 ? -EIO : 0);
		if (mode == 3) {
			zassert_equal(snapshot.sim, GT02_MODEM_SIM_UNKNOWN);
			zassert_equal(snapshot.registration, 0);
			zassert_false(snapshot.data_context.active);
		}
		int64_t first_time = snapshot.checked_at_ms;
		k_sem_give(&release_round);
		zassert_ok(k_sem_take(&round_held, K_SECONDS(4)));
		zassert_ok(gt02_network_service_get_status(&snapshot));
		zassert_true(snapshot.checked_at_ms > first_time);
		zassert_equal(snapshot.state, mode == 0 ? GT02_NETWORK_NOT_READY : GT02_NETWORK_DATA_READY);
		zassert_equal(snapshot.error, 0);
		if (mode == 0) {
			zassert_equal(snapshot.registration, 2);
		}
		int64_t second_time = snapshot.checked_at_ms;
		k_sem_give(&release_round);
		snapshot = wait_status(GT02_NETWORK_DATA_READY, 0, second_time + 1);
		zassert_true(snapshot.data_context.active);
		zassert_equal(snapshot.signal_rssi, 0);
	}
	zassert_equal(atomic_get(&power_count), 1);
}

ZTEST_SUITE(gt02_network, NULL, NULL, NULL, NULL, NULL);
