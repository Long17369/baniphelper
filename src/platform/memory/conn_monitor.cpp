// 连接发现的内存实现（阶段三 S3.1）。
//
// 只回放预置内容，不碰任何系统接口。判据与真实后端一致 —— 理由写在头文件里。

#include "platform/memory/conn_monitor.h"

#include <QString>

#include "core/error.h"

namespace baniphelper::core {

MemoryConnMonitor::~MemoryConnMonitor() = default;

Result<QList<ConnectionSnapshot>> MemoryConnMonitor::snapshot() const {
  return Result<QList<ConnectionSnapshot>>::ok(rows_);
}

Result<SubscriptionId> MemoryConnMonitor::subscribe(ConnectionEventSink sink) {
  Q_UNUSED(sink);
  // 与真实后端同一句话：不声明 EventDrivenConnections，就必须**明确报不支持**。
  // 这里若返回一个「能用但永不触发」的订阅，界面会以为在等事件，实际永远等不到。
  return Result<SubscriptionId>::fail(unsupportedError(
      QStringLiteral("按事件订阅连接变化"),
      QStringLiteral("事件源属于阶段三 S3.3，本后端（与真实后端一致）不声明 "
                     "EventDrivenConnections 能力。在此之前上层只能轮询 snapshot()，"
                     "并如实标注精度差异")));
}

Result<void> MemoryConnMonitor::unsubscribe(SubscriptionId id) {
  Q_UNUSED(id);
  return Result<void>::ok();
}

void MemoryConnMonitor::setSnapshot(QList<ConnectionSnapshot> rows) {
  rows_ = std::move(rows);
}

void MemoryConnMonitor::clear() {
  rows_.clear();
}

}  // namespace baniphelper::core
