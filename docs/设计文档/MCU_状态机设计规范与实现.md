# MCU 状态机设计规范与实现

> 适用范围：裸机、FreeRTOS、Zephyr 等 MCU 固件项目。  
> 核心原则：**状态明确、事件驱动、转换受控、行为可预测、异常有归宿。**  
> 设计取向：优先采用**轻量级、集中式、事件驱动**的状态机；不为抽象而抽象。

## 1. 什么是合格的状态机

状态机不仅是 `switch (state)`，而是把系统中原本隐含的行为规则明确化。

| 要求 | 含义 |
| --- | --- |
| 状态明确 | 状态表示系统当前所处的阶段，不混入瞬时事件 |
| 事件明确 | 状态变化由可识别的事件触发 |
| 转换受控 | 状态只有一个逻辑所有者，不允许模块间随意赋值 |
| 动作明确 | 每次转换需要执行的操作一目了然 |
| 异常完整 | 超时、失败、取消、断开都有处理路径 |
| 生命周期闭环 | 操作最终能成功、失败或被取消，不会无故悬置 |

最重要的是明确两件事：**谁拥有状态？谁有权改变状态？**

## 2. 先列状态、事件和转换表，再写代码

以 4G 模组的 TCP 连接管理为例：

### 2.1 状态定义

| 状态 | 含义 |
| --- | --- |
| `NET_IDLE` | 没有待建立的连接 |
| `NET_CONNECTING` | 已发起连接，等待异步结果 |
| `NET_CONNECTED` | 已确认 TCP 连接建立 |
| `NET_RETRY_WAIT` | 连接失败或断开，等待重试 |

### 2.2 事件定义

`NET_EVT_START`、`NET_EVT_STOP`、`NET_EVT_CONNECT_OK`、`NET_EVT_CONNECT_FAILED`、`NET_EVT_CONNECT_TIMEOUT`、`NET_EVT_REMOTE_CLOSED`、`NET_EVT_RETRY_TIMEOUT`。

**状态表示当前处境，事件表示发生了什么。** 例如 `NET_CONNECTED` 是状态，`NET_EVT_CONNECT_OK` 是事件，两者不能混用。

### 2.3 状态转换表

| 当前状态 | 事件 | 下一状态 | 动作 |
| --- | --- | --- | --- |
| `IDLE` | `START` | `CONNECTING` | 发起非阻塞 TCP 连接 |
| `CONNECTING` | `CONNECT_OK` | `CONNECTED` | 停止连接超时监控、通知成功 |
| `CONNECTING` | `CONNECT_FAILED` / `CONNECT_TIMEOUT` | `RETRY_WAIT` | 结束当前连接尝试、安排重试 |
| `CONNECTED` | `REMOTE_CLOSED` | `RETRY_WAIT` | 释放失效连接资源、安排重试 |
| `RETRY_WAIT` | `RETRY_TIMEOUT` | `CONNECTING` | 发起下一次连接 |
| 任意活动状态 | `STOP` | `IDLE` | 取消/关闭操作，清理相关资源 |

> 转换表描述的是**预期行为**。实现时还必须考虑异步取消、迟到回调及底层 API 的真实语义。

## 3. 推荐的轻量级代码结构

对这种规模的状态机，**一个核心事件处理函数 + 少数必要的动作函数**通常足够，不必上来就用函数指针状态表、状态对象或通用事件总线。

### 3.1 状态、事件和实例

```c
typedef enum {
    NET_IDLE,
    NET_CONNECTING,
    NET_CONNECTED,
    NET_RETRY_WAIT,
} net_state_t;

typedef enum {
    NET_EVT_START,
    NET_EVT_STOP,
    NET_EVT_CONNECT_OK,
    NET_EVT_CONNECT_FAILED,
    NET_EVT_CONNECT_TIMEOUT,
    NET_EVT_REMOTE_CLOSED,
    NET_EVT_RETRY_TIMEOUT,
} net_event_t;

typedef struct {
    net_state_t state;
} net_fsm_t;
```

### 3.2 核心处理逻辑（结构示意）

```c
/* 下列 net_* / modem_* 是示意接口，不是完整可运行的驱动实现。 */
static void net_enter_connecting(net_fsm_t *fsm)
{
    fsm->state = NET_CONNECTING;
    modem_tcp_open_async();
}

static void net_enter_retry_wait(net_fsm_t *fsm)
{
    fsm->state = NET_RETRY_WAIT;
    net_retry_timer_start();
}

void net_fsm_handle(net_fsm_t *fsm, net_event_t event)
{
    if (event == NET_EVT_STOP) {
        net_state_t old_state = fsm->state;
        fsm->state = NET_IDLE;

        switch (old_state) {
        case NET_CONNECTING:
            modem_tcp_cancel_open();
            break;
        case NET_CONNECTED:
            modem_tcp_close_async();
            break;
        case NET_RETRY_WAIT:
            net_retry_timer_stop();
            break;
        default:
            break;
        }
        return;
    }

    switch (fsm->state) {
    case NET_IDLE:
        if (event == NET_EVT_START) {
            net_enter_connecting(fsm);
        }
        break;

    case NET_CONNECTING:
        if (event == NET_EVT_CONNECT_OK) {
            fsm->state = NET_CONNECTED;
        } else if (event == NET_EVT_CONNECT_FAILED ||
                   event == NET_EVT_CONNECT_TIMEOUT) {
            modem_tcp_abort_open();
            net_enter_retry_wait(fsm);
        }
        break;

    case NET_CONNECTED:
        if (event == NET_EVT_REMOTE_CLOSED) {
            modem_tcp_release_closed();
            net_enter_retry_wait(fsm);
        }
        break;

    case NET_RETRY_WAIT:
        if (event == NET_EVT_RETRY_TIMEOUT) {
            net_enter_connecting(fsm);
        }
        break;

    default:
        break;
    }
}
```

这段示例是为了说明**结构**，并非可直接用于生产的连接管理器。实际实现至少还需补齐：底层调用即时失败、连接超时定时器启停、取消的完成语义、重复事件、请求身份标识以及错误上报。

推荐阅读顺序：

1. 找到当前状态对应的 `case`。
2. 找到事件分支。
3. 看执行的动作及下一状态。

**主函数负责展示流程，辅助函数只隐藏真正独立或复杂的动作。**

## 4. MCU 状态机的关键边界

### 4.1 单一状态所有权

不要让多个模块直接修改 `fsm->state`。外部模块只提交事件或发起请求，**由状态机自身决定是否转换**。

### 4.2 状态机不能长时间阻塞

错误的组织方式：

```c
case NET_CONNECTING:
    modem_send_at("AT+CIPOPEN=...");
    modem_wait_response(30000);  /* 长时间占用状态机执行上下文 */
    break;
```

更适合事件驱动的方式是：

1. 网络状态机提出连接请求；
2. AT 事务层异步执行命令并管理响应/超时；
3. 事务层通过适配层语义转换产出连接结果；
4. 网络状态机收到结果事件后决定下一步。

**注意：AT 指令返回 `OK` 不一定表示 TCP 已建立**；应依据具体模组协议确认最终成功条件。

### 4.3 处理迟到、重复和过期事件

典型竞态：

```text
T1: START         -> CONNECTING，开始连接尝试 A
T2: STOP          -> IDLE，取消 A
T3: CONNECT_OK(A) -> 迟到的旧事件
```

仅检查当前状态能过滤一部分旧事件，但还不够。若随后开始了连接尝试 B，旧的 `CONNECT_OK(A)` 可能被误认为 B 的结果。

对异步请求，推荐增加**操作代号（generation/request_id）**，仅接受与当前操作匹配的结果。取消完成后如果底层仍建立了旧连接，还需关闭它。定时器同样应防止旧超时事件误触发新一轮操作。

### 4.4 明确执行上下文和线程安全

- 尽量规定状态机由**单一线程/工作队列**串行处理事件。
- ISR、URC 回调及其他线程负责投递事件，不直接改状态。
- 若允许不同上下文调用，则必须设计锁或其他同步机制，不能只依赖 `volatile`。
- 定义事件队列满时的处理策略，避免关键断开事件丢失。

### 4.5 区分“生命周期状态”和“普通属性”

`CSQ`、SIM 就绪、网络注册、PDP 状态、IP 地址、TCP 状态不一定要合成为一个巨大的枚举。

**状态机管理一个清晰的生命周期；普通属性以独立数据表示。** 必要时将注册、PDP、TCP 拆为职责明确的子流程，而不是穷举所有组合状态。

## 5. 对应现有 4G 工程的职责建议

```text
gt02_network_service.c       网络连接状态机、重连策略、业务状态
          │ 请求 / 结果事件
          ▼
gt02_modem_at.c              AT 事务处理、响应匹配、超时与 URC 分发
          │
          ▼
gt02_modem_lierda.c          厂商 AT 指令语义适配
          │
          ▼
gt02_modem_zephyr.c          UART/GPIO/底层硬件交互
```

上图是**职责示意**，实际函数调用和事件回传方向可以双向，并不强制每个请求逐层穿过所有模块。

两个必须分开的状态概念：

- **AT 事务状态**：空闲、等待响应、完成、超时等；属于 AT 事务层。
- **网络连接状态**：空闲、连接中、已连接、重试等待等；属于网络服务层。

底层模块不应替网络服务做业务重连决策，网络服务也不应直接解析厂商返回的 AT 字符串。

## 6. 设计检查清单

- [ ] 每个状态是否能准确说明“系统现在在做什么”？
- [ ] 每个事件是否有明确来源和语义？
- [ ] 状态是否只有一个受控的修改入口？
- [ ] 关键状态 × 事件组合是否有确定行为？
- [ ] 超时、失败、取消、远端断开是否都能收敛？
- [ ] 是否避免在状态处理函数中长时间阻塞？
- [ ] 是否处理异步回调、重复事件、迟到事件和旧定时器？
- [ ] 连接、定时器、缓冲区等资源的所有权及释放时机是否明确？
- [ ] 新增状态或事件是否只需修改少量集中的代码？
- [ ] 能否利用事件序列对状态转换进行单元测试？

## 7. 最终原则

> **当前状态 + 输入事件 → 执行动作 + 下一状态。**

判断一个 MCU 状态机好不好，不是看用了多少设计模式，而是看：

1. 阅读者能否顺着一个清晰的主流程理解它；
2. 异常路径、资源所有权和生命周期是否完整；
3. 新增需求时，是否不会牵动大量无关模块。

对于中小型 MCU 控制流程，建议从**一个状态枚举、一个事件枚举、一个核心处理函数和少量辅助动作函数**开始，真正出现复杂性再增加抽象。
