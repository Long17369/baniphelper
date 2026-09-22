#pragma once

#include <QHash>
#include <QList>
#include <QMutex>

#include <cstdint>
#include <memory>

#include "core/result.h"
#include "core/types.h"
#include "platform/api/iconnmonitor.h"
#include "platform/win/etw_network.h"

namespace baniphelper::core {

/// 连接发现的 Windows 实现（阶段三 S3.1）。
///
/// 数据源只有两个公开接口，各自的能力边界是硬的：
///
/// - **TCP**：`GetExtendedTcpTable(TCP_TABLE_OWNER_PID_ALL)`，IPv4 与 IPv6 各一次。
///   它给出四元组、状态与所属进程，是完整的一条连接。
/// - **UDP**：`GetExtendedUdpTable(UDP_TABLE_OWNER_PID)`，IPv4 与 IPv6 各一次。
///   ⚠️ **它只有本地地址、本地端口与进程，没有对端字段**
///   （`MIB_UDPROW_OWNER_PID` 就三个字段）。已实测：只 bind 与已 connect 的套接字
///   在这张表里**完全同形**。所以 UDP 行的 `remote` 是通配取值、`direction` 是
///   `Direction::Unknown` —— 详见 `ConnectionSnapshot` 的接口说明。
///
/// 三条**刻意排除**的行（都在下文的实现注释里给了理由与实测依据）：
///
/// 1. `LISTEN` 状态的行：那是「绑定了端口」，不是一条连接；
/// 2. `PID == 0` 的行：系统已经不把它归属给任何进程（实测本机 371 条 `TIME_WAIT` 全是 0）；
/// 3. 端口为 0 的行。
///
/// 被排除的行都会**写在文档里**，因为「连接清单与系统工具对照一致」这条验收
/// 必须建立在一份明确的口径上，而不是「差不多」。
///
/// ⚠️ 方向是**推断**出来的，判据与残余风险都写在实现里：
/// 本地端口落在「本机处于 LISTEN 的端口集合」里就判为入站，否则判为出站。
/// 实现注释里给出了这个判据为什么成立、以及在什么场景下会判错。
///
/// `subscribe` **已实现**（阶段三 S3.3）：订的是
/// `Microsoft-Windows-Kernel-Network` 的 **TCP 生命周期事件**，能力位
/// `EventDrivenConnections` 因此被声明。三件必须知道的事：
///
/// 1. **UDP 的数据报事件不进这条通道**。它们是**逐报文**的（一次 DNS 风暴就是一次事件风暴），
///    而接口契约要求「同一个连接在同一个状态上只上报一次」；把它们当 `Appeared` 上报会把
///    记录表打爆。UDP 行仍然只从 `snapshot()` 来，逐包数据留给 S3.4 的统计用途；
/// 2. **起 ETW 会话要提权**。权限不足时 `subscribe` 返回 `ErrorCode::NotPermitted`
///    并说明怎么办 —— 不返回「成功但永远收不到事件」；
/// 3. **`TcpConnectFailed`（仅 IPv4 有这个事件）当作 `Disappeared`**，
///    所以一次失败的连接尝试会先 `Appeared` 再 `Disappeared`。IPv6 没有失败事件，
///    那一边失败尝试留下的一条记录只能等后续的关闭事件或保留期清理 ——
///    这一点如实记在阶段文档里，不靠猜。
class WinConnMonitor final : public IConnMonitor {
 public:
  WinConnMonitor();
  ~WinConnMonitor() override;

  WinConnMonitor(const WinConnMonitor&) = delete;
  WinConnMonitor& operator=(const WinConnMonitor&) = delete;

  [[nodiscard]] Result<QList<ConnectionSnapshot>> snapshot() const override;
  [[nodiscard]] Result<SubscriptionId> subscribe(ConnectionEventSink sink) override;
  [[nodiscard]] Result<void> unsubscribe(SubscriptionId id) override;

 private:
  /// 一个订阅者。
  struct SinkEntry {
    SubscriptionId id = kInvalidSubscription;
    ConnectionEventSink sink;
  };

  /// 取进程标识。**刻意不缓存**：连接生命周期事件是低频的，而按 PID 长期缓存
  /// 会把「PID 被系统复用」变成「A 的程序名挂到 B 的连接上」—— 正是本项目最怕的错法。
  [[nodiscard]] static ProcessRef resolveProcessForEvent(std::uint32_t pid);

  /// 把事件分发给全部订阅者。回调在事件线程上同步执行，因此**不许持锁调用**。
  void dispatchEvent(const ConnectionEvent& event);

  mutable QMutex mutex_;
  QList<SinkEntry> sinks_;
  SubscriptionId nextId_ = 1;
  std::unique_ptr<KernelNetworkSession> session_;
  AppearanceGate gate_;
};

}  // namespace baniphelper::core
