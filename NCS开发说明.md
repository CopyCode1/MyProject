# GT02 — nRF Connect SDK 3.4.1

当前 NCS 开发入口：`E:\NRF52840\ncsproject\MyProject`。

本目录是从 `E:\NRF52840\zephyrproject\MyProject` 迁移出的应用副本，使用 `C:\ncs\v3.4.1` 的 SDK 和配套工具链。原工程与原构建目录保留。后续修改本目录源码，两个副本不会自动同步。

## VS Code 操作

1. 打开本目录。
2. 按 `Ctrl+Shift+B` 执行 `NCS: Build GT02`。
3. 连接 GT02 的 J-Link，执行任务 `NCS: Flash GT02 (J-Link)`。
4. 调试时选择 `NCS: Debug GT02 nRF52840 (J-Link)`，按 `F5`。此操作会下载固件并停在 `main`。

烧录与调试需要真实硬件连接；迁移过程中的编译验证不等于板上验证。

## 命令行操作

在本目录打开普通 PowerShell 即可。脚本会自动选择 NCS 3.4.1 的配套工具链：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\ncs.ps1 -Action Check
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\ncs.ps1 -Action Build
```

需要全量重新构建、烧录或查看 Kconfig 时，分别使用 `-Action Rebuild`、`-Action Flash`、`-Action Menuconfig`。

固件输出：`build_ncs_341/zephyr/zephyr.hex` 和 `build_ncs_341/zephyr/zephyr.elf`。

本轮保持单应用镜像，使用 `--no-sysbuild`。增加 MCUboot/OTA 时再单独设计 Sysbuild 和 Flash 分区。

## 4G：从硬件适配重新构建（2026-10-29）

板级DTS新增 `modem0` 接线节点与UART1默认/休眠引脚：MCU TX=P0.27、RX=P0.07，
115200无流控；电源=P0.31、PWRKEY=P1.08，均按旧板MCU端高有效描述。
UART0日志配置不变，UART1保留okay加延迟初始化。
旧AT识别、SIM/注册查询和网络服务代码已删除，按目标架构逐步重建。
仅开启`CONFIG_GT02_MODEM_HW=y`时，只准备未激活的POWER/PWRKEY。
**当前临时开启`CONFIG_GT02_MODEM_HW_TEST=y`**：初始化成功后自动启动硬件上下电测试和原始URC打印。
首次上电完成后，每10秒发起一次“下电→重新上电”；上下电等待计入周期，主循环和LED心跳继续运行。
UART1收到的完整文本行通过UART0日志输出为`4G RX: RDY`等；不发送AT、不解析URC含义，不接TCP。
测试出现上下电或在线RX错误时停止循环并打印原因，不自动恢复；验证结束后将测试开关改为`n`并重新构建/烧录。
硬件适配对外收敛为初始化、上电、硬件下电、读、写五个入口；GPIO、UART启停、接收等待及故障检查封装在内部。
上电先启用UART接收，再执行旧板硬件步骤，不等待AT；下电按当前板要求先切电再挂起UART，不代替模组正常关机。
接口、备份位置、当前范围和下一步见[4G重建第一步](docs/product/11_4G重建第一步_硬件适配.md)。

用户以前的ATI及查询日志仍是历史验证证据；
[S1识别](docs/product/09_4G上电与AT识别验证.md)和[状态查询](docs/product/10_4G_SIM信号与注册查询.md)保留为历史参考，旧开关不再适用。
IMU、Flash、LED及应用已有IMU轮询注释保持原样；本轮不恢复持续采样，也不启用Flash擦写。

四连接的软件职责、缓存、调度与线程方案见 [4G多连接分层架构设计](docs/设计文档/4G多连接分层架构设计.md)。
项目说明文档的中文名称及阅读入口见 [文档索引](docs/设计文档/文档索引.md)。

## 当前接入：LED心跳 + LSM6DSO FIFO + W25Q128JV检查与受限擦写自测

**当前 `CONFIG_GT02_FLASH_SELF_TEST=n`，不执行破坏性自测。** 已保留用户授权的外部Flash
最后4 KiB测试分区 `0x00FFF000～0x00FFFFFF`。只有重新启用自测并烧录后，
才会每次启动执行三轮擦写/读回比较，最后擦回全FF；原有数据不会恢复。
独立测试文件、分区保护、预期日志和验收步骤见 [Flash擦写自测](docs/product/07_外部Flash限定扇区擦写自测.md)。

2026-10-22：在NCS版 `MyProject` 恢复两个外设；旁边的 `hello_world_1` 和旧Zephyr工程未修改。复用已有IMU服务/适配器与Flash适配器，不新增业务层，不直接在应用中操作寄存器或引脚。

- `CMakeLists.txt`：编译入口、应用、LED适配器、IMU服务、IMU适配器和Flash适配器。
- `boards/others/gt02/gt02_nrf52840.dts`：集中描述固定硬件接线，包括UART0日志引脚、115200波特率、无硬件流控及默认控制台选择。
- `app.overlay`：保留I2C1/LSM6DSO、SPI2/W25Q128JV的启用覆盖和Flash测试分区，不再配置UART0。串口描述由overlay迁入板级DTS，没有改变实际接线和通信参数。
- `prj.conf`：启用I2C、SPI、Flash、SPI NOR、运行时SFDP、JEDEC及布局查询；主线程栈4096字节。`CONFIG_LSM6DSO=n`，六轴仅由现有自定义FIFO适配器管理。未重新加入之前的RC实验配置，也未改变DC/DC设置。
- `app/gt02_app.c`：LED初始化后，检查一次Flash，再初始化六轴；工作模式104 Hz，约每100 ms排空硬件FIFO，约每1秒打印最新六轴数据、该日志间隔内读取的样本数和LED心跳计数。
- 原Flash探测接口仅查询JEDEC ID、容量、擦除布局，并读取首尾各32字节。独立擦写测试接口当前关闭；未启用文件系统或格式化整片Flash。
- Flash检查失败时继续验证IMU；IMU初始化或轮询发生非溢出错误时停止采样，保留LED心跳；FIFO溢出报告数据丢失并在下一轮继续，不无限重新初始化。

日志保持用户当前选择的 **UART0：TX=P1.05、RX=P0.24，115200、8N1、无流控**；RTT关闭。用兼容板子电平的USB转串口接收TX并共地，不接RS-232电平，不通过串口电源脚向板子倒灌供电。

### 烧录后验收

1. 执行 `NCS: Flash GT02 (J-Link)`，它会先构建当前NCS应用。
2. 用串口查看从上电开始的完整日志。预期有 `External Flash JEDEC ID: ef 40 18`（或 `ef 70 18`）、`W25Q128JV ready`、`External Flash first/last 32-byte reads passed; no erase or program`、`LSM6DSO FIFO ready, WHO_AM_I=0x6c`。
3. 初始化完成后，LED约每1秒翻转；`counter`递增，`IMU samples=...`及加速度/角速度约每1秒更新。`samples`是本日志间隔内排空的样本数，不是硬件频率寄存器的读回值；读取时间及调度可能让计数波动。静止时加速度模长应约为9800 mm/s²，转动板子时角速度应变化。
4. 同时验证连接调试器和独立断电重启两种情况。此前脱机常亮问题没有明确根因；本轮外设接入不代表该问题已解决。若再出现Fault，保留此次ELF与完整串口日志，不用旧ELF解释新故障地址。

加入自测前的接入版本：NCS 3.4.1 / Zephyr 4.4.2编译通过，内部Flash占用48,624字节，RAM占用12,416字节。该版本没有应用Flash擦写调用；当前启用自测版本的验证结果见上述自测文档。Flash接线与只读检查细节见 [外部Flash验证](docs/product/06_外部Flash接入与只读验证.md)。

## 历史记录：2026-10-20计数 + LED心跳隔离实验

以下为当时IMU/Flash关闭、RTT及RC对照的历史说明，不代表上面的当前配置。

当前使用设备树 `led0` 作为不依赖RTT连接的运行指示。GT02的 `led0` 映射到P1.15绿色LED、低电平点亮；若另一款PCB连接不同，应调整该板的设备树，而不是修改应用代码。进入应用并初始化成功后立即点亮，每约1秒翻转一次（亮1秒、灭1秒，一个完整周期约2秒），同时递增并打印counter。主线程仍每100 ms调用一次 `k_sleep()`，保留此前的idle路径。

- `app/gt02_app.c` 只编排计数和LED心跳，计时变量在首次比较前明确初始化；GPIO初始化或翻转失败时记录错误并结束测试。
- `adapters/indicator/gt02_status_led_zephyr.c` 使用Zephyr GPIO API读取 `led0` 的引脚及有效电平。应用不直接依赖具体引脚或GPIO驱动头文件。
- `CMakeLists.txt` 编译入口、应用和LED适配器；IMU服务、IMU和Flash适配器不参与构建，原文件保留。
- `prj.conf` 保持GPIO和RTT启用，I2C、官方LSM6DSO、SPI、Flash和SPI NOR关闭。RC默认校准自动启用Sensor子系统和SoC内部温度传感器TEMP_NRF5；它不使用I2C，也不会启用六轴，因此不再强制设置 `CONFIG_SENSOR=n`。
- 自动加载的 `app.overlay` 将LSM6DSO、I2C1、W25Q128JV和SPI2保持disabled。
- 当前低频时钟从外部32.768 kHz晶振切换为片内RC（500 ppm）进行脱机对照；RTC系统定时器、LED/休眠逻辑、DC/DC、主线程栈和RTT连接配置不变。RC校准使用SDK默认设置，这是定位问题用的配置，不代表已经确定晶振故障或完成产品功耗验证。

首次加入或后续移除 `app.overlay` 时，执行 `NCS: Pristine Build GT02`，避免旧的overlay文件列表缓存影响结果。只修改已有overlay内容时，正常增量构建即可。

历史最小计数构建的链接器报告为Flash占用31,424字节、RAM占用12,544字节；此前“计数 + 六轴”构建为Flash占用37,340字节、RAM占用13,312字节。它们不代表当前LED心跳固件的资源占用，也不代表板上测试已通过。

2026-10-20 LED心跳固件编译通过：Flash占用31,860字节、RAM占用12,544字节。检查确认 `led0` 为P1.15低有效、IMU/Flash及其总线仍禁用，应用只编译入口、心跳应用和LED适配器。尚未执行烧录或板上验收。

上述31,860字节/12,544字节是切换RC之前的外部晶振版本记录。RC对照版本在 `prj.conf` 中设置 `CONFIG_CLOCK_CONTROL_NRF_K32SRC_XTAL=n`、`CONFIG_CLOCK_CONTROL_NRF_K32SRC_RC=y`、`CONFIG_CLOCK_CONTROL_NRF_K32SRC_500PPM=y`；构建后应检查生成的 `.config` 确实启用RC、禁用XTAL。恢复晶振对照时，将低频源改回XTAL，并移除RC及500 ppm覆盖，再重新编译。

2026-10-20 RC对照版本编译通过：Flash占用33,848字节、RAM占用13,952字节。已核对低频源为RC、精度配置为500 ppm，默认RC校准及片内温度传感器启用；IMU、外部Flash及其总线保持关闭。资源增量包含RC校准和片内温度驱动，不是重新启用了六轴。尚未执行烧录或脱机板上验收。

测试时先关闭独立RTT Viewer、断开额外的VS Code RTT连接，烧录后复位并让程序运行（若调试器停在main，需要继续运行）。不打开RTT Viewer观察LED至少60秒，再连接一个RTT接收端对比LED是否继续按相同节奏翻转；调试会话存在时，外部Viewer使用Existing Session。预期RTT输出 `LED heartbeat test: IMU and external Flash disabled`，随后counter递增、LED=ON/OFF交替；不应出现IMU/Flash初始化或Fault日志。不能仅凭没有日志认定程序正常，LED暂停也可能是调试器主动停住，需要同时确认会话状态。若复现异常，保留此次固件对应的ELF及完整Fault日志，旧固件地址映射不能复用。编译通过不代表硬件测试通过。

脱机RC对照需保持屏幕拔除，使用板子正常的独立电源：先烧录新HEX，再断电、断开J-Link后重新上电，观察LED至少60秒。若RC版本恢复翻转，只能优先定位到时钟/唤醒相关路径，不能直接判定晶振损坏；若仍常亮，应保留现场，用不复位、不下载的Attach检查是否进入Fault。不要同时修改DC/DC、NFC引脚或改成忙等，否则无法区分本次时钟变量的影响。

后续恢复外部Flash时，需要同步恢复应用调用、CMake源文件选择、prj.conf及app.overlay覆盖。原W25Q128JV只读诊断的接线与日志见 [外部Flash验证](docs/product/06_外部Flash接入与只读验证.md)。

详细说明与验证记录见 [NCS 迁移记录](docs/NCS3.4.1迁移记录.md)。
