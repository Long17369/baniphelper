# 单元测试与（自 S8.1 起的）平台契约测试。
#
# 一律用 CTest 注册，因此 `ctest --preset debug` 与 `cmake --workflow --preset debug`
# 都能直接跑，不需要记第二套命令。

find_package(Qt6 REQUIRED COMPONENTS Test Network)

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

baniphelper_add_test(config baniphelper_config_test
  config_test.cpp
)

baniphelper_add_test(database baniphelper_database_test
  database_test.cpp
)

baniphelper_add_test(types baniphelper_types_test
  types_test.cpp
)

# 「先封后断」的编排：只用假实现就能验，因此只链核心层。
baniphelper_add_test(ban_action baniphelper_ban_action_test
  ban_action_test.cpp
)

baniphelper_add_test(rule_model baniphelper_rule_model_test
  rule_model_test.cpp
)

# 规则仓储：跑在真实的 SQLite 上（QTemporaryDir 里的临时库），不碰 %APPDATA%。
baniphelper_add_test(rule_store baniphelper_rule_store_test
  rule_store_test.cpp
)

# 规则生命周期编排：只用假实现就能验顺序与回滚，因此只链核心层。
baniphelper_add_test(rule_runtime baniphelper_rule_runtime_test
  rule_runtime_test.cpp
)

# 展开器的拆法是纯逻辑（不包含任何平台专有头），但它按约定放在 platform/win/ 下
# —— 拆法允许与平台的过滤机制贴紧。因此这个测试要链真实后端才能拿到实现。
# 若将来出现第二个平台的拆法与 Windows 相同，把这几个文件上移到 core/ 即可。
baniphelper_add_test(filter_plan baniphelper_filter_plan_test
  filter_plan_test.cpp
)
target_link_libraries(baniphelper_filter_plan_test PRIVATE BanIPHelper::Backend)

# 契约测试要把同一套断言跑在真实后端与内存后端上，因此两边都要链。
baniphelper_add_test(platform_contract baniphelper_platform_contract_test
  platform_contract_test.cpp
)
target_link_libraries(baniphelper_platform_contract_test PRIVATE
  BanIPHelper::PlatformApi
  BanIPHelper::PlatformMemory
  # 真实后端：提供 createPlatformBackend 的实现，由 platform/CMakeLists.txt 在编译期挑。
  BanIPHelper::Backend
  # 契约测试要自己造一对可控的 TCP 连接与 UDP 端点，才能核对「刚建立的连接在不在、方向对不对」。
  # 核心层的 Qt6::Network 是 PRIVATE 的，include 目录不会传过来，所以这里显式链一次。
  Qt6::Network
)
