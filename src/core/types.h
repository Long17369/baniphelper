#pragma once

#include <QDateTime>
#include <QList>
#include <QString>

#include <cstdint>
#include <functional>

namespace baniphelper::core {

// ---------------------------------------------------------------------------
// 订阅
// ---------------------------------------------------------------------------

/// 订阅句柄。由平台层或核心层在订阅成功时发放，退订时原样交回。
/// 0 表示无效句柄，任何接口都不允许发放 0。
using SubscriptionId = std::uint64_t;

inline constexpr SubscriptionId kInvalidSubscription = 0;

// ---------------------------------------------------------------------------
// 网络
// ---------------------------------------------------------------------------

enum class AddressFamily : std::uint8_t {
  V4,
  V6,
};

enum class TransportProtocol : std::uint8_t {
  Tcp,
  Udp,
};

/// 数据流方向，站在本机视角：Out 是本机发起，In 是外部发起。
enum class Direction : std::uint8_t {
  Out,
  In,
};

/// IP 地址。
///
/// 一律用字符串保存，**不引入 `in_addr` / `in6_addr` 之类平台结构**，
/// 否则地址族差异会顺着值类型渗进核心层与界面层。
/// 文本形式遵循标准写法：V4 用点分十进制，V6 用 RFC 5952 的压缩冒分十六进制。
struct Address {
  QString text;
  AddressFamily family = AddressFamily::V4;
};

/// 端点 = 地址 + 端口。
struct Endpoint {
  Address address;
  std::uint16_t port = 0;
};

[[nodiscard]] inline bool operator==(const Address& lhs, const Address& rhs) noexcept {
  return lhs.family == rhs.family && lhs.text == rhs.text;
}

[[nodiscard]] inline bool operator==(const Endpoint& lhs, const Endpoint& rhs) noexcept {
  return lhs.address == rhs.address && lhs.port == rhs.port;
}

/// 连接的稳定标识。
///
/// 用「协议 + 两端地址端口」而不是平台句柄：句柄是平台内部的一次性资源，
/// 重启即失效，而记录去重与规则命中判定都要求这个标识可比、可持久化。
struct ConnectionKey {
  TransportProtocol protocol = TransportProtocol::Tcp;
  Endpoint local;
  Endpoint remote;
};

[[nodiscard]] inline bool operator==(const ConnectionKey& lhs, const ConnectionKey& rhs) noexcept {
  return lhs.protocol == rhs.protocol && lhs.local == rhs.local && lhs.remote == rhs.remote;
}

/// 累计计数。
///
/// 口径约定：计数**单调不减**，且只增不减；归零只可能出现在连接被重建时。
/// 若平台只能从某个时刻起计数（例如 IP 层扩展统计是开启后才累计），
/// 必须把该时刻写进 `since`，界面据此标注，**不允许假装是连接的全程计数**。
struct ConnectionCounters {
  std::uint64_t bytesOut = 0;
  std::uint64_t bytesIn = 0;
  std::uint64_t segmentsOut = 0;
  std::uint64_t segmentsIn = 0;

  /// 计数有效的起点。默认构造表示「无有效起点」，即该连接拿不到计数。
  QDateTime since;
};

// ---------------------------------------------------------------------------
// 进程
// ---------------------------------------------------------------------------

/// 进程的**身份**：与 PID 无关，跨进程重启仍然成立。
struct ProcessIdentity {
  /// 可执行文件全路径，Windows 上是 `C:\...` 形式的 DOS 路径。
  ///
  /// 比较时**不区分大小写，也不区分分隔符方向**（`C:\a` 与 `C:/a` 是同一个文件）。
  /// 归一由 `sameProcessIdentity` 负责，调用方不必自己预处理。
  QString imagePath;

  /// 文件名，只用于显示，不参与任何判定。
  QString displayName;
};

/// 进程引用 = 身份 + 当下的 PID。
///
/// `pid` 是「此刻在哪里」，`identity` 是「是谁」。凡是需要跨时间的判断（例如进程是否还活着、
/// 记录归属到哪个程序）一律以 `identity` 为准，**不得用 PID 当身份**：PID 会被系统复用，
/// 会造成把 A 的记录算到 B 头上。
struct ProcessRef {
  ProcessIdentity identity;
  std::uint32_t pid = 0;
};

[[nodiscard]] bool sameProcessIdentity(const ProcessIdentity& lhs,
                                       const ProcessIdentity& rhs) noexcept;

/// 平台内部的进程标识，**对核心层与界面层不透明**。
///
/// 例如 Windows 上它是 NT 设备路径 `\Device\HarddiskVolume3\...`。
/// 核心层只负责在 `ITargetResolver` 与 `IFilterEngine` 之间原样转交，
/// **不得解析、拼接或写进规则与记录**，否则平台概念就上浮了。
struct PlatformTargetId {
  QString value;
};

// ---------------------------------------------------------------------------
// 连接快照
// ---------------------------------------------------------------------------

/// 连接的一次观测结果。
struct ConnectionSnapshot {
  ConnectionKey key;
  ProcessRef process;
  Direction direction = Direction::Out;

  /// 本机视角上的采样时刻，不是「连接创建时刻」。连接的真实创建时间平台未必给得出。
  QDateTime observedAt;
};

}  // namespace baniphelper::core
