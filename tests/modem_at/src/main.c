/** @file
 * @brief QEMU假串口测试，真实执行SDK文本解析和AT核心，不接触GPIO或实物电源。
 */
#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/ztest.h>

#include "adapters/modem/gt02_modem_at.h"
#include "adapters/modem/gt02_modem_lierda.h"

/* ID同时用于回调核对顺序；HOLD事务留到最后触发不可恢复的流失效。 */
enum command_id {
	AT = 1, CSQ, REJECT, MISSING, MULTI, EXT_ERROR, HOLD,
	PLAIN, PLAIN_ERROR, PLAIN_MISSING, RESERVED,
	PARAM = 100, LGPIO = LIERDA_CMD_LGPIO,
};
struct completion {
	int id;                 /* 命令表注册的ID。 */
	enum gt02_at_result result; /* 最终结果。 */
	size_t size;            /* 交付正文字节数。 */
	bool null_body;         /* 失败必须不交付正文。 */
	char text[513];         /* 仅测试保存回调快照，避免引用核心复用的缓存。 */
};

K_MSGQ_DEFINE(rx_bytes, sizeof(uint8_t), 2048, 1);
K_MSGQ_DEFINE(tx_events, sizeof(int), 32, 4);
K_MSGQ_DEFINE(completions, sizeof(struct completion), 16, 4);
K_MSGQ_DEFINE(parameter_tx, GT02_AT_COMMAND_MAX + 1, 16, 1);
K_SEM_DEFINE(release_tx, 0, 1);
static atomic_t urc_count;
static atomic_t follow_up;
static atomic_t follow_up_result;
static atomic_t rx_error;
static size_t fragment_size = 1;
static int callback_ids[] = {
	AT, CSQ, REJECT, MISSING, MULTI, EXT_ERROR, HOLD, PLAIN, PLAIN_ERROR, PLAIN_MISSING,
	PARAM, LGPIO,
};

/* 往假的UART队列放入原始字节，允许故意构造拆包、NUL和超长输入。 */
static void inject_bytes(const char *text, size_t length)
{
	for (size_t i = 0; i < length; ++i) {
		zassert_ok(k_msgq_put(&rx_bytes, &text[i], K_NO_WAIT), "fake RX queue full");
	}
}

/* C字符串样例的简便入口，不适用于包含NUL的输入。 */
static void inject_text(const char *text)
{
	inject_bytes(text, strlen(text));
}

/* 假硬件保留实际超时和分片读取语义，由测试选择单字节或64字节一批。 */
static int fake_read(uint8_t *buffer, size_t capacity, size_t *received, k_timeout_t timeout)
{
	*received = 0;
	if (atomic_cas(&rx_error, 1, 0)) {
		return -EIO;
	}
	int ret = k_msgq_get(&rx_bytes, buffer, timeout);

	if (ret != 0) {
		return -EAGAIN;
	}
	*received = 1;
	while (*received < MIN(capacity, fragment_size) &&
	       k_msgq_get(&rx_bytes, &buffer[*received], K_NO_WAIT) == 0) {
		++*received;
	}
	return 0;
}

/* 假write核对CRLF并立即回包；HOLD有意等待测试填满待执行队列后再注入故障。 */
static int fake_write(const uint8_t *buffer, size_t length, size_t *written)
{
	char command[131];
	int id;

	zassert_true(length < sizeof(command));
	zassert_mem_equal(&buffer[length - 2], "\r\n", 2);
	memcpy(command, buffer, length - 2);
	command[length - 2] = '\0';
	*written = length;
	if (strcmp(command, "AT") == 0) {
		id = AT;
		inject_text("AT\r\nOKAY\r\nRDY\r\nOK\r\n");
	} else if (strcmp(command, "AT+CSQ") == 0) {
		id = CSQ;
		inject_text("+CSQ: 18,0\r\nRDY\r\nOK\r\n");
	} else if (strcmp(command, "AT+REJECT") == 0) {
		id = REJECT;
		inject_text("+CSQ: 18,0\r\nERROR\r\n");
	} else if (strcmp(command, "AT+MISSING") == 0) {
		id = MISSING;
		inject_text("OK\r\n");
	} else if (strcmp(command, "AT+MULTI") == 0) {
		id = MULTI;
		inject_text("+CSQ: 18,0\r\n+CSQ: 19,1\r\nOK\r\n");
	} else if (strcmp(command, "AT+EXT") == 0) {
		id = EXT_ERROR;
		inject_text("+CME ERROR: 13\r\n");
	} else if (strcmp(command, "ATI") == 0) {
		id = PLAIN;
		inject_text("ATI\r\nLierda\r\nRDY\r\nNT26-KCN\r\nRevision: test\r\nOK\r\n");
	} else if (strcmp(command, "AT+PLAINERR") == 0) {
		id = PLAIN_ERROR;
		inject_text("123456789012345\r\nERROR\r\n");
	} else if (strcmp(command, "AT+PLAINMISSING") == 0) {
		id = PLAIN_MISSING;
		inject_text("RDY\r\nOK\r\n");
	} else if (strncmp(command, "AT+PARAM=", 9) == 0 || strncmp(command, "AT+LGPIO=", 9) == 0) {
		id = command[3] == 'P' ? PARAM : LGPIO;
		/* 固定长度队列保存TX快照，不借用本函数栈；未使用的尾部清零。 */
		memset(command + strlen(command) + 1, 0, sizeof(command) - strlen(command) - 1);
		zassert_ok(k_msgq_put(&parameter_tx, command, K_NO_WAIT));
		zassert_ok(k_msgq_put(&tx_events, &id, K_NO_WAIT));
		if (strcmp(command, "AT+PARAM=hold") == 0) {
			zassert_ok(k_sem_take(&release_tx, K_SECONDS(2)), "parameter hold not released");
		}
		/* 参数化回显必须按本次完整文本过滤，而非表中的命令名。 */
		inject_text(command);
		inject_text("\r\nparameter body\r\nOK\r\n");
		return 0;
	} else {
		id = HOLD;
		zassert_equal(strcmp(command, "AT+HOLD"), 0);
		zassert_ok(k_msgq_put(&tx_events, &id, K_NO_WAIT));
		zassert_ok(k_sem_take(&release_tx, K_SECONDS(2)), "test did not release TX");
		switch (CONFIG_AT_TEST_FAULT_CASE) {
		case 1:
			*written = 1;
			return -EIO;
		case 2: {
			char oversized[257];

			memset(oversized, 'X', sizeof(oversized));
			inject_bytes(oversized, sizeof(oversized));
			inject_text("\r\nOK\r\n");
			break;
		}
		case 3:
			for (int i = 0; i < 50; ++i) {
				inject_text("+CSQ: 18,0\r\n");
			}
			inject_text("OK\r\n");
			break;
		case 4:
			atomic_set(&rx_error, 1);
			break;
		default:
			break;
		}
		return 0;
	}
	zassert_ok(k_msgq_put(&tx_events, &id, K_NO_WAIT));
	return 0;
}

/* 保存最终回调快照；可选从回调再入队，验证核心没有持请求锁调用用户函数。 */
static void on_complete(enum gt02_at_result result, const char *response,
			size_t length, void *user_data)
{
	struct completion done = {
		.id = *(int *)user_data, .result = result, .size = length,
		.null_body = response == NULL,
	};

	zassert_true(length < sizeof(done.text));
	if (response != NULL) {
		memcpy(done.text, response, length);
		zassert_equal(response[length], '\0');
	}
	if (done.id == AT && atomic_cas(&follow_up, 1, 0)) {
		atomic_set(&follow_up_result, gt02_at_submit(CSQ));
	}
	zassert_ok(k_msgq_put(&completions, &done, K_NO_WAIT), "duplicate/unconsumed callback");
}

/* 验证完整行、参数指针和终止符有效期，不阻塞AT线程。 */
static void on_urc(const char *content, size_t length, void *user_data)
{
	zassert_equal(length, 3);
	zassert_mem_equal(content, "RDY", 3);
	zassert_equal(content[length], '\0');
	zassert_equal_ptr(user_data, &urc_count);
	atomic_inc(&urc_count);
}

static const struct gt02_at_command commands[] = {
	{ AT, "AT", NULL, on_complete, &callback_ids[0], 200, false, false, false },
	{ CSQ, "AT+CSQ", "+CSQ:", on_complete, &callback_ids[1], 200, true, false, false },
	{ REJECT, "AT+REJECT", "+CSQ:", on_complete, &callback_ids[2], 200, true, false, false },
	{ MISSING, "AT+MISSING", "+CSQ:", on_complete, &callback_ids[3], 200, true, false, false },
	{ MULTI, "AT+MULTI", "+CSQ:", on_complete, &callback_ids[4], 200, true, false, false },
	{ EXT_ERROR, "AT+EXT", NULL, on_complete, &callback_ids[5], 200, false, false, false },
	{ HOLD, "AT+HOLD", "+CSQ:", on_complete, &callback_ids[6], 200, true, false, false },
	{ PLAIN, "ATI", "", on_complete, &callback_ids[7], 200, true, false, false },
	{ PLAIN_ERROR, "AT+PLAINERR", "", on_complete, &callback_ids[8], 200, true, false, false },
	{ PLAIN_MISSING, "AT+PLAINMISSING", "", on_complete, &callback_ids[9], 200, true, false, false },
	{ .id = RESERVED, .command = "AT+QISEND=%hhu,%hu", .timeout_ms = 10000,
	  .reserved = true },
	{ PARAM, "AT+PARAM", "", on_complete, &callback_ids[10], 200, true, false, true },
	{ LGPIO, "AT+LGPIO", "", on_complete, &callback_ids[11], 200, true, false, true },
};
static const struct gt02_at_urc urcs[] = {
	{ "RDY", true, on_urc, &urc_count },
};
static const struct gt02_at_profile profile = {
	.commands = commands, .command_count = ARRAY_SIZE(commands),
	.urcs = urcs, .urc_count = ARRAY_SIZE(urcs),
};

/* 显式注入静态假I/O，不导出任何同名硬件函数，验证AT核心可独立于Zephyr适配器链接。 */
static const struct gt02_at_io_ops fake_io = {
	.read = fake_read,
	.write = fake_write,
};
static const struct gt02_at_config configuration = {
	.profile = &profile,
	.io = &fake_io,
};

/* 同时检查实际TX和最终回调的顺序，失败时必须清空交付正文。 */
static struct completion expect_result(int id, enum gt02_at_result result)
{
	struct completion done;
	int sent;

	zassert_ok(k_msgq_get(&tx_events, &sent, K_SECONDS(2)));
	zassert_equal(sent, id, "TX FIFO order");
	zassert_ok(k_msgq_get(&completions, &done, K_SECONDS(2)));
	zassert_equal(done.id, id, "callback order");
	zassert_equal(done.result, result);
	if (result != GT02_AT_SUCCESS) {
		zassert_true(done.null_body);
		zassert_equal(done.size, 0);
	}
	return done;
}

/* 验证同一ID的不同参数、局部缓存复用及在途/排队槽位隔离，不驱动实物GPIO。 */
static void check_parameter_requests(void)
{
	char text[GT02_AT_COMMAND_MAX + 2];
	char sent_text[GT02_AT_COMMAND_MAX + 1];
	struct completion done;
	int sent;

	zassert_equal(gt02_at_submit(PARAM), -EINVAL);
	zassert_equal(gt02_at_submit_text(AT, "AT=1"), -EINVAL);
	zassert_equal(gt02_at_submit_text(PARAM, NULL), -EINVAL);
	zassert_equal(gt02_at_submit_text(PARAM, ""), -EINVAL);
	zassert_equal(gt02_at_submit_text(PARAM, "AT+PARAM"), -EINVAL);
	zassert_equal(gt02_at_submit_text(PARAM, "AT+PARAM="), -EINVAL);
	zassert_equal(gt02_at_submit_text(PARAM, "AT+PARAMETER=1"), -EINVAL);
	zassert_equal(gt02_at_submit_text(PARAM, "AT+PARAM=1\r\nAT"), -EINVAL);
	zassert_equal(gt02_at_submit_text(PARAM, "AT+PARAM=\x01"), -EINVAL);
	zassert_equal(gt02_at_submit_text(PARAM, "AT+PARAM=\x80"), -EINVAL);
	zassert_equal(gt02_at_submit_text(RESERVED, "AT+QISEND=1,1"), -ENOTSUP);
	zassert_equal(gt02_lierda_gpio_config(96), -EINVAL);
	zassert_equal(gt02_lierda_gpio_set(100, true), -EINVAL);
	memset(text, 'X', sizeof(text) - 1);
	memcpy(text, "AT+PARAM=", 9);
	text[sizeof(text) - 1] = '\0';
	zassert_equal(gt02_at_submit_text(PARAM, text), -EMSGSIZE);
	text[GT02_AT_COMMAND_MAX] = '\0';
	zassert_ok(gt02_at_submit_text(PARAM, text));
	done = expect_result(PARAM, GT02_AT_SUCCESS);
	zassert_equal(strcmp(done.text, "parameter body"), 0);
	zassert_ok(k_msgq_get(&parameter_tx, sent_text, K_SECONDS(2)));
	zassert_equal(strlen(sent_text), GT02_AT_COMMAND_MAX);
	zassert_equal(strcmp(sent_text, text), 0);

	/* 在途命令停在write里，四个排队请求绕回队列；它们不能覆盖在途文本。 */
	zassert_ok(gt02_at_submit_text(PARAM, "AT+PARAM=hold"));
	zassert_ok(k_msgq_get(&tx_events, &sent, K_SECONDS(2)));
	zassert_equal(sent, PARAM);
	zassert_ok(k_msgq_get(&parameter_tx, sent_text, K_SECONDS(2)));
	zassert_equal(strcmp(sent_text, "AT+PARAM=hold"), 0);
	strcpy(text, "AT+PARAM=first");
	zassert_ok(gt02_at_submit_text(PARAM, text));
	strcpy(text, "AT+PARAM=second");
	zassert_ok(gt02_at_submit_text(PARAM, text));
	memset(text, 'Z', sizeof(text));
	zassert_ok(gt02_lierda_gpio_config(97));
	zassert_ok(gt02_lierda_gpio_set(99, false));
	zassert_equal(gt02_at_submit_text(PARAM, "AT+PARAM=extra"), -ENOBUFS);
	zassert_equal(gt02_at_submit(CSQ), -ENOBUFS);
	k_sem_give(&release_tx);
	zassert_ok(k_msgq_get(&completions, &done, K_SECONDS(2)));
	zassert_equal(done.id, PARAM);
	zassert_equal(done.result, GT02_AT_SUCCESS);
	zassert_equal(strcmp(done.text, "parameter body"), 0);
	const char *expected[] = {
		"AT+PARAM=first", "AT+PARAM=second", "AT+LGPIO=97,1", "AT+LGPIO=99,1,0",
	};

	for (size_t i = 0; i < ARRAY_SIZE(expected); ++i) {
		done = expect_result(i < 2 ? PARAM : LGPIO, GT02_AT_SUCCESS);
		zassert_equal(strcmp(done.text, "parameter body"), 0);
		zassert_ok(k_msgq_get(&parameter_tx, sent_text, K_SECONDS(2)));
		zassert_equal(strcmp(sent_text, expected[i]), 0);
	}
	zassert_ok(gt02_lierda_gpio_set(97, true));
	expect_result(LGPIO, GT02_AT_SUCCESS);
	zassert_ok(k_msgq_get(&parameter_tx, sent_text, K_SECONDS(2)));
	zassert_equal(strcmp(sent_text, "AT+LGPIO=97,1,1"), 0);
}

/* 单次开机先验证所有可继续的路径，再注入一种停止性故障，符合生产核心无reset的约束。 */
ZTEST(gt02_at, test_protocol_and_fault)
{
	zassert_equal(gt02_at_submit(AT), -EACCES);
	zassert_equal(gt02_at_submit_text(PARAM, "AT+PARAM=1"), -EACCES);
	zassert_equal(gt02_at_init(NULL), -EINVAL);
	struct gt02_at_config invalid_config = configuration;
	struct gt02_at_io_ops invalid_io = fake_io;

	invalid_config.io = NULL;
	zassert_equal(gt02_at_init(&invalid_config), -EINVAL);
	invalid_config.io = &invalid_io;
	invalid_io.read = NULL;
	zassert_equal(gt02_at_init(&invalid_config), -EINVAL);
	invalid_io = fake_io;
	invalid_io.write = NULL;
	zassert_equal(gt02_at_init(&invalid_config), -EINVAL);
	invalid_config = configuration;
	invalid_config.profile = NULL;
	zassert_equal(gt02_at_init(&invalid_config), -EINVAL);
	struct gt02_at_profile invalid = profile;
	struct gt02_at_command duplicate[2] = { commands[0], commands[0] };

	invalid.commands = duplicate;
	invalid.command_count = 2;
	invalid_config = configuration;
	invalid_config.profile = &invalid;
	zassert_equal(gt02_at_init(&invalid_config), -EINVAL);
	struct gt02_at_urc conflict = { "+CSQ:", false, on_urc, NULL };

	invalid = profile;
	invalid.urcs = &conflict;
	zassert_equal(gt02_at_init(&invalid_config), -EINVAL);
	struct gt02_at_command bad_command = {
		.id = PARAM, .command = "AT+BAD=1", .callback = on_complete,
		.timeout_ms = 200, .parameterized = true,
	};

	invalid = profile;
	invalid.commands = &bad_command;
	invalid.command_count = 1;
	zassert_equal(gt02_at_init(&invalid_config), -EINVAL);
	zassert_ok(gt02_at_init(&configuration));
	zassert_equal(gt02_at_init(&configuration), -EALREADY);
	zassert_equal(gt02_at_submit(999), -EINVAL);
	/* 预留模板不得入队、发送或回调；启用项仍要求最终回调。 */
	zassert_equal(gt02_at_submit(RESERVED), -ENOTSUP);
	struct gt02_at_config lierda_config = { .profile = &gt02_lierda_at_profile, .io = &fake_io };

	/* init先检查协议表，再检查已初始化状态；此返回值确认真实厂家表通过冲突校验。 */
	zassert_equal(gt02_at_init(&lierda_config), -EALREADY);
	size_t reserved_count = 0;

	zassert_equal(gt02_lierda_at_profile.command_count, 21);
	for (size_t i = 0; i < gt02_lierda_at_profile.command_count; ++i) {
		const struct gt02_at_command *cmd = &gt02_lierda_at_profile.commands[i];

		reserved_count += cmd->reserved;
		zassert_true(cmd->reserved || strchr(cmd->command, '%') == NULL,
			     "format template must never be executable");
	}
	zassert_equal(reserved_count, 9);
	/* 无事务的孤立OK不能抢先完成之后的命令。 */
	inject_text("OK\r\n");
	k_sleep(K_MSEC(40));
	zassert_ok(gt02_at_submit(AT));
	struct completion done = expect_result(AT, GT02_AT_SUCCESS);

	zassert_true(done.null_body);
	zassert_equal(atomic_get(&urc_count), 1);
	zassert_ok(gt02_at_submit(CSQ));
	done = expect_result(CSQ, GT02_AT_SUCCESS);
	zassert_equal(strcmp(done.text, "+CSQ: 18,0"), 0);
	zassert_equal(atomic_get(&urc_count), 2);
	/* 兜底收集保留多行原文，排除回显、已注册URC和最终结果。 */
	zassert_ok(gt02_at_submit(PLAIN));
	done = expect_result(PLAIN, GT02_AT_SUCCESS);
	zassert_equal(strcmp(done.text, "Lierda\nNT26-KCN\nRevision: test"), 0);
	zassert_equal(atomic_get(&urc_count), 3);
	zassert_ok(gt02_at_submit(PLAIN_ERROR));
	expect_result(PLAIN_ERROR, GT02_AT_FAILED);
	zassert_ok(gt02_at_submit(PLAIN_MISSING));
	expect_result(PLAIN_MISSING, GT02_AT_FAILED);
	zassert_equal(atomic_get(&urc_count), 4);
	zassert_ok(gt02_at_submit(REJECT));
	expect_result(REJECT, GT02_AT_FAILED);
	zassert_ok(gt02_at_submit(MISSING));
	expect_result(MISSING, GT02_AT_FAILED);
	zassert_ok(gt02_at_submit(EXT_ERROR));
	expect_result(EXT_ERROR, GT02_AT_FAILED);
	/* 后半段使用同批次多行，覆盖粘包和正文共享缓存。 */
	fragment_size = 64;
	zassert_ok(gt02_at_submit(MULTI));
	done = expect_result(MULTI, GT02_AT_SUCCESS);
	zassert_equal(strcmp(done.text, "+CSQ: 18,0\n+CSQ: 19,1"), 0);
	atomic_set(&follow_up, 1);
	zassert_ok(gt02_at_submit(AT));
	expect_result(AT, GT02_AT_SUCCESS);
	expect_result(CSQ, GT02_AT_SUCCESS);
	zassert_equal(atomic_get(&follow_up_result), 0);
	check_parameter_requests();

	/* HOLD停在假write中，确保先填满4个排队位置，再放行并触发终止性故障。 */
	zassert_ok(gt02_at_submit(HOLD));
	int sent;

	zassert_ok(k_msgq_get(&tx_events, &sent, K_SECONDS(2)));
	zassert_equal(sent, HOLD);
	for (int i = 0; i < 4; ++i) {
		zassert_ok(i % 2 == 0 ? gt02_at_submit_text(PARAM, "AT+PARAM=cancel") : gt02_at_submit(CSQ));
	}
	zassert_equal(gt02_at_submit(CSQ), -ENOBUFS);
	zassert_equal(gt02_at_submit(RESERVED), -ENOTSUP);
	k_sem_give(&release_tx);
	zassert_ok(k_msgq_get(&completions, &done, K_SECONDS(2)));
	zassert_equal(done.id, HOLD);
	zassert_equal(done.result, CONFIG_AT_TEST_FAULT_CASE == 0 ?
		      GT02_AT_TIMEOUT : GT02_AT_FAILED);
	zassert_true(done.null_body);
	for (int i = 0; i < 4; ++i) {
		zassert_ok(k_msgq_get(&completions, &done, K_SECONDS(2)));
		zassert_equal(done.id, i % 2 == 0 ? PARAM : CSQ);
		zassert_equal(done.result, GT02_AT_FAILED);
		zassert_true(done.null_body);
	}
	zassert_equal(gt02_at_submit(AT), -EPIPE);
	zassert_equal(gt02_at_submit_text(PARAM, "AT+PARAM=late"), -EPIPE);
	zassert_equal(gt02_at_init(&configuration), -EALREADY);
	inject_text("+CSQ: 31,0\r\nOK\r\n");
	k_sleep(K_MSEC(60));
	zassert_equal(k_msgq_get(&completions, &done, K_NO_WAIT), -ENOMSG);
	zassert_equal(k_msgq_get(&tx_events, &sent, K_NO_WAIT), -ENOMSG);
}

ZTEST_SUITE(gt02_at, NULL, NULL, NULL, NULL, NULL);
