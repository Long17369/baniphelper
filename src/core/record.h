#pragma once

#include <QDateTime>
#include <QList>
#include <QString>

#include <cstdint>
#include <optional>

#include "core/types.h"

namespace baniphelper::core {

/// 一条被观测到的连接组合。
///
/// 记录的对象是「进程 + 协议 + 两端地址端口」这个组合，不是某一次具体的连接：
/// 同一组合反复出现只更新计数与末次时间，**不新增行**。这是记录量可控的前提。
struct ConnectionObservation {
  ProcessRef process;
  ConnectionKey key;

  /// 累计计数。`counters.since` 无效表示这次拿不到字节数，
  /// 此时界面必须显示为「不可归属」，**不允许填 0 冒充**。
  ConnectionCounters counters;

  QDateTime firstSeen;
  QDateTime lastSeen;

  /// 命中过多少次。用于判断是长连接还是反复重连。
  std::uint64_t hitCount = 0;
};

/// 记录查询条件。所有字段都可留空，留空即不限制。
struct RecordQuery {
  /// 模糊搜索：同时匹配进程名、地址与备注。
  QString searchText;

  std::optional<TransportProtocol> protocol;
  std::optional<ProcessIdentity> process;

  /// 只保留 `lastSeen` 落在该区间内的记录。
  std::optional<QDateTime> from;
  std::optional<QDateTime> to;

  /// 排序字段，取值 `lastSeen`、`firstSeen`、`bytesOut`、`bytesIn`、`hitCount`。
  /// 遇到不认识的取值必须返回 `InvalidArgument`，不得静默退回默认排序。
  QString sortBy;

  bool descending = true;

  /// 上限。界面用虚拟化表格按页取，不允许一次拉全表。
  int limit = 200;
  int offset = 0;
};

/// 一页记录。
struct RecordPage {
  QList<ConnectionObservation> items;

  /// 满足条件的总条数，供界面显示「共 N 条」。
  std::uint64_t totalMatching = 0;

  /// 是否还有下一页。
  bool hasMore = false;
};

/// 记录表的整体状况，供设置页与保留期策略显示。
struct RecordStats {
  std::uint64_t rowCount = 0;
  QDateTime oldestSeen;
  QDateTime newestSeen;
  std::uint64_t databaseBytes = 0;
};

}  // namespace baniphelper::core
