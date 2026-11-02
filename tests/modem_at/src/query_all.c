/** @file
 * @brief 用真实厂家解析、AT核心和启动测试验证11条顺序查询，只替换电源和UART。
 */
#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/ztest.h>

#include "adapters/modem/gt02_modem_lierda.h"
#include "tests/bringup/gt02_modem_at_test.h"

/* 明确检查发送内容和先后次序，任何GPIO/设置命令、重复或额外命令都会使测试失败。 */
static const char *const expected[] = {
	"AT", "ATE0", "ATI", "AT+CPIN?", "AT+CGSN", "AT+CIMI", "AT+QCCID",
	"AT+CSQ", "AT+CGREG?", "AT+CCLK?", "AT+QIACT?",
};
static const char *const replies[] = {
	"OK\r\n",
	"OK\r\n",
	"Lierda\r\nNT26-KCN\r\nRevision: mock\r\nOK\r\n",
	"+CPIN: READY\r\nOK\r\n",
	"123456789012345\r\nOK\r\n",
	"460001234567890\r\nOK\r\n",
#if CONFIG_AT_TEST_QUERY_FAULT_CASE == 1
	"+QCCID: 898600Z234567890123g\r\nOK\r\n", /* 合成标识覆盖非十六进制字母及小写。 */
#else
	"+QCCID: 898600A2345678901234\r\nOK\r\n", /* 合成标识复现带A的实际回复形式。 */
#endif
	"+CSQ: 18,0\r\nOK\r\n",
	"+CGREG: 0,1\r\nOK\r\n",
	"+CCLK: \"26/10/30,08:00:00+00\"\r\nOK\r\n",
	"OK\r\n", /* 查询不到已激活上下文也是合法的查询回复。 */
};

K_MSGQ_DEFINE(fake_rx, sizeof(uint8_t), 1024, 1);
K_SEM_DEFINE(last_sent, 0, 1);
static atomic_t power_count;
static atomic_t tx_count;

/* 将模拟回复逐字节放入队列，保留真实SDK的文本拼接路径。 */
static void inject_text(const char *text)
{
	for (size_t i = 0; i < strlen(text); ++i) {
		zassert_ok(k_msgq_put(&fake_rx, &text[i], K_NO_WAIT));
	}
}

/* 模拟首次上电立即产生RDY；任何自动上电重试都会被计数检查发现。 */
static int fake_power_on(void)
{
	zassert_equal(atomic_inc(&power_count), 0);
	inject_text("RDY\r\n");
	return 0;
}

/* 未上电时沿用硬件接口的-EPIPE，之后每次读一个字节，不绕过真实AT接收循环。 */
static int fake_read(uint8_t *buffer, size_t capacity, size_t *received, k_timeout_t timeout)
{
	zassert_true(capacity > 0);
	*received = 0;
	if (atomic_get(&power_count) == 0) {
		return -EPIPE;
	}
	if (k_msgq_get(&fake_rx, buffer, timeout) != 0) {
		return -EAGAIN;
	}
	*received = 1;
	return 0;
}

/* 核对命令并回包；可注入明确ERROR或不回包，验证继续/停止策略。 */
static int fake_write(const uint8_t *buffer, size_t length, size_t *written)
{
	unsigned int index = (unsigned int)atomic_inc(&tx_count);

	zassert_true(index < ARRAY_SIZE(expected), "unexpected/repeated command");
	zassert_equal(length, strlen(expected[index]) + 2);
	zassert_mem_equal(buffer, expected[index], length - 2);
	zassert_mem_equal(buffer + length - 2, "\r\n", 2);
	*written = length;
	if (CONFIG_AT_TEST_QUERY_FAULT_CASE == 2 && index == 3) {
		k_sem_give(&last_sent);
		return 0;
	}
	if (CONFIG_AT_TEST_QUERY_FAULT_CASE == 1 && index == 9) {
		inject_text("ERROR\r\n");
	} else {
		inject_text(replies[index]);
	}
	if (index == ARRAY_SIZE(expected) - 1) {
		k_sem_give(&last_sent);
	}
	return 0;
}

static const struct gt02_at_io_ops io = { .read = fake_read, .write = fake_write };
static const struct gt02_modem_power_ops power = { .power_on = fake_power_on };
static const struct gt02_at_config config = { .profile = &gt02_lierda_at_profile, .io = &io };

/* 实际运行生产测试入口；不修改厂家原表，只允许每条查询执行一次。 */
ZTEST(gt02_query_all, test_query_sequence)
{
	struct gt02_at_profile missing = gt02_lierda_at_profile;
	struct gt02_at_config invalid = config;
	gt02_at_callback_t original_callback = gt02_lierda_at_profile.commands[0].callback;

	missing.command_count = 2;
	invalid.profile = &missing;
	zassert_equal(gt02_modem_at_test_start(&invalid, &power), -EINVAL);
	zassert_equal(atomic_get(&power_count), 0);
	zassert_ok(gt02_modem_at_test_start(&config, &power));
	zassert_equal(gt02_modem_at_test_start(&config, &power), -EALREADY);
	zassert_ok(k_sem_take(&last_sent, K_SECONDS(10)));
	if (CONFIG_AT_TEST_QUERY_FAULT_CASE == 2) {
		k_sleep(K_SECONDS(3));
		zassert_equal(atomic_get(&tx_count), 4);
		zassert_equal(gt02_at_submit(LIERDA_CMD_CSQ), -EPIPE);
	} else {
		/* 最后一条已回包，留出接收、厂家解析和最终测试汇总的执行时间。 */
		k_sleep(K_MSEC(200));
		zassert_equal(atomic_get(&tx_count), ARRAY_SIZE(expected));
		zassert_equal(gt02_at_submit(LIERDA_CMD_LGPIO), -EINVAL);
		zassert_equal(gt02_at_submit(LIERDA_CMD_RESET), -ENOTSUP);
	}
	zassert_equal(atomic_get(&power_count), 1);
	zassert_equal_ptr(gt02_lierda_at_profile.commands[0].callback, original_callback);
}

ZTEST_SUITE(gt02_query_all, NULL, NULL, NULL, NULL, NULL);
