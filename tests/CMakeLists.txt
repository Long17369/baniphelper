# 单元测试与（自 S8.1 起的）平台契约测试。
#
# 一律用 CTest 注册，因此 `ctest --preset debug` 与 `cmake --workflow --preset debug`
# 都能直接跑，不需要记第二套命令。

find_package(Qt6 REQUIRED COMPONENTS Test)

# Qt Test 的测试类需要 moc。tests 是 src 的兄弟目录，不继承 src/ui 里
# qt_standard_project_setup() 的设置，所以在这里单独打开。
set(CMAKE_AUTOMOC ON)

# 注册一个 Qt 测试可执行文件。
function(baniphelper_add_test name target source)
  add_executable(${target} ${source})
  target_link_libraries(${target} PRIVATE Qt6::Test BanIPHelper::Core)
  baniphelper_enable_warnings(${target})
  baniphelper_forbid_platform_headers(${target})

  # 用 `cmake -E env` 把 Qt 的 bin 目录临时加进 PATH：
  # 构建产物不自带 Qt 运行库，直接跑会因为找不到 DLL 而失败。
  # 这样做而不是在预设里写死路径，是为了不让机器相关路径进入仓库；
  # 也不用 ENVIRONMENT_MODIFICATION，那会把 CMake 最低版本抬到 3.22。
  set(_path "$<TARGET_FILE_DIR:Qt6::Core>;$ENV{PATH}")
  add_test(NAME ${name}
    COMMAND "${CMAKE_COMMAND}" -E env "PATH=${_path}" "$<TARGET_FILE:${target}>")
endfunction()

baniphelper_add_test(capabilities baniphelper_capabilities_test
  capabilities_test.cpp
)

baniphelper_add_test(log baniphelper_log_test
  log_test.cpp
)

# 契约测试要把同一套断言跑在真实后端与内存后端上，因此两边都要链。
baniphelper_add_test(platform_contract baniphelper_platform_contract_test
  platform_contract_test.cpp
)
target_link_libraries(baniphelper_platform_contract_test PRIVATE
  BanIPHelper::PlatformApi
  BanIPHelper::PlatformMemory
  # 真实后端：提供 createPlatformBackend 的实现，由 platform/CMakeLists.txt 在编译期挑。
  BanIPHelper::Backend
)
