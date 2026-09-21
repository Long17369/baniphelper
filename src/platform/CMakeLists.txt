# 平台层。
#
# 目录职责：
#   api/     纯接口头文件，上层可依赖的一切契约都在这里
#   memory/  与操作系统无关的一组实现，给契约测试当对照物，也让界面开发不必等真实后端
#   win/     本期的真实实现
#   linux/   占位，尚未实现
#   macos/   占位，尚未实现
#
# **编译期挑一个真实后端**：换平台就是改下面这段条件，core/ 与 ui/ 一行都不用动。
# 这就是阶段八 S8.2 要求的「同一构建产物不混用多平台实现」的最小形态。

add_subdirectory(api)
add_subdirectory(memory)

if(WIN32)
  add_subdirectory(win)
elseif(APPLE)
  message(FATAL_ERROR
    "macOS 后端尚未实现。见 src/platform/macos/README.md 与 docs/phases/08-portability.md")
else()
  message(FATAL_ERROR
    "Linux 后端尚未实现。见 src/platform/linux/README.md 与 docs/phases/08-portability.md")
endif()

# 上层只认这个名字，不必知道自己链的是哪个平台实现。
add_library(baniphelper_backend INTERFACE)
add_library(BanIPHelper::Backend ALIAS baniphelper_backend)
target_link_libraries(baniphelper_backend INTERFACE baniphelper_backend_impl)
