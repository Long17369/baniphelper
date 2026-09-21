# 界面：Qt Quick

find_package(Qt6 REQUIRED COMPONENTS Quick)

# REQUIRES 把 Qt 6.8 之前引入的策略全部设为 NEW，避免 QTP0001/QTP0004 之类
# 的开发者警告长期堆积。
qt_standard_project_setup(REQUIRES 6.8)

# 提权清单必须编进可执行文件，否则双击启动时程序只是普通权限，
# 而失败点会远在第一次下发过滤器的地方，离「没提权」这个原因很远。
#
# .rc 由 windres 编译，与 g++ 的头搜索路径毫无关系，因此它不受平台隔离检查约束。
set(BANIPHELPER_WINDOWS_RESOURCES "")
if(WIN32)
  enable_language(RC)
  set(BANIPHELPER_WINDOWS_RESOURCES baniphelper.rc)
endif()

qt_add_executable(baniphelper
  main.cpp
  ${BANIPHELPER_WINDOWS_RESOURCES}
)

# 产品形态是提权托盘程序，**不能带控制台窗口**。
# 控制台子系统会让双击启动时多出一个黑框，与托盘工具的观感完全不符（实测踩到）。
#
# 代价是发布版看不到 stdout/stderr。按 architecture.md 的规划，提权进程的排查手段
# 本来就是日志文件，由 S1.4 提供。从终端启动时仍能看到输出 ——
# GUI 子系统的进程会继承调用者的标准输出句柄，只是终端不会等它结束。
if(WIN32)
  set_target_properties(baniphelper PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

qt_add_qml_module(baniphelper
  URI BanIPHelper
  VERSION 1.0
  QML_FILES
    qml/Main.qml
)

target_link_libraries(baniphelper PRIVATE
  Qt6::Quick
  BanIPHelper::Core
  BanIPHelper::PlatformApi
  BanIPHelper::Backend
)

baniphelper_enable_warnings(baniphelper)

# 界面层不得包含平台专有头：它只允许通过 core 与 platform/api 的接口取数据。
baniphelper_forbid_platform_headers(baniphelper)

# 把 Qt 运行库放到可执行文件旁边。
#
# 这一步**不是可选的优化**。本程序带 requireAdministrator 清单，经由 UAC 提权启动时，
# Windows 用的是用户注册表里的环境，**不继承开发终端临时加进 PATH 的 Qt 目录**，
# 于是加载器找不到 Qt6Gui.dll，程序在 main 之前就失败 ——
# 双击启动与编辑器里启动都会这样，现象与代码错误完全不像（实测踩到）。
#
# 也不能要求用户自己配 PATH：这是给别人用的桌面工具，
# 唯一能保证「拷过去就能跑」的做法就是把依赖放在 exe 旁边。
# 到阶段八 S8.5 的平台化打包时，这一步会并入正式的部署流程。
if(BANIPHELPER_DEPLOY_QT_TO_BUILD_TREE AND WIN32)
  if(TARGET Qt6::Core)
    get_target_property(_bh_qt_core_location Qt6::Core IMPORTED_LOCATION)
    get_filename_component(_bh_qt_bin_dir "${_bh_qt_core_location}" DIRECTORY)
  endif()
  find_program(BANIPHELPER_WINDEPLOYQT
    NAMES windeployqt
    HINTS "${_bh_qt_bin_dir}"
    REQUIRED
  )

  # 这里刻意只排除翻译文件，不排除编译器运行时与软件渲染回退：
  # MinGW 的 libgcc / libstdc++ / libwinpthread 同样不在提权进程的环境里，
  # 而 OpenGL 软件回退是「显卡驱动异常时唯一的退路」，两者都不能省。
  add_custom_command(TARGET baniphelper POST_BUILD
    COMMAND "${BANIPHELPER_WINDEPLOYQT}" --no-translations "$<TARGET_FILE:baniphelper>"
    COMMENT "把 Qt 运行库部署到可执行文件旁边（提权启动不继承终端的 PATH）"
    VERBATIM
  )
endif()
