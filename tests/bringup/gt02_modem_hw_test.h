/**
 * @file
 * @brief 临时4G硬件验证入口，不是产品网络服务。
 *
 * 测试线程执行周期性硬件断电/上电，唯一接收线程将模组原始文本转发到日志串口。
 */
#ifndef GT02_MODEM_HW_TEST_H_
#define GT02_MODEM_HW_TEST_H_

/**
 * @brief 启动一次临时上下电循环和原始RX打印，不等待硬件操作完成。
 * @retval 0 两个测试线程已启动；不代表硬件接口或URC已验证通过。
 * @retval -EALREADY 本次系统启动已启动过测试，包括测试已经失败停止的情况。
 * @note 前提为gt02_modem_hw_init()成功；仅在线程中调用。
 *       首次上电完成后，每10秒发起一次硬件下电再上电，硬件等待计入周期。
 *       上下电或在线接收出现异常时停止循环，不自动恢复，不影响主线程继续运行。
 *       不发送AT、不做正常关机/TCP；关闭CONFIG_GT02_MODEM_HW_TEST可移除本测试。
 */
int gt02_modem_hw_test_start(void);

#endif /* GT02_MODEM_HW_TEST_H_ */
