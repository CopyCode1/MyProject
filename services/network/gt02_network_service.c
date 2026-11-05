/** @file
 * @brief 周期检测与一次会话恢复；主流程按故障、事件、期限的顺序推进。
 */
#include <errno.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include "services/network/gt02_network_service.h"

LOG_MODULE_REGISTER(gt02_network, LOG_LEVEL_INF);

static const enum gt02_modem_query check_plan[] = {
	GT02_MODEM_QUERY_SIM, GT02_MODEM_QUERY_SIGNAL,
	GT02_MODEM_QUERY_LTE_REGISTRATION, GT02_MODEM_QUERY_DATA_CONTEXT,
};

/* 内部阶段与公开结论分开；step表示本轮唯一在途查询。 */
enum monitor_phase { WAIT_STARTUP, WAIT_REARM, WAIT_PREPARE, RETRY_PREPARE, CHECKING, IDLE, HALTED };
struct monitor {
	enum monitor_phase phase;
	size_t step;
	int64_t deadline_ms;
	bool recovery_used; /* 完整四项查询成功后才重新获得一次恢复机会。 */
	bool restarting; /* 重启后的RDY必须先经过rearm。 */
	struct gt02_network_status result;
};

K_MSGQ_DEFINE(modem_events, sizeof(struct gt02_modem_event), 4, 4);
K_MUTEX_DEFINE(status_lock);
static struct gt02_network_config config;
static struct gt02_network_status status = { .signal_rssi = 99 };
static atomic_t event_overflow;
static atomic_t pending_at_fault; /* 独立于队列容量，保留首个待处理故障。 */
static bool started;

/* 厂家线程只复制事件，早到RDY在power_on尚未返回时也能保留。 */
static void on_modem_event(const struct gt02_modem_event *event, void *user_data)
{
	ARG_UNUSED(user_data);
	if (k_msgq_put(&modem_events, event, K_NO_WAIT) != 0) {
		atomic_set(&event_overflow, 1);
	}
}

/* 锁存故障再尝试唤醒；队列已满时已有事件可唤醒服务，不丢失错误原因。 */
void gt02_network_service_notify_at_fault(int error)
{
	if (error < 0) {
		const struct gt02_modem_event wake = {0};

		(void)atomic_cas(&pending_at_fault, 0, error);
		(void)k_msgq_put(&modem_events, &wake, K_NO_WAIT);
	}
}

/* 原子发布整轮快照，读取者不会看到半轮结果。 */
static void publish_status(const struct gt02_network_status *result)
{
	k_mutex_lock(&status_lock, K_FOREVER);
	status = *result;
	k_mutex_unlock(&status_lock);
	LOG_INF("4G status: state=%d error=%d query=%d SIM=%d RSSI=%u reg=%u context=%d IP=%u.%u.%u.%u",
		result->state, result->error, result->query, result->sim, result->signal_rssi,
		result->registration, result->data_context.active,
		result->data_context.ipv4[0], result->data_context.ipv4[1],
		result->data_context.ipv4[2], result->data_context.ipv4[3]);
}

/* 整轮结束才发布；失败不混用上次数据，未就绪继续间隔检测。 */
static void finish_check(struct monitor *monitor, int error)
{
	struct gt02_network_status result = error == 0 ? monitor->result :
		(struct gt02_network_status){ .signal_rssi = 99 };
	uint8_t first = result.data_context.ipv4[0];

	result.state = GT02_NETWORK_NOT_READY;
	result.error = error;
	result.query = check_plan[monitor->step];
	result.checked_at_ms = k_uptime_get();
	/* 保留已有IPv4放行规则，不把未知信号或弱信号直接当成网络故障。 */
	if (error == 0 && result.sim == GT02_MODEM_SIM_READY &&
	    (result.registration == 1 || result.registration == 5) &&
	    result.data_context.active && first != 0 && first != 127 && first < 224) {
		result.state = GT02_NETWORK_DATA_READY;
	}
	publish_status(&result);
	monitor->phase = IDLE;
	monitor->deadline_ms = result.checked_at_ms + (result.state == GT02_NETWORK_DATA_READY ?
		config.check_interval_ms : config.retry_interval_ms);
}

/* 先登记在途步骤再提交；隔离期拒绝不产生完成事件，直接安排下轮。 */
static int submit_query(struct monitor *monitor)
{
	monitor->phase = CHECKING;
	monitor->deadline_ms = k_uptime_get() + config.operation_timeout_ms;
	int ret = config.modem->query(check_plan[monitor->step]);

	if (ret == -EAGAIN) {
		finish_check(monitor, ret);
		return 0;
	}
	return ret;
}

/* 新一轮只清空私有临时结果，保留仍在有效期内的公开结论。 */
static int begin_check(struct monitor *monitor)
{
	monitor->result = (struct gt02_network_status){ .signal_rssi = 99 };
	monitor->step = 0;
	return submit_query(monitor);
}

/* 本步完成校验才保存数据；完整四项成功表示通道已重新稳定，恢复机会可以重置。 */
static int advance_check(struct monitor *monitor, const struct gt02_modem_event *event)
{
	struct gt02_network_status result = monitor->result;
	int error = event->error;

	if (error == -EPIPE) {
		return error;
	}
	if (error == 0) {
		switch (event->query) {
		case GT02_MODEM_QUERY_SIM:
			if ((unsigned int)event->sim > GT02_MODEM_SIM_PUK_REQUIRED) {
				error = -EBADMSG;
				break;
			}
			result.sim = event->sim;
			break;
		case GT02_MODEM_QUERY_SIGNAL:
			if (event->signal_rssi > 31 && event->signal_rssi != 99) {
				error = -EBADMSG;
				break;
			}
			result.signal_rssi = event->signal_rssi;
			break;
		case GT02_MODEM_QUERY_LTE_REGISTRATION:
			if (event->registration > 5) {
				error = -EBADMSG;
				break;
			}
			result.registration = event->registration;
			break;
		case GT02_MODEM_QUERY_DATA_CONTEXT:
			result.data_context = event->data_context;
			break;
		default:
			return -EBADMSG;
		}
	}
	if (error != 0) {
		finish_check(monitor, error);
		return 0;
	}
	monitor->result = result;
	if (monitor->step + 1 == ARRAY_SIZE(check_plan)) {
		monitor->recovery_used = false;
	}
	if (result.sim != GT02_MODEM_SIM_READY ||
	    (event->query == GT02_MODEM_QUERY_LTE_REGISTRATION &&
	     result.registration != 1 && result.registration != 5) ||
	    monitor->step + 1 == ARRAY_SIZE(check_plan)) {
		finish_check(monitor, 0);
		return 0;
	}
	monitor->step++;
	return submit_query(monitor);
}

/* 准备阶段遇到AT隔离也只安排重试，不跳过握手直接查询。 */
static int request_prepare(struct monitor *monitor)
{
	monitor->phase = WAIT_PREPARE;
	monitor->deadline_ms = k_uptime_get() + config.operation_timeout_ms;
	int ret = config.modem->prepare();

	if (ret == -EAGAIN) {
		monitor->phase = RETRY_PREPARE;
		monitor->deadline_ms = k_uptime_get() + config.retry_interval_ms;
		return 0;
	}
	return ret;
}

/* 仅服务线程请求恢复；边界尚不完整时稍后重试，保留原有启动总期限。 */
static int try_rearm(struct monitor *monitor)
{
	int ret = config.session_rearm();

	if (ret == -EAGAIN) {
		return 0;
	}
	if (ret != 0) {
		return ret;
	}
	monitor->restarting = false;
	return request_prepare(monitor);
}

/* 事件只推进对应阶段；重复RDY不重复握手，故障停止阶段不复用迟到结果。 */
static int handle_modem_event(struct monitor *monitor, const struct gt02_modem_event *event)
{
	if (monitor->phase == HALTED) {
		return 0;
	}
	if (event->type == GT02_MODEM_EVENT_STARTUP_READY) {
		if (monitor->phase != WAIT_STARTUP) {
			return 0;
		}
		if (event->error != 0) {
			return event->error;
		}
		LOG_INF("4G startup ready: %s", monitor->restarting ? "rearm session" : "prepare AT");
		if (monitor->restarting) {
			monitor->phase = WAIT_REARM;
			return try_rearm(monitor);
		}
		return request_prepare(monitor);
	}
	if (monitor->phase == WAIT_PREPARE && event->type == GT02_MODEM_EVENT_PREPARE_DONE) {
		if (event->error == -ETIMEDOUT) {
			monitor->phase = RETRY_PREPARE;
			monitor->deadline_ms = k_uptime_get() + config.retry_interval_ms;
			return 0;
		}
		return event->error == 0 ? begin_check(monitor) : event->error;
	}
	if (monitor->phase == CHECKING && event->type == GT02_MODEM_EVENT_QUERY_DONE &&
	    event->query == check_plan[monitor->step]) {
		return advance_check(monitor, event);
	}
	return monitor->phase == IDLE || monitor->phase == RETRY_PREPARE ||
		monitor->phase == WAIT_REARM ? 0 : -EBADMSG;
}

/* 空闲到期开始新一轮，准备重试到期再提交；其他阶段到期不得假装取消了旧请求。 */
static int handle_deadline(struct monitor *monitor)
{
	if (monitor->phase == IDLE) {
		return begin_check(monitor);
	}
	return monitor->phase == RETRY_PREPARE ? request_prepare(monitor) : -ETIMEDOUT;
}

/* 停止普通推进并发布故障，但保留服务线程接收独立AT故障通知。 */
static void halt_monitor(struct monitor *monitor, int error)
{
	const struct gt02_network_status result = {
		.state = GT02_NETWORK_FAULT, .error = error, .signal_rssi = 99,
		.query = check_plan[monitor->step], .checked_at_ms = k_uptime_get(),
	};

	monitor->phase = HALTED;
	publish_status(&result);
}

/* 仅AT故障消费一次恢复机会；切电成功后清旧事件，先登记等待再启动供电。 */
static int restart_modem(struct monitor *monitor, int error)
{
	halt_monitor(monitor, error);
	if (monitor->recovery_used || config.power->power_off == NULL || config.session_rearm == NULL) {
		return -ENOTSUP;
	}
	monitor->recovery_used = true;
	int ret = config.power->power_off();

	if (ret != 0) {
		return ret;
	}
	/* 故障通知前AT已完成请求取消，旧完成事件不交给新一轮。 */
	k_msgq_purge(&modem_events);
	atomic_clear(&event_overflow);
	monitor->step = 0;
	monitor->restarting = true;
	monitor->phase = WAIT_STARTUP;
	LOG_WRN("4G recovery: one power cycle, waiting for new RDY");
	ret = config.power->power_on();
	monitor->deadline_ms = k_uptime_get() + config.startup_timeout_ms;
	return ret;
}

/* 主流程优先处理故障锁存，再推进事件或期限；没有新增恢复线程。 */
static void service_worker(void *p1, void *p2, void *p3)
{
	struct monitor monitor = { .phase = WAIT_STARTUP };
	const struct gt02_network_status initial = { .state = GT02_NETWORK_STARTING, .signal_rssi = 99 };
	struct gt02_modem_event event;
	int ret;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	publish_status(&initial);
	ret = config.power->power_on();
	monitor.deadline_ms = k_uptime_get() + config.startup_timeout_ms;
	if (ret != 0) {
		halt_monitor(&monitor, ret);
	}
	for (;;) {
		int fault = (int)atomic_set(&pending_at_fault, 0);

		if (fault != 0) {
			ret = restart_modem(&monitor, fault);
			if (ret != 0) {
				halt_monitor(&monitor, ret == -ENOTSUP ? fault : ret);
			}
			continue;
		}
		if (atomic_set(&event_overflow, 0) != 0) {
			halt_monitor(&monitor, -ENOBUFS);
		}
		int64_t remaining_ms = monitor.deadline_ms - k_uptime_get();

		ret = 0;
		if (monitor.phase != HALTED && remaining_ms <= 0) {
			ret = handle_deadline(&monitor);
		} else if (monitor.phase == WAIT_REARM) {
			ret = try_rearm(&monitor);
			if (ret == 0 && monitor.phase == WAIT_REARM) {
				/* 残行结束后无需新RDY即可重试，等待总预算不重新开始。 */
				(void)k_msgq_get(&modem_events, &event, K_MSEC(MIN(remaining_ms, 20)));
			}
		} else if (k_msgq_get(&modem_events, &event,
				     monitor.phase == HALTED ? K_FOREVER : K_MSEC(remaining_ms)) == 0) {
			if (atomic_get(&pending_at_fault) != 0) {
				continue;
			}
			ret = monitor.phase != HALTED && k_uptime_get() >= monitor.deadline_ms ?
				handle_deadline(&monitor) : handle_modem_event(&monitor, &event);
		}
		if (ret != 0) {
			halt_monitor(&monitor, ret);
		}
	}
}

K_THREAD_DEFINE(network_thread, 2048, service_worker, NULL, NULL, NULL, 7, 0, SYS_FOREVER_MS);

/* 完整验证配置后才绑定接收者并启动，操作表和回调须覆盖开机生命周期。 */
int gt02_network_service_start(const struct gt02_network_config *provided)
{
	if (k_is_in_isr()) {
		return -EWOULDBLOCK;
	}
	if (provided == NULL || provided->power == NULL || provided->power->power_on == NULL ||
	    (provided->session_rearm != NULL && provided->power->power_off == NULL) ||
	    provided->modem == NULL || provided->modem->init == NULL ||
	    provided->modem->prepare == NULL || provided->modem->query == NULL ||
	    provided->startup_timeout_ms == 0 || provided->operation_timeout_ms == 0 ||
	    provided->retry_interval_ms == 0 || provided->check_interval_ms == 0 ||
	    provided->result_valid_ms <= provided->check_interval_ms) {
		return -EINVAL;
	}
	k_mutex_lock(&status_lock, K_FOREVER);
	int ret = started ? -EALREADY : provided->modem->init(on_modem_event, NULL);

	if (ret == 0) {
		config = *provided;
		started = true;
	}
	k_mutex_unlock(&status_lock);
	if (ret == 0) {
		k_thread_start(network_thread);
	}
	return ret;
}

/* 复制一致快照再检查时效；过期只撤销本次快照的放行结论，不修改共享状态。 */
int gt02_network_service_get_status(struct gt02_network_status *out)
{
	if (k_is_in_isr()) {
		return -EWOULDBLOCK;
	}
	if (out == NULL) {
		return -EINVAL;
	}
	k_mutex_lock(&status_lock, K_FOREVER);
	struct gt02_network_status snapshot = status;

	if (snapshot.state == GT02_NETWORK_DATA_READY &&
	    k_uptime_get() - snapshot.checked_at_ms >= config.result_valid_ms) {
		snapshot.state = GT02_NETWORK_NOT_READY;
		snapshot.error = -ESTALE;
	}
	k_mutex_unlock(&status_lock);
	*out = snapshot;
	return 0;
}
