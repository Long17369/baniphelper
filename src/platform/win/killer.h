#pragma once

#include "core/result.h"
#include "platform/api/ikiller.h"

namespace baniphelper::core {

/// 连接中断的 Windows 实现。
///
/// 手段是**删除传输控制块**（`SetTcpEntry` + `MIB_TCP_STATE_DELETE_TCB`）。
/// 这是本平台唯一可用的系统接口，因此边界也就由它划定：
///
/// - 只对**已建立的 IPv4 TCP** 有效；
/// - **IPv6 没有等价接口**。Windows 不允许用户态发原始 TCP 报文
///   （Winsock 明文规定「TCP 数据不能经原始套接字发送」），所以连自己构造一个
///   RST 都做不到。能力位 `KillTcpV6` 因此不声明，调用返回 `NotSupported`，
///   上层只能退化成「丢包卡死」；
/// - **UDP 没有连接语义**，没有可删的控制块。`canKill` 返回 `false`
///   （能判断、但断不了），`kill` 返回 `NotSupported`。
///
/// ⚠️ **时序不变量「先封后断」不在本类里。** 本类只做「断」这一个动作，
/// 不判断时机 —— 时机属于编排，在 `core/ban_action.{h,cpp}`。
/// 放在那里而不是这里，是因为它必须对**每一个平台**都成立，
/// 而写在这里就只能靠每个平台的实现各自自觉。
///
/// 需要提权：`SetTcpEntry` 对非管理员返回 `ERROR_ACCESS_DENIED`。
class WinKiller final : public IKiller {
 public:
  WinKiller();
  ~WinKiller() override;

  WinKiller(const WinKiller&) = delete;
  WinKiller& operator=(const WinKiller&) = delete;

  [[nodiscard]] Result<bool> canKill(const ConnectionKey& connection) const override;
  [[nodiscard]] Result<void> kill(const ConnectionKey& connection) override;
  [[nodiscard]] Result<KillReport> killMany(const QList<ConnectionKey>& connections) override;
};

}  // namespace baniphelper::core
