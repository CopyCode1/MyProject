/**
 * @file
 * @brief 单线程串行执行AT请求，区分事务正文与URC，管理超时隔离及会话恢复。
 *
 * 阅读顺序：状态/资源 → 请求收尾与故障动作 → 行解析 → Worker推进 → 公共接口。
 * Worker独占事务及解析数据；提交线程只操作受锁保护的FIFO和恢复请求。
 * 不控制电源、不决定网络重试策略，SDK只负责文本缓存和完整行回调。
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

/* 核心状态表示是否允许调度，不等同于模组已启动、驻网或TCP已连接。 */
enum core_state {
	CORE_UNINITIALIZED, /* 尚未绑定配置。 */
	CORE_STARTING,      /* 初始化解析器；Worker尚未启动。 */
	CORE_READY,         /* 可接受请求；tx.command区分空闲与在途。 */
	CORE_DRAINING,      /* 当前请求已超时，等旧最终响应；拒绝新请求。 */
	CORE_FAULTED,       /* 禁止普通发送，继续解析URC，等待显式会话恢复。 */
};

/* 已接受的每个请求持有独立文本，出队后复制到唯一在途事务。 */
struct at_request {
	const struct gt02_at_command *command;
	char text[GT02_AT_COMMAND_MAX + 1];
};

/* 只有Worker修改；command在完成回调前清空，正文在回调返回后才能复用。 */
static struct {
	struct at_request request;
	const struct gt02_at_command *command;
	char body[AT_BODY_SIZE + 1];
	size_t body_size;
	int64_t deadline_ms;
	bool completed; /* 已解析最终响应，尚未在SDK外执行完成回调。 */
	enum gt02_at_result result;
} tx;

/* 接收状态独立于是否允许发送；残行只能在SDK返回后的Worker安全点清理。 */
NET_BUF_POOL_FIXED_DEFINE(at_pool, 4, 512, 0, NULL);
static struct {
	struct modem_cmd_handler handler;
	struct modem_cmd_handler_data data;
	char match_buffer[AT_LINE_SIZE + 1];
	const uint8_t *input;
	size_t input_left;
	size_t line_size;
	bool discard_line;
	bool read_succeeded_once; /* read曾返回0；覆盖Worker整个生命周期，不随残行或会话清理重置。 */
} rx;

static struct k_spinlock queue_lock; /* 保护核心状态、FIFO和恢复请求，不持锁调用用户。 */
static enum core_state state;
static struct at_request queue[AT_QUEUE_SIZE];
static size_t queue_head;
static size_t queue_count;
static const struct gt02_at_profile *registered;
static const struct gt02_at_io_ops *transport;
static gt02_at_fault_callback_t fault_callback;
static void *fault_user_data;
static uint32_t drain_timeout_ms;

/* 隔离的结束证据和期限只归Worker；不能靠清缓存或静默直接放行。 */
static bool old_terminal_seen;
static int64_t drain_deadline_ms;
static int pending_error; /* 解析/发送路径只登记错误，退出SDK后再收尾。 */

/* 故障后的本地通道观测是恢复的必要条件，不能证明物理模组已结束旧命令。 */
enum session_progress {
	SESSION_UNCONFIRMED,  /* 未观察到故障后的通道断开。 */
	SESSION_DISCONNECTED, /* 已观察到断开，尚无随后接收的数据。 */
	SESSION_RECEIVING,    /* 已收到断开后的数据；仍须检查残行及调用方的会话确认。 */
};
static enum session_progress session_progress;
K_MUTEX_DEFINE(rearm_lock); /* 串行化外部恢复调用。 */
K_SEM_DEFINE(rearm_done, 0, 1);
static bool rearm_pending; /* queue_lock保护；实际恢复仍由Worker完成。 */
static int rearm_result;

/* 仅在SDK解析返回后的Worker安全点释放残行；保留协议表、固定池和解析器锁。
 * read_succeeded_once是历史观测，不属于待清理的残行状态。
 */
static void clear_rx_context(void)
{
	if (rx.data.rx_buf != NULL) {
		net_buf_unref(rx.data.rx_buf);
		rx.data.rx_buf = NULL;
	}
	rx.data.last_error = 0;
	rx.match_buffer[0] = '\0';
	rx.line_size = 0;
	rx.discard_line = false;
	rx.input = NULL;
	rx.input_left = 0;
}

/* 请求已全部通知后重建本地解析上下文；清缓存本身不改变调度状态。 */
static void clear_context(void)
{
	tx.command = NULL;
	tx.body_size = 0;
	tx.completed = false;
	old_terminal_seen = false;
	pending_error = 0;
	tx.deadline_ms = 0;
	drain_deadline_ms = 0;
	clear_rx_context();
}

/* 完整批次已处理且SDK没有残留字节时，才允许调度或重建会话。 */
static bool parser_at_boundary(void)
{
	return rx.line_size == 0 && !rx.discard_line && rx.input_left == 0 &&
		(rx.data.rx_buf == NULL || net_buf_frags_len(rx.data.rx_buf) == 0);
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
		tx.request = queue[queue_head];
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
	const struct gt02_at_command *cmd = tx.command;

	tx.command = NULL;
	tx.completed = false;
	LOG_INF("AT done: id=%u result=%d", cmd->id, result);
	cmd->callback(result, result == GT02_AT_SUCCESS && tx.body_size != 0 ? tx.body : NULL,
		      result == GT02_AT_SUCCESS ? tx.body_size : 0, cmd->user_data);
	tx.body_size = 0;
}

/* 调度已封闭后结束所有已接受请求；当前请求按原因完成，待发请求统一FAILED。 */
static void cancel_requests(enum gt02_at_result current_result)
{
	if (tx.command != NULL) {
		finish_current(current_result);
	}
	while (take_request()) {
		const struct gt02_at_command *cmd = tx.request.command;

		cmd->callback(GT02_AT_FAILED, NULL, 0, cmd->user_data);
	}
	pending_error = 0;
}

/* 命令超时：先封闭提交并通知请求，再保留旧解析上下文等待最终响应。 */
static void begin_isolation(void)
{
	k_spinlock_key_t key = k_spin_lock(&queue_lock);

	state = CORE_DRAINING;
	k_spin_unlock(&queue_lock, key);
	old_terminal_seen = false;
	LOG_WRN("AT requests ended: error=%d; queued requests cancelled", -ETIMEDOUT);
	drain_deadline_ms = k_uptime_get() + drain_timeout_ms;
	LOG_WRN("AT timeout: waiting for old final result; RX/URC remain active");
	cancel_requests(GT02_AT_TIMEOUT);
}

/* 流失效或隔离到期：收尾并清解析数据，通知一次故障；重复错误只清接收残留。 */
static void enter_fault(int error)
{
	k_spinlock_key_t key = k_spin_lock(&queue_lock);
	bool already_faulted = state == CORE_FAULTED;

	state = CORE_FAULTED;
	k_spin_unlock(&queue_lock, key);
	if (already_faulted) {
		clear_rx_context();
		if (session_progress == SESSION_RECEIVING) {
			session_progress = SESSION_DISCONNECTED;
		}
		pending_error = 0;
		return;
	}
	old_terminal_seen = false;
	LOG_WRN("AT requests ended: error=%d; queued requests cancelled", error);
	LOG_ERR("AT transport/parser fault; normal scheduling suspended");
	cancel_requests(GT02_AT_FAILED);
	clear_context();
	session_progress = SESSION_UNCONFIRMED;
	if (fault_callback != NULL) {
		fault_callback(error, fault_user_data);
	}
}

/* 在唯一共享缓存中追加一行正文；超限使流失效，不交付截断结果。 */
static void append_body(const char *line, size_t length)
{
	size_t separator = tx.body_size != 0 ? 1 : 0;

	if (length + separator > AT_BODY_SIZE - tx.body_size) {
		pending_error = -EMSGSIZE;
		return;
	}
	if (separator != 0) {
		tx.body[tx.body_size++] = '\n';
	}
	memcpy(&tx.body[tx.body_size], line, length);
	tx.body_size += length;
	tx.body[tx.body_size] = '\0';
}

/* SDK已拼好完整行；结束行优先分发。隔离期的结束行只用于确认旧事务结束，不再次回调。 */
MODEM_CMD_DEFINE(on_line)
{
	const char *line = data->match_buf;
	bool active = tx.command != NULL && !tx.completed;

	ARG_UNUSED(argv);
	ARG_UNUSED(argc);
	data->match_buf[len] = '\0';
	log_line(line, len);
	if (pending_error != 0) {
		return 0;
	}
	if (matches(line, len, "OK", true) ||
	    matches(line, len, "ERROR", true) ||
	    matches(line, len, "+CME ERROR:", false) ||
	    matches(line, len, "+CMS ERROR:", false)) {
		if (active) {
			tx.result = matches(line, len, "OK", true) &&
				(!tx.command->require_response || tx.body_size != 0) ?
				GT02_AT_SUCCESS : GT02_AT_FAILED;
			tx.completed = true;
		} else if (state == CORE_DRAINING) {
			old_terminal_seen = true;
		}
		return 0;
	}
	if (active && matches(line, len, tx.request.text, true)) {
		return 0;
	}
	if (active && tx.command->response_prefix != NULL && tx.command->response_prefix[0] != '\0' &&
	    matches(line, len, tx.command->response_prefix, false)) {
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
	if (active && tx.command->response_prefix != NULL && tx.command->response_prefix[0] == '\0') {
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
	*received = MIN(capacity, rx.input_left);
	if (*received != 0) {
		memcpy(buffer, rx.input, *received);
		rx.input += *received;
		rx.input_left -= *received;
	}
	return 0;
}

static struct modem_iface parser_iface = { .read = parser_read };

/* 喂入不超过一条行边界的小片段，SDK负责跨批次缓存及CR/LF识别。 */
static void feed_parser(const uint8_t *bytes, size_t length)
{
	if (length != 0) {
		rx.input = bytes;
		rx.input_left = length;
		modem_cmd_handler_process(&rx.handler, &parser_iface);
		rx.input = NULL;
	}
}

/* 进入SDK前限制行长，防止其截断匹配缓存或耗尽固定池；空闲超长噪声丢到行尾。 */
static void receive_bytes(const uint8_t *bytes, size_t length)
{
	size_t start = 0;

	for (size_t i = 0; i < length; ++i) {
		bool end = bytes[i] == '\r' || bytes[i] == '\n';

		if (rx.discard_line) {
			rx.discard_line = !end;
			start = i + 1;
			continue;
		}
		if (end) {
			feed_parser(&bytes[start], i + 1 - start);
			rx.line_size = 0;
			start = i + 1;
			if (pending_error != 0) {
				return;
			}
		} else if (++rx.line_size > AT_LINE_SIZE) {
			if (tx.command != NULL && !tx.completed) {
				pending_error = -EMSGSIZE;
				return;
			}
			LOG_WRN("Discarding oversized idle RX line");
			clear_rx_context();
			rx.discard_line = true;
			start = i + 1;
		}
	}
	feed_parser(&bytes[start], length - start);
}

/* 事务先就位再发送完整命令；原始write的错误与短发送均会停止后续请求。 */
static void start_request(void)
{
	const struct gt02_at_command *cmd = tx.request.command;
	char text[GT02_AT_COMMAND_MAX + 2];
	size_t length = strlen(tx.request.text);
	size_t written = 0;
	int ret;

	tx.command = cmd;
	tx.body_size = 0;
	tx.completed = false;
	tx.deadline_ms = k_uptime_get() + cmd->timeout_ms;
	memcpy(text, tx.request.text, length);
	memcpy(&text[length], "\r\n", 2);
	LOG_INF("AT TX: %s", tx.request.text);
	ret = transport->write((const uint8_t *)text, length + 2, &written);
	if (ret != 0 || written != length + 2) {
		LOG_ERR("AT write failed: error=%d written=%u/%u", ret,
			(unsigned int)written, (unsigned int)(length + 2));
		pending_error = ret != 0 ? ret : -EIO;
	}
}

/* AT线程接收并解析一批输入；长度留在本函数，错误登记到pending_error后由推进步骤收尾。
 * 首次成功读取前容忍通道未就绪，曾成功读取后再断开则进入故障。
 */
static void receive_input(void)
{
	uint8_t bytes[64];
	size_t received = 0;
	int ret = transport->read(bytes, sizeof(bytes), &received, K_MSEC(AT_READ_SLICE_MS));

	if (ret == -EAGAIN) {
		return;
	}
	if (ret == -EPIPE) {
		/* 断开总是结束旧残行，但从不自行证明新会话已建立。 */
		if (state != CORE_FAULTED && rx.read_succeeded_once) {
			enter_fault(ret);
		}
		clear_rx_context();
		if (state == CORE_FAULTED) {
			session_progress = SESSION_DISCONNECTED;
		}
		k_sleep(K_MSEC(AT_READ_SLICE_MS));
		return;
	}
	if (ret != 0) {
		k_sleep(K_MSEC(AT_READ_SLICE_MS));
		if (ret < 0) {
			pending_error = ret;
		}
		return;
	}
	rx.read_succeeded_once = true;
	/* read期间也可能越过期限：先通知超时，再把本批输入交给隔离路径，不能丢掉旧结束行。 */
	if (tx.command != NULL && k_uptime_get() >= tx.deadline_ms) {
		begin_isolation();
	}
	/* read也可能跨过隔离期限；过期的最终响应不能再解除故障。 */
	if (state == CORE_DRAINING && k_uptime_get() >= drain_deadline_ms) {
		enter_fault(-ETIMEDOUT);
	}
	if (state == CORE_FAULTED && session_progress != SESSION_UNCONFIRMED && received != 0) {
		session_progress = SESSION_RECEIVING;
	}
	receive_bytes(bytes, received);
}

/* 批次处理完后按当前状态解释结果：错误优先，其次隔离结束，最后正常完成/出队。
 * 行解析只登记结果；此处在SDK之外改变调度状态和调用事务完成回调。
 */
static void advance_transaction(void)
{
	if (pending_error != 0) {
		enter_fault(pending_error);
		return;
	}
	switch (state) {
	case CORE_FAULTED:
		/* receive_input已经继续分发URC，此处只禁止普通发送。 */
		return;
	case CORE_DRAINING:
		if (old_terminal_seen && parser_at_boundary()) {
			k_spinlock_key_t key = k_spin_lock(&queue_lock);

			state = CORE_READY;
			k_spin_unlock(&queue_lock, key);
			old_terminal_seen = false;
			LOG_INF("AT old transaction ended; scheduling resumed");
		}
		return;
	case CORE_READY:
		if (tx.completed) {
			finish_current(tx.result);
		}
		/* 必须处理完本批次且没有残行，不要求UART完全空闲。 */
		if (tx.command == NULL && parser_at_boundary() && take_request()) {
			start_request();
		}
		return;
	case CORE_UNINITIALIZED:
	case CORE_STARTING:
		/* 初始化完成后才启动Worker，这两个状态不进入运行调度。 */
		return;
	}
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
	int ret = 0;

	if (state != CORE_FAULTED) {
		ret = -EBUSY;
	} else if (session_progress != SESSION_RECEIVING || !parser_at_boundary()) {
		ret = -EAGAIN;
	}

	if (ret == 0) {
		clear_context();
		session_progress = SESSION_UNCONFIRMED;
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
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	for (;;) {
		/* 1. 先结束超时或发送失败的请求，确保迟到回复不会交付给新事务。 */
		if (tx.command != NULL && !tx.completed && k_uptime_get() >= tx.deadline_ms) {
			begin_isolation();
		}
		if (pending_error != 0) {
			enter_fault(pending_error);
		}
		if (state == CORE_DRAINING && k_uptime_get() >= drain_deadline_ms) {
			enter_fault(-ETIMEDOUT);
		}
		handle_rearm();

		/* 2. 接收和解析；初始未上电、等待、硬件错误由接收步骤区分。 */
		receive_input();

		/* 3. 按正常/隔离/故障状态调度，不因一次超时永久跳过调度入口。 */
		advance_transaction();
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
		.match_buf = rx.match_buffer,
		.match_buf_len = sizeof(rx.match_buffer),
		.buf_pool = &at_pool,
		.alloc_timeout = K_NO_WAIT,
		.eol = "\r\n",
		.response_cmds = line_dispatch,
		.response_cmds_len = ARRAY_SIZE(line_dispatch),
	};

	ret = modem_cmd_handler_init(&rx.handler, &rx.data, &parser_config);
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
