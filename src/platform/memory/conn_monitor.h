#pragma once

#include <QList>

#include "core/result.h"
#include "platform/api/iconnmonitor.h"

namespace baniphelper::core {

/// 连接发现的内存实现。
///
/// 它**不枚举任何真实连接** —— 那是真实后端的事。价值与 `MemoryFilterEngine` 相同：
///
/// 1. 给契约测试一个**可控的对照物**。真实后端做不到「快照里恰好只有我放进去的这几条」，
///    而「UDP 行必须是未知方向 + 通配对端」这类契约恰恰需要可控输入才能验；
/// 2. 让界面在还没接上真实后端时就能跑起来。
///
/// 与真实后端保持同一套说法（否则「先用内存后端做界面」就失去了意义）：
///
/// - `subscribe` 同样返回 `NotSupported`，`EventDrivenConnections` 同样**不声明**；
/// - `unsubscribe` 对未知句柄同样返回成功。
///
/// ⚠️ 它**不声明** `ProcessEnumeration`：它确实枚举不了系统里的进程，
/// 而能力位的语义就是「真的做得到」。界面据此显示为不可用是对的 ——
/// 内存后端本来就只是开发期的替身。
class MemoryConnMonitor final : public IConnMonitor {
 public:
  MemoryConnMonitor() = default;
  ~MemoryConnMonitor() override;

  MemoryConnMonitor(const MemoryConnMonitor&) = delete;
  MemoryConnMonitor& operator=(const MemoryConnMonitor&) = delete;

  [[nodiscard]] Result<QList<ConnectionSnapshot>> snapshot() const override;
  [[nodiscard]] Result<SubscriptionId> subscribe(ConnectionEventSink sink) override;
  [[nodiscard]] Result<void> unsubscribe(SubscriptionId id) override;

  /// 预置快照内容，默认是空表。
  ///
  /// 只给测试与界面开发用：它不做任何合法性检查，
  /// 「放进来的行是否满足接口约定」由放的人负责 —— 这正是契约测试要验的东西。
  void setSnapshot(QList<ConnectionSnapshot> rows);

  /// 清空预置内容。
  void clear();

 private:
  QList<ConnectionSnapshot> rows_;
};

}  // namespace baniphelper::core
