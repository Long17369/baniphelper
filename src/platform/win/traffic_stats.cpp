// 字节统计的 Windows 实现（阶段三 S3.2）。本文件是平台实现，可以包含 Windows 专有头。
//
// 全部判据来自一次性探针的实测（结论写在头文件的表里），其中三条决定了
// 这个实现的形状：
//
// 1. **未启用采集就调 `GetPerTcpConnectionEStats` 会「成功」并返回垃圾**（未初始化内存）。
//    所以「启用过没有」必须由我们自己记账，读之前先查账 —— 指望 API 报错是不行的。
// 2. **计数覆盖整条连接**（含启用之前传过的字节），而连接建立时刻拿不到 →
//    `since` 只能填「我们开始采集的时刻」，并按**保守下限**来解释。
// 3. **不需要状态位**：只拿四元组构造的行（`MIB_TCP_STATE_ESTAB`）与从端点表里取的行
//    读到同一个数。接口只给四元组，这一条让它能落地。
//
// ⚠️ 构造行时要处理 IPv6 的**作用域标识**：连接快照里的 v6 文本是 `InetNtopW` 给的，
// 链路本地地址会带 `%12` 这样的后缀（S2.5 记过这件事）。`ucLocalAddr` 只有 16 字节，
// 作用域要单独放进 `dwLocalScopeId`。

#include "platform/win/traffic_stats.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <tcpmib.h>
#include <tcpestats.h>

#include <QMutexLocker>
#include <QString>

#include "core/error.h"

namespace baniphelper::core {
namespace {

/// 把 Win32 错误码翻成带中文说明的失败。
///
/// 1214（`ERROR_INVALID_NETNAME`）是「这条连接在表里已经没有了」——
/// 实测对不存在的四元组，`Get` 与 `Set` 都返回它。别把它当成泛泛的平台错误：
/// 连接被重建、被关掉都会走到这里，上层要能区分「读不到」与「出错了」。
[[nodiscard]] Error esStatsFailure(const QString& what, DWORD code) {
  if (code == ERROR_ACCESS_DENIED) {
    return makeError(ErrorCode::NotPermitted,
                     QStringLiteral("%1 被系统拒绝：需要以管理员身份运行").arg(what),
                     static_cast<std::int32_t>(code),
                     QStringLiteral("IP Helper eStats"));
  }
  if (code == ERROR_INVALID_NETNAME) {
    return makeError(ErrorCode::NotFound,
                     QStringLiteral("%1 失败：这条连接已经不存在了（可能已经断开或被重建）")
                         .arg(what),
                     static_cast<std::int32_t>(code),
                     QStringLiteral("IP Helper eStats"));
  }
  return makeError(ErrorCode::Platform,
                   QStringLiteral("%1 失败：返回 %2").arg(what).arg(static_cast<qulonglong>(code)),
                   static_cast<std::int32_t>(code),
                   QStringLiteral("IP Helper eStats"));
}

/// 地址文本 → 16 或 4 字节。返回作用域标识（只有 v6 的链路本地地址会带）。
///
/// 文本里可能带 `%<数字>`：那是**接口**的事（S2.5 的实测），
/// `ucLocalAddr` 放不下它，必须切出来单独交给 `dwLocalScopeId`。
[[nodiscard]] bool parseAddress(const Address& address, PVOID out, DWORD* scopeId) {
  QString text = address.text.trimmed();
  *scopeId = 0;

  const int percent = text.indexOf(QLatin1Char('%'));
  if (percent >= 0) {
    bool ok = false;
    const uint value = text.mid(percent + 1).toUInt(&ok);
    if (!ok) {
      return false;
    }
    *scopeId = value;
    text = text.left(percent);
  }

  const int family = address.family == AddressFamily::V4 ? AF_INET : AF_INET6;
  return ::InetPtonW(family, reinterpret_cast<const wchar_t*>(text.utf16()), out) == 1;
}

/// 四元组 → eStats 要的那一行。**状态位是猜着填的常量**，而实测证明这样够用。
[[nodiscard]] bool makeRow(const ConnectionKey& key, MIB_TCPROW* row, MIB_TCP6ROW* row6) {
  if (key.protocol != TransportProtocol::Tcp) {
    return false;
  }
  if (key.local.address.family != key.remote.address.family) {
    return false;  // 两端地址族必须一致，混着来构造不出合法的行
  }

  DWORD localScope = 0;
  DWORD remoteScope = 0;
  if (key.local.address.family == AddressFamily::V4) {
    MIB_TCPROW one{};
    one.dwState = MIB_TCP_STATE_ESTAB;
    if (!parseAddress(key.local.address, &one.dwLocalAddr, &localScope) ||
        !parseAddress(key.remote.address, &one.dwRemoteAddr, &remoteScope)) {
      return false;
    }
    one.dwLocalPort = ::htons(key.local.port);
    one.dwRemotePort = ::htons(key.remote.port);
    *row = one;
    return true;
  }

  MIB_TCP6ROW one{};
  // ⚠️ `MIB_TCP6ROW`（不是 `MIB_TCP6ROW_OWNER_PID`）的成员顺序与名字与 SDK 一致：
  // `State` 在最前，地址字段叫 `LocalAddr` / `RemoteAddr`（端点表那个结构才是 `ucLocalAddr`）。
  one.State = MIB_TCP_STATE_ESTAB;
  if (!parseAddress(key.local.address, &one.LocalAddr, &localScope) ||
      !parseAddress(key.remote.address, &one.RemoteAddr, &remoteScope)) {
    return false;
  }
  one.dwLocalScopeId = localScope;
  one.dwRemoteScopeId = remoteScope;
  one.dwLocalPort = ::htons(key.local.port);
  one.dwRemotePort = ::htons(key.remote.port);
  *row6 = one;
  return true;
}

[[nodiscard]] bool isV6(const ConnectionKey& key) {
  return key.local.address.family == AddressFamily::V6;
}

/// 开或关采集。
[[nodiscard]] DWORD setCollection(const ConnectionKey& key, bool enable) {
  MIB_TCPROW row{};
  MIB_TCP6ROW row6{};
  if (!makeRow(key, &row, &row6)) {
    return ERROR_INVALID_PARAMETER;
  }
  TCP_ESTATS_DATA_RW_v0 rw{};
  rw.EnableCollection = enable ? TRUE : FALSE;
  if (isV6(key)) {
    return ::SetPerTcp6ConnectionEStats(
        &row6, TcpConnectionEstatsData, reinterpret_cast<PUCHAR>(&rw), 0, sizeof(rw), 0);
  }
  return ::SetPerTcpConnectionEStats(
      &row, TcpConnectionEstatsData, reinterpret_cast<PUCHAR>(&rw), 0, sizeof(rw), 0);
}

/// 读一次 `TCP_ESTATS_DATA_ROD_v0`。
[[nodiscard]] DWORD readData(const ConnectionKey& key, TCP_ESTATS_DATA_ROD_v0* out) {
  MIB_TCPROW row{};
  MIB_TCP6ROW row6{};
  if (!makeRow(key, &row, &row6)) {
    return ERROR_INVALID_PARAMETER;
  }
  if (isV6(key)) {
    return ::GetPerTcp6ConnectionEStats(&row6,
                                        TcpConnectionEstatsData,
                                        nullptr,
                                        0,
                                        0,
                                        nullptr,
                                        0,
                                        0,
                                        reinterpret_cast<PUCHAR>(out),
                                        0,
                                        sizeof(*out));
  }
  return ::GetPerTcpConnectionEStats(&row,
                                     TcpConnectionEstatsData,
                                     nullptr,
                                     0,
                                     0,
                                     nullptr,
                                     0,
                                     0,
                                     reinterpret_cast<PUCHAR>(out),
                                     0,
                                     sizeof(*out));
}

}  // namespace

WinTrafficStats::WinTrafficStats() = default;

WinTrafficStats::~WinTrafficStats() {
  // 析构里没有报错渠道，只能尽力把跟踪关掉；调用方应当显式 disable 过。
  // ⚠️ 不关的后果是内核继续为这条连接维护计数器，而且没有地方能看到这件事。
  QMutexLocker locker(&mutex_);
  for (const EnabledEntry& entry : enabled_) {
    const DWORD ignored = setCollection(entry.key, false);
    Q_UNUSED(ignored);
  }
  enabled_.clear();
}

bool WinTrafficStats::findEnabled(const ConnectionKey& connection, QDateTime* since) const {
  for (const EnabledEntry& entry : enabled_) {
    if (entry.key == connection) {
      if (since != nullptr) {
        *since = entry.since;
      }
      return true;
    }
  }
  return false;
}

Result<void> WinTrafficStats::enableCollection(const ConnectionKey& connection) {
  if (connection.protocol != TransportProtocol::Tcp) {
    // UDP 没有按连接的 eStats（本机头文件里就没有 UDP 版），不许在这里假装能统计。
    return Result<void>::fail(unsupportedError(
        QStringLiteral("按连接统计 UDP 的字节数"),
        QStringLiteral("Windows 没有 UDP 版的按连接扩展统计（只有 TCP 的 tcpestats），"
                       "UDP 的字节数必须走事件源，属于 S3.4")));
  }

  const QDateTime now = QDateTime::currentDateTime();
  {
    QMutexLocker locker(&mutex_);
    if (findEnabled(connection, nullptr)) {
      // 契约：对已启用的连接重复调用是安全的。**不能重置 since** ——
      // 那会让「从我第一次开始采集」这个下限往后漂。
      return Result<void>::ok();
    }
  }

  const DWORD code = setCollection(connection, true);
  if (code != NO_ERROR) {
    return Result<void>::fail(
        esStatsFailure(QStringLiteral("对这条连接启用字节统计"), code));
  }

  QMutexLocker locker(&mutex_);
  EnabledEntry entry;
  entry.key = connection;
  entry.since = now;  // 保守下限，含义见头文件
  enabled_.append(entry);
  return Result<void>::ok();
}

Result<void> WinTrafficStats::disableCollection(const ConnectionKey& connection) {
  {
    QMutexLocker locker(&mutex_);
    if (!findEnabled(connection, nullptr)) {
      // 没启用过：没有可释放的东西。契约只要求「退出前要停」，不要求报错。
      return Result<void>::ok();
    }
  }

  const DWORD code = setCollection(connection, false);
  // 连接已经消失了的话，跟踪资源由系统一并回收，这不算失败 ——
  // 退出收尾时会走这条路径，报错只是制造假警报。
  if (code != NO_ERROR && code != ERROR_INVALID_NETNAME) {
    return Result<void>::fail(esStatsFailure(QStringLiteral("停止这条连接的字节统计"), code));
  }

  QMutexLocker locker(&mutex_);
  for (int i = 0; i < enabled_.size(); ++i) {
    if (enabled_.at(i).key == connection) {
      enabled_.removeAt(i);
      break;
    }
  }
  return Result<void>::ok();
}

Result<ConnectionCounters> WinTrafficStats::read(const ConnectionKey& connection) const {
  QDateTime since;
  {
    QMutexLocker locker(&mutex_);
    if (!findEnabled(connection, &since)) {
      // ⚠️ 这条检查是**必须的**，不是防御性代码：实测未启用时
      // `GetPerTcpConnectionEStats` 返回成功、并给出一个看着像真数字的垃圾值。
      // 交给 API 去报错就会把垃圾当流量显示出去。
      return Result<ConnectionCounters>::fail(makeError(
          ErrorCode::InvalidArgument,
          QStringLiteral("还没对这条连接启用采集；未启用时的读数是未初始化内存的值，不可信")));
    }
  }

  TCP_ESTATS_DATA_ROD_v0 rod{};
  const DWORD code = readData(connection, &rod);
  if (code != NO_ERROR) {
    return Result<ConnectionCounters>::fail(
        esStatsFailure(QStringLiteral("读取这条连接的字节统计"), code));
  }

  ConnectionCounters counters;
  counters.bytesOut = rod.DataBytesOut;
  counters.bytesIn = rod.DataBytesIn;
  counters.segmentsOut = rod.DataSegsOut;
  counters.segmentsIn = rod.DataSegsIn;
  counters.since = since;
  return Result<ConnectionCounters>::ok(counters);
}

Result<QList<ConnectionCounters>> WinTrafficStats::readMany(
    const QList<ConnectionKey>& connections) const {
  // 批量读故意**不整体失败**：连接一直在这台机器上建立与消失，
  // 「快照里有它、读的时候没了」是常态，整体失败会让界面每次刷新都报错。
  // 拿不到计数的那一条按值类型自己的语义表示：`since` 空 = 「该连接拿不到计数」。
  // 想区分「连接没了」与「本来就不支持」，用单条 `read()`（它会明确报 NotFound）。
  QList<ConnectionCounters> results;
  results.reserve(connections.size());
  for (const ConnectionKey& connection : connections) {
    const Result<ConnectionCounters> one = read(connection);
    if (one) {
      results.append(one.value());
      continue;
    }
    const ErrorCode code = one.error().code;
    if (code == ErrorCode::NotFound || code == ErrorCode::InvalidArgument) {
      results.append(ConnectionCounters());
      continue;
    }
    // 系统性的失败（权限不足、平台错误）必须整体报出来，不能变成一堆「无计数」。
    return Result<QList<ConnectionCounters>>::fail(one.error());
  }
  return Result<QList<ConnectionCounters>>::ok(std::move(results));
}

int WinTrafficStats::trackedCount() const {
  QMutexLocker locker(&mutex_);
  return static_cast<int>(enabled_.size());
}

}  // namespace baniphelper::core
