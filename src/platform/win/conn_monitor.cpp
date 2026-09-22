// 连接发现的 Windows 实现（阶段三 S3.1）。
//
// 数据源是两个公开接口，边界都是硬的：
//
//   TCP：GetExtendedTcpTable(TCP_TABLE_OWNER_PID_ALL)，v4 与 v6 各一次 —— 四元组 + 状态 + 进程。
//   UDP：GetExtendedUdpTable(UDP_TABLE_OWNER_PID)，v4 与 v6 各一次 —— **只有本地地址/端口 + 进程**。
//
// ⚠️「UDP 有没有对端」这件事是**实测**过的，不是照文档推的（一次性探针）：
//    只 bind 的套接字与已 connect 的套接字在表里完全同形，`MIB_UDPROW_OWNER_PID` 里
//    根本没有对端字段。`netstat` 能打出已 connect 的对端，靠的是**未公开导出**
//    （`objdump -p netstat.exe`：IPHLPAPI!InternalGetUdpTable2 与 NSI!NsiAllocateAndGetTable）。
//    那条路也试过（一次性探针，五种调用形状各放一个进程）：
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
#include <QMutexLocker>
#include <QSet>
#include <QString>

#include <functional>
#include <memory>
#include <vector>

#include "core/error.h"
#include "core/log.h"

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

/// PID → 进程身份。
///
/// ⚠️ 解不出来时**不留空壳、也不编名字**：`imagePath` 与 `displayName` 都留空，
/// 界面据此显示「PID n（读不到程序信息）」。实测未提权时本机 52 个 PID 里只有 20 个能解出
/// （其余是别的会话或受保护进程的 `OpenProcess` 拒绝访问），
/// 所以这条分支一定会被走到，不能当成异常。
[[nodiscard]] ProcessRef resolveProcessNow(DWORD pid) {
  ProcessRef ref;
  ref.pid = pid;

  // PID 0 不是一个进程（系统已不归属），不必白问一次。
  // 调用方本来就过滤掉这类行，这里再挡一次是防止将来别的地方直接调进来。
  if (pid == 0) {
    return ref;
  }

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
  return ref;
}

/// 带缓存的进程解析，供**单次快照**内部使用。
///
/// 缓存不是优化而是**必需**：一台机器上几十条连接常常只属于几个进程，
/// 而 `OpenProcess` + `QueryFullProcessImageNameW` 是这里的绝大部分开销。
/// 缓存的生命周期就是这一次快照，所以不会碰上「PID 被复用」的问题。
class ProcessResolver {
 public:
  [[nodiscard]] ProcessRef resolve(DWORD pid) {
    const auto cached = cache_.constFind(pid);
    if (cached != cache_.constEnd()) {
      return cached.value();
    }

    const ProcessRef ref = resolveProcessNow(pid);
    cache_.insert(pid, ref);
    return ref;
  }

 private:
  QHash<DWORD, ProcessRef> cache_;
};

/// 一条事件要不要变成连接事件，以及变成哪一种。
///
/// 判据（都建立在 `observedConnection` 的实测约定之上）：
///
/// | 事件 | 结果 |
/// | --- | --- |
/// | 连接尝试 / 重连尝试（12/16，28/32） | `Appeared`，方向 Out |
/// | 接受连接（15/31） | `Appeared`，方向 In |
/// | 连接关闭（13/29）、尝试失败（17） | `Disappeared` |
/// | 数据类事件（10/11/14/18/42/43/49/58/59） | **不产生连接事件**，返回 false |
///
/// ⚠️ 把「尝试」当成出现，是有意的：对封禁工具来说「某个程序正在往外连」本身就是
/// 要展示的信息，而**出站方向的连接只有这一个事件**（实测：本机主动连出去时
/// 只有 12 与 13，「接受」事件只在被动一侧产生）。
/// 代价是一次失败的尝试会先出现再消失 —— 如实发生，不掩盖。
[[nodiscard]] bool connectionEventFrom(const NetworkEventRecord& record,
                                       const ProcessRef& process,
                                       ConnectionEvent* out) {
  switch (record.kind) {
    case NetworkEventKind::TcpConnectAttempt:
    case NetworkEventKind::TcpReconnectAttempt:
    case NetworkEventKind::TcpAccepted:
      out->kind = ConnectionEventKind::Appeared;
      break;
    case NetworkEventKind::TcpClosed:
    case NetworkEventKind::TcpConnectFailed:
      out->kind = ConnectionEventKind::Disappeared;
      break;
    default:
      return false;
  }

  const ObservedConnection observed = observedConnection(record);
  out->snapshot.key = observed.key;
  out->snapshot.direction = observed.direction;
  out->snapshot.process = process;
  out->snapshot.observedAt = QDateTime::currentDateTime();
  return true;
}

}  // namespace

WinConnMonitor::WinConnMonitor() = default;

WinConnMonitor::~WinConnMonitor() {
  // 退出路径必须退订：平台侧的 ETW 会话是**进程外**的资源，不收会留在系统里。
  // 析构里没有报错渠道，因此只能尽力而为；装配点会在退出时显式调 unsubscribe。
  if (session_) {
    const Result<void> stopped = session_->stop();
    Q_UNUSED(stopped);
  }
}

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
  // 实测：本地 5502 是监听口的那条判为入站、
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

ProcessRef WinConnMonitor::resolveProcessForEvent(std::uint32_t pid) {
  return resolveProcessNow(pid);
}

void WinConnMonitor::dispatchEvent(const ConnectionEvent& event) {
  // 先把订阅者拷出来，**在锁外**回调：契约明说回调在平台层自己的线程上同步调用，
  // 持锁调用会给界面留一个「回调里再调回来就自锁」的坑。
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

Result<SubscriptionId> WinConnMonitor::subscribe(ConnectionEventSink sink) {
  if (!sink) {
    return Result<SubscriptionId>::fail(
        makeError(ErrorCode::InvalidArgument, QStringLiteral("订阅回调为空，事件无处可去")));
  }

  // ⚠️ 这里持锁调 `start` 是安全的：`start` **不 join 消费者线程**，
  // 那条线程顶多在分发的路上等一下这把锁。
  // 反面例子是 `unsubscribe` 里的 `stop`（它要 join），所以那一边必须先放锁再停 ——
  // 否则「持锁 join 一条正等锁的线程」就是死锁。
  QMutexLocker locker(&mutex_);

  if (sinks_.isEmpty()) {
    // 第一次订阅才开会话：没有订阅者时不该占着内核侧的采集资源
    // （退到空订阅时会停会话，见 unsubscribe）。
    session_ = std::make_unique<KernelNetworkSession>();
    gate_.clear();

    const Result<void> started = session_->start(
        connectionLifecycleEventIds(), [this](const NetworkEventRecord& record) {
          ConnectionEvent event;
          if (!connectionEventFrom(record, resolveProcessForEvent(record.pid), &event)) {
            return;
          }
          if (event.kind == ConnectionEventKind::Appeared) {
            // 「同一个连接在同一个状态上只上报一次」：重复的尝试与重连一律压掉。
            //
            // ⚠️ 闸门的状态与 `sinks_` 共用同一把锁：闸门会在退订时被清空，
            // 而那一刻消费者线程可能正在投递事件。虽然正常路径上「停会话 → 清闸门」
            // 已经把两者错开，但这里仍然加锁 —— 这把锁的代价是每条生命周期事件一次，
            // 换来的是「不必依赖调用顺序也正确」。
            bool report = false;
            {
              QMutexLocker gateLocker(&mutex_);
              report = gate_.acceptAppeared(event.snapshot.key);
            }
            if (!report) {
              return;
            }
          } else {
            // 关闭事件不做门口：漏掉一条关闭会在记录里留下一条永远不消失的连接，
            // 而重复的关闭对一个已经消失的键本来就是无操作。
            QMutexLocker gateLocker(&mutex_);
            gate_.noteDisappeared(event.snapshot.key);
          }
          dispatchEvent(event);
        });
    if (!started) {
      session_.reset();
      return Result<SubscriptionId>::fail(started.error());
    }
    logWrite(LogLevel::Info,
             QStringLiteral("已订阅连接事件：ETW Microsoft-Windows-Kernel-Network，"
                            "按事件号过滤后订了 %1 个生命周期事件")
                 .arg(connectionLifecycleEventIds().size()));
  }

  SinkEntry entry;
  entry.id = nextId_++;
  entry.sink = std::move(sink);
  sinks_.append(entry);
  return Result<SubscriptionId>::ok(entry.id);
}

Result<void> WinConnMonitor::unsubscribe(SubscriptionId id) {
  bool shouldStopSession = false;
  {
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
    shouldStopSession = sinks_.isEmpty() && session_ != nullptr;
  }

  if (shouldStopSession) {
    // **在锁外停**：`stop` 会 join 消费者线程，而那条线程可能正卡在 dispatchEvent 的锁上。
    const Result<void> stopped = session_->stop();
    if (!stopped) {
      // 停不掉要如实报，**不能悄悄把会话留在系统里**：
      // 那会让下一个实例撞上「同名会话已存在」。
      return Result<void>::fail(stopped.error());
    }
    QMutexLocker locker(&mutex_);
    session_.reset();
    gate_.clear();
    logWrite(LogLevel::Info, QStringLiteral("连接事件订阅已全部退订，ETW 会话已停止"));
  }
  return Result<void>::ok();
}

}  // namespace baniphelper::core
