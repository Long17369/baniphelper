// 连接发现的 Windows 实现（阶段三 S3.1）。
//
// 数据源是两个公开接口，边界都是硬的：
//
//   TCP：GetExtendedTcpTable(TCP_TABLE_OWNER_PID_ALL)，v4 与 v6 各一次 —— 四元组 + 状态 + 进程。
//   UDP：GetExtendedUdpTable(UDP_TABLE_OWNER_PID)，v4 与 v6 各一次 —— **只有本地地址/端口 + 进程**。
//
// ⚠️「UDP 有没有对端」这件事是**实测**过的，不是照文档推的（tmp/probe-conn-enum.cpp）：
//    只 bind 的套接字与已 connect 的套接字在表里完全同形，`MIB_UDPROW_OWNER_PID` 里
//    根本没有对端字段。`netstat` 能打出已 connect 的对端，靠的是**未公开导出**
//    （`objdump -p netstat.exe`：IPHLPAPI!InternalGetUdpTable2 与 NSI!NsiAllocateAndGetTable）。
//    那条路也试过（tmp/probe-udp-shapes.cpp、tmp/probe-udp-shape-d.cpp）：
//    五种调用形状里四种直接访问违例，唯一不崩的那一种给出的表**结构无法证实**
//    （`HeapSize` 18156 字节与行数 96 除不尽，也看不到对端），
//    于是按「宁可标未知，不猜」回退 —— 真实对端等 S3.4 的事件源。
//
// 本文件是平台实现，允许包含 Windows 专有头。

#include "platform/win/conn_monitor.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <tcpmib.h>

#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <QString>

#include <functional>
#include <vector>

#include "core/error.h"

namespace baniphelper::core {
namespace {

/// 端点表里一条**未经解释**的原始记录。UDP 的 `remote` 恒为空：表里没有这个字段。
struct RawRow {
  AddressFamily family = AddressFamily::V4;
  TransportProtocol protocol = TransportProtocol::Tcp;
  QString localAddress;
  std::uint16_t localPort = 0;
  QString remoteAddress;
  std::uint16_t remotePort = 0;
  DWORD state = 0;
  DWORD pid = 0;
};

/// 把 Win32 错误包成中文明确的失败说明。
Error tableError(const QString& what, DWORD code) {
  return makeError(ErrorCode::Platform,
                   QStringLiteral("枚举连接失败：%1 返回 %2（0x%3）")
                       .arg(what)
                       .arg(static_cast<qulonglong>(code))
                       .arg(QString::number(static_cast<qulonglong>(code), 16).toUpper()),
                   static_cast<std::int32_t>(code),
                   QStringLiteral("IP Helper"));
}

/// 地址字节 → 文本。v6 走 `InetNtopW`，它给的就是 RFC 5952 的压缩写法（与 `netstat` 同一套）。
QString addressText(int family, const void* bytes) {
  wchar_t text[64] = {};
  if (::InetNtopW(family, const_cast<void*>(bytes), text, 64) == nullptr) {
    return QString();
  }
  return QString::fromWCharArray(text);
}

/// 表里的端口是「网络字节序放在 DWORD 的低 16 位」（与 S2.8 断连那边同一个约定）。
std::uint16_t hostPort(DWORD rawPort) {
  return ::ntohs(static_cast<u_short>(rawPort));
}

/// 取一张端点表。
///
/// 「先问大小、再取一次」是这个 API 族的固定用法；两次之间表会变（连接一直在建立与消失），
/// 所以 `ERROR_INSUFFICIENT_BUFFER` 要**重试**而不是当成失败 ——
/// 一次抖动就让整个快照失败，会让界面上「刷新」按钮变成一个随机出错的东西。
template <typename Fetch>
Result<std::vector<unsigned char>> readTable(Fetch fetch, const QString& label) {
  DWORD size = 0;
  DWORD code = fetch(nullptr, &size);
  if (code != ERROR_INSUFFICIENT_BUFFER) {
    return Result<std::vector<unsigned char>>::fail(
        tableError(label + QStringLiteral("（问大小）"), code));
  }

  for (int attempt = 0; attempt < 4; ++attempt) {
    std::vector<unsigned char> buffer(size == 0 ? 1 : size);
    DWORD used = size;
    code = fetch(buffer.data(), &used);
    if (code == NO_ERROR) {
      buffer.resize(used);
      return Result<std::vector<unsigned char>>::ok(std::move(buffer));
    }
    if (code == ERROR_INSUFFICIENT_BUFFER) {
      size = used;  // 表变大了，按新的要求重来
      continue;
    }
    return Result<std::vector<unsigned char>>::fail(tableError(label, code));
  }

  return Result<std::vector<unsigned char>>::fail(
      tableError(label + QStringLiteral("（连续几次之间表一直在变）"), ERROR_INSUFFICIENT_BUFFER));
}

Result<void> readTcpTable(AddressFamily family, QList<RawRow>& out) {
  const ULONG rawFamily = family == AddressFamily::V4 ? AF_INET : AF_INET6;
  const QString label = family == AddressFamily::V4 ? QStringLiteral("TCP 表（IPv4）")
                                                    : QStringLiteral("TCP 表（IPv6）");
  auto fetched = readTable(
      [rawFamily](void* table, DWORD* size) {
        return ::GetExtendedTcpTable(table, size, FALSE, rawFamily, TCP_TABLE_OWNER_PID_ALL, 0);
      },
      label);
  if (!fetched) {
    return Result<void>::fail(fetched.error());
  }
  const std::vector<unsigned char>& bytes = fetched.value();

  if (family == AddressFamily::V4) {
    const auto* table = reinterpret_cast<const MIB_TCPTABLE_OWNER_PID*>(bytes.data());
    for (DWORD i = 0; i < table->dwNumEntries; ++i) {
      const MIB_TCPROW_OWNER_PID& row = table->table[i];
      RawRow one;
      one.family = AddressFamily::V4;
      one.protocol = TransportProtocol::Tcp;
      one.localAddress = addressText(AF_INET, &row.dwLocalAddr);
      one.localPort = hostPort(row.dwLocalPort);
      one.remoteAddress = addressText(AF_INET, &row.dwRemoteAddr);
      one.remotePort = hostPort(row.dwRemotePort);
      one.state = row.dwState;
      one.pid = row.dwOwningPid;
      out.append(one);
    }
  } else {
    const auto* table = reinterpret_cast<const MIB_TCP6TABLE_OWNER_PID*>(bytes.data());
    for (DWORD i = 0; i < table->dwNumEntries; ++i) {
      const MIB_TCP6ROW_OWNER_PID& row = table->table[i];
      RawRow one;
      one.family = AddressFamily::V6;
      one.protocol = TransportProtocol::Tcp;
      one.localAddress = addressText(AF_INET6, row.ucLocalAddr);
      one.localPort = hostPort(row.dwLocalPort);
      one.remoteAddress = addressText(AF_INET6, row.ucRemoteAddr);
      one.remotePort = hostPort(row.dwRemotePort);
      one.state = row.dwState;
      one.pid = row.dwOwningPid;
      out.append(one);
    }
  }
  return Result<void>::ok();
}

Result<void> readUdpTable(AddressFamily family, QList<RawRow>& out) {
  const ULONG rawFamily = family == AddressFamily::V4 ? AF_INET : AF_INET6;
  const QString label = family == AddressFamily::V4 ? QStringLiteral("UDP 表（IPv4）")
                                                    : QStringLiteral("UDP 表（IPv6）");
  auto fetched = readTable(
      [rawFamily](void* table, DWORD* size) {
        return ::GetExtendedUdpTable(table, size, FALSE, rawFamily, UDP_TABLE_OWNER_PID, 0);
      },
      label);
  if (!fetched) {
    return Result<void>::fail(fetched.error());
  }
  const std::vector<unsigned char>& bytes = fetched.value();

  if (family == AddressFamily::V4) {
    const auto* table = reinterpret_cast<const MIB_UDPTABLE_OWNER_PID*>(bytes.data());
    for (DWORD i = 0; i < table->dwNumEntries; ++i) {
      const MIB_UDPROW_OWNER_PID& row = table->table[i];
      RawRow one;
      one.family = AddressFamily::V4;
      one.protocol = TransportProtocol::Udp;
      one.localAddress = addressText(AF_INET, &row.dwLocalAddr);
      one.localPort = hostPort(row.dwLocalPort);
      one.pid = row.dwOwningPid;
      out.append(one);
    }
  } else {
    const auto* table = reinterpret_cast<const MIB_UDP6TABLE_OWNER_PID*>(bytes.data());
    for (DWORD i = 0; i < table->dwNumEntries; ++i) {
      const MIB_UDP6ROW_OWNER_PID& row = table->table[i];
      RawRow one;
      one.family = AddressFamily::V6;
      one.protocol = TransportProtocol::Udp;
      one.localAddress = addressText(AF_INET6, row.ucLocalAddr);
      one.localPort = hostPort(row.dwLocalPort);
      one.pid = row.dwOwningPid;
      out.append(one);
    }
  }
  return Result<void>::ok();
}

/// PID → 进程身份，带缓存。
///
/// 缓存不是优化而是**必需**：一台机器上几十条连接常常只属于几个进程，
/// 而 `OpenProcess` + `QueryFullProcessImageNameW` 是这里的绝大部分开销。
///
/// ⚠️ 解不出来时**不留空壳、也不编名字**：`imagePath` 与 `displayName` 都留空，
/// 界面据此显示「PID n（读不到程序信息）」。实测未提权时本机 52 个 PID 里只有 20 个能解出
/// （其余是别的会话或受保护进程的 `OpenProcess` 拒绝访问），
/// 所以这条分支一定会被走到，不能当成异常。
class ProcessResolver {
 public:
  [[nodiscard]] ProcessRef resolve(DWORD pid) {
    const auto cached = cache_.constFind(pid);
    if (cached != cache_.constEnd()) {
      return cached.value();
    }

    ProcessRef ref;
    ref.pid = pid;

    // PID 0 不是一个进程（系统已不归属），不必白问一次。
    // 调用方本来就过滤掉这类行，这里再挡一次是防止将来别的地方直接调进来。
    if (pid != 0) {
      const HANDLE handle = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
      if (handle != nullptr) {
        wchar_t path[1024] = {};
        DWORD length = 1024;
        if (::QueryFullProcessImageNameW(handle, 0, path, &length) != FALSE) {
          ref.identity.imagePath = QString::fromWCharArray(path, static_cast<int>(length));
          ref.identity.displayName = QFileInfo(ref.identity.imagePath).fileName();
        }
        ::CloseHandle(handle);
      }
    }

    cache_.insert(pid, ref);
    return ref;
  }

 private:
  QHash<DWORD, ProcessRef> cache_;
};

}  // namespace

WinConnMonitor::~WinConnMonitor() = default;

Result<QList<ConnectionSnapshot>> WinConnMonitor::snapshot() const {
  QList<RawRow> tcpRows;
  for (const AddressFamily family : {AddressFamily::V4, AddressFamily::V6}) {
    const Result<void> read = readTcpTable(family, tcpRows);
    if (!read) {
      // 一张表读不出来就整体失败，**不返回半份清单**：
      // 半份清单在界面上与「完整清单」长得一模一样，用户会据此以为某个程序没有连接。
      return Result<QList<ConnectionSnapshot>>::fail(read.error());
    }
  }

  // 方向判据（推断，不是平台给的）：
  //
  //   **本地端口落在「本机处于 LISTEN 的端口集合」里 ⇒ 这条连接是别人连进来的。**
  //
  // 为什么成立：`accept()` 出来的套接字，本地端口就是监听端口；而本机主动发起的连接
  // 拿的是临时端口，且**不可能**撞上某个正在监听（或已绑定）的端口 ——
  // 绑同一个端口会被系统拒掉。所以这个判据在「端口共享」以外的场景下是可靠的。
  // 实测（tmp/probe-conn-enum.cpp）：本地 5502 是监听口的那条判为入站、
  // 本地 5503 是临时口的那条判为出站，与构造时一致。
  //
  // ⚠️ 刻意**只按端口比、不按地址比**：监听套接字常绑在 `0.0.0.0`，
  // 而被接受的连接本地地址是具体网卡地址，按地址比会把入站全判成出站。
  //
  // 已知会判错的场景：`SO_REUSEADDR` 端口共享（例如 HTTP.sys 那类）下，
  // 一个进程可能用某个也被别人监听的端口发起出站连接。数量级极小，且方向只影响显示。
  QSet<std::uint16_t> listeningPorts4;
  QSet<std::uint16_t> listeningPorts6;
  for (const RawRow& row : tcpRows) {
    if (row.state != MIB_TCP_STATE_LISTEN) {
      continue;
    }
    if (row.localPort == 0) {
      continue;
    }
    (row.family == AddressFamily::V4 ? listeningPorts4 : listeningPorts6).insert(row.localPort);
  }

  ProcessResolver resolver;
  const QDateTime sampledAt = QDateTime::currentDateTime();
  QList<ConnectionSnapshot> snapshots;

  for (const RawRow& row : tcpRows) {
    // LISTEN 行是「绑定了端口」，不是一条连接，排掉（方向判据已经用过了）。
    if (row.state == MIB_TCP_STATE_LISTEN) {
      continue;
    }
    // PID 0：系统不把这一行归属给任何进程。实测本机 371 条 TIME_WAIT 全是这种情况，
    // 它们既没有归属程序、也没有对端会再通信，收进清单只会把界面刷满噪声。
    if (row.pid == 0) {
      continue;
    }
    if (row.localPort == 0 || row.remotePort == 0) {
      continue;
    }

    ConnectionSnapshot one;
    one.key.protocol = TransportProtocol::Tcp;
    one.key.local.address.family = row.family;
    one.key.local.address.text = row.localAddress;
    one.key.local.port = row.localPort;
    one.key.remote.address.family = row.family;
    one.key.remote.address.text = row.remoteAddress;
    one.key.remote.port = row.remotePort;
    one.process = resolver.resolve(row.pid);

    const QSet<std::uint16_t>& listeningPorts =
        row.family == AddressFamily::V4 ? listeningPorts4 : listeningPorts6;
    one.direction = listeningPorts.contains(row.localPort) ? Direction::In : Direction::Out;
    one.observedAt = sampledAt;
    snapshots.append(one);
  }

  QList<RawRow> udpRows;
  for (const AddressFamily family : {AddressFamily::V4, AddressFamily::V6}) {
    const Result<void> read = readUdpTable(family, udpRows);
    if (!read) {
      return Result<QList<ConnectionSnapshot>>::fail(read.error());
    }
  }

  for (const RawRow& row : udpRows) {
    if (row.localPort == 0) {
      continue;
    }

    ConnectionSnapshot one;
    one.key.protocol = TransportProtocol::Udp;
    one.key.local.address.family = row.family;
    one.key.local.address.text = row.localAddress;
    one.key.local.port = row.localPort;

    // 通配取值 = 「未指定/不可知」，**不是**「对端是 0.0.0.0」。
    // 表里根本没有对端字段（已实测），所以这里只能这么写；真实对端等 S3.4 的事件源。
    one.key.remote.address.family = row.family;
    one.key.remote.address.text =
        row.family == AddressFamily::V4 ? QStringLiteral("0.0.0.0") : QStringLiteral("::");
    one.key.remote.port = 0;

    one.process = resolver.resolve(row.pid);
    one.direction = Direction::Unknown;
    one.observedAt = sampledAt;
    snapshots.append(one);
  }

  return Result<QList<ConnectionSnapshot>>::ok(std::move(snapshots));
}

Result<SubscriptionId> WinConnMonitor::subscribe(ConnectionEventSink sink) {
  Q_UNUSED(sink);
  return Result<SubscriptionId>::fail(unsupportedError(
      QStringLiteral("按事件订阅连接变化"),
      QStringLiteral("事件源（ETW 的 Microsoft-Windows-Kernel-Network 提供程序）属于阶段三 S3.3，"
                     "尚未落地，因此本后端**不声明** EventDrivenConnections 能力。"
                     "在此之前上层只能轮询 snapshot()，而且必须如实标注精度差异 ——"
                     "两次轮询之间的短连接会整条漏掉，这一点要显示给用户，不能悄悄降级")));
}

Result<void> WinConnMonitor::unsubscribe(SubscriptionId id) {
  Q_UNUSED(id);
  // 契约要求：对未知句柄退订返回成功。这里本来就没有可退的东西，但**不能报错** ——
  // 退出路径会把「退订」当成收尾动作无条件调一次，报错只会制造假警报。
  return Result<void>::ok();
}

}  // namespace baniphelper::core
