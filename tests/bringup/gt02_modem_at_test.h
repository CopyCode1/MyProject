/** @file
 * @brief 临时信息验证入口，上电一次并逐条完成现有查询，不是网络服务。
 */
#ifndef GT02_MODEM_AT_TEST_H_
#define GT02_MODEM_AT_TEST_H_

#include "adapters/modem/gt02_modem_at.h"

/**
 * @brief 注入AT配置和电源能力，启动一次上电及11条握手/信息查询验证。
 * @param[in] at 初始化期间有效的AT配置；协议和I/O操作表及回调参数需长期有效。
 * @param[in] power 长期有效的电源操作表，须提供power_on；硬件init已由装配方完成。
 * @retval 0 测试线程已启动，不代表命令或信号查询成功。
 * @retval -EALREADY 已启动过测试或AT核心，失败后也不重新启动。
 * @retval -EINVAL 缺少配置/电源操作、厂家表缺少所需固定查询，或配置/容量无效。
 * @return 其他负值为AT核心配置/初始化错误。
 * @note 仅线程调用，前提是硬件init成功；不能与原硬件测试同时运行。
 *       测试会使模组保持上电，不自动断电、重试或恢复，不阻塞主线程。
 *       使用长期测试表副本包装查询回调，原厂家解析和非查询配置保持有效。
 *       单条完成后才发下一条，ERROR记录后继续；超时或失步停止。
 *       等待回调的独立15秒保护到期也停止，不取消尚未结束的AT请求。
 *       最终计数仅表示AT事务结果，字段有效性以厂家解析日志为准。
 */
int gt02_modem_at_test_start(const struct gt02_at_config *at,
			     const struct gt02_modem_power_ops *power);

#endif /* GT02_MODEM_AT_TEST_H_ */
