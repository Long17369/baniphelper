# Qt 套件自带 MinGW 的工具链文件（Windows）
#
# 本文件只被 CMakePresets.json 引用，供命令行构建使用；
# Qt Creator 使用自己的套件配置，不经过这里。
#
# 依赖两个环境变量：
#   BANIPHELPER_QTDIR      Qt 套件根目录，其下须有 lib/cmake/Qt6/Qt6Config.cmake
#   BANIPHELPER_MINGW_BIN  Qt 自带 MinGW 的 bin 目录

if(NOT CMAKE_HOST_WIN32)
  message(FATAL_ERROR
    "本工具链文件只用于 Windows。其他平台请改 platform 后端与相应的预设。")
endif()

foreach(_bh_var BANIPHELPER_QTDIR BANIPHELPER_MINGW_BIN)
  if(NOT DEFINED ENV{${_bh_var}} OR "$ENV{${_bh_var}}" STREQUAL "")
    message(FATAL_ERROR
      "环境变量 ${_bh_var} 未设置，无法定位 Qt 套件自带的工具链。\n"
      "Git Bash 下设置示例：\n"
      "  export ${_bh_var}=<路径>")
  endif()
endforeach()

file(TO_CMAKE_PATH "$ENV{BANIPHELPER_QTDIR}" _bh_qt_dir)
file(TO_CMAKE_PATH "$ENV{BANIPHELPER_MINGW_BIN}" _bh_mingw_bin)

if(NOT EXISTS "${_bh_qt_dir}/lib/cmake/Qt6/Qt6Config.cmake")
  message(FATAL_ERROR
    "BANIPHELPER_QTDIR 指向的目录下没有 lib/cmake/Qt6/Qt6Config.cmake：\n"
    "  ${_bh_qt_dir}\n"
    "请把它指向 Qt 套件根目录，例如 <Qt 安装目录>/6.9.3/mingw_64。")
endif()

foreach(_bh_exe gcc.exe g++.exe)
  if(NOT EXISTS "${_bh_mingw_bin}/${_bh_exe}")
    message(FATAL_ERROR
      "BANIPHELPER_MINGW_BIN 指向的目录下没有 ${_bh_exe}：\n"
      "  ${_bh_mingw_bin}")
  endif()
endforeach()

set(CMAKE_C_COMPILER   "${_bh_mingw_bin}/gcc.exe" CACHE FILEPATH "Qt 自带 MinGW 的 C 编译器"   FORCE)
set(CMAKE_CXX_COMPILER "${_bh_mingw_bin}/g++.exe" CACHE FILEPATH "Qt 自带 MinGW 的 C++ 编译器" FORCE)
set(CMAKE_PREFIX_PATH  "${_bh_qt_dir}"            CACHE PATH     "Qt 套件根目录"                FORCE)

unset(_bh_qt_dir)
unset(_bh_mingw_bin)
