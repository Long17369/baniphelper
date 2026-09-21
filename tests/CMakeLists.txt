# 单元测试与（自 S8.1 起的）平台契约测试。
#
# 一律用 CTest 注册，因此 `ctest --preset debug` 与 `cmake --workflow --preset debug`
# 都能直接跑，不需要记第二套命令。

find_package(Qt6 REQUIRED COMPONENTS Test)

# Qt Test 的测试类需要 moc。tests 是 src 的兄弟目录，不继承 src/ui 里
# qt_standard_project_setup() 的设置，所以在这里单独打开。
set(CMAKE_AUTOMOC ON)

add_executable(baniphelper_capabilities_test
  capabilities_test.cpp
)

target_link_libraries(baniphelper_capabilities_test PRIVATE
  Qt6::Test
  BanIPHelper::Core
)

baniphelper_enable_warnings(baniphelper_capabilities_test)
baniphelper_forbid_platform_headers(baniphelper_capabilities_test)

# 用 `cmake -E env` 把 Qt 的 bin 目录临时加进 PATH：
# 构建产物不自带 Qt 运行库，直接跑会因为找不到 DLL 而失败。
# 这样做而不是在预设里写死路径，是为了不让机器相关路径进入仓库；
# 也不用 ENVIRONMENT_MODIFICATION，那会把 CMake 最低版本抬到 3.22。
set(_bh_test_path "$<TARGET_FILE_DIR:Qt6::Core>;$ENV{PATH}")

add_test(NAME capabilities
  COMMAND "${CMAKE_COMMAND}" -E env
          "PATH=${_bh_test_path}"
          "$<TARGET_FILE:baniphelper_capabilities_test>"
)

unset(_bh_test_path)
