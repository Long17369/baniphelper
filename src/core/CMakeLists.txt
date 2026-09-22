# 核心层：平台无关。规则模型、规则求值、记录聚合、配置模型与编排都在这里。
#
# 两条硬约束：
#
# 1. 本层不得包含任何平台专有头（windows.h、fwpmu.h、iphlpapi.h 等）。
#    构建期检查在 S1.10 加入，违反即构建失败。
# 2. 本层可以依赖 platform/api 的接口，但 platform/api 反过来只允许依赖本层的
#    值类型头（types / rule / record / safety / config / capabilities / error / result），
#    **不得依赖本层的 I*.h 接口头**，否则就成了双向依赖。

# Sql：数据存储走 SQLite。Qt 自带的 QSQLITE 驱动把 sqlite3 静态编在插件里，
# 所以不需要额外引第三方库，也不需要单独分发 sqlite3.dll。
#
# Network：只用 QHostAddress 做 IP 地址的解析与比较。它是跨平台的纯值类型，
# 不涉及任何平台专有头，因此不违反本层的隔离约束；而自己写 IPv6 解析
# （压缩零段、内嵌 IPv4、十六进制大小写）容易在边界上出错，
# 地址解析错的后果是静默封错范围。
find_package(Qt6 REQUIRED COMPONENTS Core Sql Network)

add_library(baniphelper_core STATIC
  capabilities.cpp
  capability_check.cpp
  capability_set.cpp
  config_descriptors.cpp
  database.cpp
  declared_capabilities.cpp
  error.cpp
  headers_selfcheck.cpp
  json_config.cpp
  log.cpp
  main_thread_dispatch.cpp
  platform_backend.cpp
  rule_model.cpp
  types.cpp
  version.cpp
)
add_library(BanIPHelper::Core ALIAS baniphelper_core)

# 以 src/ 为包含根，于是包含写法就是 core/xxx.h 与 platform/api/xxx.h，
# 依赖层次在 include 语句里直接可见。
target_include_directories(baniphelper_core PUBLIC "${PROJECT_SOURCE_DIR}/src")

target_compile_definitions(baniphelper_core PRIVATE
  BANIPHELPER_VERSION="${PROJECT_VERSION}"
)

# 字符串与容器统一用 Qt，这是已冻结的决策：接口允许依赖 Qt；
# 平台实现调系统 API 时走 Unicode 系列，避免代码页不一致造成乱码。
#
# Sql 跟着 Core 一起 PUBLIC 出去：数据库是核心层的基础设施，
# 上层不需要各自再 find_package 一次。
target_link_libraries(baniphelper_core PUBLIC Qt6::Core Qt6::Sql)

# Network 走 PRIVATE：地址解析是核心层的内部实现细节，头文件里不出现 QHostAddress，
# 所以没必要把这个依赖传给上层。
target_link_libraries(baniphelper_core PRIVATE Qt6::Network)

baniphelper_enable_warnings(baniphelper_core)

# 本层不得包含任何平台专有头。这条约束由编译期与源码扫描两道检查共同强制，
# 见 cmake/BanIPHelperGuards.cmake。
baniphelper_forbid_platform_headers(baniphelper_core)
