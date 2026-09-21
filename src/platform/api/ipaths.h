#pragma once

#include "core/result.h"
#include "platform/api/platform_types.h"

namespace baniphelper::core {

/// 配置与数据目录。
///
/// 契约要点：
///
/// - `resolve` 只回答「默认位置在哪」，**不保证目录已经存在**，也不做创建。
///   把两件事混在一个方法里，会让「目录被用户删掉」这种情形难以区分；
/// - `ensureDirectories` 必须在首次启动、以及用户删掉目录之后都能成功重建。
///   阶段一验收项「删掉配置文件可自动重建默认值」正是靠它；
/// - 路径一律用**绝对路径**，且必须符合平台惯例（Windows 上是 `%APPDATA%` 之下），
///   不允许写到程序所在目录：程序目录可能不可写，也会造成多用户互相覆盖。
class IPaths {
 public:
  IPaths() = default;
  virtual ~IPaths() = default;

  IPaths(const IPaths&) = delete;
  IPaths& operator=(const IPaths&) = delete;
  IPaths(IPaths&&) = delete;
  IPaths& operator=(IPaths&&) = delete;

  [[nodiscard]] virtual Result<Paths> resolve() const = 0;

  [[nodiscard]] virtual Result<void> ensureDirectories() = 0;
};

}  // namespace baniphelper::core
