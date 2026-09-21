# 分层顺序即依赖方向：core 与 platform/api 不依赖上层，ui 依赖两者。
#
# 平台实现目录（platform/win 等）暂不参与构建，
# 待 S1.2 冻结接口、建立实现目标后在此加入。

add_subdirectory(core)
add_subdirectory(platform/api)
add_subdirectory(ui)
