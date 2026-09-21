#pragma once

#include <QList>

#include <functional>

#include "core/result.h"
#include "core/types.h"
#include "platform/api/platform_types.h"

namespace baniphelper::core {

/// 连接事件回调。
///
/// 线程约定：**在平台层自己的线程上同步调用**。回调内不得阻塞、不得做重活，
/// 需要耗时处理时由消费方自行排队。这一条必须写进实现，否则界面会被平台线程拖住。
using ConnectionEventSink = std::function<void(const ConnectionEvent&)>;

/// 连接发现：快照与增量。
///
/// 契约要点：
///
/// - **事件驱动优先**。能力位声明支持 `EventDrivenConnections` 时，实现必须真的走事件，
///   因为「空闲时零轮询」是本项目的核心设计目标；不具备该能力时 `subscribe` 返回
///   `NotSupported`，由上层退化为轮询 `snapshot()`，**并如实标注精度差异**
///   （短连接可能被漏掉，这一点要显示给用户，不能悄悄降级）；
/// - **不重复上报**：同一个连接在同一个状态上只上报一次，`Updated` 只在计数等
///   内容真的变化时发；重复上报会把记录表打爆；
/// - **协议与地址族都要覆盖**：TCP 与 UDP、IPv4 与 IPv6 缺一不可，
///   缺哪一项靠能力位如实声明，不允许只做 IPv4 却报告成功。
class IConnMonitor {
 public:
  IConnMonitor() = default;
  virtual ~IConnMonitor() = default;

  IConnMonitor(const IConnMonitor&) = delete;
  IConnMonitor& operator=(const IConnMonitor&) = delete;
  IConnMonitor(IConnMonitor&&) = delete;
  IConnMonitor& operator=(IConnMonitor&&) = delete;

  /// 全量快照。
  ///
  /// 用于首次填充与校验增量一致性，**不作为常规数据源**：全量枚举的代价随连接数增长，
  /// 频繁调用与「空闲时零轮询」直接冲突。
  [[nodiscard]] virtual Result<QList<ConnectionSnapshot>> snapshot() const = 0;

  /// 订阅增量事件。不支持事件驱动时返回 `ErrorCode::NotSupported`。
  [[nodiscard]] virtual Result<SubscriptionId> subscribe(ConnectionEventSink sink) = 0;

  /// 退订。订阅与退订必须成对，进程退出时必须退订，否则平台侧订阅会泄漏。
  /// 对未知句柄调用返回成功。
  [[nodiscard]] virtual Result<void> unsubscribe(SubscriptionId id) = 0;
};

}  // namespace baniphelper::core
