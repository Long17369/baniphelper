#pragma once

#include <QHash>
#include <QList>
#include <QMutex>
#include <QString>

#include <cstdint>

#include "core/result.h"
#include "core/types.h"
#include "platform/api/itrafficstats.h"

namespace baniphelper::core {

/// 字节统计的 Windows 实现（阶段三 S3.2）。
///
/// 数据源只有一条：**TCP 按连接的扩展统计**（eStats），
/// `GetPerTcpConnectionEStats` 取 `TCP_ESTATS_DATA_ROD_v0`。
/// UDP **没有**对应的按连接接口（本机头文件里只有 `tcpestats.h`，没有 UDP 版），
/// 所以 `TrafficStatsUdp` 不声明，UDP 的字节数留给 S3.4 的事件源。
///
/// 六条实测结论（都由一个一次性探针跑出来，写实现前必须知道）：
///
/// | 项 | 结论 |
/// | --- | --- |
/// | 未启用就读 | ⚠️ **返回成功但值是垃圾**（未初始化内存），不是 0、也不报错 |
/// | 启用后读到的范围 | **覆盖整条连接**（启用前传过的字节也在里面），不是「自启用起」 |
/// | 关掉再打开 | 计数不丢，关采集期间传的字节也在里面 |
/// | 需求状态位吗 | **不需要**：只给四元组 + `MIB_TCP_STATE_ESTAB` 与表里的行读数一致 |
/// | 回环 | 支持，两侧方向互为镜像 |
/// | 提权 | `SetPerTcpConnectionEStats` 未提权返回 5（拒绝访问），必须报「权限不足」 |
/// | 连接已消失 | 返回 1214（`ERROR_INVALID_NETNAME`），不是静默 0 |
///
/// 「未启用读到的值是垃圾」这一条决定了实现必须**自己记着哪些连接启用过**，
/// 不能把「读不到」交给 API 去报 —— 它会安静地给你一个看着像真数字的值。
///
/// ⚠️ 已知口径缺口：计数实际覆盖**整条连接**，而连接建立时刻平台给不出来，
/// 因此 `ConnectionCounters::since` 填的是「我们第一次对它启用采集的时刻」，
/// 它是**保守下限**（真实覆盖起点不晚于它，只会更早）。
/// 界面不能据此写「自此刻起计数」。要精确表达「覆盖更早」需要扩展值类型，
/// 走变更控制，已记进阶段文档。
class WinTrafficStats final : public ITrafficStats {
 public:
  WinTrafficStats();
  ~WinTrafficStats() override;

  WinTrafficStats(const WinTrafficStats&) = delete;
  WinTrafficStats& operator=(const WinTrafficStats&) = delete;

  [[nodiscard]] Result<void> enableCollection(const ConnectionKey& connection) override;
  [[nodiscard]] Result<void> disableCollection(const ConnectionKey& connection) override;
  [[nodiscard]] Result<ConnectionCounters> read(const ConnectionKey& connection) const override;
  [[nodiscard]] Result<QList<ConnectionCounters>> readMany(
      const QList<ConnectionKey>& connections) const override;

  /// 当前记着多少条「已启用采集」的连接。退出前清空是**必须**的：
  /// eStats 的跟踪资源挂在连接上，不关就一直在内核里记。
  [[nodiscard]] int trackedCount() const;

 private:
  /// 一条已启用采集的连接：键 + 我们开始采集的时刻（`since` 的保守下限）。
  struct EnabledEntry {
    ConnectionKey key;
    QDateTime since;
  };

  /// 查「这条连接启用过吗」。
  ///
  /// ⚠️ 用线性查找是**权衡过的**：条数就是本机正在被统计的连接数（实测几百条量级），
  /// 而四元组要进哈希得先在核心层加 `qHash(ConnectionKey)`。等 S3.5 需要按连接分桶
  /// 落库时再把哈希下沉到核心层更合适，那时它有两个使用方。
  [[nodiscard]] bool findEnabled(const ConnectionKey& connection, QDateTime* since) const;

  mutable QMutex mutex_;
  QList<EnabledEntry> enabled_;
};

}  // namespace baniphelper::core
