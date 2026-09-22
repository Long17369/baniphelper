// 连接发现的内存实现（阶段三 S3.1 起）。
//
// 只回放预置内容、只转发投进来的事件，不碰任何系统接口。
// 判据与真实后端一致 —— 理由写在头文件里。

#include "platform/memory/conn_monitor.h"

#include <QMutexLocker>
#include <QString>

#include "core/error.h"

namespace baniphelper::core {

MemoryConnMonitor::~MemoryConnMonitor() = default;

Result<QList<ConnectionSnapshot>> MemoryConnMonitor::snapshot() const {
  QMutexLocker locker(&mutex_);
  return Result<QList<ConnectionSnapshot>>::ok(rows_);
}

Result<SubscriptionId> MemoryConnMonitor::subscribe(ConnectionEventSink sink) {
  if (!sink) {
    // 空回调要当场拒掉：放过去的话，订阅「成功」了却永远不会有人收到事件，
    // 排查起来会以为事件源坏了。
    return Result<SubscriptionId>::fail(
        makeError(ErrorCode::InvalidArgument, QStringLiteral("订阅回调为空，事件无处可去")));
  }

  QMutexLocker locker(&mutex_);
  SinkEntry entry;
  entry.id = nextId_++;
  entry.sink = std::move(sink);
  sinks_.append(entry);
  return Result<SubscriptionId>::ok(entry.id);
}

Result<void> MemoryConnMonitor::unsubscribe(SubscriptionId id) {
  QMutexLocker locker(&mutex_);
  // 契约要求：对未知句柄退订返回成功。退出路径会把「退订」当成收尾动作无条件调一次，
  // 报错只会制造假警报。
  for (int i = 0; i < sinks_.size(); ++i) {
    if (sinks_.at(i).id != id) {
      continue;
    }
    sinks_.removeAt(i);
    break;
  }
  return Result<void>::ok();
}

void MemoryConnMonitor::emitEvent(const ConnectionEvent& event) {
  // 与真实后端同样的规矩：订阅者清单在锁内拷出来，回调**在锁外**执行。
  // 否则一个在回调里退订的调用方就会自锁，而这种事只在真跑起来时才发现。
  QList<SinkEntry> sinks;
  {
    QMutexLocker locker(&mutex_);
    sinks = sinks_;
  }
  for (const SinkEntry& entry : sinks) {
    if (entry.sink) {
      entry.sink(event);
    }
  }
}

void MemoryConnMonitor::setSnapshot(QList<ConnectionSnapshot> rows) {
  QMutexLocker locker(&mutex_);
  rows_ = std::move(rows);
}

void MemoryConnMonitor::clear() {
  QMutexLocker locker(&mutex_);
  rows_.clear();
}

}  // namespace baniphelper::core
