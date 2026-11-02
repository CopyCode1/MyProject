/**
 * @file
 * @brief 表驱动的短文本AT事务：异步提交，单线程接收，结束后回调一次。
 *
 * 不控制电源，不实现联网策略、发送提示符或二进制载荷协议。
 */
#ifndef GT02_MODEM_AT_H_
#define GT02_MODEM_AT_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "interfaces/gt02_modem.h"

/** 单条发送命令的最大字节数，不含NUL和核心追加的CRLF。 */
#define GT02_AT_COMMAND_MAX 128U

/** AT层的最终结果，不等于上层业务正文一定解析成功。 */
enum gt02_at_result {
	GT02_AT_SUCCESS, /**< 收到最终OK，且所需正文完整。 */
	GT02_AT_FAILED,  /**< 模组拒绝、正文缺失、流故障或排队请求被取消；原因见日志。 */
	GT02_AT_TIMEOUT, /**< 在途事务等待最终结果超时，核心停止接受后续请求。 */
};

/**
 * @brief 处理命令的最终结果，仅由AT线程调用一次，不得阻塞等待。
 * @param[in] result AT事务结果；SUCCESS后仍需检查正文格式及业务范围。
 * @param[in] response 成功时的正文，去掉回显、URC、OK，并补NUL；无正文或失败时为NULL。
 * @param[in] length 正文字节数，不含NUL，多条正文之间用换行分隔。
 * @param[in,out] user_data 命令表注册的长期有效对象，允许NULL。
 * @note response只在回调内有效，需保留时应复制或解析字段。
 *       可非阻塞submit后续命令；不要等待其完成。无返回值。
 */
typedef void (*gt02_at_callback_t)(enum gt02_at_result result,
				 const char *response, size_t length, void *user_data);

/**
 * @brief 处理完整URC行，在AT线程中即时调用，不得阻塞。
 * @param[in] content 完整原始行，不含CR/LF，额外补NUL；仅本次回调有效。
 * @param[in] length 原始字节数，不含NUL，不能用strlen替代。
 * @param[in,out] user_data URC表注册的长期有效对象，允许NULL。
 * @note 回调不保留接收指针；可短小解析或投递事件。无返回值。
 */
typedef void (*gt02_at_urc_callback_t)(const char *content, size_t length, void *user_data);

/** 命令只读配置；固定项存完整命令，参数化项存命令名，字符串和上下文长期有效。 */
struct gt02_at_command {
	uint16_t id;                 /**< 模组自定义标识，同一张表内唯一。 */
	const char *command;         /**< 固定项为完整文本；参数化项为不带=或?的命令名，例如AT+LGPIO。 */
	const char *response_prefix; /**< NULL不收集；非空字符串匹配前缀；空字符串收集未命中URC的正文。 */
	gt02_at_callback_t callback; /**< 可执行项必须提供；reserved项允许NULL，提交时不回调。 */
	void *user_data;             /**< 回调参数，不复制对象；由注册者管理生命周期。 */
	uint32_t timeout_ms;         /**< 开始发送起的响应预算，毫秒，必须大于0；不含排队。 */
	bool require_response;      /**< 为true时，没有匹配正文的OK也判失败。 */
	bool reserved;              /**< true仅登记待接入命令/格式模板；submit拒绝，不向UART发送。 */
	bool parameterized;         /**< true只能通过submit_text提供command=参数，不允许普通submit。 */
};

/** 一条URC规则；不允许与非空正文前缀/最终响应冲突，优先于空前缀的兜底收集。 */
struct gt02_at_urc {
	const char *match;                /**< 非空匹配文本，以NUL结尾。 */
	bool exact;                       /**< true匹配整行，false匹配前缀。 */
	gt02_at_urc_callback_t callback;   /**< 匹配时调用，不得为NULL。 */
	void *user_data;                  /**< 长期有效的参数对象，可为NULL。 */
};

/** 初始化时一次注册两张表，不在运行中增删或修改。 */
struct gt02_at_profile {
	const struct gt02_at_command *commands; /**< 命令数组，不得为NULL。 */
	size_t command_count;                  /**< 命令元素数，必须大于0。 */
	const struct gt02_at_urc *urcs;         /**< URC数组，urc_count为0时允许NULL。 */
	size_t urc_count;                      /**< URC元素数，允许0。 */
};

/** 一次装配AT所需的协议规则和字节通道，AT核心不接收电源操作表。 */
struct gt02_at_config {
	const struct gt02_at_profile *profile; /**< 必须提供，协议表及回调参数长期有效。 */
	const struct gt02_at_io_ops *io;       /**< 必须提供read/write，操作表长期有效。 */
};

/**
 * @brief 注册协议表和字节I/O操作，启动唯一AT线程，不上电、不发命令。
 * @param[in] config 不得为NULL，初始化期间有效；保存其中profile/io指针，不保留config本身。
 * @retval 0 已启动；不代表UART、模组或网络就绪。
 * @retval -EINVAL 缺少配置、I/O操作，或协议表/字符串/回调/规则无效。
 * @retval -EALREADY 本次开机已初始化或正在初始化；故障后也不允许重新初始化绕过失步。
 * @retval -EWOULDBLOCK 从ISR调用。
 * @return 其他负值为SDK解析器初始化失败，可修正配置后再次初始化。
 * @note 初始化前应先准备硬件，随后由上层组织power_on。不得同时启用其他RX消费者。
 *       profile及其成员、io操作表和回调参数必须覆盖核心生命周期；仅初始化时绑定。
 */
int gt02_at_init(const struct gt02_at_config *config);

/**
 * @brief 非阻塞入队一个命令标识，由AT线程串行发送并最终回调。
 * @param[in] command_id 已注册的固定命令标识，可重复提交，同标识请求仍按FIFO逐个处理。
 * @retval 0 入队成功；后续通知来自该命令表项，可能很快发生，不代表执行成功。
 * @retval -EACCES 尚未完成初始化。
 * @retval -EINVAL 未注册的标识，或参数化项没有提供本次文本。
 * @retval -ENOTSUP 表项标记reserved，仅供清单登记；未入队，不发送、不回调。
 * @retval -ENOBUFS 已有4条待执行请求，未保留本请求，不回调。
 * @retval -EPIPE 核心已因失步停止接受请求，需显式整机重启恢复。
 * @retval -EWOULDBLOCK 从ISR调用。
 * @note 仅线程调用，允许多个线程或AT回调调用；复制发送文本，不复制表中的user_data对象。
 *       核心故障时，在途请求报告原因，排队请求各回调FAILED，不自动重发或切电。
 *       硬件write没有接口级超时，响应预算不提供异常硬件下严格总耗时保证。
 */
int gt02_at_submit(uint16_t command_id);

/**
 * @brief 复制一条厂家已构造的参数化文本并入队，AT线程仍按注册规则完成一次回调。
 * @param[in] command_id 已注册且parameterized=true的命令标识。
 * @param[in] text 非NULL的NUL结尾ASCII文本，1..GT02_AT_COMMAND_MAX字节，不带CR/LF；
 *                必须以该表项command紧跟=开头，等号后至少有一个字符。
 * @retval 0 已复制并入队；text可立即修改或释放，不代表模组执行成功。
 * @retval -EINVAL 空指针/空文本、控制字符、非ASCII、固定项或命令名不匹配。
 * @retval -EMSGSIZE 文本超过GT02_AT_COMMAND_MAX字节，不截断、不入队。
 * @retval -ENOTSUP 对应表项仍为reserved，不允许绕过待接入限制。
 * @return 其他错误与gt02_at_submit一致；所有拒绝均不产生回调。
 * @note 仅线程调用，可并发；text在调用期间须保持有效且不被其他线程修改。
 *       厂家层负责参数范围及字符串转义检查，AT层不解析参数含义。
 *       仅支持普通文本事务，不含提示符、二进制载荷或异步业务结果处理。
 */
int gt02_at_submit_text(uint16_t command_id, const char *text);

#endif /* GT02_MODEM_AT_H_ */
