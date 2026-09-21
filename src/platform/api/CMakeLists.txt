# 平台接口层：只描述「要做什么」，不描述「怎么做到」。
#
# 允许包含：core 的值类型头（types / rule / record / safety / config /
#          capabilities / error / result）与 Qt 的基础类型。
# 禁止包含：core 的 I*.h 接口头，以及任何平台专有头（windows.h、fwpmu.h 等）。
#
# 本目标的产物是头文件，但库里仍需要至少一个翻译单元，
# 于是用 headers_selfcheck.cpp 把全部接口头文件编译一遍；
# 它同时承担「接口必须是纯接口且不可复制」的静态断言。
# 后续 S8.1 的契约测试套件也挂在这里。

add_library(baniphelper_platform_api STATIC
  headers_selfcheck.cpp
)
add_library(BanIPHelper::PlatformApi ALIAS baniphelper_platform_api)

target_include_directories(baniphelper_platform_api PUBLIC "${PROJECT_SOURCE_DIR}/src")

target_link_libraries(baniphelper_platform_api PUBLIC BanIPHelper::Core)

baniphelper_enable_warnings(baniphelper_platform_api)

# 本层同样不得包含平台专有头：它是纯接口，一旦引了平台头就不再是纯接口了。
# 用 PRIVATE 加约束，因此不会传播到链接它的 platform/<os>/ 实现上去。
baniphelper_forbid_platform_headers(baniphelper_platform_api)
