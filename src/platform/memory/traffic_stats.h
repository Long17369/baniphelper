#pragma once

#include <QMutex>
#include <QPair>
#include <QString>

#include <memory>

#include "core/result.h"
#include "core/types.h"
#include "platform/api/itrafficstats.h"

namespace baniphelper::core {

/// 字节统计的内存实现。
///
/// 它**不读任何真实计数器** —— 那是真实后端的事。价值与 `MemoryFilterEngine` 相同：
/// 给契约测试一个**可控的对照物**，并与真实后端保持同一套说法，这样
/// 「先用内存后端做界面」才有意义。计数器由测试或界面开发直接预置。
///
/// 与 Windows 实现逐条对齐的语义（每一条都对应一条实测结论）：
///
/// - **没启用采集就 `read`** → 明确失败。真实实现这条是硬要求：
///   实测未启用时 `GetPerTcpConnectionEStats` 会返回成功并给出垃圾值，
///   所以判「读得到读不到」只能靠记账，内存实现也就照样记账；
/// - **重复启用安全，且不重置 `since`**（那个时刻是「我们开始采集」的下限）；
/// - **`disableCollection` 幂等**，对没启用过的连接返回成功；
/// - **UDP 一律 `NotSupported`**：Windows 没有 UDP 版的按连接统计，
///   内存实现也不许在这里假装能做到；
/// - **`readMany` 不整体失败**：拿不到计数的那一条用「空 `since`」表示，
///   与真实实现一致。
///
/// ⚠️ 它**不自己声明能力位**：声明在装配点（`makeMemoryBackend`）里，
/// 与真实后端一样由那一处集中决定。
class MemoryTrafficStats final : public ITrafficStats {
 public:
  MemoryTrafficStats();
  ~MemoryTrafficStats() override;

  MemoryTrafficStats(const MemoryTrafficStats&) = delete;
  MemoryTrafficStats& operator=(const MemoryTrafficStats&) = delete;

  [[nodiscard]] Result<void> enableCollection(const ConnectionKey& connection) override;
  [[nodiscard]] Result<void> disableCollection(const ConnectionKey& connection) override;
  [[nodiscard]] Result<ConnectionCounters> read(const ConnectionKey& connection) const override;
  [[nodiscard]] Result<QList<ConnectionCounters>> readMany(
      const QList<ConnectionKey>& connections) const override;

  /// 预置一条连接的计数（等价于「系统里有这条连接，它到现在为止传了这些」）。
  void setCounters(const ConnectionKey& connection, const ConnectionCounters& counters);

  /// 让一条连接「消失」，用来验「读的时候连接已经没了」那条路径。
  void forget(const ConnectionKey& connection);

  [[nodiscard]] int trackedCount() const;

 private:
  [[nodiscard]] bool findKnown(const ConnectionKey& connection, ConnectionCounters* counters) const;
  [[nodiscard]] bool findEnabled(const ConnectionKey& connection, QDateTime* since) const;

  mutable QMutex mutex_;
  QList<QPair<ConnectionKey, ConnectionCounters>> known_;
  QList<QPair<ConnectionKey, QDateTime>> enabled_;
};

}  // namespace baniphelper::core
