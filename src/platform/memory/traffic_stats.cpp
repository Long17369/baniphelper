// 字节统计的内存实现（阶段三 S3.2）。
//
// 只回放预置的计数、只按记账回答「启用过没有」，不碰任何系统接口。
// 语义与真实后端逐条对齐 —— 理由写在头文件里。

#include "platform/memory/traffic_stats.h"

#include <QMutexLocker>
#include <QString>

#include "core/error.h"

namespace baniphelper::core {

MemoryTrafficStats::MemoryTrafficStats() = default;

MemoryTrafficStats::~MemoryTrafficStats() = default;

bool MemoryTrafficStats::findKnown(const ConnectionKey& connection,
                                   ConnectionCounters* counters) const {
  for (const auto& entry : known_) {
    if (entry.first == connection) {
      if (counters != nullptr) {
        *counters = entry.second;
      }
      return true;
    }
  }
  return false;
}

bool MemoryTrafficStats::findEnabled(const ConnectionKey& connection, QDateTime* since) const {
  for (const auto& entry : enabled_) {
    if (entry.first == connection) {
      if (since != nullptr) {
        *since = entry.second;
      }
      return true;
    }
  }
  return false;
}

Result<void> MemoryTrafficStats::enableCollection(const ConnectionKey& connection) {
  if (connection.protocol != TransportProtocol::Tcp) {
    // 与真实后端同一句话：Windows 没有 UDP 版的按连接统计，
    // 在这里假装能统计，界面就会按「UDP 也能看字节数」来布局。
    return Result<void>::fail(unsupportedError(
        QStringLiteral("按连接统计 UDP 的字节数"),
        QStringLiteral("按连接的扩展统计只有 TCP 版，UDP 的字节数走事件源，属于 S3.4")));
  }

  QMutexLocker locker(&mutex_);
  if (findEnabled(connection, nullptr)) {
    // 重复启用安全，而且**不重置 since**。
    return Result<void>::ok();
  }
  if (!findKnown(connection, nullptr)) {
    return Result<void>::fail(makeError(
        ErrorCode::NotFound,
        QStringLiteral("内存后端里没有这条连接：先 setCounters 预置它，等价于系统里先有这条连接")));
  }

  enabled_.append(qMakePair(connection, QDateTime::currentDateTime()));
  return Result<void>::ok();
}

Result<void> MemoryTrafficStats::disableCollection(const ConnectionKey& connection) {
  QMutexLocker locker(&mutex_);
  for (int i = 0; i < enabled_.size(); ++i) {
    if (enabled_.at(i).first == connection) {
      enabled_.removeAt(i);
      break;
    }
  }
  // 没启用过也返回成功：契约只要求退出前停一次，不要求报错。
  return Result<void>::ok();
}

Result<ConnectionCounters> MemoryTrafficStats::read(const ConnectionKey& connection) const {
  QMutexLocker locker(&mutex_);
  QDateTime since;
  if (!findEnabled(connection, &since)) {
    return Result<ConnectionCounters>::fail(makeError(
        ErrorCode::InvalidArgument,
        QStringLiteral("还没对这条连接启用采集；先 enableCollection 再读")));
  }
  ConnectionCounters counters;
  if (!findKnown(connection, &counters)) {
    return Result<ConnectionCounters>::fail(
        makeError(ErrorCode::NotFound, QStringLiteral("这条连接已经不存在了（已被 forget）")));
  }
  counters.since = since;
  return Result<ConnectionCounters>::ok(counters);
}

Result<QList<ConnectionCounters>> MemoryTrafficStats::readMany(
    const QList<ConnectionKey>& connections) const {
  QList<ConnectionCounters> results;
  results.reserve(connections.size());
  for (const ConnectionKey& connection : connections) {
    const Result<ConnectionCounters> one = read(connection);
    // 与真实实现一致：批量里拿不到的用「空 since」（= 该连接拿不到计数）表示，
    // 不整体失败 —— 连接一直在这台机器上建立与消失。
    results.append(one ? one.value() : ConnectionCounters());
  }
  return Result<QList<ConnectionCounters>>::ok(std::move(results));
}

void MemoryTrafficStats::setCounters(const ConnectionKey& connection,
                                     const ConnectionCounters& counters) {
  QMutexLocker locker(&mutex_);
  for (auto& entry : known_) {
    if (entry.first == connection) {
      entry.second = counters;
      return;
    }
  }
  known_.append(qMakePair(connection, counters));
}

void MemoryTrafficStats::forget(const ConnectionKey& connection) {
  QMutexLocker locker(&mutex_);
  for (int i = 0; i < known_.size(); ++i) {
    if (known_.at(i).first == connection) {
      known_.removeAt(i);
      break;
    }
  }
}

int MemoryTrafficStats::trackedCount() const {
  QMutexLocker locker(&mutex_);
  return static_cast<int>(enabled_.size());
}

}  // namespace baniphelper::core
