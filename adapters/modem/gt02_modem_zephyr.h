/**
 * @file
 * @brief 4G硬件适配仅公开初始化、上电、硬件下电、原始字节读写五个能力。
 *
 * 供后续AT执行器及生命周期调用者使用，不包含联网策略或AT事务调度。
 * UART启停、接收等待、缓存和错误检测是实现细节。所有接口仅限线程调用。
 * 上下电/TX应由一个上层执行者组织；唯一RX消费者可以并行read。
 */
#ifndef GT02_MODEM_ZEPHYR_H_
#define GT02_MODEM_ZEPHYR_H_

#include <stddef.h>
#include <stdint.h>
#include <zephyr/kernel.h>

/**
 * @brief 准备未激活的POWER/PWRKEY，不上电或启动UART。
 * @retval 0 准备完成；重复调用返回0，保持当前硬件状态。
 * @retval -ENODEV 控制GPIO设备未就绪。
 * @return 其他负值为GPIO配置错误；此前成功改变的引脚不自动回滚，可再次调用。
 * @note 同步操作，可能等待控制锁；首次调用会配置控制脚。
 */
int gt02_modem_hw_init(void);

/**
 * @brief 先启用UART接收，再完成当前GT02旧板硬件上电步骤。
 * @retval 0 硬件步骤完成，允许读写；已就绪且无故障时重复调用不重发脉冲。
 * @retval -EACCES 尚未成功初始化。
 * @retval -EALREADY 硬件曾部分上电、尚未就绪，须先显式下电处理。
 * @retval -ENODEV UART初始化后未就绪。
 * @return 其他负值为GPIO、UART初始化、PM恢复、回调安装或锁存的传输错误。
 * @note 同步等待控制锁，先进行UART驱动操作，再执行旧板约1500毫秒时序，不承诺精确耗时。
 *       不等待启动URC/AT响应，不代表模组启动完成或联网成功。
 *       失败可能留在部分上电状态，不自动切电或重试。驱动初始化失败可能需要整机重启。
 *       成功建立新接收周期会清缓存/旧通知/故障；调用前由上层协调旧消费者生命周期。
 *       未开始供电阶段的framing/break和输入字节仅记录/丢弃，保持RX；开始供电后恢复严格错误处理。
 */
int gt02_modem_hw_power_on(void);

/**
 * @brief 先关闭板级供电使能，再挂起UART、清接收缓存并释放PWRKEY。
 * @retval 0 硬件切电操作完成；本来已断电时返回0，不重复等待2秒间隔。
 * @retval -EACCES 尚未成功初始化。
 * @return 负值为供电GPIO、UART挂起或PWRKEY错误；同时发生时返回本次最先遇到的错误。
 * @note 这是硬件断电，不能代替模组的AT关机请求与确认；正常关机由后续模组后端先完成。
 *       等待当前同步写结束；切电成功后立即处理UART停机，再等待旧板2秒间隔后释放控制锁。
 *       切电失败不改变供电记录、不继续关闭UART，锁存故障并唤醒read报告错误。
 *       切电成功但UART挂起失败时，供电记录仍为关闭，禁止继续收发；再次调用可重试清理。
 *       不等待RX消费者退出，但会取消旧read；未处理数据被丢弃。
 *       不清除故障，成功再次上电后才清除；GPIO/驱动返回值不证明实际供电电压。
 */
int gt02_modem_hw_power_off(void);

/**
 * @brief 等待并复制当前可读的原始字节，不拼行、不补NUL。
 * @param[out] buffer 可写缓冲，不得为NULL，容量至少capacity字节；函数不保留指针。
 * @param[in] capacity 容量，单位字节，必须大于0；单次最多复制RX缓存大小。
 * @param[out] received 实际复制字节数，不得为NULL；有效指针在入口置0，失败保持0。
 * @param[in] timeout Zephyr超时对象；K_NO_WAIT仅尝试读取，K_MSEC(ms)有限等待，
 *            K_FOREVER等待直到数据、故障或下电。不是普通整数毫秒。
 * @retval 0 读到至少一个字节；K_NO_WAIT时也可能received为0。输出可立即使用。
 * @retval -EINVAL 参数无效，不写buffer。
 * @retval -EAGAIN 有限等待预算用尽且没有数据，不写buffer。
 * @retval -EPIPE 未就绪，或本read因下电/接收周期改变而取消，不写buffer。
 * @retval -EIO 有效接收窗口内UART硬件错误，不写buffer；日志记录首次错误标志、时间及阶段。
 * @retval -ENOBUFS RX溢出且接收流失效，不写buffer。
 * @return 其他负值为内部锁存的初始化/传输错误；故障可能优先于未就绪返回。
 * @note 只允许一个RX消费者；短自旋锁保护复制，不等待TX控制锁。
 *       空唤醒不会延长本次等待预算，通知不代表完整报文。保留任意二进制字节。
 *       供电前输入不作为有效载荷交付；从使能供电阶段起才缓存原始字节。
 *       仍可能在成功返回后发生新故障；本地接收代次不识别线上迟到AT回复。
 */
int gt02_modem_hw_read(uint8_t *buffer, size_t capacity, size_t *received,
		      k_timeout_t timeout);

/**
 * @brief 同步提交指定长度的原始字节，不添加CRLF/NUL，不等待模组响应。
 * @param[in] buffer 数据地址，length为0时允许NULL；输入不要求字符串结尾。
 * @param[in] length 待提交字节数，允许0。
 * @param[out] written 已提交给UART的字节数，不得为NULL；入口置0，失败可能大于0。
 * @retval 0 全部字节已提交，输入可复用；不证明模组收到、AT成功或平台处理。
 * @retval -EINVAL 参数无效，没有发送。
 * @retval -EPIPE 硬件未就绪，没有发送。
 * @return 其他负值为锁存故障；必须结合written处理，禁止无条件重发。
 * @note 同步等待控制锁并用uart_poll_out提交，不提供接口级TX超时；不保留输入指针。
 *       单次write与上下电互斥，不能替代跨多次write的AT事务调度。
 */
int gt02_modem_hw_write(const uint8_t *buffer, size_t length, size_t *written);

#endif /* GT02_MODEM_ZEPHYR_H_ */
