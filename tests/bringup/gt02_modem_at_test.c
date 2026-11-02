/** @file
 * @brief 上电一次，逐条完成握手和现有信息查询；原厂家回调继续解析并输出结果。
 */
#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include "adapters/modem/gt02_modem_at.h"
#include "adapters/modem/gt02_modem_lierda.h"
#include "tests/bringup/gt02_modem_at_test.h"

LOG_MODULE_REGISTER(gt02_at_test, LOG_LEVEL_INF);
static atomic_t started;
static const struct gt02_modem_power_ops *power_ops;

/* 执行顺序属于本次验证，不改厂家只读命令表，也不夹带GPIO/联网等设置。 */
static const struct {
	uint16_t id;       /* 厂家内部命令类型。 */
	const char *name;  /* 日志中使用的可读查询名。 */
} queries[] = {
	{ LIERDA_CMD_AT, "AT handshake" },
	{ LIERDA_CMD_ECHO_OFF, "Echo off" },
	{ LIERDA_CMD_ATI, "Manufacturer/model/revision" },
	{ LIERDA_CMD_CPIN, "SIM state" },
	{ LIERDA_CMD_CGSN, "IMEI" },
	{ LIERDA_CMD_CIMI, "IMSI" },
	{ LIERDA_CMD_QCCID, "ICCID" },
	{ LIERDA_CMD_CSQ, "Signal RSSI/BER" },
	{ LIERDA_CMD_CGREG, "Registration" },
	{ LIERDA_CMD_CCLK, "Clock (raw)" },
	{ LIERDA_CMD_QIACT_QUERY, "PDP contexts (query only)" },
};

/* 测试副本保留完整厂家配置，只装饰本轮查询的最终回调；存储覆盖整个开机生命周期。 */
static struct gt02_at_command test_commands[LIERDA_CMD_COUNT];
static struct gt02_at_profile test_profile;
static enum gt02_at_result last_result; /* AT线程写，控制线程在取得信号量后读。 */
K_SEM_DEFINE(query_done, 0, 1);

/* AT线程先执行原厂家解析，再发布事务结果；不保留response，不阻塞等待控制线程。 */
static void on_query_complete(enum gt02_at_result result, const char *response,
			      size_t length, void *user_data)
{
	const struct gt02_at_command *original = user_data;

	original->callback(result, response, length, original->user_data);
	last_result = result;
	k_sem_give(&query_done);
}

/* 从长期厂家配置生成测试副本；保留非查询项和URC，不给AT核心增加测试专用通知接口。 */
static int prepare_profile(const struct gt02_at_profile *profile)
{
	if (profile == NULL || profile->commands == NULL || profile->command_count == 0 ||
	    profile->command_count > ARRAY_SIZE(test_commands)) {
		return -EINVAL;
	}
	memcpy(test_commands, profile->commands, profile->command_count * sizeof(test_commands[0]));
	for (size_t i = 0; i < ARRAY_SIZE(queries); ++i) {
		size_t j;

		for (j = 0; j < profile->command_count; ++j) {
			const struct gt02_at_command *cmd = &profile->commands[j];

			if (cmd->id != queries[i].id) {
				continue;
			}
			if (cmd->callback == NULL || cmd->reserved || cmd->parameterized) {
				return -EINVAL;
			}
			test_commands[j].callback = on_query_complete;
			/* 原表长期有效，此指针只读；原user_data仍由原厂家回调使用。 */
			test_commands[j].user_data = (void *)cmd;
			break;
		}
		if (j == profile->command_count) {
			return -EINVAL;
		}
	}
	test_profile = *profile;
	test_profile.commands = test_commands;
	return 0;
}

/* 控制线程每次只提交一条，等待原厂家回调完成后再推进；ERROR继续，超时/失步停止。 */
static void test_worker(void *p1, void *p2, void *p3)
{
	unsigned int successful = 0;
	unsigned int failed = 0;
	int ret;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	LOG_INF("One-shot information test: 11 commands, sequential; no PDP activation/TCP/GPIO");
	ret = power_ops->power_on();
	LOG_INF("4G power_on result=%d", ret);
	if (ret < 0) {
		return;
	}
	/* 当前NT26-KCN验证用稳定间隔，不是通用模组就绪判定或自动恢复策略。 */
	k_sleep(K_MSEC(2000));
	for (size_t i = 0; i < ARRAY_SIZE(queries); ++i) {
		LOG_INF("Query %u/%u: %s", (unsigned int)i + 1, (unsigned int)ARRAY_SIZE(queries),
			queries[i].name);
		ret = gt02_at_submit(queries[i].id);
		if (ret < 0) {
			LOG_ERR("Information test stopped: id=%u submit=%d; no retry", queries[i].id, ret);
			return;
		}
		/* 独立等待保护：覆盖最长10秒查询并留5秒调度余量；不取消或重发在途命令。 */
		ret = k_sem_take(&query_done, K_SECONDS(15));
		if (ret < 0) {
			LOG_ERR("Information test stopped: callback not received; request not cancelled");
			return;
		}
		if (last_result == GT02_AT_TIMEOUT) {
			LOG_ERR("Information test stopped: id=%u timed out; AT stream requires recovery",
				queries[i].id);
			return;
		}
		if (last_result == GT02_AT_SUCCESS) {
			++successful;
		} else {
			++failed;
			LOG_WRN("Query id=%u failed; trying next query if AT stream remains usable", queries[i].id);
		}
	}
	LOG_INF("Information queries finished once: AT success=%u, failed=%u; check vendor body logs",
		successful, failed);
}

K_THREAD_DEFINE(at_test_thread, 2048, test_worker, NULL, NULL, NULL, 7, 0, SYS_FOREVER_MS);

/* 装配长期测试配置并启动唯一RX线程，再由控制线程上电；不修改原表，不新增硬件依赖。 */
int gt02_modem_at_test_start(const struct gt02_at_config *at,
			     const struct gt02_modem_power_ops *power)
{
	if (at == NULL || power == NULL || power->power_on == NULL) {
		return -EINVAL;
	}
	if (!atomic_cas(&started, 0, 1)) {
		return -EALREADY;
	}
	int ret = prepare_profile(at->profile);

	if (ret == 0) {
		const struct gt02_at_config test_config = { .profile = &test_profile, .io = at->io };

		ret = gt02_at_init(&test_config);
	}

	if (ret < 0) {
		atomic_clear(&started);
		return ret;
	}
	power_ops = power;
	k_thread_start(at_test_thread);
	return 0;
}
