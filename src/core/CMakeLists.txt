# 核心层：平台无关。规则模型、规则求值、记录聚合、配置模型与编排都在这里。
#
# 两条硬约束：
#
# 1. 本层不得包含任何平台专有头（windows.h、fwpmu.h、iphlpapi.h 等）。
#    构建期检查在 S1.10 加入，违反即构建失败。
# 2. 本层可以依赖 platform/api 的接口，但 platform/api 反过来只允许依赖本层的
#    值类型头（types / rule / record / safety / config / capabilities / error / result），
#    **不得依赖本层的 I*.h 接口头**，否则就成了双向依赖。

find_package(Qt6 REQUIRED COMPONENTS Core)

add_library(baniphelper_core STATIC
  capabilities.cpp
  capability_check.cpp
  capability_set.cpp
  declared_capabilities.cpp
  error.cpp
  headers_selfcheck.cpp
  log.cpp
  platform_backend.cpp
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
target_link_libraries(baniphelper_core PUBLIC Qt6::Core)

baniphelper_enable_warnings(baniphelper_core)

# 本层不得包含任何平台专有头。这条约束由编译期与源码扫描两道检查共同强制，
# 见 cmake/BanIPHelperGuards.cmake。
baniphelper_forbid_platform_headers(baniphelper_core)
