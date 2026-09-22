#pragma once

#include <QList>
#include <QString>

#include <cstdint>
#include <functional>

#include "core/result.h"
#include "core/types.h"

namespace baniphelper::core {

// ---------------------------------------------------------------------------
// 事件的解码结果
// ---------------------------------------------------------------------------

/// 认得出来的 `Microsoft-Windows-Kernel-Network` 事件。
///
/// 事件号按提供程序自己的编号（同一个语义在 IPv4 与 IPv6 下编号不同），
/// 因此**事件号 → 语义**的映射集中在一处（实现里的那张表），别处不得再写死事件号。
enum class NetworkEventKind : std::uint8_t {
  /// 不认得的事件号。**不允许猜**：认不出就当未知，计数上报，不产生任何记录。
  Unknown,
  /// IPv4 事件 12 / IPv6 事件 28：本端**发起**连接。
  TcpConnectAttempt,
  /// IPv4 事件 15 / IPv6 事件 31：本端**接受**了连接。
  TcpAccepted,
  /// IPv4 事件 13 / IPv6 事件 29：连接关闭。
  TcpClosed,
  /// IPv4 事件 17：连接尝试失败。IPv6 **没有**等价事件（清单里 33 号缺失）。
  TcpConnectFailed,
  /// IPv4 事件 16 / IPv6 事件 32：重连尝试。
  TcpReconnectAttempt,
  /// IPv4 事件 10 / IPv6 事件 26：TCP 发出数据。
  TcpDataSent,
  /// IPv4 事件 11 / IPv6 事件 27：TCP 收到数据。
  TcpDataReceived,
  /// IPv4 事件 14 / IPv6 事件 30：TCP 重传。
  TcpDataRetransmitted,
  /// IPv4 事件 18 / IPv6 事件 34：协议代用户复制数据。
  TcpCopiedInProtocol,
  /// IPv4 事件 42 / IPv6 事件 58：UDP 发出数据报。
  UdpDataSent,
  /// IPv4 事件 43 / IPv6 事件 59：UDP 收到数据报。
  UdpDataReceived,
  /// IPv4 事件 49：UDP 连接尝试失败。
  UdpConnectFailed,
};

/// 一条已经解码好的网络事件。
///
/// ⚠️ `source` / `destination` 是**载荷里的 `saddr` / `daddr` 原样**，不是
/// 「本端 / 对端」—— 两者的换算方式在 TCP 与 UDP 上**不一样**，见 `observedConnection()`。
/// 把这一步留在类型外面，是为了让「哪个是原样、哪个是解释过的」在代码里一眼可辨。
struct NetworkEventRecord {
  NetworkEventKind kind = NetworkEventKind::Unknown;
  std::uint16_t eventId = 0;
  AddressFamily family = AddressFamily::V4;
  TransportProtocol protocol = TransportProtocol::Tcp;

  /// 归属进程。⚠️ 取自**载荷里的 `PID` 字段**，不是 `EVENT_RECORD::EventHeader.ProcessId`
  /// —— 后者实测在同一条流量的 4（system）与真实 PID 之间跳，还有 0 的（见阶段三 3.2）。
  std::uint32_t pid = 0;

  /// 本次报文的字节数。连接生命周期类事件的这个值是 0。
  std::uint32_t size = 0;

  /// 载荷 `saddr` / `sport`。
  Endpoint source;

  /// 载荷 `daddr` / `dport`。
  Endpoint destination;
};

/// 一次事件观测到的连接：本端与对端，加方向。
struct ObservedConnection {
  ConnectionKey key;
  Direction direction = Direction::Unknown;
};

/// 把解码结果换算成「本端 / 对端」。
///
/// ⚠️ **两种协议的方向约定不同，这是实测出来的**（探针里两端端口都已知，靠端口分辨）：
///
/// | 事件 | `saddr` 是谁 | 依据 |
/// | --- | --- | --- |
/// | TCP 全部（10/11/12/13/15/17/18…） | **恒为本端** | 回环连接里「收」事件（11）的 `sport` 是**接收方**端口（10359），而那个包实际从 10360 发出 |
/// | UDP `sent`（42/58） | 发包方 = 本端 | `sport` 是发送套接字的端口 |
/// | UDP `received`（43/59） | **发包方 = 对端** | 收包事件的 `sport` 是发送方端口（49254），不是接收套接字的端口（49253） |
///
/// 也就是说：UDP 的载荷是**逐包**的朝向，TCP 的载荷是**逐连接**的朝向。
/// 把 UDP 也按 TCP 那样解释，会让所有接收方向的对端都变成自己 —— 而且不会报错。
[[nodiscard]] ObservedConnection observedConnection(const NetworkEventRecord& record);

/// 连接生命周期事件要订的事件号（尝试、重连、接受、关闭、失败）。
///
/// **刻意不含数据事件**：同一提供程序还带着每个报文的收发事件（10/11/14/18/42/43…），
/// 本机空闲时也有几百条每秒。订阅只订生命周期，靠事件号过滤在**内核侧**就挡掉，
/// 而不是送到用户态再丢 —— 那正是「空闲时零占用」这条设计目标的落点。
[[nodiscard]] QList<std::uint16_t> connectionLifecycleEventIds();

// ---------------------------------------------------------------------------
// 出现去重
// ---------------------------------------------------------------------------

/// 「同一个连接只上报一次出现」的闸门。
///
/// 接口契约要求**不重复上报**（重复上报会把记录表打爆）。
/// 实现里只挡 `Appeared`：同一个键在还没收到关闭之前再报一次出现，一律压掉；
/// **`Disappeared` 不做门口** —— 漏掉一条关闭会在记录里留下一条永远不消失的连接，
/// 比多报一条严重得多，而重复的关闭对一个已经消失的键本来就是无操作。
///
/// ⚠️ 只在**消费线程**上使用，内部不加锁（事件是逐条串行投递的）。
class AppearanceGate {
 public:
  /// 闸门最多记多少条「存在」的连接。
  ///
  /// 上限是必要的：**永不关闭的连接会一直占着位置**（TCPv6 连「尝试失败」事件都没有，
  /// 所以一次失败尝试会被记成一个永不消失的连接）。超出时丢最早的记录 ——
  /// 代价是那条连接下次出现时会多报一条 `Appeared`（`overflowCount` 会记下它），
  /// 而记录表本身有保留期，不会因此失控。
  static constexpr int kMaxTrackedConnections = 4096;

  /// 真 = 该上报这次出现；假 = 这个连接已经处于「存在」状态。
  [[nodiscard]] bool acceptAppeared(const ConnectionKey& key);

  /// 记下一条连接已经消失，允许它下次重新出现时再报一次。
  void noteDisappeared(const ConnectionKey& key);

  /// 当前记着多少条「存在」的连接。
  [[nodiscard]] int trackedCount() const;

  /// 因为上限而丢掉追踪的次数。**不为零时要能被看见**：
  /// 丢掉的代价是那条连接下次出现时会多报一条 `Appeared`。
  [[nodiscard]] int overflowCount() const;

  /// 清空。会话重启后调用：重启前的那批连接不该继续占着位置。
  void clear();

 private:
  QList<ConnectionKey> live_;
  int overflowCount_ = 0;
};

// ---------------------------------------------------------------------------
// 实时会话
// ---------------------------------------------------------------------------

/// `Microsoft-Windows-Kernel-Network` 的实时 ETW 会话。
///
/// 两个必须记住的运行条件：
///
/// 1. **起会话要提权**（管理员或 Performance Log Users 组）。权限不足时 `start`
///    返回 `ErrorCode::NotPermitted` 并说明要怎么办 —— 不返回「成功但收不到事件」；
/// 2. 会话名里带进程号，**同名会话是互斥的**，所以两个进程不会互相顶掉。
///    进 `start` 时会先把同名残留会话停掉（进程崩溃会留下它，此时不起会话会报「已存在」，
///    看起来像权限问题）。
///
/// 事件在**自己的消费者线程**上同步回调，因此回调内不得阻塞、不得做重活，
/// 也不得从回调里调 `stop()`（那会等自己的线程结束）—— 后者会被挡下并给出说明。
class KernelNetworkSession {
 public:
  using Handler = std::function<void(const NetworkEventRecord&)>;

  KernelNetworkSession();
  ~KernelNetworkSession();

  KernelNetworkSession(const KernelNetworkSession&) = delete;
  KernelNetworkSession& operator=(const KernelNetworkSession&) = delete;
  KernelNetworkSession(KernelNetworkSession&&) = delete;
  KernelNetworkSession& operator=(KernelNetworkSession&&) = delete;

  /// 起会话、按事件号过滤、开始投递。已启动时返回 `AlreadyExists`。
  [[nodiscard]] Result<void> start(const QList<std::uint16_t>& eventIds, Handler handler);

  /// 停会话并等消费者线程结束。未启动时返回成功（退出路径会无条件调一次）。
  [[nodiscard]] Result<void> stop();

  [[nodiscard]] bool isRunning() const;

  /// 会话建立以来收到的条数（过滤之后）。
  [[nodiscard]] std::uint64_t receivedCount() const;

  /// 解不出来的条数。**不为零就说明模板与预期不符**，必须能被上层看见并记进日志，
  /// 否则事件会安静地少掉一批。
  [[nodiscard]] std::uint64_t undecodableCount() const;

 private:
  struct Impl;
  Impl* impl_ = nullptr;
};

}  // namespace baniphelper::core
