# 平台隔离的源码扫描（阶段一 S1.10）。
#
# 与 cmake/BanIPHelperGuards.cmake 里的「禁止头」互补：禁止头管编译期，
# 本脚本补上编译期管不到的两类违规。
#
#   1. 用绝对路径或相对路径包含平台头，绕过了 -I 搜索路径；
#   2. CMake 层面的违规，例如给核心层链上 fwpuclnt：那种写法能编过，
#      要到链接期才炸，且报错离原因很远。
#
# 由 add_custom_target 在每次构建时调用，失败即中断构建。
#
# 用法：cmake -DROOT=<仓库根> -P check-platform-isolation.cmake

if(NOT DEFINED ROOT)
  message(FATAL_ERROR "缺少参数：-DROOT=<仓库根>")
endif()

set(_deny_dir "${ROOT}/cmake/deny-os-headers")

# 受约束的目录：核心层、平台接口层、界面层。
# 平台实现在 src/platform/<os>/ 下，不在其列 —— 它的职责就是调用系统接口。
set(_guarded_dirs "src/core" "src/platform/api" "src/ui")

# 禁止清单直接从禁止头目录推导，而不是在这里再抄一份，
# 免得两处清单慢慢走样、还以为有检查。
file(GLOB_RECURSE _deny_files RELATIVE "${_deny_dir}" "${_deny_dir}/*.h")
list(SORT _deny_files)
if(_deny_files STREQUAL "")
  message(FATAL_ERROR "禁止头目录是空的，检查会形同虚设：${_deny_dir}")
endif()

# 核心层与界面层不该链的平台库。
set(_forbidden_libs fwpuclnt iphlpapi ws2_32 wevtapi user32 advapi32)

set(_violations "")

foreach(_dir IN LISTS _guarded_dirs)
  file(GLOB_RECURSE _sources "${ROOT}/${_dir}/*.h" "${ROOT}/${_dir}/*.cpp")

  foreach(_src IN LISTS _sources)
    file(READ "${_src}" _content)
    file(RELATIVE_PATH _rel "${ROOT}" "${_src}")

    foreach(_header IN LISTS _deny_files)
      string(REPLACE "." "\\." _header_pattern "${_header}")
      if(_content MATCHES "#[ \t]*include[ \t]*[<\"]${_header_pattern}[>\"]")
        list(APPEND _violations "${_rel} 包含了平台专有头 <${_header}>")
      endif()
    endforeach()
  endforeach()

  # CMake 层的违规：只查该目录自己的 CMakeLists。
  set(_cmake_lists "${ROOT}/${_dir}/CMakeLists.txt")
  if(EXISTS "${_cmake_lists}")
    file(READ "${_cmake_lists}" _cmake_content)
    file(RELATIVE_PATH _rel_cmake "${ROOT}" "${_cmake_lists}")
    foreach(_lib IN LISTS _forbidden_libs)
      if(_cmake_content MATCHES "target_link_libraries[^)]*[ \t(]${_lib}[ \t)]")
        list(APPEND _violations "${_rel_cmake} 给该目标链上了平台库 ${_lib}")
      endif()
    endforeach()
  endif()
endforeach()

if(_violations)
  string(REPLACE ";" "\n  - " _report "${_violations}")
  message(FATAL_ERROR
    "平台隔离检查未通过：\n"
    "  - ${_report}\n"
    "\n"
    "核心层与平台接口层不得引用任何平台专有内容，平台差异只能出现在 src/platform/<os>/ 下。\n"
    "依据见 docs/architecture.md 第 12.2 节与 docs/phases/01-foundation.md 的 S1.10。")
endif()
