// 能力协商模块的检查（阶段一 S1.11）。
//
// 这里的断言逐条对应验收标准「查询任一能力可得到明确的是否可用，且无静默失败路径」。
// 因此重点不是功能覆盖，而是**把「没有解释的失败」挡在门外**：
// 能力表漏补文案、不可用却不给原因、失败信息说不清是哪个动作，都会在这里失败。

#include <QTest>

#include <QString>

#include <iterator>

#include "core/capabilities.h"
#include "core/capability_check.h"
#include "core/capability_set.h"
#include "core/declared_capabilities.h"
#include "core/error.h"

using namespace baniphelper::core;

namespace {

/// 一个故意「不守规矩」的实现：什么能力都说没有，而且一个原因都不给。
///
/// 用它来检查 `requireCapability` 能不能自己把说明补上。
/// 「无静默失败路径」不能建立在「所有实现都很自觉」这个假设上，
/// 所以必须有一个不配合的实现来压这条路径。
class UncooperativeCapabilities final : public ICapabilities {
 public:
  [[nodiscard]] bool isSupported(Capability) const override {
    return false;
  }

  [[nodiscard]] QList<Capability> supported() const override {
    return {};
  }

  [[nodiscard]] QString unsupportedReason(Capability) const override {
    return QString();
  }

  [[nodiscard]] QString backendName() const override {
    return QStringLiteral("uncooperative");
  }
};

}  // namespace

class CapabilitiesTest : public QObject {
  Q_OBJECT

 private slots:
  void capabilityTableCoversEveryValue();
  void errorCodeTableCoversEveryValue();
  void capabilitySetKeepsStableOrder();
  void unsupportedCapabilityAlwaysHasReason();
  void unsupportedAccessReturnsNotSupported();
};

void CapabilitiesTest::capabilityTableCoversEveryValue() {
  // 用一个非法位取「兜底文案」当样本，这样断言不依赖任何硬编码字符串：
  // 只要求每个真实能力位的文案与兜底文案不同。
  const auto bogus = static_cast<Capability>(0);
  const QString bogusDisplayName = capabilityDisplayName(bogus);
  const QString bogusImpact = capabilityImpact(bogus);

  for (Capability capability : kAllCapabilities) {
    const int raw = static_cast<int>(capability);
    const QByteArray name(capabilityName(capability));

    QVERIFY2(name != QByteArray("Unknown"),
             qPrintable(QStringLiteral("能力位 %1 没补英文代号").arg(raw)));
    QVERIFY2(!capabilityDisplayName(capability).isEmpty(),
             qPrintable(QStringLiteral("能力位 %1 没补中文名").arg(raw)));
    QVERIFY2(capabilityDisplayName(capability) != bogusDisplayName,
             qPrintable(QStringLiteral("能力位 %1 落在兜底显示名上").arg(raw)));
    QVERIFY2(!capabilityImpact(capability).isEmpty(),
             qPrintable(QStringLiteral("能力位 %1 没补后果说明").arg(raw)));
    QVERIFY2(capabilityImpact(capability) != bogusImpact,
             qPrintable(QStringLiteral("能力位 %1 落在兜底后果说明上").arg(raw)));
  }
}

void CapabilitiesTest::errorCodeTableCoversEveryValue() {
  for (ErrorCode code : kAllErrorCodes) {
    const QByteArray name(errorCodeName(code));

    if (code == ErrorCode::Unknown) {
      // Unknown 自己的代号就是 Unknown，这是唯一合法的重合。
      QCOMPARE(name, QByteArray("Unknown"));
    } else {
      QVERIFY2(name != QByteArray("Unknown"),
               qPrintable(QStringLiteral("错误码 %1 没补英文代号").arg(static_cast<int>(code))));
    }
  }
}

void CapabilitiesTest::capabilitySetKeepsStableOrder() {
  CapabilitySet set;
  QVERIFY(set.isEmpty());
  QCOMPARE(set.size(), 0);
  QCOMPARE(set.toLogString(), QStringLiteral("(无)"));

  set.add(Capability::FilterIPv4);
  set.add(Capability::KillTcpV4);
  set.add(Capability::FilterIPv4);  // 重复添加不应改变状态

  QCOMPARE(set.size(), 2);
  QVERIFY(set.contains(Capability::FilterIPv4));
  QVERIFY(set.contains(Capability::KillTcpV4));
  QVERIFY(!set.contains(Capability::FilterIPv6));

  set.remove(Capability::KillTcpV4);
  QVERIFY(!set.contains(Capability::KillTcpV4));
  QCOMPARE(set.size(), 1);

  // 顺序必须由 kAllCapabilities 决定，而不是由添加顺序决定：
  // 否则同一个集合会有多种表示，日志与集合比较都不再稳定。
  const CapabilitySet forward =
      CapabilitySet::fromList({Capability::FilterIPv4, Capability::KillTcpV4});
  const CapabilitySet backward =
      CapabilitySet::fromList({Capability::KillTcpV4, Capability::FilterIPv4});
  QVERIFY(forward.list() == backward.list());
  QVERIFY(forward == backward);
  QCOMPARE(backward.toLogString(), QStringLiteral("FilterIPv4|KillTcpV4"));
}

void CapabilitiesTest::unsupportedCapabilityAlwaysHasReason() {
  const QList<Capability> declared{Capability::FilterIPv4, Capability::TrafficStatsTcp};
  DeclaredCapabilities capabilities(QStringLiteral("memory"), CapabilitySet::fromList(declared));

  QCOMPARE(capabilities.backendName(), QStringLiteral("memory"));
  QVERIFY(capabilities.isSupported(Capability::FilterIPv4));
  QVERIFY(!capabilities.isSupported(Capability::FilterIPv6));
  QVERIFY(capabilities.supported() == declared);

  // 每个能力位都要有明确答案；不可用时原因必须非空。
  for (Capability capability : kAllCapabilities) {
    if (capabilities.isSupported(capability)) {
      QVERIFY2(capabilities.unsupportedReason(capability).isEmpty(),
               "已声明支持的能力不应给出不支持原因");
    } else {
      QVERIFY2(!capabilities.unsupportedReason(capability).trimmed().isEmpty(),
               "不可用的能力必须给出原因，否则界面上就是一个没有解释的灰按钮");
    }
  }

  // 登记过的具体原因必须原样返回，不能被通用说明覆盖。
  capabilities.setUnsupportedReason(Capability::FilterIPv6,
                                    QStringLiteral("本平台未实现 IPv6 过滤"));
  QCOMPARE(capabilities.unsupportedReason(Capability::FilterIPv6),
           QStringLiteral("本平台未实现 IPv6 过滤"));

  const QList<UnsupportedCapability> unsupported = capabilities.unsupportedWithReasons();
  const int expected = static_cast<int>(std::size(kAllCapabilities)) - declared.size();
  QCOMPARE(static_cast<int>(unsupported.size()), expected);

  for (const UnsupportedCapability& entry : unsupported) {
    QVERIFY(!capabilities.isSupported(entry.capability));
    QVERIFY2(!entry.reason.trimmed().isEmpty(), "不可用能力的原因不允许为空");
  }
}

void CapabilitiesTest::unsupportedAccessReturnsNotSupported() {
  DeclaredCapabilities capabilities(QStringLiteral("memory"),
                                    CapabilitySet::fromList({Capability::FilterIPv4}));

  const Result<void> granted =
      requireCapability(capabilities, Capability::FilterIPv4, QStringLiteral("下发 IPv4 规则"));
  QVERIFY(granted.hasValue());

  const QString operation = QStringLiteral("下发 IPv6 规则");
  const Result<void> refused = requireCapability(capabilities, Capability::FilterIPv6, operation);
  QVERIFY(!refused.hasValue());
  QVERIFY(refused.error().code == ErrorCode::NotSupported);
  // 说明里必须同时有「哪个动作」和「为什么」，否则用户拿不到可判断的信息。
  QVERIFY2(refused.error().message.contains(operation), "失败说明必须点名是哪个动作");
  QVERIFY2(refused.error().message.contains(QStringLiteral("IPv6")),
           "失败说明必须说清做不到的原因");

  // 连「不给任何原因」的实现也不能产生一条没有信息的失败。
  const UncooperativeCapabilities uncooperative;
  const QString killOperation = QStringLiteral("中断 IPv6 连接");
  const Result<void> fromUncooperative =
      requireCapability(uncooperative, Capability::KillTcpV6, killOperation);

  QVERIFY(!fromUncooperative.hasValue());
  QVERIFY(fromUncooperative.error().code == ErrorCode::NotSupported);
  QVERIFY2(!fromUncooperative.error().message.trimmed().isEmpty(),
           "即使实现没给原因，也不能出现空说明");
  QVERIFY(fromUncooperative.error().message.contains(killOperation));
}

QTEST_GUILESS_MAIN(CapabilitiesTest)

#include "capabilities_test.moc"
