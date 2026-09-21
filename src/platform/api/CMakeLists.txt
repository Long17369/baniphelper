# 平台接口层：纯接口头文件，不含任何平台专有头。
#
# 核心层与界面层只允许通过本层调用平台能力；平台接口一共十个，
# 清单与职责见 docs/README.md 第 3.2 节，头文件在 S1.2 冻结。
#
# 本目标暂时只有包含根，S1.2 起携带接口头文件。

add_library(baniphelper_platform_api INTERFACE)
add_library(BanIPHelper::PlatformApi ALIAS baniphelper_platform_api)

target_include_directories(baniphelper_platform_api INTERFACE "${PROJECT_SOURCE_DIR}/src")
