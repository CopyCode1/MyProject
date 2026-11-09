/**
 * @file
 * @brief NT26-KCN厂家表及内部参数构造声明；服务只使用统一能力操作表。
 */
#ifndef GT02_MODEM_LIERDA_H_
#define GT02_MODEM_LIERDA_H_

#include "adapters/modem/gt02_modem_at.h"

/**
 * 厂家内部命令类型；前三项保留原编号。基础查询可submit，预留项返回-ENOTSUP。
 * 参数化项按类型登记，LGPIO须通过厂家私有构造函数提交，不按参数组合建立ID。
 * 本头文件供装配和厂家验证使用，服务层应依赖厂家无关的能力契约。
 */
enum gt02_lierda_command {
	LIERDA_CMD_AT,       /**< AT握手，成功只代表本次命令收到OK。 */
	LIERDA_CMD_ECHO_OFF, /**< ATE0关闭回显，接收核心仍容忍初始回显。 */
	LIERDA_CMD_CSQ,      /**< 查询RSSI和BER，成功后由回调检查数值。 */
	LIERDA_CMD_ATI,      /**< 查询厂商、型号和版本；多行文本，只展示、不自动选型。 */
	LIERDA_CMD_CPIN,     /**< 查询SIM状态；READY与SIM PIN等正文状态分别报告。 */
	LIERDA_CMD_CGSN,     /**< 查询IMEI；按旧缓存上限接受1..19位数字，不校验校验码。 */
	LIERDA_CMD_CIMI,     /**< 查询IMSI；按旧缓存上限接受1..19位数字。 */
	LIERDA_CMD_QCCID,    /**< 查询ICCID；允许20字符数字/大小写英文字母，保留原文及外层引号兼容。 */
	LIERDA_CMD_CGREG,    /**< 查询分组域注册；仅状态1/5表示本地/漫游注册。 */
	LIERDA_CMD_CCLK,     /**< 查询时钟原文；旧工程为条件功能，本版不设置系统时间。 */
	LIERDA_CMD_QIACT_QUERY, /**< 查询产品CID 1的IPv4上下文；无正文OK表示未返回目标上下文。 */
	LIERDA_CMD_RESET,    /**< 预留AT+RESET；需先定义AT核心失步/重启生命周期。 */
	LIERDA_CMD_QICSGP,   /**< 预留APN、用户名、密码和认证方式的参数模板。 */
	LIERDA_CMD_QIDEACT,  /**< 预留AT+QIDEACT=1，释放上下文。 */
	LIERDA_CMD_QIACT,    /**< AT+QIACT=1，激活产品CID 1；成功后仍须查询本地IP。 */
	LIERDA_CMD_QIOPEN,   /**< 预留TCP连接模板；OK不等于连接成功，仍需结果URC。 */
	LIERDA_CMD_QICLOSE,  /**< 预留按连接编号关闭TCP的模板。 */
	LIERDA_CMD_QISEND,   /**< 预留发送模板；需提示符、定长载荷和SEND OK/FAIL处理。 */
	LIERDA_CMD_QTTS,     /**< 预留TTS文本模板；需编码、长度检查及播放结束URC。 */
	LIERDA_CMD_LGPIO,    /**< 参数化LGPIO配置/电平控制，当前仅开放厂家内部构造入口。 */
	LIERDA_CMD_QTTSETUP, /**< 预留TTS参数类型，后续由厂家流程构造具体编号和值。 */
	LIERDA_CMD_CEREG,    /**< 查询LTE/EPS注册状态，1/5表示本地/漫游注册。 */
	LIERDA_CMD_COUNT,    /**< 命令类型数量，用于厂家验证存储容量，不是可提交的命令。 */
};

/** 长期有效的两张表；上下文归模组回调所有，不暴露可变的全局查询结果。 */
extern const struct gt02_at_profile gt02_lierda_at_profile;

/**
 * 厂家协议操作表，供装配层注入服务或专用验证入口。
 * init只绑定事件回调；AT配置/I/O仍由装配层初始化，硬件上电与启动等待由调用者组织。
 * 每次RDY上报STARTUP_READY，服务按阶段过滤重复；准备、查询和激活均以完成事件返回，不创建线程。
 * 数据查询只解释CID 1的IPv4结果；激活通过独立activate_data_context操作发起，不配置APN。
 * 激活完成不承诺IP已可用或TCP成功，服务须另行查询确认。
 * 必须在上电前绑定回调；RDY可能在power_on返回前到达。
 */
extern const struct gt02_modem_ops gt02_lierda_ops;

/**
 * @brief 厂家内部构造AT+LGPIO=pin,1并异步提交，供厂家流程及bring-up验证使用。
 * @param[in] pin 模组内部GPIO编号，当前只允许旧工程已使用的97或99。
 * @retval 0 请求文本已复制入队，不代表模组已配置成功。
 * @retval -EINVAL pin不在当前支持范围内。
 * @retval -EMSGSIZE 构造失败或文本截断，没有提交请求。
 * @return 其他负值来自gt02_at_submit_text，最终AT结果由LGPIO表项回调报告。
 * @note 仅线程调用，AT核心须已初始化；不控制MCU引脚、不等待完成、不自动重试。
 *       此声明属于厂家内部辅助，服务层不得直接依赖。
 */
int gt02_lierda_gpio_config(unsigned int pin);

/**
 * @brief 厂家内部构造AT+LGPIO=pin,1,level并异步提交，不作为公共服务能力。
 * @param[in] pin 模组内部GPIO编号，当前只允许97或99。
 * @param[in] high true构造level=1，false构造level=0，不推断音频电路的有效极性。
 * @retval 0 请求文本已复制入队，不代表模组已设置成功。
 * @retval -EINVAL pin不在当前支持范围内。
 * @retval -EMSGSIZE 构造失败或文本截断，没有提交请求。
 * @return 其他负值来自gt02_at_submit_text，最终AT结果由LGPIO表项回调报告。
 * @note 仅线程调用；允许连续提交不同参数，局部构造缓存不会被异步保留。
 */
int gt02_lierda_gpio_set(unsigned int pin, bool high);

#endif /* GT02_MODEM_LIERDA_H_ */
