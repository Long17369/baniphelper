# Windows 后端：本项目本期唯一的真实实现。
#
# 本目录是**唯一**允许包含 Windows 专有头的地方（windows.h、fwpmu.h、iphlpapi.h 等），
# 因此它不在平台隔离检查的受约束目录里。但两件事仍然成立：
#
#   1. 只通过 platform/api 的接口对外说话，不把平台类型漏出去；
#   2. 本目录不被其他平台实现引用，公共代码只能下沉到 core/ 或 platform/api/。
#
# 目标名固定为 baniphelper_backend_impl：每个平台实现都用这个名字，
# 上层的 platform/CMakeLists.txt 才知道该链谁。

add_library(baniphelper_backend_impl STATIC
  backend.cpp
  conn_monitor.cpp
  filter_engine.cpp
  filter_plan.cpp
  killer.cpp
  paths.cpp
  privilege.cpp
  single_instance.cpp
)

target_link_libraries(baniphelper_backend_impl PUBLIC
  BanIPHelper::Core
  BanIPHelper::PlatformApi
)

# shell32：提权重启用 ShellExecuteExW。
# fwpuclnt：WFP 用户态 API（FwpmEngineOpen0 等）。
# ole32：CoCreateGuid，给每条下发的过滤器取一个唯一标识。
# iphlpapi：SetTcpEntry，断连靠它删掉传输控制块。
# ws2_32：InetPtonW，把规则里的地址文本转成网络字节序的 in_addr。
target_link_libraries(baniphelper_backend_impl PRIVATE shell32 fwpuclnt ole32 iphlpapi ws2_32)

baniphelper_enable_warnings(baniphelper_backend_impl)
