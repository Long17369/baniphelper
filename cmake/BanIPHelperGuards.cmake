# 平台隔离的构建期强制手段（阶段一 S1.10）。
#
# 铁律：`src/core/` 与 `src/ui/` 不得包含任何平台专有头，平台差异只能出现在
# `src/platform/` 下。依据见 docs/architecture.md 第 12.2 节。
#
# 主手段：把 `cmake/deny-os-headers/` 放在包含路径的**最前面**。该目录为每个平台专有头
# 放了一个只含 #error 的占位文件，于是核心层一旦写下 `#include <windows.h>`，
# 包含到的是占位文件，编译当场失败。
#
# 为什么不用「扫描源码找关键字」当主手段：那种检查只覆盖它扫过的模式，换个写法就绕过去了；
# 而包含路径是编译器真实生效的机制，绕不过去。
# 之所以仍补一个扫描脚本，是因为包含路径管不到 CMake 层面的违规，例如把 fwpuclnt
# 链到核心层上（那种情况能编过，要到链接期才炸，且报错信息离原因很远）。

# 在 include 时就把本文件所在目录记下来。
#
# 不能在函数体里直接用 CMAKE_CURRENT_LIST_DIR：在函数内部它解析成**调用方**所在的目录，
# 而不是定义处的目录。踩过：脚本路径因此变成仓库根，检查直接报「Not a file」。
set(BANIPHELPER_GUARDS_DIR "${CMAKE_CURRENT_LIST_DIR}")
set(BANIPHELPER_DENY_HEADERS_DIR "${BANIPHELPER_GUARDS_DIR}/deny-os-headers")

# 给目标加上「不得包含平台专有头」的约束。
#
# 刻意用 PRIVATE：`platform/win` 会链到 `platform/api`，若用 PUBLIC 或 INTERFACE，
# 这条约束会顺着依赖链传播过去，把平台实现自己也锁死。
#
# 同时把扫描任务挂成本目标的依赖，让违规在构建一开始就报出来，
# 而不是等一堆编译错误刷屏之后再被顺带发现。
function(baniphelper_forbid_platform_headers target)
  if(NOT EXISTS "${BANIPHELPER_DENY_HEADERS_DIR}/windows.h")
    message(FATAL_ERROR "找不到禁止头目录，期望位置：${BANIPHELPER_DENY_HEADERS_DIR}")
  endif()

  # BEFORE 是必要的：必须排在 Qt 等依赖项带来的 -isystem 目录之前，
  # 否则同名头会先命中真头，约束形同虚设。
  target_include_directories(${target} BEFORE PRIVATE "${BANIPHELPER_DENY_HEADERS_DIR}")

  if(TARGET baniphelper_check_platform_isolation)
    add_dependencies(${target} baniphelper_check_platform_isolation)
  endif()
endfunction()

# 创建扫描任务。必须在 add_subdirectory 之前调用，
# 这样各目标挂依赖时它已经存在。
function(baniphelper_declare_platform_isolation_check)
  set(_script "${BANIPHELPER_GUARDS_DIR}/check-platform-isolation.cmake")

  add_custom_target(baniphelper_check_platform_isolation
    COMMAND "${CMAKE_COMMAND}" -DROOT=${PROJECT_SOURCE_DIR} -P "${_script}"
    COMMENT "检查核心层与界面层是否引用了平台专有内容"
    VERBATIM
  )
endfunction()
