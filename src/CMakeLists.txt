# 分层顺序即依赖方向：core 与 platform 不依赖上层，ui 依赖两者。
#
# platform/ 自己负责按「api 接口层 + 各平台实现」组织，并决定编译期启用哪个后端。

add_subdirectory(core)
add_subdirectory(platform)
add_subdirectory(ui)
