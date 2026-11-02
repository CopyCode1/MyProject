/** @file
 * @brief 顺序执行最小4G流程，厂家回调只投递事件，服务线程独占流程和公开状态。
 */
#include <errno.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include "services/network/gt02_network_service.h"

LOG_MODULE_REGISTER(gt02_network, LOG_LEVEL_INF);

/* 首版只有准备和SIM两个完成事件，不接URC流；等待保护不代替AT自身超时。 */
K_MSGQ_DEFINE(modem_events, sizeof(struct gt02_modem_event), 2, 4);
K_MUTEX_DEFINE(status_lock);
static struct gt02_network_config config;
static struct gt02_network_status status;
static bool started;
static atomic_t event_overflow;

/* AT线程只复制借用事件，不执行服务步骤、不等待锁或下一次AT结果。 */
static void on_modem_event(const struct gt02_modem_event *event, void *user_data)
{
	ARG_UNUSED(user_data);
	if (k_msgq_put(&modem_events, event, K_NO_WAIT) != 0) {
		atomic_set(&event_overflow, 1);
	}
}

/* 只有服务线程更新运行状态，读取者通过get_status获得按值快照。 */
static void set_status(enum gt02_network_state state, enum gt02_modem_sim_state sim, int error)
{
	k_mutex_lock(&status_lock, K_FOREVER);
	status.state = state;
	status.sim = sim;
	status.error = error;
	k_mutex_unlock(&status_lock);
}

/* 等待当前步骤的完成结果；异常或超时直接停止，不尝试取消仍在执行的底层操作。 */
static int wait_result(enum gt02_modem_event_type expected, struct gt02_modem_event *event)
{
	int ret = k_msgq_get(&modem_events, event, K_SECONDS(10));

	if (atomic_get(&event_overflow) != 0) {
		return -ENOBUFS;
	}
	if (ret != 0) {
		return -ETIMEDOUT;
	}
	if (event->type != expected ||
	    (expected == GT02_MODEM_EVENT_QUERY_DONE && event->query != GT02_MODEM_QUERY_SIM)) {
		return -EBADMSG;
	}
	return event->error;
}

/* 服务线程按上电、准备、查询顺序运行一轮；没有重试、恢复、PDP、TCP或TTS流程。 */
static void service_worker(void *p1, void *p2, void *p3)
{
	struct gt02_modem_event event;
	int ret;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	set_status(GT02_NETWORK_POWERING_ON, GT02_MODEM_SIM_UNKNOWN, 0);
	LOG_INF("4G service: power on");
	ret = config.power->power_on();
	if (ret != 0) {
		goto failed;
	}
	k_sleep(K_MSEC(config.startup_delay_ms));

	set_status(GT02_NETWORK_PREPARING, GT02_MODEM_SIM_UNKNOWN, 0);
	LOG_INF("4G service: prepare");
	ret = config.modem->prepare();
	if (ret != 0) {
		goto failed;
	}
	ret = wait_result(GT02_MODEM_EVENT_PREPARE_DONE, &event);
	if (ret != 0) {
		goto failed;
	}

	set_status(GT02_NETWORK_QUERYING_SIM, GT02_MODEM_SIM_UNKNOWN, 0);
	LOG_INF("4G service: query SIM");
	ret = config.modem->query(GT02_MODEM_QUERY_SIM);
	if (ret != 0) {
		goto failed;
	}
	ret = wait_result(GT02_MODEM_EVENT_QUERY_DONE, &event);
	if (ret != 0) {
		goto failed;
	}
	set_status(event.sim == GT02_MODEM_SIM_READY ? GT02_NETWORK_SIM_READY :
		   GT02_NETWORK_SIM_NOT_READY, event.sim, 0);
	LOG_INF("4G service complete: SIM=%d (READY=%d); network/TCP not tested",
		event.sim, event.sim == GT02_MODEM_SIM_READY);
	return;

failed:
	set_status(GT02_NETWORK_FAILED, GT02_MODEM_SIM_UNKNOWN, ret);
	LOG_ERR("4G service stopped: error=%d; no retry or automatic power-off", ret);
}

K_THREAD_DEFINE(network_thread, 2048, service_worker, NULL, NULL, NULL, 7, 0, SYS_FOREVER_MS);

/* 把一次性配置绑定和启动合成一个入口，避免首版额外拆分没有独立用途的init函数。 */
int gt02_network_service_start(const struct gt02_network_config *provided)
{
	if (k_is_in_isr()) {
		return -EWOULDBLOCK;
	}
	if (provided == NULL || provided->power == NULL || provided->power->power_on == NULL ||
	    provided->modem == NULL || provided->modem->init == NULL ||
	    provided->modem->prepare == NULL || provided->modem->query == NULL) {
		return -EINVAL;
	}
	k_mutex_lock(&status_lock, K_FOREVER);
	if (started) {
		k_mutex_unlock(&status_lock);
		return -EALREADY;
	}
	config = *provided;
	int ret = config.modem->init(on_modem_event, NULL);

	started = ret == 0;
	k_mutex_unlock(&status_lock);
	if (ret == 0) {
		k_thread_start(network_thread);
	}
	return ret;
}

/* 不暴露可变全局状态；调用者自行解释终态，不将SIM就绪等同于网络可用。 */
int gt02_network_service_get_status(struct gt02_network_status *out)
{
	if (k_is_in_isr()) {
		return -EWOULDBLOCK;
	}
	if (out == NULL) {
		return -EINVAL;
	}
	k_mutex_lock(&status_lock, K_FOREVER);
	*out = status;
	k_mutex_unlock(&status_lock);
	return 0;
}
