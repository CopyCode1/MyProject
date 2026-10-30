/**
 * @file
 * @brief 4G硬件适配只公开初始化、上电、下电、读、写五个入口。
 *
 * 当前GT02旧板电源步骤集中在内部函数；不解析AT、不创建线程、不决定联网策略。
 * UART启停、缓存、唤醒和故障检测均为内部机制。
 */
#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/device.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/clock.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/sys/util.h>

#include "adapters/modem/gt02_modem_zephyr.h"

LOG_MODULE_REGISTER(gt02_modem_hw, LOG_LEVEL_INF);

#define MODEM_NODE DT_ALIAS(modem0)
#define MODEM_UART DT_PHANDLE(MODEM_NODE, uart)

BUILD_ASSERT(DT_NODE_HAS_STATUS_OKAY(MODEM_UART), "Enable the modem UART in board DTS");
BUILD_ASSERT(DT_PROP(MODEM_UART, zephyr_deferred_init), "Modem UART must use deferred init");
BUILD_ASSERT(!DT_PROP(MODEM_UART, hw_flow_control), "GT02 has no RTS/CTS wiring");

static const struct device *const uart_dev = DEVICE_DT_GET(MODEM_UART);
static const struct gpio_dt_spec supply = GPIO_DT_SPEC_GET(MODEM_NODE, power_gpios);
static const struct gpio_dt_spec pwrkey = GPIO_DT_SPEC_GET(MODEM_NODE, pwrkey_gpios);

RING_BUF_DECLARE(rx_ring, CONFIG_GT02_MODEM_UART_RX_SIZE);
K_SEM_DEFINE(rx_ready, 0, 1);
K_MUTEX_DEFINE(control_lock); /* 上下电与单次TX互斥；RX不等待该锁。 */
static struct k_spinlock rx_lock;
static atomic_t uart_active; /* UART可能已驱动引脚；失败后仍需尝试挂起清理。 */
static atomic_t io_ready; /* UART通道已启用；上电期间先接收，TX受control_lock约束。 */
static atomic_t rx_generation; /* 仅取消本地旧read，不用于识别线上迟到回复。 */
static atomic_t transport_error;

/* 内部硬件阶段用于区分未供电输入与有效接收窗口，不表示模组或网络就绪。 */
enum power_stage {
	POWER_UART_START,    /* 初始化/恢复UART，模组尚未开始本次供电步骤。 */
	POWER_BEFORE_SUPPLY, /* 供电使能前的等待和PWRKEY动作。 */
	POWER_SUPPLY_ENABLE, /* 正在使能供电及等待100 ms。 */
	POWER_START_PULSE,   /* 供电后的PWRKEY启动步骤。 */
	POWER_ACTIVE,        /* 上电接口成功返回后的阶段。 */
	POWER_CUT,           /* 主动切电及串口收尾。 */
};

static atomic_t power_stage;
static atomic_t uart_fault_reported;
static atomic_t pre_supply_line_errors;
static atomic_t pre_supply_error_flags;
static atomic_t pre_supply_discarded_bytes;
/* 以下首次UART错误快照由rx_lock保护，日志只在线程中输出。 */
static int uart_fault_status;
static int uart_fault_stage;
static uint32_t uart_fault_ms;
static bool prepared;
static bool supply_enabled; /* MCU最后成功设置的使能值，不是电压测量。 */
static bool uart_initialized;
static bool uart_suspended;

/* 在读写路径统一检查故障和就绪状态，不把这些状态额外暴露成公共接口。 */
static int io_status(void)
{
	int error = (int)atomic_get(&transport_error);

	return error != 0 ? error : (atomic_get(&io_ready) ? 0 : -EPIPE);
}

/* 只容忍尚未开始供电时的framing/break；负API错误、overrun及供电后错误仍失败。 */
static bool is_pre_supply_line_error(int stage, int error)
{
	return (stage == POWER_UART_START || stage == POWER_BEFORE_SUPPLY) &&
	       error > 0 && (error & ~(UART_ERROR_FRAMING | UART_BREAK)) == 0;
}

/* 供电步骤结束后在线程中汇总未供电噪声，不在ISR逐次刷日志。 */
static void report_pre_supply_input(void)
{
	unsigned int errors = (unsigned int)atomic_get(&pre_supply_line_errors);
	unsigned int bytes = (unsigned int)atomic_get(&pre_supply_discarded_bytes);

	if (errors != 0 || bytes != 0) {
		LOG_WRN("UART pre-supply input discarded: line_errors=%u flags=0x%02x bytes=%u",
			errors, (unsigned int)atomic_get(&pre_supply_error_flags), bytes);
	}
}

/* 在线程中输出本接收周期的首次UART错误；复制快照后释放自旋锁再打印。 */
static void report_uart_fault(void)
{
	static const char *const stages[] = {
		"uart_start", "before_supply", "supply_enable",
		"pwrkey_start", "active", "power_cut",
	};
	k_spinlock_key_t key = k_spin_lock(&rx_lock);
	int status = uart_fault_status;
	int stage = uart_fault_stage;
	uint32_t when = uart_fault_ms;

	if (status == 0 || !atomic_cas(&uart_fault_reported, 0, 1)) {
		k_spin_unlock(&rx_lock, key);
		return;
	}
	k_spin_unlock(&rx_lock, key);
	if (status < 0) {
		LOG_ERR("uart_err_check failed: %d at %u ms", status, (unsigned int)when);
		return;
	}
	LOG_ERR("UART RX fault: at=%u ms, stage=%s, flags=0x%02x",
		(unsigned int)when,
		stage >= 0 && (size_t)stage < ARRAY_SIZE(stages) ? stages[stage] : "unknown",
		(unsigned int)status);
	LOG_ERR("UART flags: overrun=%u parity=%u framing=%u break=%u",
		(unsigned int)((status & UART_ERROR_OVERRUN) != 0),
		(unsigned int)((status & UART_ERROR_PARITY) != 0),
		(unsigned int)((status & UART_ERROR_FRAMING) != 0),
		(unsigned int)((status & UART_BREAK) != 0));
}

/* ISR锁存首个错误及原始UART状态，停收并唤醒read；uart_status为0表示非UART标志错误。 */
static void rx_fault(const struct device *dev, int error, int uart_status)
{
	k_spinlock_key_t key = k_spin_lock(&rx_lock);

	if (atomic_cas(&transport_error, 0, error)) {
		uart_fault_status = uart_status;
		uart_fault_stage = (int)atomic_get(&power_stage);
		uart_fault_ms = k_uptime_get_32();
	}
	k_spin_unlock(&rx_lock, key);
	uart_irq_rx_disable(dev);
	uart_irq_err_disable(dev);
	k_sem_give(&rx_ready);
}

/* ISR只搬运原始字节；NUL、换行及OK样内容一律不在此解释。 */
static void uart_rx(const struct device *dev, void *user_data)
{
	uint8_t bytes[32];
	int count;
	int uart_error;
	int stage;

	ARG_UNUSED(user_data);
	if (!atomic_get(&io_ready)) {
		return;
	}
	stage = (int)atomic_get(&power_stage);
	/* 本机驱动读取时会清除错误源，只读一次并保留原始结果。 */
	uart_error = uart_err_check(dev);
	if (uart_error != 0) {
		if (is_pre_supply_line_error(stage, uart_error)) {
			atomic_inc(&pre_supply_line_errors);
			atomic_or(&pre_supply_error_flags, uart_error);
			/* 不停RX：继续读取FIFO，清掉无效字节并让驱动推进后续接收。 */
		} else {
			rx_fault(dev, uart_error < 0 ? uart_error : -EIO, uart_error);
			return;
		}
	}
	while (uart_irq_update(dev) && uart_irq_rx_ready(dev)) {
		count = uart_fifo_read(dev, bytes, sizeof(bytes));
		if (count <= 0) {
			break;
		}
		k_spinlock_key_t key = k_spin_lock(&rx_lock);

		if (!atomic_get(&io_ready)) {
			k_spin_unlock(&rx_lock, key);
			return;
		}
		if (stage == POWER_UART_START || stage == POWER_BEFORE_SUPPLY) {
			/* 本轮模组尚未供电，没有有效URC；不能把输入噪声拼进启动文本。 */
			k_spin_unlock(&rx_lock, key);
			atomic_add(&pre_supply_discarded_bytes, count);
			continue;
		}
		if (ring_buf_space_get(&rx_ring) < (uint32_t)count) {
			k_spin_unlock(&rx_lock, key);
			rx_fault(dev, -ENOBUFS, 0);
			return;
		}
		(void)ring_buf_put(&rx_ring, bytes, count);
		k_spin_unlock(&rx_lock, key);
		k_sem_give(&rx_ready);
	}
}

/* 生产者停收且持有control_lock时清理旧字节；通知重置也可能唤醒旧read。 */
static void reset_rx(void)
{
	k_spinlock_key_t key = k_spin_lock(&rx_lock);

	ring_buf_reset(&rx_ring);
	k_spin_unlock(&rx_lock, key);
	k_sem_reset(&rx_ready);
}

/* 持有control_lock时挂起UART并取消旧read；失败时锁存故障并保持禁止收发。 */
static int stop_uart_locked(void)
{
	k_spinlock_key_t key = k_spin_lock(&rx_lock);
	int ret;

	atomic_clear(&io_ready);
	atomic_inc(&rx_generation);
	k_spin_unlock(&rx_lock, key);
	k_sem_give(&rx_ready);
	if (!atomic_get(&uart_active)) {
		reset_rx();
		return 0;
	}
	atomic_clear(&uart_active);
	uart_irq_rx_disable(uart_dev);
	uart_irq_err_disable(uart_dev);
	ret = pm_device_action_run(uart_dev, PM_DEVICE_ACTION_SUSPEND);
	if (ret < 0) {
		atomic_set(&uart_active, 1);
		/* 驱动可能只完成部分挂起，不假设重新开中断就能恢复收发。 */
		(void)atomic_cas(&transport_error, 0, ret);
		k_sem_give(&rx_ready);
		return ret;
	}
	uart_suspended = true;
	reset_rx();
	k_sem_give(&rx_ready);
	return 0;
}

/* 持有control_lock时首次初始化或恢复UART，成功后才开放新接收周期。 */
static int start_uart_locked(void)
{
	int ret;

	if (!uart_initialized) {
		/* 初始化可能在改变引脚后失败，仍需记录UART可能活动，供后续清理。 */
		atomic_set(&uart_active, 1);
		ret = device_init(uart_dev);
		if (ret < 0 && ret != -EALREADY) {
			(void)atomic_cas(&transport_error, 0, ret);
			return ret;
		}
		if (!device_is_ready(uart_dev)) {
			(void)atomic_cas(&transport_error, 0, -ENODEV);
			return -ENODEV;
		}
		uart_initialized = true;
	} else if (uart_suspended) {
		ret = pm_device_action_run(uart_dev, PM_DEVICE_ACTION_RESUME);
		if (ret < 0) {
			return ret;
		}
	}
	uart_suspended = false;
	uart_irq_rx_disable(uart_dev);
	uart_irq_err_disable(uart_dev);
	atomic_set(&uart_active, 1);
	ret = uart_irq_callback_user_data_set(uart_dev, uart_rx, NULL);
	if (ret < 0) {
		(void)atomic_cas(&transport_error, 0, ret);
		(void)stop_uart_locked();
		return ret;
	}
	reset_rx();
	k_spinlock_key_t key = k_spin_lock(&rx_lock);

	atomic_inc(&rx_generation);
	atomic_clear(&transport_error);
	atomic_clear(&uart_fault_reported);
	atomic_clear(&pre_supply_line_errors);
	atomic_clear(&pre_supply_error_flags);
	atomic_clear(&pre_supply_discarded_bytes);
	uart_fault_status = 0;
	uart_fault_stage = 0;
	uart_fault_ms = 0;
	atomic_set(&io_ready, 1);
	k_spin_unlock(&rx_lock, key);
	uart_irq_err_enable(uart_dev);
	uart_irq_rx_enable(uart_dev);
	return 0;
}

/* 当前GT02旧板启动步骤集中在此；换模组时调整此适配，不改调用者。 */
static int board_power_on_locked(void)
{
	int ret;

	atomic_set(&power_stage, POWER_BEFORE_SUPPLY);
	k_sleep(K_MSEC(100));
	ret = gpio_pin_set_dt(&pwrkey, 1);
	if (ret < 0) {
		return ret;
	}
	k_sleep(K_MSEC(100));
	atomic_set(&power_stage, POWER_SUPPLY_ENABLE);
	ret = gpio_pin_set_dt(&supply, 1);
	if (ret < 0) {
		return ret;
	}
	supply_enabled = true;
	k_sleep(K_MSEC(100));
	atomic_set(&power_stage, POWER_START_PULSE);
	ret = gpio_pin_set_dt(&pwrkey, 0);
	if (ret < 0) {
		return ret;
	}
	k_sleep(K_MSEC(1000));
	ret = gpio_pin_set_dt(&pwrkey, 1);
	if (ret < 0) {
		return ret;
	}
	k_sleep(K_MSEC(200));
	return 0;
}

/* 准备未激活的控制脚；重复调用不破坏已经建立的硬件状态。 */
int gt02_modem_hw_init(void)
{
	int ret = 0;

	k_mutex_lock(&control_lock, K_FOREVER);
	if (prepared) {
		goto out;
	}
	if (!gpio_is_ready_dt(&supply) || !gpio_is_ready_dt(&pwrkey)) {
		ret = -ENODEV;
		goto out;
	}
	ret = gpio_pin_configure_dt(&pwrkey, GPIO_OUTPUT_INACTIVE);
	if (ret < 0) {
		goto out;
	}
	ret = gpio_pin_configure_dt(&supply, GPIO_OUTPUT_INACTIVE);
	if (ret == 0) {
		prepared = true;
		supply_enabled = false;
	}
out:
	k_mutex_unlock(&control_lock);
	return ret;
}

/* 先启用UART接收，再执行供电/PWRKEY步骤；不等待RDY、OK或网络就绪。 */
int gt02_modem_hw_power_on(void)
{
	int ret;
	bool rx_started = false;

	k_mutex_lock(&control_lock, K_FOREVER);
	if (!prepared) {
		ret = -EACCES;
	} else if (atomic_get(&io_ready)) {
		ret = io_status();
	} else if (supply_enabled || atomic_get(&uart_active)) {
		/* 不对部分上电的硬件盲目再打一遍脉冲，先显式下电处理。 */
		ret = (int)atomic_get(&transport_error);
		if (ret == 0) {
			ret = -EALREADY;
		}
	} else {
		atomic_set(&power_stage, POWER_UART_START);
		ret = start_uart_locked();
		if (ret < 0) {
			LOG_ERR("power_on failed: UART start error=%d", ret);
		}
		if (ret == 0) {
			rx_started = true;
			ret = board_power_on_locked();
			if (ret < 0) {
				LOG_ERR("power_on failed: GPIO/power sequence error=%d, stage=%d",
					ret, (int)atomic_get(&power_stage));
			}
			if (ret == 0) {
				/* 检查上电期间是否发生接收故障。 */
				ret = io_status();
				if (ret < 0) {
					LOG_ERR("power_on failed: RX/transport error=%d after power sequence", ret);
				}
			}
			if (ret < 0) {
				/* 保留首个错误并停收，避免留下错误的就绪状态。 */
				(void)atomic_cas(&transport_error, 0, ret);
				(void)stop_uart_locked();
			} else {
				atomic_set(&power_stage, POWER_ACTIVE);
			}
		}
	}
	if (rx_started) {
		report_pre_supply_input();
	}
	k_mutex_unlock(&control_lock);
	if (ret < 0) {
		report_uart_fault();
	}
	return ret;
}

/* 按当前板要求先切断模组供电，再挂起UART并释放PWRKEY；不执行AT正常关机。 */
int gt02_modem_hw_power_off(void)
{
	int ret;
	int pwrkey_ret;
	bool supply_cut = false;

	k_mutex_lock(&control_lock, K_FOREVER);
	if (!prepared) {
		ret = -EACCES;
		goto out;
	}
	atomic_set(&power_stage, POWER_CUT);
	if (supply_enabled) {
		ret = gpio_pin_set_dt(&supply, 0);
		if (ret < 0) {
			/* 切电失败不谎报已断电，也不继续关闭UART；读写返回锁存错误。 */
			(void)atomic_cas(&transport_error, 0, ret);
			k_sem_give(&rx_ready);
			goto out;
		}
		supply_enabled = false;
		supply_cut = true;
	}
	/* 供电关闭后立即停UART，不把2秒间隔放在供电与UART停机之间。 */
	ret = stop_uart_locked();
	pwrkey_ret = gpio_pin_set_dt(&pwrkey, 0);
	if (pwrkey_ret < 0) {
		(void)atomic_cas(&transport_error, 0, pwrkey_ret);
		if (ret == 0) {
			ret = pwrkey_ret;
		}
	}
	if (supply_cut) {
		/* 即使串口收尾失败，也保留真实切电后的2秒间隔。 */
		k_sleep(K_MSEC(2000));
	}
out:
	k_mutex_unlock(&control_lock);
	return ret;
}

/* 把等待和取字节收敛为一次read；旧通知不会重新开始整个超时预算。 */
int gt02_modem_hw_read(uint8_t *buffer, size_t capacity, size_t *received,
		      k_timeout_t timeout)
{
	if (received == NULL) {
		return -EINVAL;
	}
	*received = 0;
	if (buffer == NULL || capacity == 0) {
		return -EINVAL;
	}
	k_timepoint_t deadline = sys_timepoint_calc(timeout);
	atomic_val_t generation = atomic_get(&rx_generation);

	for (;;) {
		k_spinlock_key_t key = k_spin_lock(&rx_lock);
		int ret = generation != atomic_get(&rx_generation) ? -EPIPE : io_status();

		if (ret == 0) {
			*received = ring_buf_get(&rx_ring, buffer,
						MIN(capacity, (size_t)CONFIG_GT02_MODEM_UART_RX_SIZE));
		}
		k_spin_unlock(&rx_lock, key);
		if (ret != 0 || *received != 0 || K_TIMEOUT_EQ(timeout, K_NO_WAIT)) {
			if (ret != 0) {
				report_uart_fault();
			}
			return ret;
		}
		k_timeout_t remaining = sys_timepoint_timeout(deadline);

		if (K_TIMEOUT_EQ(remaining, K_NO_WAIT)) {
			return -EAGAIN;
		}
		ret = k_sem_take(&rx_ready, remaining);
		if (ret != 0 && ret != -EAGAIN && ret != -EBUSY) {
			return ret;
		}
		/* 唤醒/超时后重新检查数据、故障和代次；停机不会被误报为收到数据。 */
	}
}

/* 同步提交原始字节，返回实际提交数；不拼接命令或等待对端确认。 */
int gt02_modem_hw_write(const uint8_t *buffer, size_t length, size_t *written)
{
	int ret;

	if (written == NULL) {
		return -EINVAL;
	}
	*written = 0;
	if (buffer == NULL && length != 0) {
		return -EINVAL;
	}
	k_mutex_lock(&control_lock, K_FOREVER);
	ret = io_status();
	while (ret == 0 && *written < length) {
		uart_poll_out(uart_dev, buffer[*written]);
		++*written;
		ret = io_status();
	}
	k_mutex_unlock(&control_lock);
	return ret;
}
