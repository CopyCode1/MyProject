# GT02 从上游 Zephyr 迁移到 NCS 3.4.1

## 范围与目录

迁移目标是保留 GT02 自定义 PCB、原有层次和 IMU FIFO 采样功能，用已安装的 NCS SDK 与配套工具链完成构建，并提供可重复的 VS Code 操作入口。

| 用途 | 路径 |
|---|---|
| 原工程，保留供对照 | `E:\NRF52840\zephyrproject\MyProject` |
| 后续 NCS 应用开发目录 | `E:\NRF52840\ncsproject\MyProject` |
| NCS SDK | `C:\ncs\v3.4.1` |
| SDK 内 Zephyr | `C:\ncs\v3.4.1\zephyr`，版本 `4.4.2` |
| 本机匹配的工具链 | `C:\ncs\toolchains\4f5b6ad6dd` |
| 构建目标 | `gt02/nrf52840` |
| 新构建目录 | `build_ncs_341` |

原目录的父级包含上游 Zephyr 的 `.west`。新应用采用独立应用方式，位于该工作区之外，通过 `ZEPHYR_BASE` 选择 NCS。应用源码、board、binding、配置、文档和项目 skill 已复制，原 `build/`、`build_gt02/` 未复制。两个应用副本不会自动同步。

本轮未增加 BLE、MCUboot、OTA 或新的产品行为。已有六轴驱动继续使用 Zephyr 通用 I²C API。

## 构建入口怎样选择 SDK

`scripts/ncs.ps1` 执行以下过程：

1. 检查 SDK 的 `nrf/VERSION`，要求 NCS `3.4.1`。
2. 从 `C:\ncs\toolchains\toolchains.json` 查找与 `v3.4.1` 对应的工具链。
3. 根据该工具链的 `environment.json` 设置当前进程的 PATH、Python 和编译器环境。
4. 将 `ZEPHYR_BASE` 指向 NCS 的 `zephyr`，使用工具链内的 Python 执行 west。
5. 以 `gt02/nrf52840` 和 `--no-sysbuild` 构建应用，输出到 `build_ncs_341`。

脚本在子进程内设置环境，不修改系统环境变量，不依赖原工程的 `.venv`。SDK 位于 C 盘、应用位于 E 盘，因此从应用目录运行 west，避免 west 的相对路径检查跨盘失败。传入 CMake 的 SDK 路径使用正斜杠。

换安装位置时，可以给脚本传 `-SdkPath` 和必要的 `-ToolchainPath`。同时更新 VS Code 的 GDB、编译器和 SVD 路径。本次调试配置使用本机已有 `D:\Program Files\SEGGER\JLink_V964`。

## 修改的配置

| 文件 | 用途 |
|---|---|
| `scripts/ncs.ps1` | 检查环境、增量/全量构建、J-Link 烧录、menuconfig |
| `.vscode/tasks.json` | `Ctrl+Shift+B` 与各项 NCS 任务 |
| `.vscode/settings.json` | NCS 编译器、compile_commands 和自定义板搜索路径 |
| `.vscode/launch.json` | 新 ELF、NCS GDB 和 NCS 内 nRF52840 SVD |
| `.gitignore` | 忽略生成目录 |

项目 `CMakeLists.txt` 已含 `BOARD_ROOT` 设置。GT02 的 board 文件、DTS、binding、`prj.conf` 与应用源文件沿用原实现；由实际构建确认兼容性。

## 如何操作

在 VS Code 中打开新应用目录，按 `Ctrl+Shift+B` 执行默认任务 `NCS: Build GT02`。任务输出会显示 SDK、工具链、应用和构建路径。

完整重建选择 `NCS: Pristine Build GT02`。这只重建本应用的 `build_ncs_341`。

连接真实 GT02 板与 J-Link 后，可执行 `NCS: Flash GT02 (J-Link)`。该任务先构建再烧录。

需要断点调试时，选择 `NCS: Debug GT02 nRF52840 (J-Link)` 并按 `F5`。该配置会构建、下载、复位并停到 `main`。

使用 Nordic 面板的 Debug 时，当前绑定的是 `Launch MyProject/build_ncs_341`（`nrf-debug`）配置。其 `config` 和 `inferiors[].elfFile` 当前使用已存在的构建目录、ELF 绝对路径，分别是 `E:/NRF52840/ncsproject/MyProject/build_ncs_341` 和该目录下的 `zephyr/zephyr.elf`。这是本机配置；移动工程或更换构建目录后，要同步更新这两个路径。两套调试配置相互独立，不会继承彼此的 ELF 设置。此 Nordic 配置也会先执行 `NCS: Build GT02`，然后停在 `main`。

已安装的 Nordic 插件版本 `2026.9.2823` 在两个阶段处理 ELF 路径：先根据路径查找所属构建，再匹配 ELF 调试域。这两个阶段的变量展开规则不同：`${workspaceFolder}` 未在后一个阶段展开，可能报 `No main domain and ELF file found`；`${activeConfig}/zephyr/zephyr.elf` 未在前一个阶段展开，可能报 `Unknown build configuration`。不能仅验证后一阶段的变量替换就认定整个调试入口可用。本机用绝对路径避开这两处变量展开差异，不修改插件或构建生成文件。

当前开发配置在 `prj.conf` 中启用 `CONFIG_DEBUG_OPTIMIZATIONS=y`（`-Og`）和 `CONFIG_DEBUG_THREAD_INFO=y`。修改后必须重新构建，并以 `build_ncs_341/zephyr/.config` 和实际编译参数为准。前者处理插件的调试优化警告，后者支持线程识别；它们不能替代正确的 ELF 路径。发布前应使用单独的发布配置，并重新验证资源占用、时序和功耗。

若通过 Nordic 扩展的 Applications 视图新增构建配置，选择 SDK `v3.4.1`、匹配的 toolchain、自定义板 `gt02/nrf52840`，并关闭 Sysbuild。用独立构建目录，避免与其他 SDK 或 Sysbuild 构建缓存混用。当前目录同时提供可直接使用的 VS Code tasks。

命令行可在任意 PowerShell 中调用本应用脚本：

```powershell
cd E:\NRF52840\ncsproject\MyProject
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\ncs.ps1 -Action Check
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\ncs.ps1 -Action Build
```

## 迁移验收

编译验收检查实际 SDK、工具链、自定义板、设备树和 Kconfig，生成新的 ELF/HEX。硬件验收另外进行：

1. 烧录到同一块 GT02 板，连接 RTT，确认正常启动。
2. 确认 LSM6DSO 的 `WHO_AM_I=0x6c` 与 FIFO 初始化成功。
3. 静置与转动板子，检查加速度和角速度变化。
4. 工作档每个约 1 秒日志区间应得到接近 104 个完整样本；日志区间与 I²C 处理有耗时，不要求每次恰好 104。
5. 检查无持续 I²C 错误和 FIFO overrun；低速档及功耗另行验证。

2026-10-19 编译验收通过：

以下为迁移完成时的基线记录。后续增加的W25Q128JV只读诊断及新的资源占用见 [外部Flash接入验证](product/06_外部Flash接入与只读验证.md)。

| 检查项 | 结果 |
|---|---|
| 完整构建 | 通过，生成 ELF/HEX |
| 从原工程目录调用新构建脚本 | 通过，仍选择 NCS 的 SDK 与工具链 |
| NCS 生成版本 | `3.4.1`；Zephyr `4.4.2`；build `ncs-v3.4.1` |
| SoC 与板 | `CONFIG_BOARD_GT02_NRF52840=y`、`CONFIG_SOC_NRF52840_QIAA=y` |
| IMU 总线 | I²C 启用、400 kHz；官方 Sensor 驱动关闭 |
| 日志 | RTT console 与 RTT log backend 启用 |
| Flash / RAM | 37,256 B / 10,240 B，来自链接器报告 |
| 源码保持 | 17 个应用、板级、binding、CMake 和 prj.conf 文件逐一 SHA-256 对比一致 |
| VS Code 配置 | JSON 解析通过；GDB、J-Link server、SVD 路径存在 |
| 烧录配置查询 | `FlashInfo` 通过；runner 为 J-Link，设备 `nRF52840_xxAA`，速率 4000 kHz |

编译命令中没有引用原上游 Zephyr 工作区。未进行烧录、在线调试或硬件验收。只读查询烧录配置可以执行 `-Action FlashInfo`，不会连接并烧写硬件。

## 参考

- [Nordic：安装 NCS](https://nrfconnectdocs.nordicsemi.com/ncs/latest/nrf/installation/install_ncs.html)
- [Nordic：应用的组织方式](https://nrfconnectdocs.nordicsemi.com/ncs/latest/nrf/app_dev/create_application.html)
- [Nordic：构建应用](https://nrfconnectdocs.nordicsemi.com/ncs/latest/nrf/app_dev/config_and_build/building.html)

本机开发以已安装 `C:\ncs\v3.4.1` 的源码、文档和构建结果为准；上述 latest 页面可能随后续版本更新。
