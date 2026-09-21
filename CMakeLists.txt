cmake_minimum_required(VERSION 3.21)

project(BanIPHelper
  VERSION 0.1.0
  DESCRIPTION "系统级 IP 封禁工具"
  LANGUAGES C CXX
)

# --------------------------------------------------------------- 全局编译选项

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "构建类型" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

# 供 clangd 等工具使用
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(BANIPHELPER_BUILD_TESTS "构建单元测试与平台契约测试" ON)

# --------------------------------------------------------------- 工具链约束

# 全项目统一使用 Qt 套件自带的 MinGW，不引入 MSVC，理由见
# docs/architecture.md 第 1.3 节与第 10 节「两套 MinGW 混用」。
if(MSVC)
  message(FATAL_ERROR
    "本项目不使用 MSVC。请改用 Qt 套件自带的 MinGW 工具链：\n"
    "  命令行构建：设置环境变量后用 CMakePresets.json 的预设，见 tmp/env-local.md\n"
    "  Qt Creator：选择 mingw_64 套件")
endif()

# 仅在明确给出期望目录时才做严格比对，避免干扰 Qt Creator 的套件配置。
if(DEFINED ENV{BANIPHELPER_MINGW_BIN} AND NOT "$ENV{BANIPHELPER_MINGW_BIN}" STREQUAL "")
  file(TO_CMAKE_PATH "$ENV{BANIPHELPER_MINGW_BIN}" _bh_expected_bin)
  get_filename_component(_bh_actual_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
  if(NOT _bh_actual_bin STREQUAL _bh_expected_bin)
    message(FATAL_ERROR
      "编译器不是 Qt 套件自带的 MinGW，禁止混用两套工具链：\n"
      "  期望：${_bh_expected_bin}\n"
      "  实际：${_bh_actual_bin}")
  endif()
endif()

# --------------------------------------------------------------- 公共函数

# 项目自有目标的统一警告开关。第三方与平台头文件不进这里。
function(baniphelper_enable_warnings target)
  target_compile_options(${target} PRIVATE -Wall -Wextra)
endfunction()

# --------------------------------------------------------------- 子目录

add_subdirectory(src)
