/** @file
 * @brief 验证真实Lierda准备、SIM及数据上下文事件，真实AT核心只注入假I/O。
 */
#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/ztest.h>

#include "interfaces/gt02_modem.h"
#include "adapters/modem/gt02_modem_lierda.h"

/* 脚本按值入队，字符串均来自长期测试常量；NULL回复表示模拟超时。 */
struct uart_script {
	const char *command;
	const char *reply;
	bool hold;
	size_t reply_length; /* 0表示strlen；非0可注入含NUL的原始正文。 */
};
K_MSGQ_DEFINE(scripts, sizeof(struct uart_script), 8, 4);
K_MSGQ_DEFINE(rx_bytes, sizeof(uint8_t), 1024, 1);
K_MSGQ_DEFINE(events, sizeof(struct gt02_modem_event), 8, 4);
K_SEM_DEFINE(tx_started, 0, 1);
K_SEM_DEFINE(release_tx, 0, 1);
static int receiver_context;
static atomic_t follow_up;
static atomic_t follow_up_result;
static const struct gt02_modem_ops *const modem = &gt02_lierda_ops;

/* 只投递模拟原始字节，不直接调用厂家解析，保留SDK拆包和AT最终回调路径。 */
static void inject_text(const char *text)
{
	for (size_t i = 0; i < strlen(text); ++i) {
		zassert_ok(k_msgq_put(&rx_bytes, &text[i], K_NO_WAIT));
	}
}

/* 假UART每次读取一个字节，按真实接口保留等待超时和实际长度语义。 */
static int fake_read(uint8_t *buffer, size_t capacity, size_t *received, k_timeout_t timeout)
{
	zassert_true(capacity > 0);
	*received = 0;
	if (k_msgq_get(&rx_bytes, buffer, timeout) != 0) {
		return -EAGAIN;
	}
	*received = 1;
	return 0;
}

/* 严格核对脚本中的命令；暂停发送用来制造确定的在途状态，验证并发拒绝。 */
static int fake_write(const uint8_t *buffer, size_t length, size_t *written)
{
	struct uart_script script;

	zassert_ok(k_msgq_get(&scripts, &script, K_NO_WAIT), "unexpected command");
	zassert_equal(length, strlen(script.command) + 2);
	zassert_mem_equal(buffer, script.command, length - 2);
	zassert_mem_equal(buffer + length - 2, "\r\n", 2);
	*written = length;
	if (script.hold) {
		k_sem_give(&tx_started);
		zassert_ok(k_sem_take(&release_tx, K_SECONDS(2)));
	}
	if (script.reply != NULL) {
		size_t reply_length = script.reply_length != 0 ? script.reply_length : strlen(script.reply);

		for (size_t i = 0; i < reply_length; ++i) {
			zassert_ok(k_msgq_put(&rx_bytes, &script.reply[i], K_NO_WAIT));
		}
	}
	return 0;
}

/* 接收者立即复制借用事件；可选再入队一次查询，验证厂家没有持锁通知或残留busy。 */
static void on_event(const struct gt02_modem_event *event, void *user_data)
{
	zassert_false(k_is_in_isr());
	zassert_equal_ptr(user_data, &receiver_context);
	zassert_not_null(event);
	if (event->type == GT02_MODEM_EVENT_QUERY_DONE && atomic_cas(&follow_up, 1, 0)) {
		int ret = modem->query(GT02_MODEM_QUERY_SIM);

		atomic_set(&follow_up_result, ret);
		zassert_ok(ret, "callback re-entry rejected");
	}
	/* 所有测试回调动作完成后才唤醒检查线程，避免下一用例提前改变follow_up。 */
	zassert_ok(k_msgq_put(&events, event, K_NO_WAIT));
}

/* 每次接受操作前准备严格的TX/RX脚本；被拒绝的操作不得消费脚本。 */
static void add_script(const char *command, const char *reply, bool hold)
{
	const struct uart_script script = { .command = command, .reply = reply, .hold = hold };

	zassert_ok(k_msgq_put(&scripts, &script, K_NO_WAIT));
}

/* 按值读取最终事件，错误时必须为UNKNOWN，不允许交付失败事务中曾出现的READY。 */
static void expect_event(enum gt02_modem_event_type type, int error, enum gt02_modem_sim_state sim)
{
	struct gt02_modem_event event;

	zassert_ok(k_msgq_get(&events, &event, K_SECONDS(3)));
	zassert_equal(event.type, type);
	zassert_equal(event.error, error);
	zassert_equal(event.sim, sim);
	if (type == GT02_MODEM_EVENT_QUERY_DONE) {
		zassert_equal(event.query, GT02_MODEM_QUERY_SIM);
	}
}

/* 数据上下文只在成功时有效；空上下文及失败必须保持全零，不能泄漏上次IP。 */
static void expect_context(int error, bool active, const uint8_t ipv4[4])
{
	struct gt02_modem_event event;

	zassert_ok(k_msgq_get(&events, &event, K_SECONDS(12)));
	zassert_equal(event.type, GT02_MODEM_EVENT_QUERY_DONE);
	zassert_equal(event.query, GT02_MODEM_QUERY_DATA_CONTEXT);
	zassert_equal(event.error, error);
	zassert_equal(event.sim, GT02_MODEM_SIM_UNKNOWN);
	zassert_equal(event.signal_rssi, 0);
	zassert_equal(event.registration, 0);
	zassert_equal(event.data_context.active, active);
	zassert_mem_equal(event.data_context.ipv4, ipv4, 4);
}

/* 单次开机覆盖可继续的协议结果，最后注入超时；不访问任何真实电源或GPIO。 */
ZTEST(gt02_capability, test_protocol_ops)
{
	static const struct gt02_at_io_ops io = { .read = fake_read, .write = fake_write };
	static const struct gt02_at_config config = { .profile = &gt02_lierda_at_profile, .io = &io };
	struct gt02_modem_event unused;

	zassert_equal(modem->prepare(), -EACCES);
	zassert_equal(modem->query(GT02_MODEM_QUERY_SIM), -EACCES);
	zassert_equal(modem->query(GT02_MODEM_QUERY_DATA_CONTEXT), -EACCES);
	zassert_equal(modem->init(NULL, NULL), -EINVAL);
	zassert_ok(modem->init(on_event, &receiver_context));
	zassert_equal(modem->init(on_event, NULL), -EALREADY);
	zassert_equal(modem->query((enum gt02_modem_query)99), -ENOTSUP);
	/* AT尚未初始化时立即拒绝，厂家必须撤销busy，且没有完成事件。 */
	zassert_equal(modem->prepare(), -EACCES);
	zassert_equal(modem->query(GT02_MODEM_QUERY_SIM), -EACCES);
	zassert_equal(k_msgq_get(&events, &unused, K_NO_WAIT), -ENOMSG);
	zassert_ok(gt02_at_init(&config));
	/* 每次启动通知独立于操作完成；服务负责按当前阶段过滤重复RDY。 */
	inject_text("RDY\r\nRDY\r\n");
	expect_event(GT02_MODEM_EVENT_STARTUP_READY, 0, GT02_MODEM_SIM_UNKNOWN);
	expect_event(GT02_MODEM_EVENT_STARTUP_READY, 0, GT02_MODEM_SIM_UNKNOWN);

	add_script("AT", "OK\r\n", true);
	add_script("ATE0", "OK\r\n", false);
	zassert_ok(modem->prepare());
	zassert_ok(k_sem_take(&tx_started, K_SECONDS(2)));
	zassert_equal(modem->prepare(), -EBUSY);
	zassert_equal(modem->query(GT02_MODEM_QUERY_SIM), -EBUSY);
	k_sem_give(&release_tx);
	expect_event(GT02_MODEM_EVENT_PREPARE_DONE, 0, GT02_MODEM_SIM_UNKNOWN);
	/* 任一步ERROR都只报告一个失败，不自动重试或继续后续准备命令。 */
	add_script("AT", "ERROR\r\n", false);
	zassert_ok(modem->prepare());
	expect_event(GT02_MODEM_EVENT_PREPARE_DONE, -EIO, GT02_MODEM_SIM_UNKNOWN);
	add_script("AT", "OK\r\n", false);
	add_script("ATE0", "ERROR\r\n", false);
	zassert_ok(modem->prepare());
	expect_event(GT02_MODEM_EVENT_PREPARE_DONE, -EIO, GT02_MODEM_SIM_UNKNOWN);

	const struct {
		const char *reply;
		int error;
		enum gt02_modem_sim_state sim;
	} cases[] = {
		{ "+CPIN: READY\r\nOK\r\n", 0, GT02_MODEM_SIM_READY },
		{ "+CPIN: SIM PIN\r\nOK\r\n", 0, GT02_MODEM_SIM_PIN_REQUIRED },
		{ "+CPIN: SIM PUK \t\r\nOK\r\n", 0, GT02_MODEM_SIM_PUK_REQUIRED },
		{ "+CPIN: NOT READY\r\nOK\r\n", 0, GT02_MODEM_SIM_NOT_READY },
		{ "+CPIN: invalid\r\nOK\r\n", -EBADMSG, GT02_MODEM_SIM_UNKNOWN },
		{ "+CPIN: READY\r\nERROR\r\n", -EIO, GT02_MODEM_SIM_UNKNOWN },
		{ "+CME ERROR: 10\r\n", -EIO, GT02_MODEM_SIM_UNKNOWN },
		{ "OK\r\n", -EIO, GT02_MODEM_SIM_UNKNOWN },
	};

	for (size_t i = 0; i < ARRAY_SIZE(cases); ++i) {
		add_script("AT+CPIN?", cases[i].reply, i == 0);
		zassert_ok(modem->query(GT02_MODEM_QUERY_SIM));
		if (i == 0) {
			zassert_ok(k_sem_take(&tx_started, K_SECONDS(2)));
			zassert_equal(modem->query(GT02_MODEM_QUERY_SIM), -EBUSY);
			zassert_equal(modem->prepare(), -EBUSY);
			k_sem_give(&release_tx);
		}
		expect_event(GT02_MODEM_EVENT_QUERY_DONE, cases[i].error, cases[i].sim);
	}

	add_script("AT+CPIN?", "+CPIN: READY\r\nOK\r\n", false);
	add_script("AT+CPIN?", "+CPIN: SIM PIN\r\nOK\r\n", false);
	atomic_set(&follow_up, 1);
	zassert_ok(modem->query(GT02_MODEM_QUERY_SIM));
	expect_event(GT02_MODEM_EVENT_QUERY_DONE, 0, GT02_MODEM_SIM_READY);
	expect_event(GT02_MODEM_EVENT_QUERY_DONE, 0, GT02_MODEM_SIM_PIN_REQUIRED);
	zassert_equal(atomic_get(&follow_up_result), 0);

	const uint8_t zero_ip[4] = {0};
	const struct {
		const char *reply;
		int error;
		bool active;
		uint8_t ipv4[4];
	} contexts[] = {
		{ "+QIACT: 1,1,1,\"10.20.30.40\"\r\nOK\r\n", 0, true, {10, 20, 30, 40} },
		{ "OK\r\n", 0, false, {0} },
		{ "+QIACT: 2,1,1,\"10.0.0.2\"\r\nOK\r\n", 0, false, {0} },
		{ "+QIACT: 2,1,2,\"2001:db8::2\"\r\n+QIACT: 1,1,1,\"10.0.0.1\"\r\n"
		  "+QIACT: 3,1,1,\"10.0.0.3\"\r\nOK\r\n", 0, true, {10, 0, 0, 1} },
		{ "+QIACT: 1,1,1,\"0.0.0.0\"\r\nOK\r\n", 0, true, {0} },
		{ "+QIACT: 1,0,1,\"\"\r\nOK\r\n", 0, false, {0} },
		{ "+QIACT: 1,0,1,\"10.20.30.40\"\r\nOK\r\n", 0, false, {0} },
		{ "+QIACT: 1,1,2,\"2001:db8::1\"\r\nOK\r\n", -ENOTSUP, false, {0} },
		{ "+QIACT: 1,bad\r\nOK\r\n", -EBADMSG, false, {0} },
		{ "+QIACT: 1,1,1,\"256.1.2.3\"\r\nOK\r\n", -EBADMSG, false, {0} },
		{ "+QIACT: 1,1,1,\"1.2.3\"\r\nOK\r\n", -EBADMSG, false, {0} },
		{ "+QIACT: 1,1,1,\"1.2.3.-4\"\r\nOK\r\n", -EBADMSG, false, {0} },
		{ "+QIACT: 1,1,1,\"1.2.3.4\"junk\r\nOK\r\n", -EBADMSG, false, {0} },
		{ "+QIACT: 1,0,1,missing_quotes\r\nOK\r\n", -EBADMSG, false, {0} },
		{ "+QIACT: 1,1,1,\"10.0.0.1\"\r\n+QIACT: 1,1,1,\"10.0.0.2\"\r\nOK\r\n",
		  -EBADMSG, false, {0} },
		{ "+QIACT: 1,1,1,\"10.20.30.40\"\r\nERROR\r\n", -EIO, false, {0} },
	};

	for (size_t i = 0; i < ARRAY_SIZE(contexts); ++i) {
		add_script("AT+QIACT?", contexts[i].reply, i == 0);
		zassert_ok(modem->query(GT02_MODEM_QUERY_DATA_CONTEXT));
		if (i == 0) {
			zassert_ok(k_sem_take(&tx_started, K_SECONDS(2)));
			zassert_equal(modem->query(GT02_MODEM_QUERY_DATA_CONTEXT), -EBUSY);
			zassert_equal(modem->query(GT02_MODEM_QUERY_SIM), -EBUSY);
			k_sem_give(&release_tx);
		}
		expect_context(contexts[i].error, contexts[i].active, contexts[i].ipv4);
	}

	static const char nul_reply[] = "+QIACT: 1,1,1,\"10.0.\0.1\"\r\nOK\r\n";
	const struct uart_script nul_script = {
		.command = "AT+QIACT?", .reply = nul_reply, .reply_length = sizeof(nul_reply) - 1,
	};

	zassert_ok(k_msgq_put(&scripts, &nul_script, K_NO_WAIT));
	zassert_ok(modem->query(GT02_MODEM_QUERY_DATA_CONTEXT));
	expect_context(-EBADMSG, false, zero_ip);

	add_script("AT+QIACT?", NULL, false);
	zassert_ok(modem->query(GT02_MODEM_QUERY_DATA_CONTEXT));
	expect_context(-ETIMEDOUT, false, zero_ip);
	zassert_equal(modem->query(GT02_MODEM_QUERY_SIM), -EAGAIN);
	zassert_equal(modem->query(GT02_MODEM_QUERY_DATA_CONTEXT), -EAGAIN);
	zassert_equal(modem->prepare(), -EAGAIN);
	k_sleep(K_MSEC(40));
	zassert_equal(k_msgq_get(&events, &unused, K_NO_WAIT), -ENOMSG);
	zassert_equal(k_msgq_num_used_get(&scripts), 0);
	/* 旧失败结束行只解除隔离，不重复通知；厂家busy应已释放，恢复后可再次查询。 */
	inject_text("+CMS ERROR: 500\r\n");
	k_sleep(K_MSEC(60));
	zassert_equal(k_msgq_get(&events, &unused, K_NO_WAIT), -ENOMSG);
	add_script("AT+CPIN?", "+CPIN: READY\r\nOK\r\n", false);
	zassert_ok(modem->query(GT02_MODEM_QUERY_SIM));
	expect_event(GT02_MODEM_EVENT_QUERY_DONE, 0, GT02_MODEM_SIM_READY);
}

ZTEST_SUITE(gt02_capability, NULL, NULL, NULL, NULL, NULL);
