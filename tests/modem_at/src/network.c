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
K_SEM_DEFINE(operation_pending, 0, 1);
K_SEM_DEFINE(power_pending, 0, 1);
K_SEM_DEFINE(release_power, 0, 1);
static atomic_t power_count;
static atomic_t powered;
static atomic_t tx_count;
static atomic_t prepare_count;
static atomic_t query_count;
static atomic_t activation_count;
static atomic_t activation_succeeded;
static atomic_t ip_query_count;
static unsigned int round_number; /* 实际UART场景由AT线程独占，替代后端场景由服务线程独占。 */
static unsigned int query_step;
static int64_t prepare_retry_at_ms;
static gt02_modem_event_cb_t inline_callback;
static void *inline_context;

/* 注入原始串口字节，保证真实SDK解析器和厂家回调均参与测试。 */
static void inject_text(const char *text)
{
	for (size_t i = 0; i < strlen(text); ++i) {
		zassert_ok(k_msgq_put(&fake_rx, &text[i], K_NO_WAIT));
	}
}

/* 启动通知可以早于上电返回；慢上电场景暂停返回，验证业务线程仍能处理故障。 */
static int fake_power_on(void)
{
	zassert_equal(atomic_inc(&power_count), 0);
	atomic_set(&powered, 1);
	if (CONFIG_AT_TEST_NETWORK_CASE >= 7) {
		const struct gt02_modem_event event = { .type = GT02_MODEM_EVENT_STARTUP_READY };

		inline_callback(&event, inline_context);
		if (CONFIG_AT_TEST_NETWORK_CASE == 17 || CONFIG_AT_TEST_NETWORK_CASE == 18) {
			k_sem_give(&power_pending);
			zassert_ok(k_sem_take(&release_power, K_SECONDS(2)));
		}
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

/* 核对握手、注册后的一次激活及四项查询；暂停后续轮次，观察一致快照。 */
static int fake_write(const uint8_t *buffer, size_t length, size_t *written)
{
	static const char *const commands[] = { "AT+CPIN?", "AT+CSQ", "AT+CEREG?", "AT+QIACT?" };
	unsigned int index = (unsigned int)atomic_inc(&tx_count);

	*written = length;
	if (length == strlen("AT+QIACT=1\r\n") && memcmp(buffer, "AT+QIACT=1\r\n", length) == 0) {
		zassert_equal(query_step, 3, "PDP activated before successful registration");
		zassert_equal(atomic_inc(&activation_count), 0, "PDP activated twice in one power session");
		atomic_set(&activation_succeeded, 1);
		inject_text("OK\r\n");
		return 0;
	}
	const char *expected = index < 2 ? (index == 0 ? "AT" : "ATE0") : commands[query_step];

	zassert_equal(length, strlen(expected) + 2);
	zassert_mem_equal(buffer, expected, length - 2);
	zassert_mem_equal(buffer + length - 2, "\r\n", 2);
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
	} else if (query_step == 3) {
		zassert_equal(atomic_get(&activation_succeeded), 1, "IP queried before activation succeeded");
		atomic_inc(&ip_query_count);
		if (CONFIG_AT_TEST_NETWORK_CASE == 1 && round_number == 1) {
			inject_text("OK\r\n");
		} else if (CONFIG_AT_TEST_NETWORK_CASE == 2 && round_number == 1) {
			inject_text("+QIACT: 1,1,1,\"0.0.0.0\"\r\nOK\r\n");
		} else if (CONFIG_AT_TEST_NETWORK_CASE == 3 && round_number == 1) {
			inject_text("+QIACT: 1,1,1,\"10.22.0.1\"\r\nERROR\r\n");
		} else {
			inject_text("+QIACT: 1,1,1,\"10.22.0.1\"\r\nOK\r\n");
		}
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

/* 通常同步交付准备事件；慢准备场景只登记已接受，稍后由测试发送最终结果。 */
static int inline_prepare(void)
{
	int attempt = atomic_inc(&prepare_count);
	int mode = CONFIG_AT_TEST_NETWORK_CASE;

	if ((mode == 12 || mode == 13) && attempt == 0) {
		prepare_retry_at_ms = k_uptime_get() + 30;
		if (mode == 12) {
			return -EAGAIN; /* 本次未接受，不应等待完成事件。 */
		}
		const struct gt02_modem_event timeout = {
			.type = GT02_MODEM_EVENT_PREPARE_DONE, .error = -ETIMEDOUT,
		};

		inline_callback(&timeout, inline_context);
		return 0;
	}
	if (mode == 12 || mode == 13) {
		zassert_true(k_uptime_get() >= prepare_retry_at_ms, "prepare retried too early");
	}
	if (mode == 14) {
		const struct gt02_modem_event ready = { .type = GT02_MODEM_EVENT_STARTUP_READY };

		/* 同步后端在服务取事件前填满队列，不能静默丢失完成通知。 */
		for (int i = 0; i < 6; ++i) {
			inline_callback(&ready, inline_context);
		}
	}
	if (CONFIG_AT_TEST_NETWORK_CASE == 10) {
		k_sem_give(&operation_pending);
		return 0;
	}
	const struct gt02_modem_event event = {
		.type = CONFIG_AT_TEST_NETWORK_CASE == 8 ? GT02_MODEM_EVENT_QUERY_DONE :
			GT02_MODEM_EVENT_PREPARE_DONE,
		.error = mode == 15 ? -EIO : 0,
	};

	inline_callback(&event, inline_context);
	return 0;
}

/* 模拟激活接受、拒绝、失败或延迟完成；成功前不得查询IP，后续轮次不应重复成功激活。 */
static int inline_activate(void)
{
	int attempt = atomic_inc(&activation_count);
	int mode = CONFIG_AT_TEST_NETWORK_CASE;
	struct gt02_modem_event event = { .type = GT02_MODEM_EVENT_ACTIVATE_DONE };

	zassert_equal(atomic_get(&activation_succeeded), 0, "PDP activated again after success");
	if (mode == 19 || mode == 25) {
		k_sem_give(&operation_pending);
		return 0;
	}
	if (mode == 21 && attempt == 0) {
		return -EAGAIN;
	}
	if ((mode == 20 || mode == 24) && attempt == 0) {
		event.error = mode == 20 ? -EIO : -ETIMEDOUT;
	} else if (mode == 23) {
		event.type = GT02_MODEM_EVENT_QUERY_DONE;
		event.query = GT02_MODEM_QUERY_DATA_CONTEXT;
	} else {
		atomic_set(&activation_succeeded, 1);
	}
	inline_callback(&event, inline_context);
	return 0;
}

/* 慢查询场景保留一项在途操作；由测试稍后完成或发故障通知，验证服务没有重复计时。 */
static int inline_query(enum gt02_modem_query query)
{
	struct gt02_modem_event event = {
		.type = GT02_MODEM_EVENT_QUERY_DONE, .query = query,
		.sim = GT02_MODEM_SIM_READY, .signal_rssi = 99, .registration = 1,
		.data_context = { .active = true, .ipv4 = { 10, 22, 0, 1 } },
	};

	atomic_inc(&query_count);
	if (query == GT02_MODEM_QUERY_DATA_CONTEXT) {
		zassert_equal(atomic_get(&activation_succeeded), 1, "IP queried before activation completed");
		atomic_inc(&ip_query_count);
	}
	if (query == GT02_MODEM_QUERY_SIM) {
		++round_number;
		if (CONFIG_AT_TEST_NETWORK_CASE == 16) {
			if (round_number == 1) {
				event.sim = GT02_MODEM_SIM_NOT_READY;
			} else if (round_number == 2) {
				zassert_equal(atomic_get(&query_count), 2,
					      "queries continued after SIM was not ready");
			}
		}
	}
	if (query == GT02_MODEM_QUERY_LTE_REGISTRATION && CONFIG_AT_TEST_NETWORK_CASE == 22) {
		if (round_number == 1) {
			event.registration = 2;
		} else if (round_number == 2) {
			zassert_equal(atomic_get(&activation_count), 0, "PDP activated before registration");
		}
	}
	if (query == GT02_MODEM_QUERY_SIGNAL &&
	    ((CONFIG_AT_TEST_NETWORK_CASE == 7 && round_number == 2) ||
	     CONFIG_AT_TEST_NETWORK_CASE == 11)) {
		k_sem_give(&operation_pending);
		return 0;
	}
	if (CONFIG_AT_TEST_NETWORK_CASE == 9 && query == GT02_MODEM_QUERY_SIM) {
		event.query = GT02_MODEM_QUERY_SIGNAL; /* 查询结果不属于当前在途项。 */
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

/* 单次开机验证注册、激活与IP确认；异常场景不隐式重新上电、配置APN或建立TCP。 */
ZTEST(gt02_network, test_periodic_readiness)
{
	static const struct gt02_at_io_ops io = { .read = fake_read, .write = fake_write };
	static const struct gt02_at_config at = { .profile = &gt02_lierda_at_profile, .io = &io };
	static const struct gt02_modem_power_ops power = { .power_on = fake_power_on };
	static const struct gt02_modem_ops immediate = {
		.init = inline_init, .prepare = inline_prepare, .query = inline_query,
		.activate_data_context = inline_activate,
	};
	struct gt02_network_config config = {
		.power = &power, .modem = CONFIG_AT_TEST_NETWORK_CASE >= 7 ? &immediate : &gt02_lierda_ops,
		.startup_timeout_ms = 200,
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
	if (mode < 7) {
		zassert_ok(gt02_at_init(&at));
	}
	zassert_ok(gt02_network_service_start(&config));
	zassert_equal(gt02_network_service_start(&config), -EALREADY);
	config.modem = NULL; /* 服务必须已复制配置，不能继续借用调用栈。 */

	if (mode == 19 || mode == 25) {
		zassert_ok(k_sem_take(&operation_pending, K_SECONDS(1)));
		k_sleep(K_MSEC(80));
		zassert_ok(gt02_network_service_get_status(&snapshot));
		zassert_not_equal(snapshot.state, GT02_NETWORK_DATA_READY);
		zassert_equal(atomic_get(&ip_query_count), 0);
		if (mode == 25) {
			gt02_network_service_notify_at_fault(-EIO);
			wait_status(GT02_NETWORK_FAULT, -EIO, 0);
		}
		const struct gt02_modem_event done = { .type = GT02_MODEM_EVENT_ACTIVATE_DONE };

		atomic_set(&activation_succeeded, 1);
		inline_callback(&done, inline_context);
		if (mode == 19) {
			wait_status(GT02_NETWORK_DATA_READY, 0, 0);
			zassert_equal(atomic_get(&activation_count), 1);
		} else {
			k_sleep(K_MSEC(80));
			zassert_ok(gt02_network_service_get_status(&snapshot));
			zassert_equal(snapshot.state, GT02_NETWORK_FAULT);
			zassert_equal(atomic_get(&ip_query_count), 0);
		}
	} else if (mode == 20 || mode == 21 || mode == 24) {
		int error = mode == 20 ? -EIO : (mode == 21 ? -EAGAIN : -ETIMEDOUT);

		snapshot = wait_status(GT02_NETWORK_NOT_READY, error, 0);
		zassert_equal(snapshot.query, GT02_MODEM_QUERY_DATA_CONTEXT);
		zassert_false(snapshot.data_context.active);
		zassert_equal(atomic_get(&ip_query_count), 0);
		wait_status(GT02_NETWORK_DATA_READY, 0, snapshot.checked_at_ms + 1);
		zassert_equal(atomic_get(&activation_count), 2);
	} else if (mode == 22) {
		snapshot = wait_status(GT02_NETWORK_NOT_READY, 0, 0);
		zassert_equal(snapshot.query, GT02_MODEM_QUERY_LTE_REGISTRATION);
		zassert_equal(atomic_get(&activation_count), 0);
		wait_status(GT02_NETWORK_DATA_READY, 0, snapshot.checked_at_ms + 1);
		zassert_equal(atomic_get(&activation_count), 1);
	} else if (mode == 23) {
		wait_status(GT02_NETWORK_FAULT, -EBADMSG, 0);
		zassert_equal(atomic_get(&ip_query_count), 0);
	} else if (mode == 17 || mode == 18) {
		/* 电源操作尚未返回，业务线程也必须能发布故障；迟到完成不得开始AT准备。 */
		zassert_ok(k_sem_take(&power_pending, K_SECONDS(1)));
		if (mode == 17) {
			gt02_network_service_notify_at_fault(-EIO);
		} else {
			const struct gt02_modem_event ready = { .type = GT02_MODEM_EVENT_STARTUP_READY };

			for (int i = 0; i < 6; ++i) {
				inline_callback(&ready, inline_context);
			}
		}
		wait_status(GT02_NETWORK_FAULT, mode == 17 ? -EIO : -ENOBUFS, 0);
		zassert_equal(atomic_get(&prepare_count), 0);
		k_sem_give(&release_power);
		k_sleep(K_MSEC(100));
		zassert_ok(gt02_network_service_get_status(&snapshot));
		zassert_equal(snapshot.state, GT02_NETWORK_FAULT);
		zassert_equal(atomic_get(&prepare_count), 0);
		zassert_equal(atomic_get(&query_count), 0);
	} else if (mode == 4) {
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
		zassert_ok(k_sem_take(&operation_pending, K_SECONDS(1)));
		wait_status(GT02_NETWORK_NOT_READY, -ESTALE, snapshot.checked_at_ms);
		int before = atomic_get(&query_count);

		/* 超过原测试500ms服务保护；不能故障或启动第二次查询，旧快照仍应过期。 */
		k_sleep(K_MSEC(600));
		struct gt02_network_status waiting;

		zassert_ok(gt02_network_service_get_status(&waiting));
		zassert_equal(waiting.state, GT02_NETWORK_NOT_READY);
		zassert_equal(waiting.error, -ESTALE);
		zassert_equal(atomic_get(&query_count), before);
		const struct gt02_modem_event late = {
			.type = GT02_MODEM_EVENT_QUERY_DONE, .query = GT02_MODEM_QUERY_SIGNAL,
			.signal_rssi = 18,
		};

		inline_callback(&late, inline_context);
		snapshot = wait_status(GT02_NETWORK_DATA_READY, 0, snapshot.checked_at_ms + 1);
		zassert_equal(snapshot.signal_rssi, 18);
		zassert_equal(atomic_get(&tx_count), 0);
	} else if (mode == 10) {
		zassert_ok(k_sem_take(&operation_pending, K_SECONDS(1)));
		k_sleep(K_MSEC(600)); /* RDY已到达，旧的200ms启动期限不能再作用于准备阶段。 */
		zassert_ok(gt02_network_service_get_status(&snapshot));
		zassert_equal(snapshot.state, GT02_NETWORK_STARTING);
		zassert_equal(atomic_get(&prepare_count), 1);
		zassert_equal(atomic_get(&query_count), 0);
		const struct gt02_modem_event done = { .type = GT02_MODEM_EVENT_PREPARE_DONE };

		inline_callback(&done, inline_context);
		wait_status(GT02_NETWORK_DATA_READY, 0, 0);
	} else if (mode == 11) {
		zassert_ok(k_sem_take(&operation_pending, K_SECONDS(1)));
		/* 即使服务只等操作事件，独立AT故障也必须及时唤醒它。 */
		gt02_network_service_notify_at_fault(-EIO);
		wait_status(GT02_NETWORK_FAULT, -EIO, 0);
		const struct gt02_modem_event late = {
			.type = GT02_MODEM_EVENT_QUERY_DONE, .query = GT02_MODEM_QUERY_SIGNAL,
		};

		inline_callback(&late, inline_context);
		k_sleep(K_MSEC(60));
		zassert_ok(gt02_network_service_get_status(&snapshot));
		zassert_equal(snapshot.state, GT02_NETWORK_FAULT);
		zassert_equal(snapshot.error, -EIO);
	} else if (mode == 12 || mode == 13) {
		wait_status(GT02_NETWORK_DATA_READY, 0, 0);
		zassert_equal(atomic_get(&prepare_count), 2);
	} else if (mode == 14 || mode == 15) {
		wait_status(GT02_NETWORK_FAULT, mode == 14 ? -ENOBUFS : -EIO, 0);
		k_sleep(K_MSEC(100));
		zassert_equal(atomic_get(&prepare_count), 1);
		zassert_equal(atomic_get(&query_count), 0);
	} else if (mode == 16) {
		snapshot = wait_status(GT02_NETWORK_NOT_READY, 0, 0);
		zassert_equal(snapshot.query, GT02_MODEM_QUERY_SIM);
		zassert_equal(snapshot.sim, GT02_MODEM_SIM_NOT_READY);
		wait_status(GT02_NETWORK_DATA_READY, 0, snapshot.checked_at_ms + 1);
	} else if (mode == 8 || mode == 9) {
		/* 错阶段/错查询必须故障；随后正确的迟到完成也不能重新启动流程。 */
		wait_status(GT02_NETWORK_FAULT, -EBADMSG, 0);
		const struct gt02_modem_event late = {
			.type = mode == 8 ? GT02_MODEM_EVENT_PREPARE_DONE : GT02_MODEM_EVENT_QUERY_DONE,
			.query = GT02_MODEM_QUERY_SIM, .sim = GT02_MODEM_SIM_READY,
		};

		inline_callback(&late, inline_context);
		k_sleep(K_MSEC(60));
		zassert_ok(gt02_network_service_get_status(&snapshot));
		zassert_equal(snapshot.state, GT02_NETWORK_FAULT);
		zassert_equal(snapshot.error, -EBADMSG);
		zassert_equal(atomic_get(&tx_count), 0);
	} else if (mode == 5) {
		snapshot = wait_status(GT02_NETWORK_DATA_READY, 0, 0);
		zassert_equal(snapshot.signal_rssi, 99);
		zassert_true(snapshot.data_context.active);
		zassert_equal(atomic_get(&tx_count), 7);
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
