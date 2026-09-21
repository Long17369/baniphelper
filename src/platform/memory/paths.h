#pragma once

#include "core/result.h"
#include "platform/api/ipaths.h"

namespace baniphelper::core {

/// 配置与数据目录的内存后端实现。
///
/// 刻意**不碰用户的真实配置目录**：内存后端的用途是「不依赖真实系统就能把上层跑起来」，
/// 如果它往 `%APPDATA%` 里写东西，跑一次测试就会污染开发者的真实配置，
/// 而且两个后端的行为差异会被掩盖。
///
/// 落在系统临时目录下的一处固定位置，所以同一台机器上的多个进程看到的是同一批路径，
/// 与真实后端「跨进程共享」的语义一致。
class MemoryPaths final : public IPaths {
 public:
  MemoryPaths() = default;

  MemoryPaths(const MemoryPaths&) = delete;
  MemoryPaths& operator=(const MemoryPaths&) = delete;

  [[nodiscard]] Result<Paths> resolve() const override;
  [[nodiscard]] Result<void> ensureDirectories() override;
};

}  // namespace baniphelper::core
