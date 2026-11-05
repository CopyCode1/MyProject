/**
 * @file
 * @brief 单线程AT事务核心：SDK负责文本分片，项目负责表分发和最终回调。
 */
#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net_buf.h>
#include <zephyr/sys/util.h>
#include "modem_cmd_handler.h"

#include "adapters/modem/gt02_modem_at.h"

LOG_MODULE_REGISTER(gt02_at, LOG_LEVEL_INF);

#define AT_QUEUE_SIZE 4
#define AT_LINE_SIZE 256
#define AT_BODY_SIZE 512
#define AT_READ_SLICE_MS 20
#define AT_DRAIN_DEFAULT_MS 5000

/* SDK每次只接收一小段，且输入先检查行长；4个固定块不使用通用堆。 */
NET_BUF_POOL_FIXED_DEFINE(at_pool, 4, 512, 0, NULL);
static struct modem_cmd_handler parser;
static struct modem_cmd_handler_data parser_data;
static char match_buffer[AT_LINE_SIZE + 1];
static const uint8_t *input;
static size_t input_left;

/* API状态和请求FIFO由queue_lock保护，其余事务/解析状态只归AT线程。 */
enum core_state { CORE_UNINITIALIZED, CORE_STARTING, CORE_READY, CORE_DRAINING, CORE_FAULTED };
static struct k_spinlock queue_lock;
static enum core_state state;
static const struct gt02_at_profile *registered;
static const struct gt02_at_io_ops *transport; /* 仅依赖注入的字节能力，不依赖具体硬件符号。 */
/* 每个排队请求拥有独立文本；出队复制到active_request后，原槽位才能重新入队。 */
struct at_request {
	const struct gt02_at_command *command; /* 长期有效的只读规则，不复制回调上下文。 */
	char text[GT02_AT_COMMAND_MAX + 1];     /* 本次完整命令，含NUL、不含CRLF。 */
};
static struct at_request queue[AT_QUEUE_SIZE];
static struct at_request active_request;
static size_t queue_head;
static size_t queue_count;
static const struct gt02_at_command *current;
static char body[AT_BODY_SIZE + 1];
static size_t body_size;
static int64_t deadline_ms;
static bool terminal_ready;
static enum gt02_at_result terminal_result;
static int fatal_error;
static size_t line_size;
static bool discard_line;
static bool old_terminal_seen; /* 只由AT线程访问：隔离期已收到旧事务的最终结束行。 */
static uint32_t drain_timeout_ms;
static int64_t drain_deadline_ms;
static gt02_at_fault_callback_t fault_callback;
static void *fault_user_data;
static bool session_broken;
static bool session_received;
K_MUTEX_DEFINE(rearm_lock);
K_SEM_DEFINE(rearm_done, 0, 1);
static bool rearm_pending; /* queue_lock保护；只有Worker执行恢复。 */
static int rearm_result;

/* 仅在SDK解析返回后的Worker安全点释放残行；保留协议表、固定池和解析器锁。 */
static void clear_rx_context(void)
{
	if (parser_data.rx_buf != NULL) {
		net_buf_unref(parser_data.rx_buf);
		parser_data.rx_buf = NULL;
	}
	parser_data.last_error = 0;
	match_buffer[0] = '\0';
	line_size = 0;
	discard_line = false;
	input = NULL;
	input_left = 0;
}

/* 请求已全部通知后重建本地解析上下文；清缓存本身不改变调度状态。 */
static void clear_context(void)
{
	current = NULL;
	body_size = 0;
	terminal_ready = false;
	old_terminal_seen = false;
	fatal_error = 0;
	deadline_ms = 0;
	drain_deadline_ms = 0;
	clear_rx_context();
}

/* 完整批次已处理且SDK没有残留字节时，才允许调度或重建会话。 */
static bool parser_at_boundary(void)
{
	return line_size == 0 && !discard_line && input_left == 0 &&
		(parser_data.rx_buf == NULL || net_buf_frags_len(parser_data.rx_buf) == 0);
}

/* 长度参与匹配，避免短行、内嵌NUL或SDK残留字符导致错误命中。 */
static bool matches(const char *line, size_t length, const char *pattern, bool exact)
{
	size_t size = strlen(pattern);

	return length >= size && (!exact || length == size) && memcmp(line, pattern, size) == 0;
}

/* 判断两条精确/前缀规则是否能命中同一行，初始化时拒绝隐含的归属冲突。 */
static bool overlaps(const char *a, bool a_exact, const char *b, bool b_exact)
{
	size_t a_size = strlen(a);
	size_t b_size = strlen(b);

	if (memcmp(a, b, MIN(a_size, b_size)) != 0) {
		return false;
	}
	return (!a_exact || a_size >= b_size) && (!b_exact || b_size >= a_size);
}

/* 排除SDK分发表之外的公共最终响应，避免自定义表吞掉事务结束标记。 */
static bool reserved_pattern(const char *pattern, bool exact)
{
	return overlaps(pattern, exact, "OK", true) ||
	       overlaps(pattern, exact, "ERROR", true) ||
	       overlaps(pattern, exact, "+CME ERROR:", false) ||
	       overlaps(pattern, exact, "+CMS ERROR:", false);
}

/* 校验长期配置，不创建每命令响应缓存，也不通过SDK类型向外泄漏实现细节。 */
static int validate_profile(const struct gt02_at_profile *profile)
{
	if (profile == NULL || profile->commands == NULL || profile->command_count == 0 ||
	    (profile->urc_count != 0 && profile->urcs == NULL)) {
		return -EINVAL;
	}
	for (size_t i = 0; i < profile->command_count; ++i) {
		const struct gt02_at_command *cmd = &profile->commands[i];

		if (cmd->command == NULL || cmd->command[0] == '\0' ||
		    strlen(cmd->command) > GT02_AT_COMMAND_MAX || strpbrk(cmd->command, "\r\n") != NULL ||
		    (!cmd->reserved && cmd->callback == NULL) || cmd->timeout_ms == 0 ||
		    (cmd->parameterized && (strpbrk(cmd->command, "=? \t") != NULL ||
		     strlen(cmd->command) > GT02_AT_COMMAND_MAX - 2)) ||
		    (cmd->require_response && cmd->response_prefix == NULL)) {
			return -EINVAL;
		}
		if (cmd->response_prefix != NULL && cmd->response_prefix[0] != '\0' &&
		    (strlen(cmd->response_prefix) > AT_LINE_SIZE ||
		     reserved_pattern(cmd->response_prefix, false))) {
			return -EINVAL;
		}
		for (size_t j = 0; j < i; ++j) {
			if (cmd->id == profile->commands[j].id) {
				return -EINVAL;
			}
		}
	}
	for (size_t i = 0; i < profile->urc_count; ++i) {
		const struct gt02_at_urc *urc = &profile->urcs[i];

		if (urc->match == NULL || urc->match[0] == '\0' ||
		    strlen(urc->match) > AT_LINE_SIZE || urc->callback == NULL ||
		    reserved_pattern(urc->match, urc->exact)) {
			return -EINVAL;
		}
		for (size_t j = 0; j < i; ++j) {
			if (overlaps(urc->match, urc->exact, profile->urcs[j].match,
				     profile->urcs[j].exact)) {
				return -EINVAL;
			}
		}
		for (size_t j = 0; j < profile->command_count; ++j) {
			const char *prefix = profile->commands[j].response_prefix;

			if (prefix != NULL && prefix[0] != '\0' &&
			    overlaps(urc->match, urc->exact, prefix, false)) {
				return -EINVAL;
			}
		}
	}
	return 0;
}

/* 唯一消费方将FIFO头复制到在途存储；其他提交线程可以安全复用已释放的队列槽位。 */
static bool take_request(void)
{
	k_spinlock_key_t key = k_spin_lock(&queue_lock);
	bool found = queue_count != 0;

	if (found) {
		active_request = queue[queue_head];
		queue_head = (queue_head + 1) % AT_QUEUE_SIZE;
		--queue_count;
	}
	k_spin_unlock(&queue_lock, key);
	return found;
}

/* AT线程输出转义后的完整行；长日志分段，原始二进制字节不交给printf解释。 */
static void log_line(const char *line, size_t length)
{
	static const char hex[] = "0123456789ABCDEF";
	char text[129];
	size_t used = 0;

	for (size_t i = 0; i < length; ++i) {
		uint8_t byte = (uint8_t)line[i];
		bool printable = byte >= 0x20 && byte <= 0x7e;

		if (used + (printable ? 1 : 4) > sizeof(text) - 1) {
			text[used] = '\0';
			LOG_INF("4G RX [part]: %s", text);
			used = 0;
		}
		if (printable) {
			text[used++] = (char)byte;
		} else {
			text[used++] = '\\';
			text[used++] = 'x';
			text[used++] = hex[byte >> 4];
			text[used++] = hex[byte & 15];
		}
	}
	text[used] = '\0';
	LOG_INF("4G RX: %s", text);
}

/* 事务已经结束再通知用户；回调返回后才能复用共享正文，禁止交付失败时的半截内容。 */
static void finish_current(enum gt02_at_result result)
{
	const struct gt02_at_command *cmd = current;

	current = NULL;
	terminal_ready = false;
	LOG_INF("AT done: id=%u result=%d", cmd->id, result);
	cmd->callback(result, result == GT02_AT_SUCCESS && body_size != 0 ? body : NULL,
		      result == GT02_AT_SUCCESS ? body_size : 0, cmd->user_data);
	body_size = 0;
}

/* 超时进入隔离等待，传输/完整性故障进入FAULT；先暂停提交，再逐个通知请求一次。
 * 排队请求尚未发送，取消后由上层决定是否重新提交；不在AT层重试业务或控制电源。
 */
static void fail_core(int error, bool isolate)
{
	k_spinlock_key_t key = k_spin_lock(&queue_lock);

	bool already_faulted = state == CORE_FAULTED;

	state = isolate ? CORE_DRAINING : CORE_FAULTED;
	k_spin_unlock(&queue_lock, key);
	if (already_faulted) {
		clear_rx_context();
		session_received = false;
		fatal_error = 0;
		return;
	}
	old_terminal_seen = false;
	LOG_WRN("AT requests ended: error=%d; queued requests cancelled", error);
	if (isolate) {
		drain_deadline_ms = k_uptime_get() + drain_timeout_ms;
		LOG_WRN("AT timeout: waiting for old final result; RX/URC remain active");
	} else {
		LOG_ERR("AT transport/parser fault; normal scheduling suspended");
	}
	if (current != NULL) {
		finish_current(isolate ? GT02_AT_TIMEOUT : GT02_AT_FAILED);
	}
	while (take_request()) {
		const struct gt02_at_command *cmd = active_request.command;

		cmd->callback(GT02_AT_FAILED, NULL, 0, cmd->user_data);
	}
	fatal_error = 0;
	if (!isolate) {
		clear_context();
		session_broken = false;
		session_received = false;
		if (fault_callback != NULL) {
			fault_callback(error, fault_user_data);
		}
	}
}

/* 在唯一共享缓存中追加一行正文；超限使流失效，不交付截断结果。 */
static void append_body(const char *line, size_t length)
{
	size_t separator = body_size != 0 ? 1 : 0;

	if (length + separator > AT_BODY_SIZE - body_size) {
		fatal_error = -EMSGSIZE;
		return;
	}
	if (separator != 0) {
		body[body_size++] = '\n';
	}
	memcpy(&body[body_size], line, length);
	body_size += length;
	body[body_size] = '\0';
}

/* SDK已拼好完整行；结束行优先分发。隔离期的结束行只用于确认旧事务结束，不再次回调。 */
MODEM_CMD_DEFINE(on_line)
{
	const char *line = data->match_buf;
	bool active = current != NULL && !terminal_ready;

	ARG_UNUSED(argv);
	ARG_UNUSED(argc);
	data->match_buf[len] = '\0';
	log_line(line, len);
	if (fatal_error != 0) {
		return 0;
	}
	if (matches(line, len, "OK", true) ||
		matches(line, len, "ERROR", true) ||
	    matches(line, len, "+CME ERROR:", false) ||
	    matches(line, len, "+CMS ERROR:", false)) {
		if (active) {
			terminal_result = matches(line, len, "OK", true) &&
				(!current->require_response || body_size != 0) ?
				GT02_AT_SUCCESS : GT02_AT_FAILED;
			terminal_ready = true;
		} else if (state == CORE_DRAINING) {
			old_terminal_seen = true;
		}
		return 0;
	}
	if (active && matches(line, len, active_request.text, true)) {
		return 0;
	}
	if (active && current->response_prefix != NULL && current->response_prefix[0] != '\0' &&
	    matches(line, len, current->response_prefix, false)) {
		append_body(line, len);
		return 0;
	}
	for (size_t i = 0; i < registered->urc_count; ++i) {
		const struct gt02_at_urc *urc = &registered->urcs[i];

		if (matches(line, len, urc->match, urc->exact)) {
			urc->callback(line, len, urc->user_data);
			return 0;
		}
	}
	if (active && current->response_prefix != NULL && current->response_prefix[0] == '\0') {
		append_body(line, len);
	}
	return 0;
}

static const struct modem_cmd line_dispatch[] = { MODEM_CMD("", on_line, 0, "") };

/* SDK只从当前已检查的输入片段取字节，不再直接读硬件或等待新数据。 */
static int parser_read(struct modem_iface *iface, uint8_t *buffer, size_t capacity,
		       size_t *received)
{
	ARG_UNUSED(iface);
	*received = MIN(capacity, input_left);
	if (*received != 0) {
		memcpy(buffer, input, *received);
		input += *received;
		input_left -= *received;
	}
	return 0;
}

static struct modem_iface parser_iface = { .read = parser_read };

/* 喂入不超过一条行边界的小片段，SDK负责跨批次缓存及CR/LF识别。 */
static void feed_parser(const uint8_t *bytes, size_t length)
{
	if (length != 0) {
		input = bytes;
		input_left = length;
		modem_cmd_handler_process(&parser, &parser_iface);
		input = NULL;
	}
}

/* 进入SDK前限制行长，防止其截断匹配缓存或耗尽固定池；空闲超长噪声丢到行尾。 */
static void receive_bytes(const uint8_t *bytes, size_t length)
{
	size_t start = 0;

	for (size_t i = 0; i < length; ++i) {
		bool end = bytes[i] == '\r' || bytes[i] == '\n';

		if (discard_line) {
			discard_line = !end;
			start = i + 1;
			continue;
		}
		if (end) {
			feed_parser(&bytes[start], i + 1 - start);
			line_size = 0;
			start = i + 1;
			if (fatal_error != 0) {
				return;
			}
		} else if (++line_size > AT_LINE_SIZE) {
			if (current != NULL && !terminal_ready) {
				fatal_error = -EMSGSIZE;
				return;
			}
			LOG_WRN("Discarding oversized idle RX line");
			clear_rx_context();
			discard_line = true;
			start = i + 1;
		}
	}
	feed_parser(&bytes[start], length - start);
}

/* 事务先就位再发送完整命令；原始write的错误与短发送均会停止后续请求。 */
static void start_request(const struct gt02_at_command *cmd)
{
	char text[GT02_AT_COMMAND_MAX + 2];
	size_t length = strlen(active_request.text);
	size_t written = 0;
	int ret;

	current = cmd;
	body_size = 0;
	terminal_ready = false;
	deadline_ms = k_uptime_get() + cmd->timeout_ms;
	memcpy(text, active_request.text, length);
	memcpy(&text[length], "\r\n", 2);
	LOG_INF("AT TX: %s", active_request.text);
	ret = transport->write((const uint8_t *)text, length + 2, &written);
	if (ret != 0 || written != length + 2) {
		LOG_ERR("AT write failed: error=%d written=%u/%u", ret,
			(unsigned int)written, (unsigned int)(length + 2));
		fatal_error = ret != 0 ? ret : -EIO;
	}
}

/* AT线程接收并解析一批输入，返回字节数或负错误码；尚未上电和正常等待返回0。
 * channel_seen记录是否曾成功读取已启用通道，用于区分初始未上电与后续意外掉线。
 */
static int receive_input(bool *channel_seen)
{
	uint8_t bytes[64];
	size_t received = 0;
	int ret = transport->read(bytes, sizeof(bytes), &received, K_MSEC(AT_READ_SLICE_MS));

	if (ret == -EAGAIN) {
		return 0;
	}
	if (ret == -EPIPE) {
		/* 断开总是结束旧残行，但从不自行证明新会话已建立。 */
		if (state != CORE_FAULTED && *channel_seen) {
			fail_core(ret, false);
		}
		clear_rx_context();
		if (state == CORE_FAULTED) {
			session_broken = true;
			session_received = false;
		}
		k_sleep(K_MSEC(AT_READ_SLICE_MS));
		return 0;
	}
	if (ret != 0) {
		k_sleep(K_MSEC(AT_READ_SLICE_MS));
		return ret;
	}
	*channel_seen = true;
	/* read期间也可能越过期限：先通知超时，再把本批输入交给隔离路径，不能丢掉旧结束行。 */
	if (current != NULL && k_uptime_get() >= deadline_ms) {
		fail_core(-ETIMEDOUT, true);
	}
	/* read也可能跨过隔离期限；过期的最终响应不能再解除故障。 */
	if (state == CORE_DRAINING && k_uptime_get() >= drain_deadline_ms) {
		fail_core(-ETIMEDOUT, false);
	}
	if (state == CORE_FAULTED && session_broken && received != 0) {
		session_received = true;
	}
	receive_bytes(bytes, received);
	return (int)received;
}

/* AT线程统一推进调度：故障收尾、隔离期排空、正常完成及出队。
 * 没有旧结束行时保持隔离，不把静默或清缓存当成已恢复；URC和RX由接收路径继续处理。
 */
static void advance_transaction(int received)
{
	const struct gt02_at_command *cmd;

	if (received < 0) {
		fatal_error = received;
	}
	if (fatal_error != 0) {
		goto failed;
	}
	if (state == CORE_FAULTED) {
		return;
	}
	if (state == CORE_DRAINING) {
		if (old_terminal_seen && parser_at_boundary()) {
			k_spinlock_key_t key = k_spin_lock(&queue_lock);

			state = CORE_READY;
			k_spin_unlock(&queue_lock, key);
			old_terminal_seen = false;
			LOG_INF("AT old transaction ended; scheduling resumed");
		}
		return;
	}
	if (terminal_ready) {
		finish_current(terminal_result);
	}
	/* 先排空已到达的输入，不能让同批次旧OK完成下一条命令。 */
	if (current != NULL || !parser_at_boundary()) {
		return;
	}
	if (!take_request()) {
		return;
	}
	cmd = active_request.command;
	start_request(cmd);
	return;
failed:
	fail_core(fatal_error, false);
}

/* 外部只登记请求，Worker在SDK调用之外检查证据、重建上下文并回送结果。 */
static void handle_rearm(void)
{
	k_spinlock_key_t key = k_spin_lock(&queue_lock);
	bool pending = rearm_pending;

	k_spin_unlock(&queue_lock, key);
	if (!pending) {
		return;
	}
	int ret = state != CORE_FAULTED ? -EBUSY :
		!session_broken || !session_received || !parser_at_boundary() ? -EAGAIN : 0;

	if (ret == 0) {
		clear_context();
		session_broken = false;
		session_received = false;
	}
	key = k_spin_lock(&queue_lock);
	if (ret == 0) {
		state = CORE_READY;
	}
	rearm_result = ret;
	rearm_pending = false;
	k_spin_unlock(&queue_lock, key);
	if (ret == 0) {
		LOG_INF("AT new session rearmed; normal scheduling resumed");
	}
	k_sem_give(&rearm_done);
}

/* 唯一AT线程持续处理期限、RX和调度；超时后等待旧结束行，明确ERROR只结束当前请求。
 * 不自动重发命令或复位模组；真正的传输/解析故障暂停调度，保留接收路径。
 */
static void at_worker(void *p1, void *p2, void *p3)
{
	bool channel_seen = false;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	for (;;) {
		/* 1. 先结束超时或发送失败的请求，确保迟到回复不会交付给新事务。 */
		if (current != NULL && !terminal_ready && k_uptime_get() >= deadline_ms) {
			fail_core(-ETIMEDOUT, true);
		}
		if (fatal_error != 0) {
			fail_core(fatal_error, false);
		}
		if (state == CORE_DRAINING && k_uptime_get() >= drain_deadline_ms) {
			fail_core(-ETIMEDOUT, false);
		}
		handle_rearm();

		/* 2. 接收和解析；初始未上电、等待、硬件错误由接收步骤区分。 */
		int received = receive_input(&channel_seen);

		/* 3. 按正常/隔离/故障状态调度，不因一次超时永久跳过调度入口。 */
		advance_transaction(received);
	}
}

K_THREAD_DEFINE(at_thread, 3072, at_worker, NULL, NULL, NULL, 6, 0, SYS_FOREVER_MS);

/* 串行提交恢复控制并等待Worker确认，不允许回调等待自身或绕过隔离状态。 */
int gt02_at_session_rearm(void)
{
	if (k_is_in_isr()) {
		return -EWOULDBLOCK;
	}
	if (k_current_get() == at_thread) {
		return -EDEADLK;
	}
	k_mutex_lock(&rearm_lock, K_FOREVER);
	k_spinlock_key_t key = k_spin_lock(&queue_lock);
	int ret = state == CORE_UNINITIALIZED || state == CORE_STARTING ? -EACCES :
		state != CORE_FAULTED ? -EBUSY : 0;

	if (ret == 0) {
		rearm_pending = true;
	}
	k_spin_unlock(&queue_lock, key);
	if (ret == 0) {
		k_sem_take(&rearm_done, K_FOREVER);
		ret = rearm_result;
	}
	k_mutex_unlock(&rearm_lock);
	return ret;
}

/* 一次绑定协议配置和I/O实现；验证成功才启动线程，不执行任何电源操作。 */
int gt02_at_init(const struct gt02_at_config *config)
{
	if (k_is_in_isr()) {
		return -EWOULDBLOCK;
	}
	if (config == NULL || config->io == NULL ||
	    config->io->read == NULL || config->io->write == NULL) {
		return -EINVAL;
	}
	int ret = validate_profile(config->profile);

	if (ret != 0) {
		return ret;
	}
	k_spinlock_key_t key = k_spin_lock(&queue_lock);

	if (state != CORE_UNINITIALIZED) {
		k_spin_unlock(&queue_lock, key);
		return -EALREADY;
	}
	state = CORE_STARTING;
	k_spin_unlock(&queue_lock, key);
	const struct modem_cmd_handler_config parser_config = {
		.match_buf = match_buffer,
		.match_buf_len = sizeof(match_buffer),
		.buf_pool = &at_pool,
		.alloc_timeout = K_NO_WAIT,
		.eol = "\r\n",
		.response_cmds = line_dispatch,
		.response_cmds_len = ARRAY_SIZE(line_dispatch),
	};

	ret = modem_cmd_handler_init(&parser, &parser_data, &parser_config);
	key = k_spin_lock(&queue_lock);
	registered = ret == 0 ? config->profile : NULL;
	transport = ret == 0 ? config->io : NULL;
	drain_timeout_ms = config->drain_timeout_ms != 0 ? config->drain_timeout_ms : AT_DRAIN_DEFAULT_MS;
	fault_callback = config->on_fault;
	fault_user_data = config->fault_user_data;
	state = ret == 0 ? CORE_READY : CORE_UNINITIALIZED;
	k_spin_unlock(&queue_lock, key);
	if (ret == 0) {
		k_thread_start(at_thread);
	}
	return ret;
}

/* 两种提交共用校验和有界复制；只在调用期间借用text，不将调用者缓冲指针放进队列。 */
static int enqueue_request(uint16_t command_id, const char *text)
{
	if (k_is_in_isr()) {
		return -EWOULDBLOCK;
	}
	struct at_request request = {0};

	if (text != NULL) {
		size_t length = strnlen(text, GT02_AT_COMMAND_MAX + 1);

		if (length > GT02_AT_COMMAND_MAX) {
			return -EMSGSIZE;
		}
		if (length == 0) {
			return -EINVAL;
		}
		memcpy(request.text, text, length + 1);
		for (size_t i = 0; i < length; ++i) {
			if ((uint8_t)request.text[i] < 0x20 || (uint8_t)request.text[i] > 0x7e) {
				return -EINVAL;
			}
		}
	}
	k_spinlock_key_t key = k_spin_lock(&queue_lock);
	int ret = -EINVAL;

	if (state != CORE_READY) {
		ret = state == CORE_DRAINING ? -EAGAIN :
			state == CORE_FAULTED ? -EPIPE : -EACCES;
		goto out;
	}
	for (size_t i = 0; i < registered->command_count; ++i) {
		const struct gt02_at_command *cmd = &registered->commands[i];

		if (cmd->id != command_id) {
			continue;
		}
		if (cmd->reserved) {
			ret = -ENOTSUP;
		} else if (cmd->parameterized != (text != NULL)) {
			ret = -EINVAL;
		} else if (text != NULL && (strncmp(request.text, cmd->command, strlen(cmd->command)) != 0 ||
			   request.text[strlen(cmd->command)] != '=' ||
			   request.text[strlen(cmd->command) + 1] == '\0')) {
			ret = -EINVAL;
		} else if (queue_count == AT_QUEUE_SIZE) {
			ret = -ENOBUFS;
		} else {
			request.command = cmd;
			if (text == NULL) {
				memcpy(request.text, cmd->command, strlen(cmd->command) + 1);
			}
			queue[(queue_head + queue_count) % AT_QUEUE_SIZE] = request;
			++queue_count;
			ret = 0;
		}
		break;
	}
out:
	k_spin_unlock(&queue_lock, key);
	return ret;
}

/* 固定项保持原调用方式，入队后也拥有独立文本，不改变FIFO和最终回调语义。 */
int gt02_at_submit(uint16_t command_id)
{
	return enqueue_request(command_id, NULL);
}

/* 参数化项要求明确提供完整文本，NULL不允许退化成固定命令提交。 */
int gt02_at_submit_text(uint16_t command_id, const char *text)
{
	if (k_is_in_isr()) {
		return -EWOULDBLOCK;
	}
	return text == NULL ? -EINVAL : enqueue_request(command_id, text);
}
