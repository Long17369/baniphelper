# 界面：Qt Quick

find_package(Qt6 REQUIRED COMPONENTS Quick)

# REQUIRES 把 Qt 6.8 之前引入的策略全部设为 NEW，避免 QTP0001/QTP0004 之类
# 的开发者警告长期堆积。
qt_standard_project_setup(REQUIRES 6.8)

qt_add_executable(baniphelper
  main.cpp
)

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
)

baniphelper_enable_warnings(baniphelper)

# 界面层不得包含平台专有头：它只允许通过 core 与 platform/api 的接口取数据。
baniphelper_forbid_platform_headers(baniphelper)
