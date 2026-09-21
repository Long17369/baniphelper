#pragma once

#include <QList>

#include "core/result.h"
#include "core/types.h"

namespace baniphelper::core {

/// 字节统计。
///
/// 契约要点：
///
/// - **必须先启用采集才读得到值**。平台侧的按连接扩展统计默认是关闭的，
///   直接读会拿到空值；因此 `enableCollection` 不是可选的优化，而是必要前置。
///   实操时机是在**连接刚被发现时**立刻启用；
/// - **单调不减**。实现若发现计数回退，只能来自连接被重建，此时应重新启用采集；
/// - **拿不到就说不支持**：不支持的能力返回 `ErrorCode::NotSupported`，
///   只能从启用时刻起计数的情形必须把该时刻写进 `ConnectionCounters::since`。
///   **绝对不允许用 0 冒充**「没有流量」，那会让用户以为连接是空跑的；
/// - **退出前要停**：连接消失时调用 `disableCollection` 释放平台侧的跟踪资源，
///   否则长时间运行会积累出泄漏。
class ITrafficStats {
 public:
  ITrafficStats() = default;
  virtual ~ITrafficStats() = default;

  ITrafficStats(const ITrafficStats&) = delete;
  ITrafficStats& operator=(const ITrafficStats&) = delete;
  ITrafficStats(ITrafficStats&&) = delete;
  ITrafficStats& operator=(ITrafficStats&&) = delete;

  /// 对一条连接启用采集。对已启用的连接重复调用是安全的。
  [[nodiscard]] virtual Result<void> enableCollection(const ConnectionKey& connection) = 0;

  /// 停止采集并释放跟踪资源。
  [[nodiscard]] virtual Result<void> disableCollection(const ConnectionKey& connection) = 0;

  /// 读取累计计数。前置条件是已经 `enableCollection`。
  [[nodiscard]] virtual Result<ConnectionCounters> read(const ConnectionKey& connection) const = 0;

  /// 批量读取，减少跨层调用次数。界面一次刷新整页时用它。
  [[nodiscard]] virtual Result<QList<ConnectionCounters>> readMany(
      const QList<ConnectionKey>& connections) const = 0;
};

}  // namespace baniphelper::core
