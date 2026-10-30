# W25Q128JV 外部 Flash 接入与只读验证

本文描述仍保持只读的 `gt02_flash_zephyr_probe()`。当前固件另启用了用户明确授权的
最后4 KiB扇区擦写自测，范围、风险与关闭方法见 [擦写自测](07_外部Flash限定扇区擦写自测.md)。
不能把本文“探测接口不擦写”理解为“当前启用自测的整份固件不擦写”。

## 本轮范围

用户已确认型号为 W25Q128JV。接线沿用旧 `board_screen_4g_spec.h` 和硬件资源表，仍需用本次固件在实板验证。旧 `efs_port_w25qxx.c` 接受的 JEDEC ID 为 `EF 40 18` 或 `EF 70 18`，两种变体都纳入识别范围。

当前代码在 `E:\NRF52840\ncsproject\MyProject`，使用 NCS 3.4.1 / Zephyr 4.4.2。

| 项目 | 配置 |
|---|---|
| Flash | Winbond W25Q128JV，128 Mbit / 16 MiB |
| 总线 | nRF52840 SPIM2，普通单线SPI，mode 0，8 MHz |
| SCK | P0.04 |
| MOSI | P0.25 |
| MISO | P0.28 |
| CS | P0.26，低有效 |
| DTS compatible | `jedec,spi-nor` |
| DTS alias | `ext-flash0`，C宏对应 `ext_flash0` |

内部 `flash0` 仍是程序启动存储器。外部Flash仅增加设备节点，未修改内部Flash布局，未增加MCUboot或产品存储分区。

## 各文件职责

- `boards/others/gt02/gt02_nrf52840.dts`：SPI2 pinctrl、片选、频率、Flash类型与标称容量。`size` 属性单位是 bit，因此 `0x08000000` 表示128 Mbit。
- `prj.conf`：启用 SPI、Flash、SPI NOR、运行时 SFDP、JEDEC查询接口和擦除块布局查询，并显式选择4 KiB布局粒度以保持旧工程的扇区语义。
- `adapters/storage/gt02_flash_zephyr.c`：调用 Zephyr Flash API 查询与读取，不自行编写SPI命令。检查ID是否属于旧实现兼容范围，并比较SFDP探测容量与DTS标称容量。
- `app/gt02_app.c`：启动时调用一次诊断；外部Flash诊断失败会输出警告，IMU验证继续运行。

运行时SFDP让官方驱动从芯片获取容量、编程页及擦除指令等参数，因此DTS不固定为两种变体中的某一个JEDEC ID。官方驱动初始化之后，适配器仍会明确校验ID与容量，不能把未知器件当成W25Q128JV继续使用。

当前适配器仅提供 `gt02_flash_zephyr_probe()`。这是一条板上验证路径，不为它再增加没有业务职责的storage转调层。后续明确参数、日志或OTA的用途与分区后，再由storage/OTA服务管理产品数据。

## 固件上电会做什么

1. Zephyr启动阶段初始化SPI NOR设备。
2. 应用检查 `device_is_ready()`。
3. 用 `flash_read_jedec_id()` 查询并校验芯片身份。
4. 用 `flash_get_size()` 获取实际容量，预期16,777,216字节。
5. 查询擦除块布局与最小写入对齐。
6. 读取地址0的32字节，以及Flash末尾的32字节，检查读取是否成功。

本轮没有 `flash_write()` 或 `flash_erase()`，也没有开启Flash Shell、NVS或文件系统自动格式化，不会覆盖原有用户数据。读取内容不打印到日志。读API成功仅说明这些读取事务完成，尚不能证明全片数据正确或擦写功能通过。

## 在板上验证

1. VS Code打开NCS版应用目录，执行 `NCS: Build GT02`。
2. 连接同一块GT02板与J-Link，执行 `NCS: Flash GT02 (J-Link)`。
3. 按当前配置打开UART0串口（TX=P1.05、RX=P0.24，115200 8N1，无流控），观察Flash诊断及后续IMU日志；RTT已关闭。

预期日志示例（ID也可能为 `ef 70 18`）：

```text
<inf> gt02_flash: External Flash JEDEC ID: ef 40 18
<inf> gt02_flash: W25Q128JV ready: size=16777216 bytes, erase_block=4096, write_align=1
<inf> gt02_flash: External Flash first/last 32-byte reads passed; no erase or program
```

这里的 `erase_block` 是Zephyr暴露的擦除布局单位；`write_align=1` 是最小写入对齐，不表示芯片编程页只有1字节。W25Q128JV的编程页为256字节，跨编程页的拆分由SPI NOR驱动处理。

若提示驱动未就绪，应同时查看更早的 `spi_nor` 日志，优先核对接线、电源、CS、WP#/HOLD#状态及SFDP读取情况。若芯片ID不在兼容范围，日志会保留实际ID；不能简单去掉校验以掩盖型号不符。

## 状态与下一步

2026-10-22：从LED隔离实验恢复本接入路径，保留UART日志、LED心跳和自定义IMU FIFO。重新构建通过：内部Flash占用48,624字节、RAM占用12,416字节。配置与生成设备树确认SPI2、W25Q128JV、运行时SFDP、JEDEC查询及4096字节布局启用；应用没有擦除/编程调用。此次固件尚未烧录或实板验收，不能用早期固件的正常日志替代本次验证；此前脱机常亮问题也未判定解决。

2026-10-19：NCS 3.4.1 / Zephyr 4.4.2 构建通过，生成 `build_ncs_341/zephyr/zephyr.elf` 与 `.hex`。链接器报告内部Flash占用48,028字节、RAM占用10,496字节。

已检查生成的设备树与 `.config`：SPIM2启用、CS为P0.26、频率8 MHz、标称容量128 Mbit、运行时SFDP与JEDEC查询启用、擦除布局4096字节；启动存储器仍为内部flash0。静态检查确认应用没有调用Flash写入/擦除，没有启用NVS、Settings、文件系统或Flash Shell自动存储路径。尚未实际烧录或获取板上日志；读取功能状态为IMPLEMENTED，未标记为板上VERIFIED。

后续用户已明确授权最后4 KiB用于有边界的擦写读回测试，实现与验收见上述独立自测文档。产品分区仍需按参数、日志、OTA等用途规划；没有把整片Flash格式化或作为OTA secondary slot。
