// `Microsoft-Windows-Kernel-Network` 实时事件的订阅与解码（阶段三 S3.3）。
//
// 本文件是平台实现，允许包含 Windows 专有头。
//
// 两件在写代码之前**实测**过的事（一次性探针，两端端口都已知）：
//
// 1. **TCP 与 UDP 的载荷朝向约定不一样**：
//    - TCP 的每条事件（收 11 / 发 10 / 尝试 12 / 接受 15 / 关闭 13）里 `saddr` **都是本端**；
//      判据：回环连接里「收」事件的 `sport` 是**接收方**的端口（10359），
//      而那个包实际是从 10360 发出的；
//    - UDP 的载荷是**逐包**朝向：`sent`（42/58）里 `saddr` 是本端，
//      `received`（43/59）里 `saddr` 是**对端**（同一组回环里，收包事件的 `sport` 是 49254，
//      即发送方的端口，而接收套接字在 49253）。
//    把 UDP 按 TCP 那样解释，会让所有接收方向的对端都变成自己，而且不会报错。
// 2. **归属进程只认载荷里的 `PID` 字段**：`EVENT_RECORD::EventHeader.ProcessId` 实测会在
//    4（system）、0 与真实 PID 之间跳。
//
// 还有一条与性能直接相关的取舍：同一个提供程序也带着**逐报文**的数据事件
// （TCP 收发/重传、UDP 收发），本机空闲时也有几百条每秒。连接发现只订生命周期事件，
// 并且是在**内核侧**按事件号过滤掉的，不是送到用户态再丢 —— 这是「空闲时零占用」的落点。
//
// ⚠️ `EVENT_FILTER_TYPE_EVENT_ID` 与 `EVENT_FILTER_EVENT_ID` 在 MinGW 的头文件里没有，
// 按 MS Learn「Event Filtering」的定义补了一份（数值 0x80000200）。
// 补错不会静默失效：`EnableTraceEx2` 会直接报错，而且订阅侧还有一次按事件号的复核。

#include "platform/win/etw_network.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <evntcons.h>
#include <evntrace.h>
#include <tdh.h>

#include <QString>

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/error.h"

namespace baniphelper::core {
namespace {

/// `Microsoft-Windows-Kernel-Network` 的提供程序 GUID。
/// 出处：`logman query providers Microsoft-Windows-Kernel-Network` 与本机 `wevtutil gp`。
const GUID kKernelNetworkProvider = {
    0x7dd42a49, 0x5329, 0x4832, {0x8d, 0xfd, 0x43, 0xd9, 0x79, 0x15, 0x3a, 0x88}};

/// 两个地址族的关键字。事件自带的 keywords 就是这两位之一（失败类事件两位都带）。
constexpr ULONGLONG kKeywordIPv4 = 0x8000000000000010ULL;
constexpr ULONGLONG kKeywordIPv6 = 0x8000000000000020ULL;

/// `EVENT_FILTER_TYPE_EVENT_ID`。MinGW 的头里没有这个常量，来源是 MS Learn 的事件过滤文档。
constexpr ULONG kEventFilterTypeEventId = 0x80000200;

/// `EVENT_FILTER_EVENT_ID` 的布局：`FilterIn` + 保留 + 个数 + 事件号数组。
/// 同样是 MinGW 缺的那一个（它的 `evntprov.h` 只给到 `EVENT_FILTER_TYPE_TRACEHANDLE`）。
struct EventIdFilter {
  BOOLEAN filterIn = TRUE;
  UCHAR reserved = 0;
  USHORT count = 0;
  // 紧随其后是 count 个 USHORT 事件号（用一段连续缓冲承载）。
};

/// 一个事件号对应的语义。**事件号只能在实现里的这张表里出现一次。**
struct EventSpec {
  std::uint16_t eventId;
  NetworkEventKind kind;
  AddressFamily family;
  TransportProtocol protocol;
};

constexpr EventSpec kEventSpecs[] = {
    {10, NetworkEventKind::TcpDataSent, AddressFamily::V4, TransportProtocol::Tcp},
    {11, NetworkEventKind::TcpDataReceived, AddressFamily::V4, TransportProtocol::Tcp},
    {12, NetworkEventKind::TcpConnectAttempt, AddressFamily::V4, TransportProtocol::Tcp},
    {13, NetworkEventKind::TcpClosed, AddressFamily::V4, TransportProtocol::Tcp},
    {14, NetworkEventKind::TcpDataRetransmitted, AddressFamily::V4, TransportProtocol::Tcp},
    {15, NetworkEventKind::TcpAccepted, AddressFamily::V4, TransportProtocol::Tcp},
    {16, NetworkEventKind::TcpReconnectAttempt, AddressFamily::V4, TransportProtocol::Tcp},
    {17, NetworkEventKind::TcpConnectFailed, AddressFamily::V4, TransportProtocol::Tcp},
    {18, NetworkEventKind::TcpCopiedInProtocol, AddressFamily::V4, TransportProtocol::Tcp},
    {26, NetworkEventKind::TcpDataSent, AddressFamily::V6, TransportProtocol::Tcp},
    {27, NetworkEventKind::TcpDataReceived, AddressFamily::V6, TransportProtocol::Tcp},
    {28, NetworkEventKind::TcpConnectAttempt, AddressFamily::V6, TransportProtocol::Tcp},
    {29, NetworkEventKind::TcpClosed, AddressFamily::V6, TransportProtocol::Tcp},
    {30, NetworkEventKind::TcpDataRetransmitted, AddressFamily::V6, TransportProtocol::Tcp},
    {31, NetworkEventKind::TcpAccepted, AddressFamily::V6, TransportProtocol::Tcp},
    {32, NetworkEventKind::TcpReconnectAttempt, AddressFamily::V6, TransportProtocol::Tcp},
    // ⚠️ TCPv6 **没有**「连接尝试失败」事件（清单里 33 号不存在）。
    {34, NetworkEventKind::TcpCopiedInProtocol, AddressFamily::V6, TransportProtocol::Tcp},
    {42, NetworkEventKind::UdpDataSent, AddressFamily::V4, TransportProtocol::Udp},
    {43, NetworkEventKind::UdpDataReceived, AddressFamily::V4, TransportProtocol::Udp},
    {49, NetworkEventKind::UdpConnectFailed, AddressFamily::V4, TransportProtocol::Udp},
    {58, NetworkEventKind::UdpDataSent, AddressFamily::V6, TransportProtocol::Udp},
    {59, NetworkEventKind::UdpDataReceived, AddressFamily::V6, TransportProtocol::Udp},
};

[[nodiscard]] const EventSpec* findEventSpec(std::uint16_t eventId) {
  for (const EventSpec& spec : kEventSpecs) {
    if (spec.eventId == eventId) {
      return &spec;
    }
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// 字段取值
// ---------------------------------------------------------------------------

/// 按**字段名**取原始字节。
///
/// 不按类型编号解释：MinGW 的 `tdh.h` 里没有 `TDH_INTYPE_*` 那一族常量，
/// 照 SDK 手抄一份数值表有抄错的风险，而抄错的后果是「字段看着有值、值是错的」。
/// 这里改成按取回来的字节数判断该按什么读，与探针里验证过的那套完全一致。
[[nodiscard]] bool propertyBytes(PEVENT_RECORD record,
                                 const wchar_t* name,
                                 std::vector<unsigned char>* out) {
  PROPERTY_DATA_DESCRIPTOR descriptor{};
  // ⚠️ 这个字段在头里的类型是 `ULONGLONG`：它存的是**字符串的地址**，不是 `PCWSTR`。
  descriptor.PropertyName = reinterpret_cast<ULONGLONG>(name);
  descriptor.ArrayIndex = ULONG_MAX;
  descriptor.Reserved = 0;

  ULONG size = 0;
  if (::TdhGetPropertySize(record, 0, nullptr, 1, &descriptor, &size) != ERROR_SUCCESS ||
      size == 0) {
    return false;
  }
  out->assign(size, 0);
  return ::TdhGetProperty(record, 0, nullptr, 1, &descriptor, size, out->data()) == ERROR_SUCCESS;
}

[[nodiscard]] bool propertyUInt32(PEVENT_RECORD record, const wchar_t* name, std::uint32_t* out) {
  std::vector<unsigned char> bytes;
  if (!propertyBytes(record, name, &bytes) || bytes.size() != 4) {
    return false;
  }
  std::memcpy(out, bytes.data(), 4);
  return true;
}

/// 端口：两字节**网络序**（已由 DNS 的 53 端口定过案），要 `ntohs`。
[[nodiscard]] bool propertyPort(PEVENT_RECORD record, const wchar_t* name, std::uint16_t* out) {
  std::vector<unsigned char> bytes;
  if (!propertyBytes(record, name, &bytes) || bytes.size() != 2) {
    return false;
  }
  std::uint16_t raw = 0;
  std::memcpy(&raw, bytes.data(), 2);
  *out = ::ntohs(raw);
  return true;
}

/// 地址：v4 四字节、v6 十六字节，都交给 `InetNtopW` 出文本
/// （与连接枚举那边同一套，v6 就是 RFC 5952 的压缩写法）。
[[nodiscard]] bool propertyAddress(PEVENT_RECORD record,
                                   const wchar_t* name,
                                   AddressFamily family,
                                   QString* out) {
  std::vector<unsigned char> bytes;
  if (!propertyBytes(record, name, &bytes)) {
    return false;
  }
  const int expected = family == AddressFamily::V4 ? 4 : 16;
  if (bytes.size() != static_cast<std::size_t>(expected)) {
    return false;
  }
  wchar_t text[64] = {};
  const int rawFamily = family == AddressFamily::V4 ? AF_INET : AF_INET6;
  if (::InetNtopW(rawFamily, bytes.data(), text, 64) == nullptr) {
    return false;
  }
  *out = QString::fromWCharArray(text);
  return true;
}

/// 一条记录解码失败时的原因，只为日志与「解不出来多少条」的计数服务。
enum class DecodeFailure {
  None,
  /// 事件号不在表里（或者模板取不到）。**认不出就当认不出，不猜**。
  UnknownEvent,
  Pid,
  Endpoints,
};

[[nodiscard]] DecodeFailure decodeInto(PEVENT_RECORD record, NetworkEventRecord* out) {
  const std::uint16_t eventId = record->EventHeader.EventDescriptor.Id;
  const EventSpec* spec = findEventSpec(eventId);
  if (spec == nullptr) {
    // 认不出的事件号**不猜**：交给上层计数与记录，不产生任何记录。
    return DecodeFailure::UnknownEvent;
  }

  out->eventId = eventId;
  out->kind = spec->kind;
  out->family = spec->family;
  out->protocol = spec->protocol;

  if (!propertyUInt32(record, L"PID", &out->pid)) {
    return DecodeFailure::Pid;
  }

  // `size` 只有数据类事件才有；连接类事件取不到就是 0，不算失败。
  std::uint32_t size = 0;
  if (propertyUInt32(record, L"size", &size)) {
    out->size = size;
  }

  std::uint16_t sport = 0;
  std::uint16_t dport = 0;
  QString saddr;
  QString daddr;
  if (!propertyAddress(record, L"saddr", spec->family, &saddr) ||
      !propertyAddress(record, L"daddr", spec->family, &daddr) ||
      !propertyPort(record, L"sport", &sport) || !propertyPort(record, L"dport", &dport)) {
    return DecodeFailure::Endpoints;
  }

  out->source.address.family = spec->family;
  out->source.address.text = saddr;
  out->source.port = sport;
  out->destination.address.family = spec->family;
  out->destination.address.text = daddr;
  out->destination.port = dport;
  return DecodeFailure::None;
}

// ---------------------------------------------------------------------------
// 方向与去重的判据
// ---------------------------------------------------------------------------

/// 这次事件是「本端发出的数据」还是「本端收到的数据」。
/// 只有数据类事件用得上；连接生命周期事件没有逐包方向。
[[nodiscard]] bool isInboundPayload(NetworkEventKind kind) {
  switch (kind) {
    case NetworkEventKind::TcpDataReceived:
    case NetworkEventKind::UdpDataReceived:
      return true;
    default:
      return false;
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// 事件号 → 本端 / 对端
// ---------------------------------------------------------------------------

ObservedConnection observedConnection(const NetworkEventRecord& record) {
  ObservedConnection observed;
  observed.key.protocol = record.protocol;

  // UDP 的载荷是**逐包**朝向：收包事件里 `saddr` 是对端。
  if (record.protocol == TransportProtocol::Udp && isInboundPayload(record.kind)) {
    observed.key.local = record.destination;
    observed.key.remote = record.source;
    observed.direction = Direction::In;
    return observed;
  }

  // 其余全部（含 TCP 的收包事件）：`saddr` 就是本端，见文件头的实测说明。
  observed.key.local = record.source;
  observed.key.remote = record.destination;

  switch (record.kind) {
    case NetworkEventKind::TcpConnectAttempt:
    case NetworkEventKind::TcpReconnectAttempt:
      observed.direction = Direction::Out;
      break;
    case NetworkEventKind::TcpAccepted:
      observed.direction = Direction::In;
      break;
    case NetworkEventKind::TcpDataReceived:
      observed.direction = Direction::In;
      break;
    case NetworkEventKind::TcpDataSent:
    case NetworkEventKind::TcpDataRetransmitted:
    case NetworkEventKind::TcpCopiedInProtocol:
    case NetworkEventKind::UdpDataSent:
      observed.direction = Direction::Out;
      break;
    case NetworkEventKind::TcpClosed:
    case NetworkEventKind::TcpConnectFailed:
    case NetworkEventKind::UdpConnectFailed:
      // ⚠️ 关闭与失败事件里判不出方向：回环实验里两端**各自**产生一条关闭事件，
      // 都把自己放在 `saddr`，所以「谁先关的」这件事事件本身没说。
      // 如实标 Unknown，**不挑一个填上** —— 方向不是连接键的一部分，不影响匹配。
      observed.direction = Direction::Unknown;
      break;
    case NetworkEventKind::UdpDataReceived:
    case NetworkEventKind::Unknown:
      observed.direction = Direction::Unknown;
      break;
  }
  return observed;
}

QList<std::uint16_t> connectionLifecycleEventIds() {
  QList<std::uint16_t> ids;
  for (const EventSpec& spec : kEventSpecs) {
    switch (spec.kind) {
      case NetworkEventKind::TcpConnectAttempt:
      case NetworkEventKind::TcpReconnectAttempt:
      case NetworkEventKind::TcpAccepted:
      case NetworkEventKind::TcpClosed:
      case NetworkEventKind::TcpConnectFailed:
        ids.append(spec.eventId);
        break;
      default:
        break;
    }
  }
  return ids;
}

// ---------------------------------------------------------------------------
// 出现去重
// ---------------------------------------------------------------------------

bool AppearanceGate::acceptAppeared(const ConnectionKey& key) {
  for (const ConnectionKey& live : live_) {
    if (live == key) {
      return false;
    }
  }
  if (live_.size() >= static_cast<int>(kMaxTrackedConnections)) {
    live_.removeFirst();
    ++overflowCount_;
  }
  live_.append(key);
  return true;
}

void AppearanceGate::noteDisappeared(const ConnectionKey& key) {
  live_.removeAll(key);
}

int AppearanceGate::trackedCount() const {
  return static_cast<int>(live_.size());
}

int AppearanceGate::overflowCount() const {
  return overflowCount_;
}

void AppearanceGate::clear() {
  live_.clear();
  overflowCount_ = 0;
}

// ---------------------------------------------------------------------------
// 实时会话
// ---------------------------------------------------------------------------

struct KernelNetworkSession::Impl {
  QString sessionName;

  /// 会话名的宽字符形式。`EVENT_TRACE_LOGFILEW::LoggerName` 要的是**可写的**宽串指针，
  /// 而那根指针在 `ProcessTrace` 运行期间还要有效，所以存成成员，不用局部量。
  std::wstring sessionNameWide;

  Handler handler;

  /// 会话属性缓冲。`stop` 还要拿它去 `ControlTraceW`，所以必须留着，
  /// 不能只在 `start` 里当局部变量。
  std::vector<unsigned char> propertiesBuffer;

  /// 过滤描述符指向的缓冲。`EnableTraceEx2` 只在调用期间读它，
  /// 但把它留成成员更省事，也避免「指向已释放栈内存」这类难查的问题。
  std::vector<unsigned char> filterBuffer;
  ENABLE_TRACE_PARAMETERS enableParameters{};
  EVENT_FILTER_DESCRIPTOR filterDescriptor{};

  TRACEHANDLE sessionHandle = 0;
  TRACEHANDLE consumerHandle = INVALID_PROCESSTRACE_HANDLE;
  std::thread consumer;
  std::thread::id consumerThreadId;
  std::atomic<bool> running{false};
  std::atomic<std::uint64_t> received{0};
  std::atomic<std::uint64_t> undecodable{0};

  mutable std::mutex stateMutex;

  static void WINAPI eventRecordCallback(PEVENT_RECORD record);

  void onRecord(PEVENT_RECORD record) {
    ++received;
    NetworkEventRecord decoded;
    if (decodeInto(record, &decoded) != DecodeFailure::None) {
      ++undecodable;
      return;
    }
    // 复核一次事件号：过滤是内核侧的事，真出了岔子（过滤器被忽略）也在这里挡住，
    // 不让数据事件涌进上层。
    const EventSpec* spec = findEventSpec(decoded.eventId);
    if (spec == nullptr) {
      ++undecodable;
      return;
    }

    Handler sink;
    {
      std::lock_guard<std::mutex> lock(stateMutex);
      sink = handler;
    }
    if (sink) {
      // ⚠️ 回调在**消费者线程**上同步执行（接口契约如此），因此这里不能持锁调用。
      sink(decoded);
    }
  }
};

void WINAPI KernelNetworkSession::Impl::eventRecordCallback(PEVENT_RECORD record) {
  if (record == nullptr || record->UserContext == nullptr) {
    return;
  }
  auto* self = static_cast<Impl*>(record->UserContext);
  self->onRecord(record);
}

namespace {

[[nodiscard]] QString win32Text(DWORD code) {
  return QStringLiteral("0x%1").arg(static_cast<qulonglong>(code), 8, 16, QLatin1Char('0'));
}

/// 把 ETW 的失败码翻成错误分类。
///
/// ⚠️ **权限不足这件事在两处都会出现，而且不一定是第一处**：实测未提权时
/// `StartTraceW` 会**成功**（会话建出来了），失败发生在 `EnableTraceEx2`
/// （返回 5），因为权限检查是在启用系统提供程序时才做的。
/// 只查第一处的话，界面上看到的是一个不知道该怎么办的「平台错误」。
[[nodiscard]] Error etwFailure(const QString& what, DWORD code, const QString& nativeSource) {
  if (code == ERROR_ACCESS_DENIED) {
    return makeError(ErrorCode::NotPermitted,
                     QStringLiteral("%1 被系统拒绝：需要以管理员身份运行"
                                    "（或属于 Performance Log Users 组）")
                         .arg(what),
                     static_cast<std::int32_t>(code),
                     nativeSource);
  }
  if (code == ERROR_ALREADY_EXISTS) {
    return makeError(ErrorCode::Busy,
                     QStringLiteral("%1 失败：同名 ETW 会话已经存在"
                                    "（可能是上次运行的残留），稍后重试")
                         .arg(what),
                     static_cast<std::int32_t>(code),
                     nativeSource);
  }
  return makeError(ErrorCode::Platform,
                   QStringLiteral("%1 失败：%2 返回 %3").arg(what, nativeSource, win32Text(code)),
                   static_cast<std::int32_t>(code),
                   nativeSource);
}

}  // namespace

KernelNetworkSession::KernelNetworkSession() : impl_(new Impl()) {
  // 会话名带进程号：同名会话是互斥的，带上 PID 之后两个进程不会互相顶掉。
  impl_->sessionName =
      QStringLiteral("BanIPHelper-KernelNetwork-%1").arg(::GetCurrentProcessId());
}

KernelNetworkSession::~KernelNetworkSession() {
  const Result<void> stopped = stop();
  Q_UNUSED(stopped);  // 析构里没有可用的报错渠道；启动路径与退出路径都会显式调 stop()
  delete impl_;
}

Result<void> KernelNetworkSession::start(const QList<std::uint16_t>& eventIds, Handler handler) {
  if (!handler) {
    return Result<void>::fail(
        makeError(ErrorCode::InvalidArgument, QStringLiteral("订阅回调为空，事件无处可去")));
  }
  if (eventIds.isEmpty()) {
    return Result<void>::fail(makeError(
        ErrorCode::InvalidArgument, QStringLiteral("订阅的事件号清单为空：那会订到全部事件")));
  }
  if (impl_->running.load()) {
    return Result<void>::fail(
        makeError(ErrorCode::AlreadyExists, QStringLiteral("这个实时会话已经在运行了")));
  }

  // ---- 过滤 blob：[EVENT_FILTER_EVENT_ID 头][事件号数组] ----
  const std::size_t headerSize = sizeof(EventIdFilter);
  const std::size_t eventsSize = static_cast<std::size_t>(eventIds.size()) * sizeof(USHORT);
  impl_->filterBuffer.assign(headerSize + eventsSize, 0);
  auto* filter = reinterpret_cast<EventIdFilter*>(impl_->filterBuffer.data());
  filter->filterIn = TRUE;
  filter->count = static_cast<USHORT>(eventIds.size());
  auto* events = reinterpret_cast<USHORT*>(impl_->filterBuffer.data() + headerSize);
  for (int i = 0; i < eventIds.size(); ++i) {
    events[i] = static_cast<USHORT>(eventIds.at(i));
  }

  impl_->filterDescriptor.Ptr = reinterpret_cast<ULONGLONG>(impl_->filterBuffer.data());
  impl_->filterDescriptor.Size = static_cast<ULONG>(impl_->filterBuffer.size());
  impl_->filterDescriptor.Type = kEventFilterTypeEventId;
  impl_->enableParameters = ENABLE_TRACE_PARAMETERS{};
  impl_->enableParameters.Version = ENABLE_TRACE_PARAMETERS_VERSION_2;
  impl_->enableParameters.EnableFilterDesc = &impl_->filterDescriptor;
  impl_->enableParameters.FilterDescCount = 1;

  // ---- 会话属性 ----
  impl_->sessionNameWide = impl_->sessionName.toStdWString();
  const std::wstring& sessionName = impl_->sessionNameWide;
  const ULONG nameBytes = static_cast<ULONG>((sessionName.size() + 1) * sizeof(wchar_t));
  const ULONG propertiesSize = sizeof(EVENT_TRACE_PROPERTIES) + nameBytes;
  impl_->propertiesBuffer.assign(propertiesSize, 0);
  auto* properties = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(impl_->propertiesBuffer.data());
  properties->Wnode.BufferSize = propertiesSize;
  properties->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
  properties->Wnode.ClientContext = 1;  // 时间戳用 QPC
  properties->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
  properties->BufferSize = 64;
  properties->MinimumBuffers = 8;
  properties->MaximumBuffers = 32;
  properties->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
  std::memcpy(impl_->propertiesBuffer.data() + sizeof(EVENT_TRACE_PROPERTIES),
              sessionName.c_str(),
              nameBytes);

  // 先把同名会话停掉：进程上一次崩掉会留下它，此时 StartTrace 报「已存在」，
  // 看起来像权限问题，实际是残留。
  ::ControlTraceW(0, sessionName.c_str(), properties, EVENT_TRACE_CONTROL_STOP);
  // 停过一次会把 properties 的内容改掉（BufferSize 等），重新填一遍。
  properties->Wnode.BufferSize = propertiesSize;
  properties->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
  properties->Wnode.ClientContext = 1;
  properties->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
  properties->BufferSize = 64;
  properties->MinimumBuffers = 8;
  properties->MaximumBuffers = 32;
  properties->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);

  TRACEHANDLE session = 0;
  DWORD code = ::StartTraceW(&session, sessionName.c_str(), properties);
  if (code != ERROR_SUCCESS) {
    return Result<void>::fail(
        etwFailure(QStringLiteral("启动 ETW 实时会话"), code, QStringLiteral("ETW StartTraceW")));
  }
  impl_->sessionHandle = session;

  code = ::EnableTraceEx2(session,
                          &kKernelNetworkProvider,
                          EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                          TRACE_LEVEL_VERBOSE,
                          kKeywordIPv4 | kKeywordIPv6,
                          0,
                          0,
                          &impl_->enableParameters);
  if (code != ERROR_SUCCESS) {
    ::ControlTraceW(session, sessionName.c_str(), properties, EVENT_TRACE_CONTROL_STOP);
    impl_->sessionHandle = 0;
    return Result<void>::fail(etwFailure(
        QStringLiteral("订阅 Microsoft-Windows-Kernel-Network"),
        code,
        QStringLiteral("ETW EnableTraceEx2")));
  }

  EVENT_TRACE_LOGFILEW logFile{};
  logFile.LoggerName = const_cast<LPWSTR>(sessionName.c_str());
  logFile.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
  logFile.Context = impl_;
  logFile.EventRecordCallback = &Impl::eventRecordCallback;
  const TRACEHANDLE consumer = ::OpenTraceW(&logFile);
  if (consumer == INVALID_PROCESSTRACE_HANDLE) {
    ::ControlTraceW(session, sessionName.c_str(), properties, EVENT_TRACE_CONTROL_STOP);
    impl_->sessionHandle = 0;
    return Result<void>::fail(makeError(ErrorCode::Platform,
                                        QStringLiteral("打不开实时消费者（OpenTraceW 失败）"),
                                        static_cast<std::int32_t>(::GetLastError()),
                                        QStringLiteral("ETW")));
  }
  impl_->consumerHandle = consumer;
  {
    std::lock_guard<std::mutex> lock(impl_->stateMutex);
    impl_->handler = std::move(handler);
  }

  impl_->running.store(true);
  impl_->consumer = std::thread([this, consumer]() {
    impl_->consumerThreadId = std::this_thread::get_id();
    // ProcessTrace 要的是**可写的**句柄数组指针（它会在内部改写）。
    TRACEHANDLE handle = consumer;
    ::ProcessTrace(&handle, 1, nullptr, nullptr);
  });

  return Result<void>::ok();
}

Result<void> KernelNetworkSession::stop() {
  if (!impl_->running.load()) {
    return Result<void>::ok();
  }
  if (std::this_thread::get_id() == impl_->consumerThreadId) {
    // 等自己的线程结束是死锁。宁可明确报错，也不要挂住整个程序。
    return Result<void>::fail(makeError(
        ErrorCode::Busy,
        QStringLiteral("不能在事件回调所在的线程里停会话（会等自己结束），请在别的线程退订")));
  }

  const std::wstring& sessionName = impl_->sessionNameWide;
  auto* properties = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(impl_->propertiesBuffer.data());
  // 停会话会让 ProcessTrace 返回，消费者线程随之结束。
  ::ControlTraceW(impl_->sessionHandle, sessionName.c_str(), properties, EVENT_TRACE_CONTROL_STOP);
  impl_->sessionHandle = 0;
  if (impl_->consumer.joinable()) {
    impl_->consumer.join();
  }
  // join 之后不会再有回调进来，此时才安全地把回调清掉。
  {
    std::lock_guard<std::mutex> lock(impl_->stateMutex);
    impl_->handler = nullptr;
  }
  if (impl_->consumerHandle != INVALID_PROCESSTRACE_HANDLE) {
    ::CloseTrace(impl_->consumerHandle);
    impl_->consumerHandle = INVALID_PROCESSTRACE_HANDLE;
  }
  impl_->running.store(false);
  return Result<void>::ok();
}

bool KernelNetworkSession::isRunning() const {
  return impl_->running.load();
}

std::uint64_t KernelNetworkSession::receivedCount() const {
  return impl_->received.load();
}

std::uint64_t KernelNetworkSession::undecodableCount() const {
  return impl_->undecodable.load();
}

}  // namespace baniphelper::core
