// 事件解码约定的单元测试（阶段三 S3.3）。
//
// 这里测的都是**从实测里定下来的约定**，它们错的后果都很隐蔽：
//
// - `saddr` 在 TCP 与 UDP 上指的**不是同一端**：TCP 每一条事件里都是本端，
//   而 UDP 的收包事件里是**对端**。解释错会让接收方向的对端全变成自己，
//   而且不报错、不崩溃，只是记录全错；
// - 订阅只订生命周期事件，数据事件（每报文一条）不能进这条通道；
// - 同一个连接只报一次出现，否则记录表会被重连风暴打爆。
//
// 真实流量下的事件本身就是一次性的行为证据，放在演练里；这里锁的是判据本身。

#include <QTest>

#include <QList>

#include <algorithm>

#include "core/types.h"
#include "platform/win/etw_network.h"

using namespace baniphelper::core;

namespace {

/// 造一条事件记录。地址文本用不可路由的取值，避免与真实数据混淆。
NetworkEventRecord makeRecord(NetworkEventKind kind, AddressFamily family, TransportProtocol proto) {
  NetworkEventRecord record;
  record.kind = kind;
  record.family = family;
  record.protocol = proto;
  record.pid = 4242;
  const bool v4 = family == AddressFamily::V4;
  record.source.address.family = family;
  record.source.address.text = v4 ? QStringLiteral("203.0.113.7") : QStringLiteral("2001:db8::7");
  record.source.port = 51000;
  record.destination.address.family = family;
  record.destination.address.text =
      v4 ? QStringLiteral("203.0.113.8") : QStringLiteral("2001:db8::8");
  record.destination.port = 443;
  return record;
}

ConnectionKey makeKey(std::uint16_t localPort) {
  ConnectionKey key;
  key.protocol = TransportProtocol::Tcp;
  key.local.address.family = AddressFamily::V4;
  key.local.port = localPort;
  key.remote.address.family = AddressFamily::V4;
  key.remote.port = 443;
  return key;
}

}  // namespace

class EtwNetworkTest : public QObject {
  Q_OBJECT

 private slots:
  void lifecycleListCoversBothFamiliesOnly();
  void tcpEventsPutLocalEndpointInSource();
  void udpSentPutsLocalEndpointInSource();
  void udpReceivedPutsRemoteEndpointInSource();
  void appearanceGateReportsEachConnectionOnce();
  void appearanceGateIsBounded();
};

/// 订阅清单只能是生命周期事件：数据事件一个都不能有。
void EtwNetworkTest::lifecycleListCoversBothFamiliesOnly() {
  const QList<std::uint16_t> ids = connectionLifecycleEventIds();
  QList<std::uint16_t> sorted = ids;
  std::sort(sorted.begin(), sorted.end());

  // IPv4 与 IPv6 各一份，加上 IPv4 独有的「尝试失败」（17）。
  const QList<std::uint16_t> expected = {12, 13, 15, 16, 17, 28, 29, 31, 32};
  QCOMPARE(sorted, expected);

  // 数据事件（TCP 收发/重传/代复制、UDP 收发）一个都不许出现。
  for (const std::uint16_t dataEvent : {10, 11, 14, 18, 26, 27, 30, 34, 42, 43, 49, 58, 59}) {
    QVERIFY2(!ids.contains(dataEvent),
             qPrintable(QStringLiteral("订阅清单里混进了数据事件 %1："
                                       "那是每个报文一条，会把用户态的入口打爆")
                            .arg(dataEvent)));
  }
}

/// TCP：**每一条**事件的 `saddr` 都是本端。
void EtwNetworkTest::tcpEventsPutLocalEndpointInSource() {
  const NetworkEventRecord attempt =
      makeRecord(NetworkEventKind::TcpConnectAttempt, AddressFamily::V4, TransportProtocol::Tcp);
  const ObservedConnection outbound = observedConnection(attempt);
  QVERIFY(outbound.key.local == attempt.source);
  QVERIFY(outbound.key.remote == attempt.destination);
  QCOMPARE(outbound.direction, Direction::Out);

  const NetworkEventRecord accepted =
      makeRecord(NetworkEventKind::TcpAccepted, AddressFamily::V6, TransportProtocol::Tcp);
  const ObservedConnection inbound = observedConnection(accepted);
  // ⚠️ 接受事件里 `saddr` 也是本端（实测：那一端是监听口），
  // 所以本端/对端**不能**按「谁先发起」去猜。
  QVERIFY2(inbound.key.local == accepted.source,
           "接受事件把 saddr 当成了对端：实测里 saddr 就是本端（监听侧）");
  QVERIFY(inbound.key.remote == accepted.destination);
  QCOMPARE(inbound.direction, Direction::In);

  // 关闭与失败事件里判不出方向，如实标 Unknown，而不是挑一个填上。
  for (const NetworkEventKind kind :
       {NetworkEventKind::TcpClosed, NetworkEventKind::TcpConnectFailed}) {
    const NetworkEventRecord record =
        makeRecord(kind, AddressFamily::V4, TransportProtocol::Tcp);
    const ObservedConnection closed = observedConnection(record);
    QVERIFY2(closed.key.local == record.source, "关闭/失败事件没有把 saddr 当成本端");
    QCOMPARE(closed.direction, Direction::Unknown);
  }

  // 收到数据的事件：本端仍在 saddr，方向是 In。
  const NetworkEventRecord receivedRecord =
      makeRecord(NetworkEventKind::TcpDataReceived, AddressFamily::V4, TransportProtocol::Tcp);
  const ObservedConnection received = observedConnection(receivedRecord);
  QVERIFY(received.key.local == receivedRecord.source);
  QVERIFY(received.key.remote == receivedRecord.destination);
  QCOMPARE(received.direction, Direction::In);
}

/// UDP 发出：`saddr` 是发包方，也就是本端。
void EtwNetworkTest::udpSentPutsLocalEndpointInSource() {
  const NetworkEventRecord record =
      makeRecord(NetworkEventKind::UdpDataSent, AddressFamily::V6, TransportProtocol::Udp);
  const ObservedConnection observed = observedConnection(record);
  QVERIFY(observed.key.local == record.source);
  QVERIFY(observed.key.remote == record.destination);
  QCOMPARE(observed.direction, Direction::Out);
  QCOMPARE(observed.key.protocol, TransportProtocol::Udp);
}

/// UDP 收到：`saddr` 是**发包方 = 对端**，本端在 `daddr` 里。
///
/// 这一条与 TCP 相反，是实测出来的（回环里两端端口都已知：收包事件的 `sport`
/// 是发送方端口，不是接收套接字的端口）。写错的后果是「所有接收方向的对端都变成自己」。
void EtwNetworkTest::udpReceivedPutsRemoteEndpointInSource() {
  const NetworkEventRecord record =
      makeRecord(NetworkEventKind::UdpDataReceived, AddressFamily::V4, TransportProtocol::Udp);
  const ObservedConnection observed = observedConnection(record);
  QVERIFY2(observed.key.remote == record.source, "UDP 收包事件把本端当成了对端");
  QVERIFY2(observed.key.local == record.destination, "UDP 收包事件没有把 daddr 当成本端");
  QCOMPARE(observed.direction, Direction::In);

  // 两端确实不同，否则上面的断言等于没测（本端 51000、对端 443 是故意错开的）。
  // ⚠️ `Endpoint` 只定义了 `==`（值类型里没必要再造一个 `!=`），所以这里取反。
  QVERIFY(!(observed.key.local == observed.key.remote));
}

/// 同一个连接只报一次出现；关闭之后允许它重新出现。
void EtwNetworkTest::appearanceGateReportsEachConnectionOnce() {
  AppearanceGate gate;
  const ConnectionKey key = makeKey(51001);

  QVERIFY(gate.acceptAppeared(key));
  QVERIFY2(!gate.acceptAppeared(key), "同一个连接重复上报出现：记录表会被重连风暴打爆");
  QCOMPARE(gate.trackedCount(), 1);

  // 另一个键不受影响。
  QVERIFY(gate.acceptAppeared(makeKey(51002)));
  QCOMPARE(gate.trackedCount(), 2);

  // 关闭之后允许重新出现（连接被重建是常见事）。
  gate.noteDisappeared(key);
  QCOMPARE(gate.trackedCount(), 1);
  QVERIFY(gate.acceptAppeared(key));

  // 关闭一个从来没出现过的键也不出错（订阅之前就存在的连接会被关闭）。
  gate.noteDisappeared(makeKey(51009));
  QCOMPARE(gate.trackedCount(), 2);

  gate.clear();
  QCOMPARE(gate.trackedCount(), 0);
  QCOMPARE(gate.overflowCount(), 0);
}

/// 闸门有上限：超出时丢最早的记录并**记账**，不能静默无界增长。
void EtwNetworkTest::appearanceGateIsBounded() {
  AppearanceGate gate;
  const int cap = AppearanceGate::kMaxTrackedConnections;
  for (int i = 0; i < cap; ++i) {
    QVERIFY(gate.acceptAppeared(makeKey(static_cast<std::uint16_t>(1000 + i))));
  }
  QCOMPARE(gate.trackedCount(), cap);
  QCOMPARE(gate.overflowCount(), 0);

  // 第 cap+1 条：丢掉最早那一条，并把这件事记下来。
  QVERIFY(gate.acceptAppeared(makeKey(60000)));
  QCOMPARE(gate.trackedCount(), cap);
  QCOMPARE(gate.overflowCount(), 1);

  // 被丢掉的那条下次出现时会再报一次 —— 代价已知，且不会无界增长。
  QVERIFY(gate.acceptAppeared(makeKey(1000)));
  QCOMPARE(gate.trackedCount(), cap);
  QCOMPARE(gate.overflowCount(), 2);
}

QTEST_GUILESS_MAIN(EtwNetworkTest)

#include "etw_network_test.moc"
