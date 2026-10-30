# 4G最小实现：SIM、信号与注册状态查询

> 历史归档说明（2026-10-29）：本页所述旧查询代码和开关已清理，保留作行为、故障与旧实现参考；不代表当前固件仍执行查询。当前入口见[4G重建第一步](11_4G重建第一步_硬件适配.md)。

> 日期：2026-10-23。正常AT/ATI通信已由用户日志确认，实物为Lierda NT26-KCN，
> 固件为 `NT26KCNF20NNA-Q02011622`。本页新增状态查询尚待实板验证。

## 1. 范围与调用关系

不新增产品C源文件、目录或线程，不改变GPIO、UART、DTS、Flash或IMU功能。
继续复用现有服务线程和接收解析线程：

```text
服务线程：上电 → AT/ATE0 → ATI → 限时循环查询
                                  ├─ CPIN?：SIM未就绪则不继续本轮CSQ/CGREG
                                  ├─ CSQ：保留原始信号与误码率等级
                                  └─ CGREG?：沿用旧工程的注册查询
接收线程：UART缓存 → SDK AT解析 → 数据回调 → 最终响应通知
```

本轮不配置APN、不激活PDP、不建立TCP，不输入PIN/PUK，不反复上电、不实现自动恢复。
查询结束后不持续跟踪掉网，`REGISTERED`只是本轮查询结果，不是实时可用性承诺。
当前应用中已经注释的IMU轮询保持原样；“不阻塞主循环”不等于此次已验证IMU持续采样。

## 2. 为什么先用CGREG？

旧 `appcom/netmod/netmod_module_lierda.c` 的命令描述明确使用
`AT+CPIN?`、`AT+CSQ`、`AT+CGREG?`，注册判断使用stat为1或5。
本轮优先复现这个最小查询路径，而不是擅自切到另一套厂商命令。

标准定义中，CGREG查询GPRS注册，CEREG查询EPS注册，两者不是同义词。
本轮日志特意写明 `legacy CGREG registered`，不能仅凭它承诺LTE数据业务已经可用。
公开检索未取得与该实物固件完全匹配的NT26 AT手册，命令依据是旧工程，通用字段依据是
[ETSI TS 127 007](https://www.etsi.org/deliver/etsi_TS/127000_127099/127007/18.08.00_60/ts_127007v180800p.pdf)
第8.3、8.5、9.2.1、10.1.20及10.1.22节；具体固件差异仍需实板确认。
后续如要严格判断LTE/EPS状态，再根据对应手册和实测单独接入CEREG，不静默替换。

## 3. 简单的接口与分工

| 文件 | 新增职责 |
|---|---|
| `interfaces/gt02_modem.h` | SIM枚举、CGREG枚举、值快照和一个`gt02_modem_query_status()`接口 |
| `adapters/modem/gt02_modem_lierda.c` | 三条查询及响应校验；标准SIM CME状态映射 |
| `services/network/gt02_network_service.c/.h` | 间隔、预算、状态保存和结束条件 |
| `Kconfig`、`prj.conf` | 两个时间参数，继续使用原4G开关 |

查询接口同步返回0只表示查询成功，必须再检查字段：

- `sim`：未知/暂忙、READY、无卡、PIN、PUK或其他锁定。
- `rssi`：原始CSQ等级0～31，99是未知；没有伪造为dBm，也不设置任意联网阈值。
- `ber`：原始误码率等级0～7，99是未知。
- `signal_valid`：RSSI为0～31时为true，包括最低等级0，不表示信号一定足够。
- `registration`：只接受本轮旧格式stat为0～5；1为本地注册，5为漫游注册，3为拒绝。
- `registration_valid`：有合法查询数据行时为true；收到OK本身不会置true。

SIM未READY时，信号/注册保持未知且valid为false。
接口失败时输出也保持未知，不返回看似成功的部分结果；服务保留上一次成功快照。
服务快照的 `has_status=false` 表示还没有有效查询结果，此时不要解释嵌套modem字段。

## 4. 结束条件与错误处理

默认 `CONFIG_GT02_NETWORK_WAIT_TIMEOUT_MS=60000`，
`CONFIG_GT02_NETWORK_POLL_INTERVAL_MS=2000`，均从ATI完成后开始使用。
每轮最多三条查询，各自使用原 `CONFIG_GT02_MODEM_AT_TIMEOUT_MS=3000`。
60秒是开始新查询的总预算，已经在执行的一轮可能在预算后结束，最多再受三条AT超时约束及调度影响。
这不是绝对60秒硬实时保证，也不是60秒后重启模组。

| 情况 | 服务处理 |
|---|---|
| CGREG为1/5 | `REGISTERED`结束，明确提示APN/PDP/TCP未测 |
| 明确无卡、PIN/PUK或其他锁定 | `SIM_NOT_READY`结束，不自动解锁或重启 |
| SIM暂忙、未注册、搜索中、状态未知 | 在总预算内间隔查询 |
| CGREG为3 | `REGISTRATION_DENIED`结束 |
| 总预算用尽 | `WAIT_TIMEOUT`，error为-ETIMEDOUT；不主动停接收或断电 |
| AT超时、普通ERROR、数据缺失/非法、UART故障 | `FAILED`，保存错误并终止接收，不继续发下一条 |

`+CME ERROR`数字10/11/12/14，或对应标准文本，分别表示无卡/PIN/PUK/暂忙；
只有CPIN查询将它们转换成SIM结果。13（SIM failure）不被错误转换为无卡。
这些明确SIM状态不锁存为传输故障，交服务处理；其他CME仍按错误结束。
未强制改写CMEE设置：若当前固件只返回普通ERROR，本轮报告FAILED，不猜测是不是缺卡。

解析回调先记录数据或格式错误，等最终OK/错误响应后再返回事务结果；不抢先发下一条。
CGREG单字段URC或带引号位置字段的URC不当查询回复；本轮不主动开启注册URC，
只支持查询前两字段n为0～2、stat为0～5，后续位置扩展不使用。
完整多连接URC管理仍是后续工作，不把这轮当成通用modem驱动。

## 5. 烧录后看什么？

正常示意，非本轮实际采集日志：

```text
gt02_modem: TX: AT+CPIN?
gt02_modem_uart: RX raw: +CPIN: READY
gt02_modem_uart: RX raw: OK
gt02_modem: TX: AT+CSQ
gt02_modem_uart: RX raw: +CSQ: 20,99
gt02_modem_uart: RX raw: OK
gt02_modem: TX: AT+CGREG?
gt02_modem_uart: RX raw: +CGREG: 0,1
gt02_modem_uart: RX raw: OK
gt02_network: SIM: READY
gt02_network: CSQ: rssi=20 (known), ber=99; CGREG: 1 (home)
gt02_network: Status check PASS: legacy CGREG registered; APN/PDP/TCP NOT tested
```

保持原开关 `CONFIG_GT02_MODEM_BRINGUP=y` 即执行识别和本轮查询，关闭它仍不编译三个4G源文件。
按原任务构建和烧录，先保留从冷启动开始的UART0日志；确认LED心跳不停止、原始RX按完整行显示。
Flash SFDP错误是独立问题，本轮没有修改Flash驱动或执行擦写自测。

## 6. 验证记录与后续验收

NCS 3.4.1 / Zephyr 4.4.2、`gt02/nrf52840`、原 `build_ncs_341` 单镜像构建通过：
Flash 58348 B、RAM 25408 B；相对完整行RX日志基线增加2128 B和64 B，没有新增线程或产品C源文件。
已核对新时间配置为60000/2000毫秒，原始RX日志仍开启，Flash破坏性自测仍关闭。
已检查新函数的中文用途注释及头文件契约；构建仍有原MODEM_CONTEXT实验性API提示，未屏蔽。
没有自动烧录，也未执行真实硬件或协议输入注入测试。
下列是待执行的实板/输入注入验收，不表示已经测试通过：

- 正常卡：先搜索再注册，本地1和漫游5均可结束；CGREG仍不能代替PDP/TCP验证。
- 无卡、PIN、PUK、暂忙：核对实际CPIN或CME文本，不输入密码，不反复上电。
- 信号99、99：保留未知，不能显示成正常信号；等级0应保留且valid=true。
- 只有OK没有数据行：不能报PASS；缺OK则按AT超时退出。
- 非法数字、超长数字、缺字段、非法状态：不能溢出、数组越界或伪造已注册。
- 逐字节分片、同批多行、URC交错：状态与最终响应正确配对，原始日志不逐字符刷屏。
- 查询超时、UART错误：停止本轮，应用主循环继续；总等待预算用尽不触发重启。

下一步在这轮实板结果确认后做单路TCP回显，再扩展四路；本轮不预建四路缓存或调度器。

## 7. CPIN误判的边界修复（2026-10-23）

用户日志显示 `+CPIN: READY` 和OK均正常收到，但随后服务报-77（EBADMSG）；
此前出现的-140（ECANCELED）是服务失败后调用abort产生的清理状态，不是根因。

原因是当前SDK解析路径的零参数规则只按行长度覆盖match_buf，不自动补NUL。
新加入的字符串比较读到了之前ATI长回复的残留尾部，将正确READY误判为格式错误。
例如复用缓存可能形成 `+CPIN: READY26KCNF20NNA-Q02011622`。

本次仅在 `gt02_modem_lierda.c` 增加私有 `response_text()`，由CPIN、CSQ、CGREG、
CME四个零参数非DIRECT回调统一使用。先校验容量，再在“匹配前缀长度+回调len”处补NUL，
返回参数起点后才做字符串解析；SDK给出的len不是整行长度，不能直接写match_buf[len]。
只修改解析副本，不修改SDK源码、UART原始数据、接线、上电时序、查询间隔或服务策略。
ATI原本按显式长度复制，OK等回调不依赖字符串尾部，因此没有扩大修改范围。

构建通过：Flash 58412 B、RAM 25408 B，相对修复前增加64 B Flash，静态RAM不变。
主机侧18项缓冲区边界模型检查通过，覆盖长回复后短回复、四类前缀、空参数、255字节行和非法长度。
这些检查用主机数组模拟边界操作，不是直接执行C回调，也不是完整SDK解析/线程集成测试。
尚未自动烧录或实板复测；下一次应确认READY后的流程能够继续发送AT+CSQ和AT+CGREG?。
