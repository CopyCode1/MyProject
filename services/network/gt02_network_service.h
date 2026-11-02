/**
 * @file
 * @brief 最小4G服务：一次上电、基础准备、SIM检查；只依赖公共操作表。
 */
#ifndef GT02_NETWORK_SERVICE_H_
#define GT02_NETWORK_SERVICE_H_

#include "interfaces/gt02_modem.h"

/** 一次启动流程的阶段，SIM_READY不表示已经注册、取得IP或建立TCP。 */
enum gt02_network_state {
	GT02_NETWORK_STOPPED,       /**< 尚未启动。 */
	GT02_NETWORK_POWERING_ON,   /**< 执行硬件上电及启动等待。 */
	GT02_NETWORK_PREPARING,     /**< 等待厂家基础准备完成。 */
	GT02_NETWORK_QUERYING_SIM,  /**< 等待SIM查询完成。 */
	GT02_NETWORK_SIM_READY,     /**< 本次SIM查询完成，SIM就绪。 */
	GT02_NETWORK_SIM_NOT_READY, /**< 查询完成，SIM未就绪或需要PIN/PUK。 */
	GT02_NETWORK_FAILED,        /**< 本次流程失败，原因见error。 */
};

/** 装配方提供能力和启动等待参数；不包含厂家命令、设备树或UART实例。 */
struct gt02_network_config {
	const struct gt02_modem_power_ops *power; /**< 长期有效，必须提供power_on；硬件init已完成。 */
	const struct gt02_modem_ops *modem;       /**< 长期有效，必须提供init、prepare和query。 */
	uint32_t startup_delay_ms;               /**< 上电接口完成后的等待毫秒数，允许0，由装配方确定。 */
};

/** 服务拥有的状态快照；只有SIM查询成功完成时sim才表示本次有效结果。 */
struct gt02_network_status {
	enum gt02_network_state state; /**< 当前阶段或一次流程的终态。 */
	enum gt02_modem_sim_state sim; /**< SIM查询结果；尚无有效结果或失败时为UNKNOWN。 */
	int error;                    /**< FAILED时为失败原因，其他状态为0。 */
};

/**
 * @brief 复制配置、绑定厂家事件回调并启动唯一服务线程，本次开机只执行一轮。
 * @param[in] provided 非NULL，调用期间有效；其中的操作表及实现需覆盖整个开机生命周期。
 * @retval 0 服务线程已启动，不代表上电、准备或SIM查询已经成功。
 * @retval -EINVAL 配置或必需操作缺失。
 * @retval -EALREADY 服务已启动过，或厂家回调已经被其他控制者绑定。
 * @retval -EWOULDBLOCK 从ISR调用。
 * @return 其他负值为厂家init返回的错误，没有启动服务线程。
 * @note 调用前由装配方完成硬件init和AT初始化，确保先建立RX再由服务上电。
 *       不与bring-up控制入口同时运行；异步结果由get_status读取及服务日志报告。
 *       不重试、不自动下电、不取消请求、不支持本次开机再次启动。
 */
int gt02_network_service_start(const struct gt02_network_config *provided);

/**
 * @brief 在短互斥锁内复制状态，调用方得到独立快照。
 * @param[out] out 非NULL，成功写入完整快照；失败时不修改。
 * @retval 0 已复制；启动前返回STOPPED，SIM未就绪与操作失败由状态分别表达。
 * @retval -EINVAL out为NULL。
 * @retval -EWOULDBLOCK 从ISR调用。
 * @note 仅线程调用，可与服务线程并行；可能短暂等待锁，不等待模组或AT操作。
 */
int gt02_network_service_get_status(struct gt02_network_status *out);

#endif /* GT02_NETWORK_SERVICE_H_ */
