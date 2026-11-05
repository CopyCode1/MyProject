/** @file
 * @brief 真实AT/SDK/厂家/服务故障注入，模拟通道边界、连续URC和一次重启策略。
 */
#include <errno.h>
#include <string.h>
#include <zephyr/irq_offload.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/ztest.h>
#include "adapters/modem/gt02_modem_lierda.h"
#include "services/network/gt02_network_service.h"

K_MSGQ_DEFINE(rx, sizeof(uint8_t), 2048, 1);
K_MSGQ_DEFINE(results, sizeof(enum gt02_at_result), 16, 4);
K_SEM_DEFINE(fault_seen, 0, 1);
static atomic_t boundary;
static atomic_t flood;
static atomic_t fault_count;
static atomic_t urcs;
static atomic_t opened;
static atomic_t tx_count;
static atomic_t off_count;
static atomic_t boot_count;
static atomic_t powered;
static atomic_t rearm_in_callback;
static atomic_t fail_next_sim;
static bool core;

/* 中断只记录返回码，验证接口拒绝阻塞控制操作。 */
static void check_isr_rearm(const void *context)
{
	atomic_set((atomic_t *)context, gt02_at_session_rearm());
}

/* 逐字节注入保留分片语义，不借用调用者缓冲。 */
static void inject(const char *text)
{
	for (size_t i = 0; i < strlen(text); ++i) {
		zassert_ok(k_msgq_put(&rx, &text[i], K_NO_WAIT));
	}
}

/* 未交付的通道边界先于新字节；连续URC模式每轮有数据，不能依赖RX空闲发命令。 */
static int fake_read(uint8_t *buffer, size_t capacity, size_t *received, k_timeout_t timeout)
{
	*received = 0;
	if (atomic_cas(&boundary, 1, 0) || (!core && !atomic_get(&powered))) {
		return -EPIPE;
	}
	if (atomic_get(&flood) && k_msgq_num_used_get(&rx) == 0) {
		k_sleep(K_MSEC(1)); /* 模拟字节在线路上传输，给提交者运行机会。 */
		zassert_true(capacity >= 5);
		memcpy(buffer, "RDY\r\n", 5);
		*received = 5;
		return 0;
	}
	if (k_msgq_get(&rx, buffer, timeout) != 0) {
		return -EAGAIN;
	}
	*received = 1;
	while (*received < capacity && k_msgq_get(&rx, &buffer[*received], K_NO_WAIT) == 0) {
		++*received;
	}
	return 0;
}

/* 第一轮SIM故意不回应；恢复分支分别注入准备失败、再次超时或正常四项查询。 */
static int fake_write(const uint8_t *buffer, size_t length, size_t *written)
{
	char command[132];

	zassert_true(length >= 2 && length < sizeof(command));
	zassert_mem_equal(buffer + length - 2, "\r\n", 2);
	memcpy(command, buffer, length - 2);
	command[length - 2] = '\0';
	*written = length;
	atomic_inc(&tx_count);
	if (core) {
		if (strcmp(command, "AT+HOLD") != 0) {
			inject(strcmp(command, "AT+OPEN") == 0 ? "OK\r\n+CIPOPEN: 0,0\r\n" : "OK\r\n");
		}
		return 0;
	}
	int boot = atomic_get(&boot_count);
	int mode = CONFIG_AT_TEST_RECOVERY_CASE;

	if (strcmp(command, "AT") == 0 && boot == 2 && mode == 6) {
		return 0;
	}
	if (strcmp(command, "AT") == 0 || strcmp(command, "ATE0") == 0) {
		inject(boot == 2 && mode == 4 ? "ERROR\r\n" : "OK\r\n");
	} else if (strcmp(command, "AT+CPIN?") == 0) {
		if (boot != 1 && !atomic_cas(&fail_next_sim, 1, 0)) {
			inject("+CPIN: READY\r\nOK\r\n");
		}
	} else if (strcmp(command, "AT+CSQ") == 0) {
		inject("+CSQ: 18,0\r\nOK\r\n");
	} else if (strcmp(command, "AT+CEREG?") == 0) {
		inject("+CEREG: 0,1\r\nOK\r\n");
	} else {
		zassert_equal(strcmp(command, "AT+QIACT?"), 0);
		inject("+QIACT: 1,1,1,\"10.1.2.3\"\r\nOK\r\n");
	}
	return 0;
}

/* 回调只记录一次结果，不保留共享正文。 */
static void on_result(enum gt02_at_result result, const char *text, size_t length, void *context)
{
	ARG_UNUSED(context);
	if (result != GT02_AT_SUCCESS) {
		zassert_is_null(text);
		zassert_equal(length, 0);
	}
	zassert_ok(k_msgq_put(&results, &result, K_NO_WAIT));
}

/* URC独立于事务终止；验证故障期间仍能分发。 */
static void on_urc(const char *text, size_t length, void *context)
{
	ARG_UNUSED(text);
	ARG_UNUSED(length);
	atomic_inc(context == NULL ? &urcs : &opened);
}

/* 故障回调检查禁止等待自身；服务配置则只转交故障事件。 */
static void on_fault(int error, void *context)
{
	ARG_UNUSED(context);
	zassert_true(error < 0);
	atomic_inc(&fault_count);
	atomic_set(&rearm_in_callback, gt02_at_session_rearm());
	if (!core) {
		gt02_network_service_notify_at_fault(error);
		if (CONFIG_AT_TEST_RECOVERY_CASE == 8) {
			/* AT优先级高于服务：填满唤醒队列仍不能丢失首个故障原因。 */
			for (int i = 0; i < 8; ++i) {
				gt02_network_service_notify_at_fault(error);
			}
		}
	}
	k_sem_give(&fault_seen);
}

/* 等待测试可观察结果，验证接受的请求没有遗漏完成。 */
static void expect(enum gt02_at_result expected)
{
	enum gt02_at_result actual;

	zassert_ok(k_msgq_get(&results, &actual, K_SECONDS(2)));
	zassert_equal(actual, expected);
}

/* 假切电保留边界，即使消费者未运行到断电窗口，也会在下次读取前报告。 */
static int power_off(void)
{
	atomic_inc(&off_count);
	if (CONFIG_AT_TEST_RECOVERY_CASE == 2) {
		return -EIO;
	}
	atomic_clear(&powered);
	k_msgq_purge(&rx);
	atomic_set(&boundary, 1);
	return 0;
}

/* RDY故意在接口返回前交付并重复一次；新会话不得漏收或重复准备。 */
static int power_on(void)
{
	int boot = atomic_inc(&boot_count) + 1;

	if (boot == 2 && CONFIG_AT_TEST_RECOVERY_CASE == 5) {
		return -EIO;
	}
	atomic_set(&powered, 1);
	if (boot != 2 || CONFIG_AT_TEST_RECOVERY_CASE != 3) {
		inject("RDY\r\nRDY\r\n");
	}
	int before = atomic_get(&tx_count);

	k_sleep(K_MSEC(40));
	zassert_equal(atomic_get(&tx_count), before, "TX before power_on returned");
	return 0;
}

/* 只替换恢复操作的失败结果，不修改厂家或AT核心状态。 */
static int rearm_session(void)
{
	return CONFIG_AT_TEST_RECOVERY_CASE == 7 ? -EIO : gt02_at_session_rearm();
}

/* 同一开机顺序验证迟到结果、永久无响应、重建会话与持续URC调度。 */
static void check_core(void)
{
	zassert_equal(gt02_at_session_rearm(), -EBUSY);
	zassert_ok(gt02_at_submit(1));
	expect(GT02_AT_SUCCESS);
	zassert_ok(gt02_at_submit(3));
	expect(GT02_AT_SUCCESS);
	zassert_equal(atomic_get(&opened), 1);
	zassert_ok(gt02_at_submit(2));
	expect(GT02_AT_TIMEOUT);
	zassert_equal(gt02_at_session_rearm(), -EBUSY);
	inject("OK\r\n");
	k_sleep(K_MSEC(40));
	zassert_ok(gt02_at_submit(1));
	expect(GT02_AT_SUCCESS);

	zassert_ok(gt02_at_submit(2));
	k_sleep(K_MSEC(40));
	for (int i = 0; i < 4; ++i) {
		zassert_ok(gt02_at_submit(1));
	}
	expect(GT02_AT_TIMEOUT);
	for (int i = 0; i < 4; ++i) {
		expect(GT02_AT_FAILED);
	}
	zassert_equal(gt02_at_submit(1), -EAGAIN);
	zassert_ok(k_sem_take(&fault_seen, K_SECONDS(1)));
	zassert_equal(atomic_get(&fault_count), 1);
	zassert_equal(atomic_get(&rearm_in_callback), -EDEADLK);
	zassert_equal(gt02_at_submit(1), -EPIPE);
	zassert_equal(gt02_at_session_rearm(), -EAGAIN);
	inject("OK\r\nOLD_PARTIAL");
	k_sleep(K_MSEC(40));
	zassert_equal(gt02_at_session_rearm(), -EAGAIN);
	int previous = atomic_get(&urcs);

	/* 没有尾部换行的旧数据不能拼接RDY；边界先交付，新半行仍阻止rearm。 */
	atomic_set(&boundary, 1);
	k_sleep(K_MSEC(40));
	zassert_equal(gt02_at_session_rearm(), -EAGAIN, "disconnect alone must not rearm");
	inject("RDY\r\nNEW_PARTIAL");
	k_sleep(K_MSEC(60));
	zassert_equal(atomic_get(&urcs), previous + 1);
	zassert_equal(gt02_at_session_rearm(), -EAGAIN);
	inject("\r\n");
	k_sleep(K_MSEC(40));
	zassert_ok(gt02_at_session_rearm());
	zassert_ok(gt02_at_submit(1));
	expect(GT02_AT_SUCCESS);
	previous = atomic_get(&urcs);
	atomic_set(&flood, 1);
	while (atomic_get(&urcs) < previous + 5) {
		k_sleep(K_MSEC(1));
	}
	zassert_ok(gt02_at_submit(1));
	expect(GT02_AT_SUCCESS);
	atomic_clear(&flood);
	k_sleep(K_MSEC(40));
	zassert_equal(atomic_get(&fault_count), 1);
	enum gt02_at_result unused;

	zassert_equal(k_msgq_get(&results, &unused, K_NO_WAIT), -ENOMSG);
}

/* 通过公开状态验收恢复结果；失败分支等待足够久确认不会反复重启。 */
static void check_service(void)
{
	static const struct gt02_modem_power_ops power = { .power_on = power_on, .power_off = power_off };
	const struct gt02_network_config config = {
		.power = &power, .modem = &gt02_lierda_ops, .session_rearm = rearm_session,
		.startup_timeout_ms = 250, .operation_timeout_ms = 4000,
		.retry_interval_ms = 100, .check_interval_ms = 500, .result_valid_ms = 1000,
	};
	struct gt02_network_status status;
	int mode = CONFIG_AT_TEST_RECOVERY_CASE;
	bool second_fault_injected = false;
	int64_t end = k_uptime_get() + 7000;

	zassert_ok(gt02_network_service_start(&config));
	do {
		zassert_ok(gt02_network_service_get_status(&status));
		if ((mode == 1 || mode == 8) && status.state == GT02_NETWORK_DATA_READY) {
			if (mode == 1 && !second_fault_injected) {
				/* 完整检查成功后才注入后续独立故障，验证恢复机会确实重置。 */
				atomic_set(&fail_next_sim, 1);
				second_fault_injected = true;
			} else if (mode == 8 || atomic_get(&boot_count) == 3) {
				break;
			}
		}
		k_sleep(K_MSEC(10));
	} while (k_uptime_get() < end);
	if (mode == 1 || mode == 8) {
		zassert_equal(status.state, GT02_NETWORK_DATA_READY);
		zassert_equal(atomic_get(&boot_count), mode == 1 ? 3 : 2);
	} else {
		zassert_equal(status.state, GT02_NETWORK_FAULT);
		zassert_equal(atomic_get(&boot_count), mode == 2 ? 1 : 2);
	}
	zassert_equal(atomic_get(&off_count), mode == 1 ? 2 : 1);
	zassert_equal(atomic_get(&fault_count), mode == 1 || mode == 6 ? 2 : 1);
}

/* 每种场景独立启动，使用真实解析器及核心，静态配置覆盖线程生命周期。 */
ZTEST(gt02_recovery, test_isolation_and_recovery)
{
	static const struct gt02_at_io_ops io = { .read = fake_read, .write = fake_write };
	static const struct gt02_at_command commands[] = {
		{ .id = 1, .command = "AT", .callback = on_result, .timeout_ms = 80 },
		{ .id = 2, .command = "AT+HOLD", .callback = on_result, .timeout_ms = 100 },
		{ .id = 3, .command = "AT+OPEN", .callback = on_result, .timeout_ms = 80 },
	};
	static const struct gt02_at_urc urc[] = {
		{ .match = "RDY", .exact = true, .callback = on_urc },
		{ .match = "+CIPOPEN:", .callback = on_urc, .user_data = &opened },
	};
	static const struct gt02_at_profile profile = {
		.commands = commands, .command_count = ARRAY_SIZE(commands),
		.urcs = urc, .urc_count = ARRAY_SIZE(urc),
	};

	core = CONFIG_AT_TEST_RECOVERY_CASE == 0;
	const struct gt02_at_config config = {
		.profile = core ? &profile : &gt02_lierda_at_profile, .io = &io,
		.drain_timeout_ms = 180, .on_fault = on_fault,
	};

	zassert_equal(gt02_at_session_rearm(), -EACCES);
	atomic_t isr_result = ATOMIC_INIT(0);

	irq_offload(check_isr_rearm, &isr_result);
	zassert_equal(atomic_get(&isr_result), -EWOULDBLOCK);
	zassert_ok(gt02_at_init(&config));
	if (core) {
		check_core();
	} else {
		check_service();
	}
}

ZTEST_SUITE(gt02_recovery, NULL, NULL, NULL, NULL, NULL);
