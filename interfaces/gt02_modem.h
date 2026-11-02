/**
 * @file
 * @brief 4G统一能力契约：集中描述电源、字节I/O和厂家协议操作，按使用者分别注入。
 *
 * 本文件只定义能力，不操作GPIO、不创建线程，也不绑定具体硬件函数。
 * 当前为单模组、Zephyr工程接口，超时继续使用k_timeout_t；不承诺跨RTOS直接兼容。
 * 操作表及其实现启动时固定绑定，运行期间不得改写或释放。
 */
#ifndef GT02_MODEM_H_
#define GT02_MODEM_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <zephyr/kernel.h>

/**
 * @brief 等待并复制原始字节的实现接口，只能由唯一RX消费者在线程中调用。
 * @param[out] buffer 可写缓冲区，不得为NULL，容量至少capacity字节。
 * @param[in] capacity 容量，单位字节，必须大于0。
 * @param[out] received 实际复制字节数，不得为NULL；入口置0，失败保持0。
 * @param[in] timeout Zephyr超时对象；K_NO_WAIT仅尝试读取，有限等待不因空唤醒延长。
 * @retval 0 读取成功；K_NO_WAIT时也允许received为0。
 * @retval -EAGAIN 有限等待到期且无数据。
 * @retval -EPIPE 通道未就绪或本次read被停机取消。
 * @return 其他负值为参数、缓存溢出或传输错误，失败不交付部分数据。
 * @note 不拼行、不补NUL，不保留调用者指针；允许二进制字节。
 *       read必须能与电源控制及TX并行，不能等待贯穿整个上电过程的控制锁。
 */
typedef int (*gt02_modem_read_fn_t)(uint8_t *buffer, size_t capacity,
				  size_t *received, k_timeout_t timeout);

/**
 * @brief 同步提交原始字节的实现接口，仅在线程中调用。
 * @param[in] buffer 输入数据，length为0时允许NULL，不要求NUL结尾。
 * @param[in] length 提交字节数，允许0。
 * @param[out] written 实际提交数，不得为NULL；入口置0，失败可能已部分发送。
 * @retval 0 已提交length字节，调用者可以复用输入缓冲区。
 * @return 负值为参数、未就绪或传输错误，调用者必须结合written处理。
 * @note 不添加CRLF，不等待AT/平台响应；当前契约没有接口级TX超时。
 *       不保留输入指针；单次写与上下电互斥，由上层统一组织其调用顺序。
 */
typedef int (*gt02_modem_write_fn_t)(const uint8_t *buffer, size_t length, size_t *written);

/** 只向AT事务核心注入字节通道，不给它控制模组电源的能力。 */
struct gt02_at_io_ops {
	gt02_modem_read_fn_t read;   /**< 必须提供，读写的字节数/错误语义见对应类型契约。 */
	gt02_modem_write_fn_t write; /**< 必须提供，不能把正数字节数直接当作返回值。 */
};

/**
 * @brief 向生命周期调用者注入三种电源操作，不包含串口读写。
 *
 * 三个操作均无参数、仅线程调用，成功返回0、失败返回负错误码；返回0不是电压测量或联网保证。
 * 调用者串行组织生命周期，避免在途事务期间直接断电；实现负责真实GPIO、UART及板级时序。
 */
struct gt02_modem_power_ops {
	/** 准备未激活控制脚，不上电、不启动UART；重复成功初始化保持当前状态。 */
	int (*init)(void);
	/** 先建立接收通道再执行板级启动时序；同步完成，不等待RDY/AT/注册。
	 * 失败可能留有部分上电状态，不自动重试或回滚，具体错误及清理由实现说明。
	 */
	int (*power_on)(void);
	/** 硬件切电后停止UART并取消旧read，丢弃未处理输入；不代替模组AT正常关机。
	 * 切电失败不能谎报已关闭；UART收尾失败也应保留真实供电状态并报告错误。
	 */
	int (*power_off)(void);
};

/** 公共查询类型，不暴露厂家AT命令ID；首版仅实现SIM查询。 */
enum gt02_modem_query {
	GT02_MODEM_QUERY_SIM, /**< 查询SIM是否就绪或需要PIN/PUK，不执行解锁。 */
};

/** 公共SIM状态；查询失败必须配合事件error判断，不能把UNKNOWN当成无卡。 */
enum gt02_modem_sim_state {
	GT02_MODEM_SIM_UNKNOWN,      /**< 没有有效结果，或本次操作失败。 */
	GT02_MODEM_SIM_NOT_READY,    /**< 查询成功，模组明确报告尚未就绪。 */
	GT02_MODEM_SIM_READY,        /**< 查询成功，SIM就绪，不代表网络或TCP就绪。 */
	GT02_MODEM_SIM_PIN_REQUIRED, /**< 查询成功，需要PIN，不自动尝试解锁。 */
	GT02_MODEM_SIM_PUK_REQUIRED, /**< 查询成功，需要PUK，不自动尝试解锁。 */
};

/** 厂家层向唯一调用者交付的完成事件；首版不包含通道恢复或TCP/TTS事件。 */
enum gt02_modem_event_type {
	GT02_MODEM_EVENT_PREPARE_DONE, /**< 本次基础握手准备结束，结果见error。 */
	GT02_MODEM_EVENT_QUERY_DONE,   /**< 本次查询结束，类型和数据见query/sim。 */
};

/** 按值交付的厂家无关结果；所有字段均由产生事件的厂家实现填写。 */
struct gt02_modem_event {
	enum gt02_modem_event_type type; /**< 必须先检查的事件类型。 */
	enum gt02_modem_query query;     /**< 仅QUERY_DONE有效，表示公共查询类型。 */
	int error;                      /**< 0为协议操作及正文解析成功；其他值为负错误码。 */
	enum gt02_modem_sim_state sim;   /**< 仅SIM查询成功时有效，否则为UNKNOWN。 */
};

/**
 * @brief 在厂家解析完成后接收一个事件，首版由AT线程同步调用，不得阻塞等待。
 * @param[in] event 非NULL，仅回调期间有效；需要跨线程使用时复制整个结构。
 * @param[in,out] user_data init注册的长期上下文，可为NULL，不由厂家实现释放。
 * @note 回调没有返回值；可非阻塞投递事件，也可提交下一次协议操作。
 *       实现调用回调前已释放内部操作占用和锁。不得保存event指针或在回调内等待AT。
 */
typedef void (*gt02_modem_event_cb_t)(const struct gt02_modem_event *event, void *user_data);

/**
 * @brief 向服务或验证入口注入厂家协议能力；与电源操作表、AT字节操作表分开使用。
 *
 * 单模组、一次绑定、同一时刻只接受一个协议操作。上层不得混用此表与相同命令的原始AT提交，
 * 也不得同时运行旧bring-up控制者；首版事件没有请求编号。所有入口仅限线程调用。
 * init为同步绑定，prepare/query为异步操作；后两者返回0时可能已经发生回调，不代表业务就绪。
 */
struct gt02_modem_ops {
	/**
	 * @brief 绑定唯一事件接收者，不上电、不启动AT核心、不发送命令。
	 * @param[in] callback 必须提供的结果回调，覆盖整个开机生命周期。
	 * @param[in,out] user_data 长期有效的回调上下文，允许NULL。
	 * @retval 0 已绑定，尚未执行准备或查询。
	 * @retval -EINVAL callback为NULL。
	 * @retval -EALREADY 已绑定，不支持运行中更换或注销。
	 * @retval -EWOULDBLOCK 从ISR调用。
	 */
	int (*init)(gt02_modem_event_cb_t callback, void *user_data);
	/**
	 * @brief 异步执行厂家基础准备；当前Lierda实现为AT后ATE0。
	 * @retval 0 已接受，最终交付一次PREPARE_DONE。
	 * @retval -EACCES 尚未绑定回调，或AT核心尚未初始化。
	 * @retval -EBUSY 已有协议操作在执行。
	 * @return 其他负值为AT提交错误，未接受时不产生事件；ISR返回-EWOULDBLOCK。
	 * @note 调用者先完成硬件上电及所需启动等待；本方法不操作电源、不等待RDY/注册。
	 *       不自动重试，AT失败或第二步入队失败均通过已接受操作的完成事件报告。
	 */
	int (*prepare)(void);
	/**
	 * @brief 异步查询一种公共状态，厂家负责命令选择、正文解析和事件转换。
	 * @param[in] query 公共能力枚举，首版仅支持GT02_MODEM_QUERY_SIM。
	 * @retval 0 已接受，最终交付一次QUERY_DONE，数据仅在error为0时有效。
	 * @retval -ENOTSUP 查询类型尚未支持。
	 * @retval -EACCES 尚未绑定回调，或AT核心尚未初始化。
	 * @retval -EBUSY 已有协议操作在执行。
	 * @return 其他负值为AT提交错误，未接受时不产生事件；ISR返回-EWOULDBLOCK。
	 * @note 查询成功与SIM就绪分别表达；结果错误可为-ETIMEDOUT、-EBADMSG或-EIO。
	 *       EIO为当前AT失败的通用映射，不声称能区分模组拒绝和底层失步。
	 */
	int (*query)(enum gt02_modem_query query);
};

#endif /* GT02_MODEM_H_ */
