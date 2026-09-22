#pragma once

#include "core/result.h"
#include "platform/api/iconnmonitor.h"

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
/// ⚠️ `subscribe` **不实现**：事件源是阶段三 S3.3 的事（ETW 的
/// `Microsoft-Windows-Kernel-Network`），能力位 `EventDrivenConnections`
/// 因此不声明，本方法如实返回 `NotSupported` 并说明上层该退化成什么。
class WinConnMonitor final : public IConnMonitor {
 public:
  WinConnMonitor() = default;
  ~WinConnMonitor() override;

  WinConnMonitor(const WinConnMonitor&) = delete;
  WinConnMonitor& operator=(const WinConnMonitor&) = delete;

  [[nodiscard]] Result<QList<ConnectionSnapshot>> snapshot() const override;
  [[nodiscard]] Result<SubscriptionId> subscribe(ConnectionEventSink sink) override;
  [[nodiscard]] Result<void> unsubscribe(SubscriptionId id) override;
};

}  // namespace baniphelper::core
