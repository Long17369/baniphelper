# 核心层：平台无关。规则模型、规则求值、记录聚合、配置模型与编排都在这里。
#
# 铁律：本层不得包含任何平台专有头（windows.h、fwpmu.h、iphlpapi.h 等），
# 违反时构建失败（检查在 S1.10 加入）。

add_library(baniphelper_core STATIC
  version.cpp
)
add_library(BanIPHelper::Core ALIAS baniphelper_core)

# 以 src/ 为包含根，于是包含写法就是 core/xxx.h 与 platform/api/xxx.h，
# 依赖层次在 include 语句里直接可见。
target_include_directories(baniphelper_core PUBLIC "${PROJECT_SOURCE_DIR}/src")

target_compile_definitions(baniphelper_core PRIVATE
  BANIPHELPER_VERSION="${PROJECT_VERSION}"
)

baniphelper_enable_warnings(baniphelper_core)
