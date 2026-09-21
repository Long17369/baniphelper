# 内存后端：不碰任何操作系统接口的一组实现。
#
# 两个用途：
#   1. 给契约测试当对照物 —— 真实后端做不到的场景（例如「未提权」与「已提权」两条
#      分支同时可测），只有可控制状态的内存实现能覆盖；
#   2. 让界面开发与持续集成不必先有真实实现就能跑起来。
#
# 它同样不得包含平台专有头：内存后端一旦引了系统头，就不再是「与平台无关的对照物」了。

add_library(baniphelper_platform_memory STATIC
  backend.cpp
  privilege.cpp
  single_instance.cpp
)
add_library(BanIPHelper::PlatformMemory ALIAS baniphelper_platform_memory)

target_link_libraries(baniphelper_platform_memory PUBLIC
  BanIPHelper::Core
  BanIPHelper::PlatformApi
)

baniphelper_enable_warnings(baniphelper_platform_memory)
baniphelper_forbid_platform_headers(baniphelper_platform_memory)
