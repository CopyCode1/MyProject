# 声明本工程要求的最低CMake版本；Zephyr 4.x满足这个要求。
cmake_minimum_required(VERSION 3.20.0)

# 把当前工程根目录加入Zephyr的板级搜索路径。
# Zephyr会在${CMAKE_CURRENT_SOURCE_DIR}/boards下查找我们的GT02自定义板。
# 这句必须位于find_package(Zephyr ...)之前，否则Zephyr初始化时无法发现自定义板。
list(APPEND BOARD_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

# 查找并加载Zephyr构建系统。
# REQUIRED表示找不到Zephyr时立即停止配置；ZEPHYR_BASE指向本机Zephyr源码目录。
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

# 定义应用工程名称；该名称会出现在CMake生成的工程信息中。
project(MyProject)

# 把src/main.c加入Zephyr自动创建的app构建目标。
# PRIVATE表示该源文件只属于当前app目标，不向其他CMake目标传播。
target_sources(app PRIVATE
    # 应用入口只负责进入产品装配层。
    src/main.c
    # 产品装配：LED心跳、IMU FIFO采样、外部Flash只读检查。
    app/gt02_app.c
    adapters/indicator/gt02_status_led_zephyr.c
    services/imu/gt02_imu_service.c
    adapters/imu/gt02_imu_zephyr.c
    adapters/storage/gt02_flash_zephyr.c
)

# 使组件头文件可使用从工程根目录开始的稳定路径。
target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# 官方Flash驱动仍由Zephyr选择；此处只按应用开关选择我们的测试文件。
target_sources_ifdef(CONFIG_GT02_FLASH_SELF_TEST app PRIVATE
    tests/bringup/gt02_flash_selftest.c
)

# 4G重建第一步只编译硬件底座，不依赖SDK内部AT头文件或网络服务。
target_sources_ifdef(CONFIG_GT02_MODEM_HW app PRIVATE
    adapters/modem/gt02_modem_zephyr.c
)

# 临时验证与硬件适配分开；关闭测试开关即移除测试线程和原始RX日志。
target_sources_ifdef(CONFIG_GT02_MODEM_HW_TEST app PRIVATE
    tests/bringup/gt02_modem_hw_test.c
)

# 保留下层AT及厂家能力；是否启动由装配入口决定，编译不表示自动上电。
if(CONFIG_GT02_MODEM_AT)
    target_sources(app PRIVATE
        adapters/modem/gt02_modem_at.c
        adapters/modem/gt02_modem_lierda.c
    )
    # SDK内部解析器仅由AT核心使用，不向上层传播其头文件路径。
    target_include_directories(app PRIVATE ${ZEPHYR_BASE}/drivers/modem)
endif()

target_sources_ifdef(CONFIG_GT02_MODEM_AT_TEST app PRIVATE
    tests/bringup/gt02_modem_at_test.c
)

# 检测服务只依赖统一操作表，不直接使用厂家命令或硬件引脚。
target_sources_ifdef(CONFIG_GT02_NETWORK_SERVICE app PRIVATE
    services/network/gt02_network_service.c
)
