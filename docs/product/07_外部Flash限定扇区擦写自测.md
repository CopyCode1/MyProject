# W25Q128JV 限定扇区擦写自测

## 授权与风险

2026-10-22，用户明确同意覆盖外部W25Q128JV的最后一个4 KiB扇区：

- 相对外部Flash的起始偏移：`0x00FFF000`。
- 长度：`0x1000`（4096字节）。
- 最后一个字节：`0x00FFFFFF`。

该区域原有数据将被永久覆盖，不做备份和恢复。未授权测试其他区域，也不测试内部Flash。
这是板上功能验证，不是全片扫描、耐久测试、掉电恢复测试或产品数据存储实现。
编译和下面的负向编译检查不会连接硬件；只有用户烧录并运行测试固件后才真正擦写。

## 文件与开关

| 文件 | 职责 |
|---|---|
| 根目录 `Kconfig` | 声明 `GT02_FLASH_SELF_TEST`，默认n，要求Flash、SPI NOR、布局和Flash map可用 |
| `prj.conf` | 当前经授权设为 `CONFIG_GT02_FLASH_SELF_TEST=y`，并启用 `CONFIG_FLASH_MAP=y` |
| `app.overlay` | 在外部Flash下保留 `flash_test_partition`，偏移/长度单位均为字节 |
| `CMakeLists.txt` | 用 `target_sources_ifdef()` 控制是否编译测试源文件 |
| `tests/bringup/gt02_flash_selftest.c/.h` | 独立测试入口、边界校验、有限轮次和结果日志 |
| `app/gt02_app.c` | 只读身份检查成功后执行测试，再初始化IMU、进入心跳/采样循环 |

没有修改Zephyr驱动、板级引脚、UART、DC/DC或时钟源。原来的
`gt02_flash_zephyr_probe()` 保持只读；擦写只存在于独立自测路径。

`Kconfig`默认n不代表当前固件关闭：`prj.conf`明确设置y，构建结果以
`build_ncs_341/zephyr/.config` 为准。

## 每次启动的执行顺序

1. LED初始化并点亮。
2. 原Flash只读检查：ID、容量、布局、首尾读取。
3. 打开测试分区，核对设备、固定授权范围、16 MiB容量、4 KiB擦除对齐、写入对齐和擦除值。
4. 第1轮：擦除并逐字节检查全FF；分块写满全00，读回整个扇区逐字节比较。
5. 第2轮：重新擦除并检查；写满交替AA/55，读回比较。
6. 第3轮：重新擦除并检查；写满随分区偏移及页位置变化的模式，读回比较。
7. 最后再擦除一次，并确认整个扇区全FF。
8. 初始化六轴，进入正常FIFO轮询及LED心跳。

正常完成共4次扇区擦除、3次整扇区写入。单次写入512字节，覆盖多个编程页，
由官方SPI NOR驱动拆页。两个静态缓冲区共1024字节，不占用1 KiB主线程栈。
测试期间IMU尚未启动，LED可能暂时保持亮，待测试结束后开始正常翻转。

一次启动最多调用一次，重复调用返回 `-EALREADY`。每次重新上电/复位都会重新执行，
因此测试完必须关掉开关并烧录新固件，不能长期保持测试固件反复启动。

## 范围保护与失败处理

- 编译期要求分区属于 `ext-flash0`，且偏移与长度必须精确匹配本轮授权。
- 运行期再次检查设备、偏移、大小、容量及对齐；所有检查都在第一次擦除之前完成。
- 所有擦写通过 `flash_area_*()`，偏移相对分区，不直接写整片Flash的绝对偏移。
- `flash_area_*()`不能替代正确分区规划，也无法阻止未来其他代码绕过分区直接写整片Flash。
- API错误或校验不一致时停止此次自测，不自动重试、不执行额外清理擦除。
- 校验失败会记录轮次、阶段、分区内偏移、预期字节、实际字节；API错误打印错误码。
- 自测返回错误后，应用记录错误并继续IMU与LED验证；这不表示Flash测试通过。
- 掉电或失败可能留下部分测试数据；只有打印最终PASS才表示最后的全FF检查通过。
- 传感器/供电/硬件故障造成的驱动阻塞或系统Fault不在应用返回码处理的保证范围内。

## 板上验收

1. 在NCS版 `MyProject` 执行构建，确认最终 `.config` 中自测开关为y。
2. 再确认只允许覆盖的区域正是上面的最后4 KiB，关闭任何其他访问Flash的程序。
3. 执行 `NCS: Flash GT02 (J-Link)`。本固件运行后就会覆盖该扇区，不存在第二次交互确认。
4. 使用当前UART0日志：TX=P1.05、RX=P0.24，115200 8N1、无流控、共地。UART0的引脚、波特率与控制台选择已迁入板级 `gt02_nrf52840.dts`，不再由 `app.overlay` 配置；测试分区仍留在overlay。
5. 查看完整启动日志，预期关键输出：

```text
gt02_flash_test: DESTRUCTIVE self-test: external Flash [0x00fff000, 0x00ffffff], size=4096; old data will be lost
gt02_flash_test: round=1 pattern=zero: erase/write/readback PASS (4096 bytes)
gt02_flash_test: round=2 pattern=aa55: erase/write/readback PASS (4096 bytes)
gt02_flash_test: round=3 pattern=address: erase/write/readback PASS (4096 bytes)
gt02_flash_test: Flash self-test PASS: 3 patterns; final sector is all 0xff
```

后续应看到IMU初始化、counter和六轴数据；既要检查API返回码，也要看到最后一条PASS。
之前的脱机常亮问题不能靠增加此测试判定解决；出现Fault时保留此次ELF和完整UART日志。

## 关闭与构建验证

验收后将 `prj.conf` 改为：

```ini
CONFIG_GT02_FLASH_SELF_TEST=n
```

重新构建和烧录。测试源文件与调用随之排除；分区可以保留，`FLASH_MAP=y`本身不会擦除数据。
仅修改配置而不烧录，不会改变板子正在运行的固件。

也可在NCS命令行环境中构建隔离的关闭版本，不改变常用构建目录：

```powershell
west build --no-sysbuild -b gt02/nrf52840 -d build_ncs_341_flash_selftest_off . -- '-DEXTRA_CONF_FILE=tests/bringup/flash_selftest_off.conf' '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON'
```

负向编译夹具 `tests/bringup/fixtures/flash_test_bad_offset.overlay` 故意把偏移移至
`0x00FFE000`。以 `EXTRA_DTC_OVERLAY_FILE` 加载它时，测试源文件必须因授权偏移的
`BUILD_ASSERT`编译失败；该夹具不得用于烧录，不是另一个可测试区域。

## 验证记录

当前启用版本已在NCS 3.4.1 / Zephyr 4.4.2构建通过：内部Flash 50,828字节，RAM 13,440字节。
关闭版本构建通过：内部Flash 48,624字节，RAM 12,416字节；已从编译列表和ELF确认测试源文件及入口符号被排除。
负向编译按预期失败，明确报告 `Flash test offset differs from the approved region`，验证编译期偏移保护。
已检查当前常用构建目录中的自测/Flash map开关、最终分区偏移与大小、条件源文件选择、
ELF测试入口以及两个各512字节的静态缓冲区。当前可烧录输出仍是启用自测的 `build_ncs_341`，
没有把负向夹具或关闭版本混入正常构建。
尚未自动烧录，尚无此次自测的实板擦写结果。
