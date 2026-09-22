// 连接中断的 Windows 实现（阶段二 S2.8）。
//
// 做法只有一条：把已建立连接的传输控制块删掉。系统接口是 `SetTcpEntry`，
// 状态填 `MIB_TCP_STATE_DELETE_TCB`。
//
// ⚠️ 这一条路只覆盖 IPv4 TCP。IPv6 没有等价接口，UDP 没有控制块可删 ——
// 两者都必须**如实报「不支持」**，不允许静默返回成功：
// 用户看到「已断开」而连接其实还在，比看到「这条断不了」危险得多。
//
// 本文件是平台实现，允许包含 Windows 专有头。

#include "platform/win/killer.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>

#include <QString>

#include "core/error.h"
#include "core/types.h"

namespace baniphelper::core {
namespace {

/// 把失败包成中文明确的错误。`action` 是「刚才在做什么」。
Error killError(const QString& action, DWORD code, const QString& api) {
  return makeError(ErrorCode::Platform,
                   QStringLiteral("%1 失败：%2 返回 %3（0x%4）")
                       .arg(action, api)
                       .arg(static_cast<qulonglong>(code))
                       .arg(QString::number(static_cast<qulonglong>(code), 16).toUpper()),
                   static_cast<std::int32_t>(code),
                   api);
}

/// 把 `Address` 里的文本地址转成 `in_addr`（已是网络字节序）。
///
/// 走 W 系列并按项目约定避开代码页问题。地址文本来自规则模型，
/// 是 `QHostAddress::toString()` 的规范形式，这里再解析一次是**校验**：
/// 解析不出来的取值绝不能当成「0.0.0.0 那个地址」送下去 ——
/// 那会把一条本该删掉的连接变成「删掉了另一条」。
bool toNativeV4(const Address& address, in_addr& out) {
  const QString text = address.text.trimmed();
  if (text.isEmpty()) {
    return false;
  }
  return ::InetPtonW(AF_INET, reinterpret_cast<const wchar_t*>(text.utf16()), &out) == 1;
}

/// 一个 `ConnectionKey` 能不能被这条路处理，以及不能时的原因。
///
/// 判据集中在这里，`canKill` 与 `kill` 都调它 —— 两处各写一遍的话，
/// 「界面说能断、真去断却报不支持」迟早会出现。
struct Killability {
  bool ok = false;
  Error reason;
};

Killability judge(const ConnectionKey& connection) {
  if (connection.protocol != TransportProtocol::Tcp) {
    return Killability{false,
                       unsupportedError(QStringLiteral("中断 UDP 的「连接」"),
                                        QStringLiteral("UDP 没有建立与拆除这一步，也就没有可删的"
                                                       "传输控制块。它只在发出数据报时占用端口，"
                                                       "封禁它对后续数据报即刻生效"))};
  }

  const AddressFamily family = connection.remote.address.family;
  if (family != connection.local.address.family) {
    return Killability{
        false,
        makeError(ErrorCode::InvalidArgument,
                  QStringLiteral("连接两端的地址族不一致（本端 %1、对端 %2），"
                                 "这不可能是同一条连接")
                      .arg(connection.local.address.text, connection.remote.address.text))};
  }

  if (family == AddressFamily::V6) {
    return Killability{false,
                       unsupportedError(QStringLiteral("中断 IPv6 连接"),
                                        QStringLiteral("本平台没有等价于删除传输控制块的 IPv6 "
                                                       "接口，而且不允许用户态发原始 TCP 报文，"
                                                       "连自己构造一个 RST 都做不到。"
                                                       "封禁对后续连接仍然有效，"
                                                       "但已建立的那条只能等它自己结束"))};
  }

  in_addr local{};
  in_addr remote{};
  if (!toNativeV4(connection.local.address, local) ||
      !toNativeV4(connection.remote.address, remote)) {
    return Killability{
        false,
        makeError(ErrorCode::InvalidArgument,
                  QStringLiteral("连接两端的地址里有一个不是合法的 IPv4 地址"
                                 "（本端「%1」、对端「%2」）")
                      .arg(connection.local.address.text, connection.remote.address.text))};
  }

  if (connection.local.port == 0 || connection.remote.port == 0) {
    return Killability{false,
                       makeError(ErrorCode::InvalidArgument,
                                 QStringLiteral("连接两端的端口都必须非零（本端 %1、"
                                                "对端 %2）")
                                     .arg(connection.local.port)
                                     .arg(connection.remote.port))};
  }

  return Killability{true, Error{}};
}

/// 把 `ConnectionKey` 翻成 `MIB_TCPROW`。
///
/// ⚠️ 端口要写成 `htons(port)`：这张表里的端口是**网络字节序**，
/// 而它放在一个 `DWORD` 里（高 16 位为空）。直接填主机的端口号，
/// 删掉的会是另一个端口上的连接 —— 而且照样返回成功。
MIB_TCPROW toRow(const ConnectionKey& connection) {
  in_addr local{};
  in_addr remote{};
  ::InetPtonW(
      AF_INET, reinterpret_cast<const wchar_t*>(connection.local.address.text.utf16()), &local);
  ::InetPtonW(
      AF_INET, reinterpret_cast<const wchar_t*>(connection.remote.address.text.utf16()), &remote);

  MIB_TCPROW row{};
  row.dwState = MIB_TCP_STATE_DELETE_TCB;
  row.dwLocalAddr = local.S_un.S_addr;
  row.dwLocalPort = static_cast<DWORD>(::htons(connection.local.port));
  row.dwRemoteAddr = remote.S_un.S_addr;
  row.dwRemotePort = static_cast<DWORD>(::htons(connection.remote.port));
  return row;
}

}  // namespace

WinKiller::WinKiller() = default;
WinKiller::~WinKiller() = default;

Result<bool> WinKiller::canKill(const ConnectionKey& connection) const {
  const Killability verdict = judge(connection);
  if (verdict.ok) {
    return Result<bool>::ok(true);
  }

  // 「能判断，但这条断不了」与「连判断都做不到」是两件事。
  // 前者（协议不对、地址族不支持）返回 ok(false)，界面据此把按钮置灰；
  // 后者（取值本身不合法）返回失败，因为它说明上游给了个不该出现的东西。
  switch (verdict.reason.code) {
    case ErrorCode::NotSupported:
      return Result<bool>::ok(false);
    default:
      return Result<bool>::fail(verdict.reason);
  }
}

Result<void> WinKiller::kill(const ConnectionKey& connection) {
  const Killability verdict = judge(connection);
  if (!verdict.ok) {
    return Result<void>::fail(verdict.reason);
  }

  MIB_TCPROW row = toRow(connection);
  const DWORD code = ::SetTcpEntry(&row);
  if (code != NO_ERROR) {
    if (code == ERROR_ACCESS_DENIED) {
      return Result<void>::fail(
          makeError(ErrorCode::NotPermitted,
                    QStringLiteral("中断连接需要管理员权限：SetTcpEntry 被拒绝（错误 %1）。"
                                   "请以管理员身份重新启动")
                        .arg(static_cast<qulonglong>(code)),
                    static_cast<std::int32_t>(code),
                    QStringLiteral("SetTcpEntry")));
    }
    return Result<void>::fail(killError(QStringLiteral("中断连接 %1:%2 → %3:%4")
                                            .arg(connection.local.address.text)
                                            .arg(connection.local.port)
                                            .arg(connection.remote.address.text)
                                            .arg(connection.remote.port),
                                        code,
                                        QStringLiteral("SetTcpEntry")));
  }
  return Result<void>::ok();
}

Result<KillReport> WinKiller::killMany(const QList<ConnectionKey>& connections) {
  KillReport report;
  report.requested = static_cast<int>(connections.size());

  // 逐条上报，不聚合成一句「部分失败」：用户要能知道**哪一条**还活着。
  for (const ConnectionKey& connection : connections) {
    const Result<void> outcome = kill(connection);
    if (outcome) {
      ++report.killed;
      continue;
    }
    KillFailure failure;
    failure.connection = connection;
    failure.reason = outcome.error().message;
    report.failures.append(failure);
  }

  return Result<KillReport>::ok(report);
}

}  // namespace baniphelper::core
