#pragma once

#include <QDateTime>
#include <QList>

#include <cstdint>

#include "core/record.h"
#include "core/result.h"

namespace baniphelper::core {

/// 连接记录的落库与查询。
///
/// 契约要点：
///
/// - **写入必须批量**。记录策略是「新出现的组合一律落库」，量大时逐条一次事务会把自己拖死，
///   因此实现必须走 WAL 加批量事务，`observe` 只是把数据放进待写缓冲；
/// - **去重口径**：同一个「进程 + 协议 + 两端地址端口」组合重复出现时只更新
///   `lastSeen`、累计计数与 `hitCount`，**不新增行**；
/// - **自动过期与总量上限是同一件事的两面**，实现必须两者都有，
///   否则记录表会无界增长（这是本项目明确列出的风险项）；
/// - 拿不到字节数时把 `counters.since` 置为无效值，**不允许用 0 冒充**，
///   界面据此显示为「不可归属」。
class IRecorder {
 public:
  IRecorder() = default;
  virtual ~IRecorder() = default;

  IRecorder(const IRecorder&) = delete;
  IRecorder& operator=(const IRecorder&) = delete;
  IRecorder(IRecorder&&) = delete;
  IRecorder& operator=(IRecorder&&) = delete;

  /// 记录一次观测。语义是「合并」而不是「插入一行」。
  [[nodiscard]] virtual Result<void> observe(const ConnectionObservation& observation) = 0;

  /// 批量版本。采样线程一次采到大量连接时用它，减少跨层调用次数。
  [[nodiscard]] virtual Result<void> observeMany(
      const QList<ConnectionObservation>& observations) = 0;

  /// 把待写缓冲落盘。退出路径与可能掉电的时刻必须调用，**不得指望析构函数**：
  /// 崩溃与强杀不会走析构，而这两条路径恰恰是过滤器残留与记录丢失的高发场景。
  [[nodiscard]] virtual Result<void> flush() = 0;

  [[nodiscard]] virtual Result<RecordPage> query(const RecordQuery& query) const = 0;

  /// 按保留期清理，返回删除的行数。由调度器按天调用。
  [[nodiscard]] virtual Result<std::uint64_t> purgeBefore(const QDateTime& cutoff) = 0;

  /// 清空全部记录。属于危险操作，调用方必须先走二次确认与审计。
  [[nodiscard]] virtual Result<std::uint64_t> purgeAll() = 0;

  [[nodiscard]] virtual Result<RecordStats> stats() const = 0;
};

}  // namespace baniphelper::core
