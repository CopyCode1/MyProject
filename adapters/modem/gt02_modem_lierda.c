/**
 * @file
 * @brief Lierda NT26-KCN命令类型、基础查询及参数构造，特殊事务保留待接入。
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include "adapters/modem/gt02_modem_lierda.h"

LOG_MODULE_REGISTER(gt02_lierda, LOG_LEVEL_INF);

/* 只由AT线程读写；失败保留旧数值但撤销本次查询有效性，不供其他线程直接访问。 */
struct lierda_status {
	bool signal_valid;   /* 最近一次CSQ事务及正文解析是否成功。 */
	unsigned int rssi;   /* CSQ原始RSSI：0..31或99（未知），不是dBm。 */
	unsigned int ber;    /* CSQ原始BER：0..7或99（未知）。 */
	bool ready_seen;     /* 仅表示收到过RDY，不能用于承诺注册或TCP已可用。 */
};
static struct lierda_status lierda_status;

/* 公共能力只允许一个在途操作；锁内仅修改短状态，不提交AT、不调用上层回调。 */
enum lierda_operation {
	LIERDA_OP_IDLE,
	LIERDA_OP_PREPARE_AT,
	LIERDA_OP_PREPARE_ECHO,
	LIERDA_OP_QUERY_SIM,
};
static struct k_spinlock operation_lock;
static enum lierda_operation operation;
static gt02_modem_event_cb_t event_callback;
static void *event_user_data;

/* 保留公共AT结果含义；FAILED目前没有细分原因，不把模组拒绝臆断成硬件故障。 */
static int at_result_error(enum gt02_at_result result)
{
	if (result == GT02_AT_SUCCESS) {
		return 0;
	}
	return result == GT02_AT_TIMEOUT ? -ETIMEDOUT : -EIO;
}

/* 只完成匹配的公共操作，原始bring-up命令仍可单独打印；释放占用后再通知上层。 */
static void finish_operation(enum lierda_operation expected, int error,
			     enum gt02_modem_sim_state sim)
{
	k_spinlock_key_t key = k_spin_lock(&operation_lock);

	if (operation != expected) {
		k_spin_unlock(&operation_lock, key);
		return;
	}
	gt02_modem_event_cb_t callback = event_callback;
	void *user_data = event_user_data;
	const struct gt02_modem_event event = {
		.type = expected == LIERDA_OP_QUERY_SIM ? GT02_MODEM_EVENT_QUERY_DONE :
			GT02_MODEM_EVENT_PREPARE_DONE,
		.query = GT02_MODEM_QUERY_SIM,
		.error = error,
		.sim = error == 0 && expected == LIERDA_OP_QUERY_SIM ? sim : GT02_MODEM_SIM_UNKNOWN,
	};

	operation = LIERDA_OP_IDLE;
	k_spin_unlock(&operation_lock, key);
	callback(&event, user_data);
}

/* 先登记在途上下文再提交，允许很快到达的回调；入队失败时撤销占用且不通知。 */
static int begin_operation(enum lierda_operation requested, uint16_t command_id)
{
	if (k_is_in_isr()) {
		return -EWOULDBLOCK;
	}
	k_spinlock_key_t key = k_spin_lock(&operation_lock);
	int ret = 0;

	if (event_callback == NULL) {
		ret = -EACCES;
	} else if (operation != LIERDA_OP_IDLE) {
		ret = -EBUSY;
	} else {
		operation = requested;
	}
	k_spin_unlock(&operation_lock, key);
	if (ret != 0) {
		return ret;
	}
	ret = gt02_at_submit(command_id);
	if (ret != 0) {
		key = k_spin_lock(&operation_lock);
		operation = LIERDA_OP_IDLE;
		k_spin_unlock(&operation_lock, key);
	}
	return ret;
}

/* 只绑定厂家无关事件出口，操作表不隐式选择UART或初始化AT核心。 */
static int lierda_init(gt02_modem_event_cb_t callback, void *user_data)
{
	if (k_is_in_isr()) {
		return -EWOULDBLOCK;
	}
	if (callback == NULL) {
		return -EINVAL;
	}
	k_spinlock_key_t key = k_spin_lock(&operation_lock);
	int ret = 0;

	if (event_callback != NULL) {
		ret = -EALREADY;
	} else {
		event_callback = callback;
		event_user_data = user_data;
	}
	k_spin_unlock(&operation_lock, key);
	return ret;
}

/* 异步准备只组织AT/ATE0，调用者先完成硬件上电和启动等待，不在AT线程中睡眠。 */
static int lierda_prepare(void)
{
	return begin_operation(LIERDA_OP_PREPARE_AT, LIERDA_CMD_AT);
}

/* 以公共查询类型选择厂家命令；未实现的类型明确拒绝，不伪造完成事件。 */
static int lierda_query(enum gt02_modem_query query)
{
	if (k_is_in_isr()) {
		return -EWOULDBLOCK;
	}
	if (query != GT02_MODEM_QUERY_SIM) {
		return -ENOTSUP;
	}
	return begin_operation(LIERDA_OP_QUERY_SIM, LIERDA_CMD_CPIN);
}

/* 最终回调在线程中报告握手/关闭回显结果，不在此等待或自动重试。 */
static void on_basic_result(enum gt02_at_result result, const char *response,
			    size_t length, void *user_data)
{
	const char *name = user_data;

	ARG_UNUSED(response);
	ARG_UNUSED(length);
	if (result == GT02_AT_SUCCESS) {
		LOG_INF("%s PASS", name);
	} else {
		LOG_ERR("%s failed: result=%d (1=failed, 2=timeout)", name, result);
	}
}

/* AT线程完成准备的第一步；成功才提交ATE0，失败或第二步拒绝都结束本次公共操作。 */
static void on_prepare_at_result(enum gt02_at_result result, const char *response,
				 size_t length, void *user_data)
{
	on_basic_result(result, response, length, user_data);
	k_spinlock_key_t key = k_spin_lock(&operation_lock);

	if (operation != LIERDA_OP_PREPARE_AT) {
		k_spin_unlock(&operation_lock, key);
		return;
	}
	if (result != GT02_AT_SUCCESS) {
		k_spin_unlock(&operation_lock, key);
		finish_operation(LIERDA_OP_PREPARE_AT, at_result_error(result), GT02_MODEM_SIM_UNKNOWN);
		return;
	}
	operation = LIERDA_OP_PREPARE_ECHO;
	k_spin_unlock(&operation_lock, key);
	int ret = gt02_at_submit(LIERDA_CMD_ECHO_OFF);

	if (ret != 0) {
		finish_operation(LIERDA_OP_PREPARE_ECHO, ret, GT02_MODEM_SIM_UNKNOWN);
	}
}

/* AT线程完成ATE0后交付一次准备结果；独立原始命令没有公共操作时只保留原日志。 */
static void on_prepare_echo_result(enum gt02_at_result result, const char *response,
				   size_t length, void *user_data)
{
	on_basic_result(result, response, length, user_data);
	finish_operation(LIERDA_OP_PREPARE_ECHO, at_result_error(result), GT02_MODEM_SIM_UNKNOWN);
}

/* 只有最终OK后的完整合法正文才能更新信号；数值宽度受限，避免任意大整数转换。 */
static void on_csq_result(enum gt02_at_result result, const char *response,
			  size_t length, void *user_data)
{
	struct lierda_status *status = user_data;
	unsigned int rssi;
	unsigned int ber;
	char extra;

	status->signal_valid = false;
	if (result != GT02_AT_SUCCESS) {
		LOG_ERR("CSQ query failed: result=%d; signal not updated", result);
		return;
	}
	if (response == NULL || memchr(response, '\0', length) != NULL ||
	    sscanf(response, "+CSQ: %3u,%3u %c", &rssi, &ber, &extra) != 2 ||
	    (rssi > 31 && rssi != 99) || (ber > 7 && ber != 99)) {
		LOG_ERR("CSQ AT completed but body is invalid; signal not updated");
		return;
	}
	status->rssi = rssi;
	status->ber = ber;
	status->signal_valid = true;
	LOG_INF("CSQ query PASS: rssi=%u, ber=%u (99=unknown)", rssi, ber);
}

/* 基础文本查询只展示最终OK后的原文；CCLK不校时，QIACT不据此建立TCP状态。 */
static void on_text_result(enum gt02_at_result result, const char *response,
			   size_t length, void *user_data)
{
	const char *name = user_data;

	if (result != GT02_AT_SUCCESS) {
		LOG_ERR("%s query failed: result=%d", name, result);
		return;
	}
	if (response == NULL) {
		/* 仅QIACT?允许无正文；不把单独OK当成PDP已激活。 */
		LOG_INF("%s query OK: no context rows returned", name);
		return;
	}
	if (memchr(response, '\0', length) != NULL) {
		LOG_ERR("%s body contains NUL; result ignored", name);
		return;
	}
	LOG_INF("%s query OK, raw body:\n%.*s", name, (int)length, response);
}

/* 最终AT结果及SIM正文分别检查；AT线程交付结构化结果，非READY不执行解锁或恢复。 */
static void on_cpin_result(enum gt02_at_result result, const char *response,
			   size_t length, void *user_data)
{
	char sim_state[32] = {0};
	char extra;
	enum gt02_modem_sim_state sim = GT02_MODEM_SIM_UNKNOWN;
	int error = at_result_error(result);

	ARG_UNUSED(user_data);
	if (error == 0) {
		if (response == NULL || memchr(response, '\0', length) != NULL ||
		    sscanf(response, "+CPIN: %31[^\r\n] %c", sim_state, &extra) != 1) {
			error = -EBADMSG;
		} else {
			size_t size = strlen(sim_state);

			while (size != 0 && (sim_state[size - 1] == ' ' || sim_state[size - 1] == '\t')) {
				sim_state[--size] = '\0';
			}
			if (strcmp(sim_state, "READY") == 0) {
				sim = GT02_MODEM_SIM_READY;
			} else if (strcmp(sim_state, "SIM PIN") == 0) {
				sim = GT02_MODEM_SIM_PIN_REQUIRED;
			} else if (strcmp(sim_state, "SIM PUK") == 0) {
				sim = GT02_MODEM_SIM_PUK_REQUIRED;
			} else if (strcmp(sim_state, "NOT READY") == 0) {
				sim = GT02_MODEM_SIM_NOT_READY;
			} else {
				error = -EBADMSG;
			}
		}
	}
	if (error == 0) {
		LOG_INF("SIM state: %s (READY=%d)", sim_state, sim == GT02_MODEM_SIM_READY);
	} else {
		LOG_ERR("CPIN query failed or invalid body: result=%d error=%d", result, error);
	}
	finish_operation(LIERDA_OP_QUERY_SIM, error, sim);
}

/* IMEI/IMSI按旧缓存上限校验纯数字，不将混入的URC或多行正文当成标识。 */
static void on_identity_result(enum gt02_at_result result, const char *response,
			       size_t length, void *user_data)
{
	const char *name = user_data;

	if (result != GT02_AT_SUCCESS || response == NULL || length == 0 || length > 19 ||
	    strspn(response, "0123456789") != length) {
		LOG_ERR("%s query failed or invalid digits: result=%d", name, result);
		return;
	}
	LOG_INF("%s query PASS: %.*s", name, (int)length, response);
}

/* ICCID按20字符标识处理，允许数字和大小写英文字母，保留原文及可选双引号兼容。 */
static void on_iccid_result(enum gt02_at_result result, const char *response,
			    size_t length, void *user_data)
{
	const size_t prefix_size = sizeof("+QCCID:") - 1;
	const char *value;
	size_t count;

	ARG_UNUSED(user_data);
	if (result != GT02_AT_SUCCESS || response == NULL || length < prefix_size ||
	    memchr(response, '\0', length) != NULL ||
	    memcmp(response, "+QCCID:", prefix_size) != 0) {
		LOG_ERR("ICCID query failed or invalid prefix: result=%d", result);
		return;
	}
	value = response + prefix_size;
	count = length - prefix_size;
	while (count != 0 && (*value == ' ' || *value == '\t')) {
		++value;
		--count;
	}
	while (count != 0 && (value[count - 1] == ' ' || value[count - 1] == '\t')) {
		--count;
	}
	if (count >= 2 && value[0] == '"' && value[count - 1] == '"') {
		++value;
		count -= 2;
	}
	if (count != 20) {
		LOG_ERR("ICCID body invalid: expected 20 characters");
		return;
	}
	for (size_t i = 0; i < count; ++i) {
		char ch = value[i];

		if (!((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') ||
		      (ch >= 'a' && ch <= 'z'))) {
			LOG_ERR("ICCID body invalid: non-alphanumeric character");
			return;
		}
	}
	LOG_INF("ICCID query PASS: %.*s", (int)count, value);
}

/* 保持旧CGREG的两字段查询格式，注册状态1/5只表示分组域注册，不承诺PDP/TCP。 */
static void on_cgreg_result(enum gt02_at_result result, const char *response,
			    size_t length, void *user_data)
{
	unsigned int mode;
	unsigned int registration;
	char extra;

	ARG_UNUSED(user_data);
	if (result != GT02_AT_SUCCESS || response == NULL ||
	    memchr(response, '\0', length) != NULL ||
	    sscanf(response, "+CGREG: %1u,%1u %c", &mode, &registration, &extra) != 2 ||
	    mode != 0 || registration > 5) {
		LOG_ERR("CGREG query failed or unsupported body: result=%d", result);
		return;
	}
	LOG_INF("CGREG: stat=%u, registered=%d (1=home, 5=roaming); PDP/TCP not tested",
		registration, registration == 1 || registration == 5);
}

/* RDY是独立启动事件，即使无在途事务也处理；只记录，不自动发下一条命令。 */
static void on_ready(const char *content, size_t length, void *user_data)
{
	struct lierda_status *status = user_data;

	ARG_UNUSED(content);
	ARG_UNUSED(length);
	status->ready_seen = true;
	LOG_INF("RDY URC received; registration/TCP not tested");
}

/* 厂家内部配置模组GPIO，只接受旧工程实际使用的引脚，不扩大硬件控制范围。 */
int gt02_lierda_gpio_config(unsigned int pin)
{
	char command[40];

	if (pin != 97 && pin != 99) {
		return -EINVAL;
	}
	int length = snprintf(command, sizeof(command), "AT+LGPIO=%u,1", pin);

	if (length < 0 || (size_t)length >= sizeof(command)) {
		return -EMSGSIZE;
	}
	return gt02_at_submit_text(LIERDA_CMD_LGPIO, command);
}

/* 厂家内部构造电平参数；AT入口在返回前复制局部文本，后续调用不会覆盖排队内容。 */
int gt02_lierda_gpio_set(unsigned int pin, bool high)
{
	char command[40];

	if (pin != 97 && pin != 99) {
		return -EINVAL;
	}
	int length = snprintf(command, sizeof(command), "AT+LGPIO=%u,1,%u", pin, high ? 1U : 0U);

	if (length < 0 || (size_t)length >= sizeof(command)) {
		return -EMSGSIZE;
	}
	return gt02_at_submit_text(LIERDA_CMD_LGPIO, command);
}

static const struct gt02_at_command lierda_commands[] = {
	{ .id = LIERDA_CMD_AT,          .command = "AT",           .timeout_ms = 2000,		.callback = on_prepare_at_result, .user_data = "AT handshake", },
	{ .id = LIERDA_CMD_ECHO_OFF,    .command = "ATE0",         .timeout_ms = 2000,		.callback = on_prepare_echo_result, .user_data = "Echo off",   },
	{ .id = LIERDA_CMD_CSQ,         .command = "AT+CSQ",       .timeout_ms = 2000,		.callback = on_csq_result,      .user_data = &lierda_status, 	.response_prefix = "+CSQ:",  .require_response = true },

	/* 空前缀只收集未命中已注册URC的正文；数字标识还须通过回调校验。 */					
	{ .id = LIERDA_CMD_ATI,         .command = "ATI",          .timeout_ms = 2000,		.callback = on_text_result,     .user_data = "ATI",  	.response_prefix = "",        	.require_response = true },
	{ .id = LIERDA_CMD_CPIN,        .command = "AT+CPIN?",     .timeout_ms = 2000,		.callback = on_cpin_result,                          	.response_prefix = "+CPIN:",  	.require_response = true },
	{ .id = LIERDA_CMD_CGSN,        .command = "AT+CGSN",      .timeout_ms = 2000,		.callback = on_identity_result, .user_data = "IMEI", 	.response_prefix = "",        	.require_response = true },
	{ .id = LIERDA_CMD_CIMI,        .command = "AT+CIMI",      .timeout_ms = 2000,		.callback = on_identity_result, .user_data = "IMSI", 	.response_prefix = "",        	.require_response = true },
	{ .id = LIERDA_CMD_QCCID,       .command = "AT+QCCID",     .timeout_ms = 1000,		.callback = on_iccid_result,                         	.response_prefix = "+QCCID:", 	.require_response = true },
	{ .id = LIERDA_CMD_CGREG,       .command = "AT+CGREG?",    .timeout_ms = 2000,		.callback = on_cgreg_result,                         	.response_prefix = "+CGREG:", 	.require_response = true },
	{ .id = LIERDA_CMD_CCLK,        .command = "AT+CCLK?",     .timeout_ms = 2000,		.callback = on_text_result,     .user_data = "CCLK", 	.response_prefix = "+CCLK:",  	.require_response = true },
	{ .id = LIERDA_CMD_QIACT_QUERY, .command = "AT+QIACT?",    .timeout_ms = 10000,		.callback = on_text_result,     .user_data = "QIACT",	.response_prefix = "+QIACT:" },

/* 以下仅登记旧功能：未接入参数、生命周期、提示符或异步业务完成条件。 */
	{ .id = LIERDA_CMD_RESET,       .command = "AT+RESET",     .timeout_ms = 2000, 		.reserved = true },
	{ .id = LIERDA_CMD_QIDEACT,     .command = "AT+QIDEACT=1", .timeout_ms = 2000,		.reserved = true },
	{ .id = LIERDA_CMD_QIACT,       .command = "AT+QIACT=1",   .timeout_ms = 10000,  	.reserved = true },	
	{ .id = LIERDA_CMD_QICSGP,      .command = "AT+QICSGP",    .timeout_ms = 5000, 		.reserved = true, 		.parameterized = true },
	{ .id = LIERDA_CMD_QIOPEN,      .command = "AT+QIOPEN",    .timeout_ms = 4000,   	.reserved = true, 		.parameterized = true },
	{ .id = LIERDA_CMD_QICLOSE,     .command = "AT+QICLOSE",   .timeout_ms = 2000,   	.reserved = true,		.parameterized = true },
	{ .id = LIERDA_CMD_QISEND,      .command = "AT+QISEND",    .timeout_ms = 10000,  	.reserved = true, 		.parameterized = true },
	{ .id = LIERDA_CMD_QTTS,        .command = "AT+QTTS",      .timeout_ms = 2000,   	.reserved = true, 		.parameterized = true },
	{ .id = LIERDA_CMD_QTTSETUP,    .command = "AT+QTTSETUP",  .timeout_ms = 2000,   	.reserved = true, 		.parameterized = true },

	/* 普通参数化文本已经接通；仅厂家内部调用，不在启动测试中自动执行。 */
	{ .id = LIERDA_CMD_LGPIO,		.command = "AT+LGPIO",	   .timeout_ms = 2000,		.callback = on_basic_result,	.user_data = "LGPIO",		.parameterized = true },
};

static const struct gt02_at_urc lierda_urcs[] = {
	{ .match = "RDY", .exact = true, .callback = on_ready, .user_data = &lierda_status },
};

const struct gt02_at_profile gt02_lierda_at_profile = {
	.commands = lierda_commands, .command_count = ARRAY_SIZE(lierda_commands),
	.urcs = lierda_urcs, .urc_count = ARRAY_SIZE(lierda_urcs),
};

/* 装配方选择具体厂家；服务只保存公共操作表，不直接调用这些静态实现。 */
const struct gt02_modem_ops gt02_lierda_ops = {
	.init = lierda_init,
	.prepare = lierda_prepare,
	.query = lierda_query,
};
