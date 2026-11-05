/**
 * @file
 * @brief 周期检测SIM、信号、LTE驻网及数据上下文；只报告TCP建连前置条件。
 */
#ifndef GT02_NETWORK_SERVICE_H_
#define GT02_NETWORK_SERVICE_H_

#include "interfaces/gt02_modem.h"

/** 公开检测结论，与内部查询步骤分开；DATA_READY不代表服务器可达或TCP已建立。 */
enum gt02_network_state {
	GT02_NETWORK_STOPPED,   /**< 本次开机尚未启动服务。 */
	GT02_NETWORK_STARTING,  /**< 上电、等待启动通知或执行基础准备。 */
	GT02_NETWORK_NOT_READY, /**< 条件不满足、查询失败，或上次结果已经过期。 */
	GT02_NETWORK_DATA_READY, /**< 本轮SIM、注册和目标IPv4数据上下文均满足条件。 */
	GT02_NETWORK_FAULT,     /**< 通道或流程故障，不放行TCP；可能进行一次受控恢复。 */
};

/** 装配配置按值复制，操作表及实现须覆盖整个开机生命周期；所有时间单位均为毫秒。 */
struct gt02_network_config {
	const struct gt02_modem_power_ops *power; /**< 必须提供power_on；硬件init由装配层完成。 */
	const struct gt02_modem_ops *modem;       /**< 必须提供init、prepare、四项query；不能共享控制者。 */
	/** 可选会话恢复能力：无参数，0表示已恢复，-EAGAIN表示边界未完整，其他负值停止恢复。
	 * 仅服务线程在确认切电成功及新RDY后同步调用；自动恢复须同时提供power_off。
	 */
	int (*session_rearm)(void);
	uint32_t startup_timeout_ms; /**< power_on返回后等待启动事件的上限，必须大于0。 */
	uint32_t operation_timeout_ms; /**< 每项准备/查询的服务保护上限，须大于实际AT事务预算。 */
	uint32_t retry_interval_ms; /**< 未就绪或可继续的查询失败后，再次检查的间隔，必须大于0。 */
	uint32_t check_interval_ms; /**< 已就绪时下一轮检查间隔，必须大于0。 */
	uint32_t result_valid_ms; /**< 成功结果有效期，必须大于check_interval_ms；过期撤销放行。 */
};

/** 按轮次发布的结果；刷新中保留上一轮结论，通过get_status获得有期限的独立快照。 */
struct gt02_network_status {
	enum gt02_network_state state; /**< 对外结论，不暴露内部阶段或在途布尔标志。 */
	int error; /**< 0表示查询成功但仍需看state；负值表示查询、通道或过期错误。 */
	enum gt02_modem_query query; /**< 本轮最后完成/失败的查询，STOPPED及STARTING时无意义。 */
	enum gt02_modem_sim_state sim; /**< 本轮SIM结果，查询错误时为UNKNOWN。 */
	unsigned int signal_rssi; /**< 本轮原始信号等级0..31或99，不设强度门槛；未查询时为99。 */
	unsigned int registration; /**< 本轮注册原值；仅SIM_READY且注册步骤已完成时有效。 */
	struct gt02_modem_data_context data_context; /**< 仅DATA_CONTEXT查询成功时有效，固定CID1/IPv4。 */
	int64_t checked_at_ms; /**< 本轮结论的开机毫秒时间；尚无结果时为0，不是日历时间。 */
};

/**
 * @brief 复制配置、绑定厂家事件出口并启动唯一周期检测线程。
 * @param[in] provided 非NULL，仅调用期间借用；所有时间非0且结果有效期大于检查间隔。
 * @retval 0 已启动，不代表收到RDY、驻网成功或数据连接就绪。
 * @retval -EINVAL 配置、必需操作或时间参数无效；没有绑定回调或启动线程。
 * @retval -EALREADY 已启动过，或厂家事件出口已绑定。
 * @retval -EWOULDBLOCK 从ISR调用。
 * @return 其他负值来自厂家init；失败不发布新状态、不启动线程。
 * @note 调用前装配层完成硬件init、AT初始化。先绑定回调，再由服务上电，早到RDY按值保留。
 *       本次开机只能启动一次。只查询，不配置APN、不激活上下文、不建TCP。
 *       周期查询超时先撤销就绪并等待下轮；AT隔离期返回-EAGAIN时继续间隔尝试，不盲目重发。
 *       流程错误停止普通推进但保留事件处理；AT故障通知触发一次恢复，须配置session_rearm及power_off。
 */
int gt02_network_service_start(const struct gt02_network_config *provided);

/**
 * @brief 非阻塞锁存AT调度故障并唤醒服务，由装配层连接AT故障出口。
 * @param[in] error 负错误码；非负值忽略，重复未处理通知保留首个错误。
 * @note 可在AT线程调用，不控制电源；未启动服务时也可锁存。队列满不会丢失故障原因。
 *       每个恢复周期最多一次断电重启，完整四项查询成功后才重新获得恢复机会。
 */
void gt02_network_service_notify_at_fault(int error);

/**
 * @brief 复制整轮结果并检查有效期，不等待模组或AT事务。
 * @param[out] out 非NULL，可写一个完整结构；失败时保持原值，成功后归调用者独立使用。
 * @retval 0 已复制快照；只有state为DATA_READY时允许尝试TCP建连。
 * @retval -EINVAL out为NULL。
 * @retval -EWOULDBLOCK 从ISR调用。
 * @note 仅线程调用，短暂等待状态锁；过期的成功快照以NOT_READY/-ESTALE交付，保留旧数据仅供诊断。
 *       刷新过程中保留上次结论；失败轮次不会把部分成功数据混入公开结果。
 *       轮询只能反映最近观测，不承诺即时发现掉网或实际服务器可达。
 */
int gt02_network_service_get_status(struct gt02_network_status *out);

#endif /* GT02_NETWORK_SERVICE_H_ */
