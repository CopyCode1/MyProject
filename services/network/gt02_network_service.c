/** @file
 * @brief 单线程协调4G启动、网络监测及受控恢复；AT结果按事件逐步推进。
 *
 * 服务线程独占业务状态，厂家回调只复制事件。上下电及会话重建交给专用工作队列，
 * 不在业务线程等待硬件时序；工作项只执行操作，完成后由服务线程决定下一步。
 */
#include <errno.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include "services/network/gt02_network_service.h"

LOG_MODULE_REGISTER(gt02_network, LOG_LEVEL_INF);

#define SESSION_RETRY_MS 20

/** 模组生命周期阶段，由服务线程维护；各阶段可包含待提交、等待完成及重试过程。 */
enum modem_phase {
	MODEM_POWERING_ON,  /**< 准备提交上电或等待上电返回；早到RDY先记录，不提前准备AT。 */
	MODEM_WAIT_READY,   /**< 上电已成功，等待RDY；受启动截止期限约束。 */
	MODEM_REARMING,     /**< 恢复启动已收到RDY，等待或重试AT会话重建；沿用启动期限。 */
	MODEM_PREPARING,    /**< 提交、等待或重试基础AT准备，已接受操作的超时由下层负责。 */
	MODEM_RUNNING,      /**< AT准备完成，允许推进网络查询；不代表数据网络已就绪。 */
	MODEM_POWERING_OFF, /**< 故障恢复已获准，准备提交切电或等待切电返回。 */
	MODEM_HALTED,       /**< 停止普通业务推进，仍接收故障及迟到完成通知。 */
};

/** 服务记录的在途AT操作类型，用于匹配完成事件；具体查询种类另存于network.query。 */
enum at_operation {
	AT_OPERATION_NONE,    /**< 当前未登记在途AT操作，可按模组阶段选择下一项。 */
	AT_OPERATION_PREPARE, /**< 基础AT准备操作，匹配PREPARE_DONE完成事件。 */
	AT_OPERATION_QUERY,   /**< 单项网络查询，匹配QUERY_DONE及其query字段。 */
	AT_OPERATION_ACTIVATE, /**< 注册成功后的PDP激活，匹配ACTIVATE_DONE；成功后再查询IP。 */
};

/** 同步底层控制操作的作业类型；专用工作队列执行，服务线程接收结果并决定下一步。 */
enum control_operation {
	CONTROL_NONE,      /**< 服务当前没有等待完成处理的控制作业，不提交此值执行。 */
	CONTROL_POWER_ON,  /**< 执行power_on完成硬件上电时序；RDY由服务另行处理。 */
	CONTROL_POWER_OFF, /**< 执行power_off切电并收尾字节通道，为恢复启动建立硬件边界。 */
	CONTROL_REARM,     /**< 调用session_rearm请求AT层重建软件会话，不执行硬件上下电。 */
};

/* 本轮临时结果与公开快照分开；query表示下一项查询，激活成功记录跨轮次保留。 */
struct network_monitor {
	bool active;
	bool pdp_activation_succeeded; /* 本次上电曾激活成功；当前是否仍激活由查询结果active表达。 */
	enum gt02_modem_query query;
	int64_t next_check_at_ms;
	struct gt02_network_status result;
};

/* 仅服务线程修改；pending_control在故障时也必须等实际操作返回后才能复用。 */
struct service_context {
	enum modem_phase phase;
	enum at_operation pending_at;
	enum control_operation pending_control;
	bool recovery_used;
	bool restore_session;
	bool ready_seen;
	int64_t startup_deadline_ms;
	int64_t next_action_at_ms;
	struct network_monitor network;
};

K_MSGQ_DEFINE(modem_events, sizeof(struct gt02_modem_event), 4, 4);
K_SEM_DEFINE(service_wake, 0, 1);
K_MUTEX_DEFINE(status_lock);
static struct gt02_network_config config;
static struct gt02_network_status status = { .signal_rssi = 99 };
static atomic_t event_overflow;
static atomic_t pending_at_fault;
static bool started;

/* 单槽控制作业：提交后字段归工作线程，done发布后归服务线程；不经过普通事件队列。 */
static struct {
	struct k_work work;
	enum control_operation operation;
	int error;
	int64_t completed_at_ms;
	atomic_t done;
} control_job;
static struct k_work_q control_queue;
K_THREAD_STACK_DEFINE(control_stack, 2048);

/* 厂家/AT线程只复制事件并唤醒；早到RDY在power_on尚未返回时也能保留。 */
static void on_modem_event(const struct gt02_modem_event *event, void *user_data)
{
	ARG_UNUSED(user_data);
	if (k_msgq_put(&modem_events, event, K_NO_WAIT) != 0) {
		atomic_set(&event_overflow, 1);
	}
	k_sem_give(&service_wake);
}

/* 独立锁存首个故障并唤醒，不伪造PREPARE_DONE，也不依赖队列剩余容量。 */
void gt02_network_service_notify_at_fault(int error)
{
	if (error < 0) {
		(void)atomic_cas(&pending_at_fault, 0, error);
		k_sem_give(&service_wake);
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

/* 专用工作线程只执行一次同步底层操作，不读写业务状态，不自行重试或恢复。 */
static void run_control_operation(struct k_work *work)
{
	ARG_UNUSED(work);
	int error;

	switch (control_job.operation) {
	case CONTROL_POWER_ON:
		error = config.power->power_on();
		break;
	case CONTROL_POWER_OFF:
		error = config.power->power_off();
		break;
	case CONTROL_REARM:
		error = config.session_rearm();
		break;
	default:
		error = -EINVAL;
		break;
	}
	control_job.error = error;
	control_job.completed_at_ms = k_uptime_get();
	atomic_set(&control_job.done, 1);
	k_sem_give(&service_wake);
}

/* 撤销普通业务并发布故障；只有独立AT故障和剩余额度允许安排一次切电。
 * 不取消正在执行的同步硬件操作，后续切电必须等它返回，避免电源操作交错。
 */
static void enter_fault(struct service_context *service, int error, bool at_fault)
{
	const struct gt02_network_status result = {
		.state = GT02_NETWORK_FAULT, .error = error, .signal_rssi = 99,
		.query = service->network.query, .checked_at_ms = k_uptime_get(),
	};

	service->pending_at = AT_OPERATION_NONE;
	service->network.active = false;
	service->phase = MODEM_HALTED;
	publish_status(&result);
	if (at_fault && !service->recovery_used &&
	    config.power->power_off != NULL && config.session_rearm != NULL) {
		service->recovery_used = true;
		service->phase = MODEM_POWERING_OFF;
		LOG_WRN("4G recovery: one power cycle scheduled");
	}
}

/* AT故障优先于溢出；两者都由业务线程消费，回调不参与恢复决策。 */
static void process_pending_fault(struct service_context *service)
{
	int fault = (int)atomic_set(&pending_at_fault, 0);
	bool overflow = atomic_set(&event_overflow, 0) != 0;

	if (fault != 0 || overflow) {
		enter_fault(service, fault != 0 ? fault : -ENOBUFS, fault != 0);
	}
}

/* 每次只交付一个控制作业；上一项完成被消费后才能复用作业槽。 */
static void submit_control(struct service_context *service, enum control_operation operation)
{
	service->pending_control = operation;
	control_job.operation = operation;
	int ret = k_work_submit_to_queue(&control_queue, &control_job.work);

	if (ret < 0) {
		service->pending_control = CONTROL_NONE;
		enter_fault(service, ret, false);
	}
}

/* RDY与上电完成都到齐后才进入会话重建或AT准备，准备不沿用RDY期限。 */
static void advance_after_ready(struct service_context *service)
{
	service->phase = service->restore_session ? MODEM_REARMING : MODEM_PREPARING;
	service->next_action_at_ms = k_uptime_get();
	LOG_INF("4G startup ready: %s", service->restore_session ? "rearm session" : "prepare AT");
}

/* 接收硬件操作的独立完成槽；故障后迟到的结果只释放槽，不重启已失效的流程。 */
static void process_control_completion(struct service_context *service)
{
	if (atomic_set(&control_job.done, 0) == 0) {
		return;
	}
	enum control_operation operation = service->pending_control;
	int error = control_job.error;

	service->pending_control = CONTROL_NONE;
	if ((operation == CONTROL_POWER_ON && service->phase != MODEM_POWERING_ON) ||
	    (operation == CONTROL_POWER_OFF && service->phase != MODEM_POWERING_OFF) ||
	    (operation == CONTROL_REARM && service->phase != MODEM_REARMING)) {
		return;
	}
	if (operation == CONTROL_REARM &&
	    k_uptime_get() >= service->startup_deadline_ms) {
		enter_fault(service, -ETIMEDOUT, false);
		return;
	}
	if (operation == CONTROL_REARM && error == -EAGAIN) {
		service->next_action_at_ms = k_uptime_get() + SESSION_RETRY_MS;
		return;
	}
	if (error != 0) {
		enter_fault(service, error, false);
		return;
	}

	switch (operation) {
	case CONTROL_POWER_ON:
		service->network.pdp_activation_succeeded = false;
		service->startup_deadline_ms = control_job.completed_at_ms + config.startup_timeout_ms;
		service->phase = MODEM_WAIT_READY;
		if (service->ready_seen) {
			advance_after_ready(service);
		}
		break;
	case CONTROL_POWER_OFF:
		/* 当前AT后端先结束旧请求再报告故障；切电后清已排队通知，新上电前不再清队列。 */
		k_msgq_purge(&modem_events);
		atomic_clear(&event_overflow);
		service->network.query = GT02_MODEM_QUERY_SIM;
		service->restore_session = true;
		service->ready_seen = false;
		service->phase = MODEM_POWERING_ON;
		break;
	case CONTROL_REARM:
		service->phase = MODEM_PREPARING;
		service->next_action_at_ms = k_uptime_get();
		break;
	default:
		enter_fault(service, -EBADMSG, false);
		break;
	}
}

/* 完成本轮才发布；查询错误清除本轮部分数据，正常未就绪则保留已成功的诊断字段。 */
static void finish_network_check(struct service_context *service, int error)
{
	struct network_monitor *network = &service->network;
	struct gt02_network_status *result = &network->result;

	if (error != 0) {
		*result = (struct gt02_network_status){ .signal_rssi = 99 };
	}
	result->state = GT02_NETWORK_NOT_READY;
	result->query = network->query;
	result->error = error;
	result->checked_at_ms = k_uptime_get();
	uint8_t first = result->data_context.ipv4[0];

	if (error == 0 && network->pdp_activation_succeeded && result->sim == GT02_MODEM_SIM_READY &&
	    (result->registration == 1 || result->registration == 5) &&
	    result->data_context.active && first != 0 && first != 127 && first < 224) {
		result->state = GT02_NETWORK_DATA_READY;
	}
	publish_status(result);
	network->active = false;
	network->next_check_at_ms = result->checked_at_ms +
		(result->state == GT02_NETWORK_DATA_READY ? config.check_interval_ms : config.retry_interval_ms);
}

/* 匹配后的查询结果只推进一步；保留原有公共值校验，不在结构重构中收紧或放宽契约。 */
static void network_on_result(struct service_context *service, const struct gt02_modem_event *event)
{
	struct network_monitor *network = &service->network;
	int error = event->error;

	if (error == -EPIPE) {
		enter_fault(service, error, false);
		return;
	}
	if (error == 0 &&
	    ((network->query == GT02_MODEM_QUERY_SIM &&
	      (unsigned int)event->sim > GT02_MODEM_SIM_PUK_REQUIRED) ||
	     (network->query == GT02_MODEM_QUERY_SIGNAL && event->signal_rssi > 31 && event->signal_rssi != 99) ||
	     (network->query == GT02_MODEM_QUERY_LTE_REGISTRATION && event->registration > 5))) {
		error = -EBADMSG;
	}
	if (error != 0) {
		finish_network_check(service, error);
		return;
	}

	switch (network->query) {
	case GT02_MODEM_QUERY_SIM:
		network->result.sim = event->sim;
		if (event->sim != GT02_MODEM_SIM_READY) {
			finish_network_check(service, 0);
			return;
		}
		network->query = GT02_MODEM_QUERY_SIGNAL;
		break;
	case GT02_MODEM_QUERY_SIGNAL:
		network->result.signal_rssi = event->signal_rssi;
		network->query = GT02_MODEM_QUERY_LTE_REGISTRATION;
		break;
	case GT02_MODEM_QUERY_LTE_REGISTRATION:
		network->result.registration = event->registration;
		if (event->registration != 1 && event->registration != 5) {
			finish_network_check(service, 0);
			return;
		}
		/* 调度器先补齐本次上电的PDP激活，再提交下一项IP查询。 */
		network->query = GT02_MODEM_QUERY_DATA_CONTEXT;
		break;
	case GT02_MODEM_QUERY_DATA_CONTEXT:
		network->result.data_context = event->data_context;
		service->recovery_used = false;
		finish_network_check(service, 0);
		break;
	}
}

/* 所有厂家事件经过同一入口；RDY独立处理，完成事件只交给当前在途操作。
 * 当前契约无请求编号，依赖后端恰好一次完成及故障前结束旧操作的保证。
 */
static void dispatch_event(struct service_context *service, const struct gt02_modem_event *event)
{
	if (event->type == GT02_MODEM_EVENT_STARTUP_READY) {
		if (service->phase != MODEM_POWERING_ON && service->phase != MODEM_WAIT_READY) {
			return; /* 本轮已经处理的重复RDY，不结束任何AT操作。 */
		}
		if (service->phase == MODEM_WAIT_READY &&
		    k_uptime_get() >= service->startup_deadline_ms) {
			enter_fault(service, -ETIMEDOUT, false);
		} else if (event->error != 0) {
			enter_fault(service, event->error, false);
		} else {
			service->ready_seen = true;
			if (service->phase == MODEM_WAIT_READY) {
				advance_after_ready(service);
			}
		}
		return;
	}
	if (service->pending_at == AT_OPERATION_NONE) {
		return; /* 空闲、故障或恢复期间的旧完成，不得重新启用业务。 */
	}
	if (service->pending_at == AT_OPERATION_PREPARE &&
	    event->type == GT02_MODEM_EVENT_PREPARE_DONE) {
		service->pending_at = AT_OPERATION_NONE;
		if (event->error == -ETIMEDOUT) {
			service->next_action_at_ms = k_uptime_get() + config.retry_interval_ms;
		} else if (event->error != 0) {
			enter_fault(service, event->error, false);
		} else {
			service->phase = MODEM_RUNNING;
			service->network.next_check_at_ms = k_uptime_get();
		}
		return;
	}
	if (service->pending_at == AT_OPERATION_ACTIVATE &&
	    event->type == GT02_MODEM_EVENT_ACTIVATE_DONE) {
		service->pending_at = AT_OPERATION_NONE;
		if (event->error == -EPIPE) {
			enter_fault(service, event->error, false);
		} else if (event->error != 0) {
			finish_network_check(service, event->error);
		} else {
			/* 激活成功只允许下一步查询IP，不直接发布DATA_READY。 */
			service->network.pdp_activation_succeeded = true;
		}
		return;
	}
	if (service->pending_at == AT_OPERATION_QUERY &&
	    event->type == GT02_MODEM_EVENT_QUERY_DONE && event->query == service->network.query) {
		service->pending_at = AT_OPERATION_NONE;
		network_on_result(service, event);
		return;
	}
	enter_fault(service, -EBADMSG, false);
}

/* 处理生命周期期限及周期到期，每次最多启动一个底层控制作业，不等待完成。 */
static void process_deadlines(struct service_context *service)
{
	int64_t now = k_uptime_get();

	if ((service->phase == MODEM_WAIT_READY || service->phase == MODEM_REARMING) &&
	    now >= service->startup_deadline_ms) {
		enter_fault(service, -ETIMEDOUT, false);
		return;
	}
	if (service->pending_control == CONTROL_NONE) {
		switch (service->phase) {
		case MODEM_POWERING_ON:
			submit_control(service, CONTROL_POWER_ON);
			break;
		case MODEM_POWERING_OFF:
			submit_control(service, CONTROL_POWER_OFF);
			break;
		case MODEM_REARMING:
			if (now >= service->next_action_at_ms) {
				submit_control(service, CONTROL_REARM);
			}
			break;
		default:
			break;
		}
	}
	if (service->phase == MODEM_RUNNING && !service->network.active &&
	    now >= service->network.next_check_at_ms) {
		service->network.active = true;
		service->network.query = GT02_MODEM_QUERY_SIM;
		service->network.result = (struct gt02_network_status){ .signal_rssi = 99 };
	}
}

/* 唯一AT仲裁入口；提交前登记归属以兼容同步回调，返回0只表示已接受。
 * 查询/激活的-EAGAIN结束本轮，准备的-EAGAIN按间隔重试；激活成功后本次上电不重复发送。
 */
static void schedule_next_at(struct service_context *service)
{
	process_pending_fault(service);
	if (service->pending_at != AT_OPERATION_NONE) {
		return;
	}
	int ret;

	if (service->phase == MODEM_PREPARING && k_uptime_get() >= service->next_action_at_ms) {
		service->pending_at = AT_OPERATION_PREPARE;
		ret = config.modem->prepare();
		if (ret != 0) {
			service->pending_at = AT_OPERATION_NONE;
			if (ret == -EAGAIN) {
				service->next_action_at_ms = k_uptime_get() + config.retry_interval_ms;
			} else {
				enter_fault(service, ret, false);
			}
		}
	} else if (service->phase == MODEM_RUNNING && service->network.active) {
		if (service->network.query == GT02_MODEM_QUERY_DATA_CONTEXT &&
		    !service->network.pdp_activation_succeeded) {
			service->pending_at = AT_OPERATION_ACTIVATE;
			ret = config.modem->activate_data_context();
		} else {
			service->pending_at = AT_OPERATION_QUERY;
			ret = config.modem->query(service->network.query);
		}
		if (ret != 0) {
			service->pending_at = AT_OPERATION_NONE;
			if (ret == -EAGAIN) {
				finish_network_check(service, ret);
			} else {
				enter_fault(service, ret, false);
			}
		}
	}
}

/* 计算最近动作/启动期限；已接受的AT操作没有第二套服务层事务超时。 */
static k_timeout_t next_timeout(const struct service_context *service)
{
	int64_t deadline = INT64_MAX;

	switch (service->phase) {
	case MODEM_POWERING_ON:
	case MODEM_POWERING_OFF:
		if (service->pending_control == CONTROL_NONE) {
			return K_NO_WAIT;
		}
		break;
	case MODEM_WAIT_READY:
		deadline = service->startup_deadline_ms;
		break;
	case MODEM_REARMING:
		deadline = service->startup_deadline_ms;
		if (service->pending_control == CONTROL_NONE) {
			deadline = MIN(deadline, service->next_action_at_ms);
		}
		break;
	case MODEM_PREPARING:
		if (service->pending_at == AT_OPERATION_NONE) {
			deadline = service->next_action_at_ms;
		}
		break;
	case MODEM_RUNNING:
		if (!service->network.active) {
			deadline = service->network.next_check_at_ms;
		} else if (service->pending_at == AT_OPERATION_NONE) {
			return K_NO_WAIT;
		}
		break;
	case MODEM_HALTED:
		break;
	}
	return deadline == INT64_MAX ? K_FOREVER : K_MSEC(MAX(deadline - k_uptime_get(), 0));
}

/* 唯一业务状态所有者：故障优先、处理完成/事件、推进到期动作，再仲裁一项AT请求。
 * 只在统一入口睡眠；合并唤醒后若仍有排队事件，继续下一轮而不等待新的生产者。
 */
static void service_worker(void *p1, void *p2, void *p3)
{
	struct service_context service = {
		.phase = MODEM_POWERING_ON, .network.query = GT02_MODEM_QUERY_SIM,
	};
	const struct gt02_network_status initial = {
		.state = GT02_NETWORK_STARTING, .signal_rssi = 99,
	};
	struct gt02_modem_event event;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	publish_status(&initial);
	for (;;) {
		(void)k_sem_take(&service_wake, next_timeout(&service));
		process_pending_fault(&service);
		process_control_completion(&service);
		if (k_msgq_get(&modem_events, &event, K_NO_WAIT) == 0) {
			dispatch_event(&service, &event);
		}
		process_deadlines(&service);
		schedule_next_at(&service);
		if (k_msgq_num_used_get(&modem_events) != 0) {
			k_sem_give(&service_wake);
		}
	}
}

K_THREAD_DEFINE(network_thread, 2048, service_worker, NULL, NULL, NULL, 7, 0, SYS_FOREVER_MS);

/* 完整验证配置后才绑定接收者并启动；操作表和回调须覆盖开机生命周期。 */
int gt02_network_service_start(const struct gt02_network_config *provided)
{
	if (k_is_in_isr()) {
		return -EWOULDBLOCK;
	}
	if (provided == NULL || provided->power == NULL || provided->power->power_on == NULL ||
	    (provided->session_rearm != NULL && provided->power->power_off == NULL) ||
	    provided->modem == NULL || provided->modem->init == NULL ||
	    provided->modem->prepare == NULL || provided->modem->query == NULL ||
	    provided->modem->activate_data_context == NULL ||
	    provided->startup_timeout_ms == 0 ||
	    provided->retry_interval_ms == 0 || provided->check_interval_ms == 0 ||
	    provided->result_valid_ms <= provided->check_interval_ms) {
		return -EINVAL;
	}
	k_mutex_lock(&status_lock, K_FOREVER);
	int ret = started ? -EALREADY : provided->modem->init(on_modem_event, NULL);

	if (ret == 0) {
		config = *provided;
		k_work_init(&control_job.work, run_control_operation);
		k_work_queue_start(&control_queue, control_stack, K_THREAD_STACK_SIZEOF(control_stack),
				   7, NULL);
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
