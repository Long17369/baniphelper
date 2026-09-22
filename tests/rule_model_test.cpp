// 规则模型的检查（阶段二 S2.1）。
//
// 断言逐条对应验收标准「非法输入被拒绝且错误信息明确」与「空条件规则无法保存」，
// 以及 architecture.md 第 4.3 至 4.7 节定下的匹配语义、具体度顺序与宽条件分级。
//
// 这个模块的失败方式不是崩溃，而是**把错误语义当成合法规则收下**，
// 所以测试的重点在「该拒的有没有全拒」与「错误信息说不说得清」，
// 而不是「功能跑得通」。

#include <QTest>

#include <QDateTime>
#include <QList>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <cstdint>
#include <iterator>

#include "core/error.h"
#include "core/result.h"
#include "core/rule.h"
#include "core/rule_model.h"

using namespace baniphelper::core;

namespace {

MatchCondition makeCondition(const QString& domain,
                             const QString& mode,
                             const QStringList& values = QStringList(),
                             bool negate = false) {
  MatchCondition condition;
  condition.domain = domain;
  condition.mode = mode;
  condition.values = values;
  condition.negate = negate;
  return condition;
}

Rule makeRule(const QList<MatchCondition>& conditions, RuleAction action = RuleAction::Block) {
  Rule rule;
  rule.id = QStringLiteral("test-rule");
  rule.action = action;
  rule.conditions = conditions;
  rule.createdAt = QDateTime::currentDateTimeUtc();
  return rule;
}

/// 每种取值形状配一个合法样例，供「所有已支持组合都能构造出合法条件」这条检查使用。
QString sampleValueFor(ValueShape shape) {
  switch (shape) {
    case ValueShape::None:
      return QString();
    case ValueShape::ExecutablePath:
      return QStringLiteral("C:\\Windows\\notepad.exe");
    case ValueShape::WildcardPath:
      return QStringLiteral("C:\\Tools\\*.exe");
    case ValueShape::DirectoryPath:
      return QStringLiteral("C:\\Tools\\bin");
    case ValueShape::AddressLiteral:
      return QStringLiteral("192.168.1.1");
    case ValueShape::Subnet:
      return QStringLiteral("192.168.1.0/24");
    case ValueShape::AddressRange:
      return QStringLiteral("192.168.1.1-192.168.1.9");
    case ValueShape::PortNumber:
      return QStringLiteral("443");
    case ValueShape::PortRange:
      return QStringLiteral("8000-8100");
  }
  return QString();
}

QString pairText(MatchDomain domain, MatchMode mode) {
  return QStringLiteral("%1/%2").arg(QString::fromLatin1(domainCode(domain)),
                                     QString::fromLatin1(modeCode(mode)));
}

}  // namespace

class RuleModelTest : public QObject {
  Q_OBJECT

 private slots:
  void tableCoversEveryDomainAndMode();
  void everySupportedCombinationAcceptsASample();
  void unknownDomainAndModeAreRejected();
  void mismatchedDomainAndModeAreRejected();
  void emptyRuleIsRejected();
  void matchesEverythingRejectsValuesAndNegation();
  void exactModeTakesExactlyOneValue();
  void valuesAreNormalizedAndDeduplicated();
  void invalidValuesAreRejectedWithReasons();
  void failedNormalizationLeavesConditionUntouched();
  void duplicateDomainIsRejected();
  void schemaVersionMustMatch();
  void normalizeRuleRewritesCodesToCanonicalForm();
  void specificityFollowsDocumentedOrder();
  void negationBonusNeverCrossesASpecificityBand();
  void allowAlwaysOutranksBlock();
  void riskLevelFollowsWideConditionCount();
  void wideConditionsAreDetected();
  void looseWildcardIsFlagged();
  void valueComplementCoversWhatIsLeftOut();
};

void RuleModelTest::tableCoversEveryDomainAndMode() {
  // 每个域都得有可用的方式，否则界面上会出现一个点开什么都没有的域。
  for (const MatchDomain domain : kAllMatchDomains) {
    const QByteArray domainCodeText(domainCode(domain));
    QVERIFY2(!domainCodeText.isEmpty(), "域缺英文代号");
    QVERIFY2(!domainTitle(domain).trimmed().isEmpty(), domainCodeText.constData());

    const auto parsedDomain = parseDomain(QString::fromLatin1(domainCodeText));
    QVERIFY2(parsedDomain.hasValue(), domainCodeText.constData());
    QVERIFY(parsedDomain.value() == domain);

    const QList<MatchMode> modes = modesOf(domain);
    QVERIFY2(!modes.isEmpty(), domainCodeText.constData());
    for (const MatchMode mode : modes) {
      const auto spec = findModeSpec(domain, mode);
      QVERIFY2(spec.hasValue(), qPrintable(pairText(domain, mode)));
      QVERIFY(spec.value().domain == domain);
      QVERIFY(spec.value().mode == mode);
    }
  }

  // 每个方式都至少在某个域里可用，且代号能往返解析。
  // 漏一处就会出现「枚举里有、表里没有」的哑值，表现是永远匹配不上而不是报错。
  for (const MatchMode mode : kAllMatchModes) {
    const QByteArray modeCodeText(modeCode(mode));
    QVERIFY2(!modeCodeText.isEmpty(), "方式缺英文代号");
    QVERIFY2(!modeTitle(mode).trimmed().isEmpty(), modeCodeText.constData());

    const auto parsedMode = parseMode(QString::fromLatin1(modeCodeText));
    QVERIFY2(parsedMode.hasValue(), modeCodeText.constData());
    QVERIFY(parsedMode.value() == mode);

    bool used = false;
    for (const MatchDomain domain : kAllMatchDomains) {
      if (modesOf(domain).contains(mode)) {
        used = true;
        break;
      }
    }
    QVERIFY2(used, modeCodeText.constData());
  }
}

void RuleModelTest::everySupportedCombinationAcceptsASample() {
  for (const MatchDomain domain : kAllMatchDomains) {
    for (const MatchMode mode : modesOf(domain)) {
      const ModeSpec spec = findModeSpec(domain, mode).value();

      MatchCondition condition = makeCondition(QString::fromLatin1(domainCode(domain)),
                                               QString::fromLatin1(modeCode(mode)));
      if (spec.shape != ValueShape::None) {
        condition.values = QStringList{sampleValueFor(spec.shape)};
      }

      const auto result = validateCondition(condition);
      QVERIFY2(result.hasValue(), qPrintable(pairText(domain, mode)));
    }
  }
}

void RuleModelTest::unknownDomainAndModeAreRejected() {
  const auto domain = parseDomain(QStringLiteral("process"));
  QVERIFY(!domain.hasValue());
  QVERIFY(domain.error().code == ErrorCode::InvalidArgument);
  // 错误信息要能让人自救：既说清哪个不认识，也列出认识的。
  QVERIFY(domain.error().message.contains(QStringLiteral("process")));
  QVERIFY(domain.error().message.contains(QStringLiteral("proc")));

  const auto mode = parseMode(QStringLiteral("regex"));
  QVERIFY(!mode.hasValue());
  QVERIFY(!mode.error().message.trimmed().isEmpty());
  QVERIFY(mode.error().message.contains(QStringLiteral("regex")));

  // 校验器走的是同一条判定，不能只在解析函数里拦。
  const auto throughValidator =
      validateCondition(makeCondition(QStringLiteral("process"),
                                      QStringLiteral("exact"),
                                      QStringList{QStringLiteral("C:\\Windows\\notepad.exe")}));
  QVERIFY(!throughValidator.hasValue());
  QVERIFY(throughValidator.error().code == ErrorCode::InvalidArgument);
  QVERIFY(!throughValidator.error().message.trimmed().isEmpty());
}

void RuleModelTest::mismatchedDomainAndModeAreRejected() {
  struct Case {
    const char* domain;
    const char* mode;
    const char* value;
  };

  const Case cases[] = {
      {"port", "wildcard", "80"},
      {"port", "cidr", "80/24"},
      {"proc", "cidr", "1.2.3.0/24"},
      {"proc", "range", "1-2"},
      {"addr", "wildcard", "*"},
      {"proto", "exact", "6"},
      {"direction", "exact", "out"},
  };

  for (const Case& one : cases) {
    MatchCondition condition = makeCondition(QString::fromLatin1(one.domain),
                                             QString::fromLatin1(one.mode),
                                             QStringList{QString::fromLatin1(one.value)});
    const auto result = validateCondition(condition);
    const QByteArray label = QByteArray(one.domain) + "/" + one.mode;
    QVERIFY2(!result.hasValue(), label.constData());
    QVERIFY2(result.error().code == ErrorCode::InvalidArgument, label.constData());
    // 提示里要列出该域支持哪些方式，否则用户只能猜。
    QVERIFY2(result.error().message.contains(QStringLiteral("支持")), label.constData());
  }
}

void RuleModelTest::emptyRuleIsRejected() {
  Rule rule = makeRule({});
  const auto result = validateRule(rule);
  QVERIFY(!result.hasValue());
  QVERIFY(result.error().code == ErrorCode::InvalidArgument);
  // 空条件被拒绝时，提示必须告诉用户「全系统」该怎么写，而不只是说"不能为空"。
  QVERIFY(result.error().message.contains(QStringLiteral("全部")));

  // 只写一个显式的全匹配条件就应该通过。
  QVERIFY(validateRule(makeRule({makeCondition(QStringLiteral("proc"), QStringLiteral("any"))}))
              .hasValue());
}

void RuleModelTest::matchesEverythingRejectsValuesAndNegation() {
  const auto withValues = validateCondition(makeCondition(
      QStringLiteral("addr"), QStringLiteral("any"), QStringList{QStringLiteral("192.168.1.1")}));
  QVERIFY(!withValues.hasValue());
  QVERIFY(withValues.error().message.contains(QStringLiteral("不接受取值")));

  // 取反一个"全部"条件等于「没有任何取值能命中」，规则会永不生效。
  // 这种规则看上去封了一批东西、实际一条都不封，必须当场拒绝。
  for (const char* domain : {"proc", "addr", "port"}) {
    const auto negated = validateCondition(
        makeCondition(QString::fromLatin1(domain), QStringLiteral("any"), QStringList(), true));
    QVERIFY2(!negated.hasValue(), domain);
    QVERIFY2(negated.error().message.contains(QStringLiteral("永不生效")), domain);
  }

  const auto negatedDirection = validateCondition(
      makeCondition(QStringLiteral("direction"), QStringLiteral("both"), QStringList(), true));
  QVERIFY(!negatedDirection.hasValue());

  const auto negatedProtocol = validateCondition(
      makeCondition(QStringLiteral("proto"), QStringLiteral("any"), QStringList(), true));
  QVERIFY(!negatedProtocol.hasValue());
}

void RuleModelTest::exactModeTakesExactlyOneValue() {
  const auto twoValues = validateCondition(
      makeCondition(QStringLiteral("addr"),
                    QStringLiteral("exact"),
                    QStringList{QStringLiteral("1.2.3.4"), QStringLiteral("1.2.3.5")}));
  QVERIFY(!twoValues.hasValue());
  QVERIFY(twoValues.error().message.contains(QStringLiteral("集合匹配")));

  const auto none =
      validateCondition(makeCondition(QStringLiteral("addr"), QStringLiteral("exact")));
  QVERIFY(!none.hasValue());
  QVERIFY(!none.error().message.trimmed().isEmpty());

  // 同一个值写两遍属于重复，去重后只剩一个，不该报成「取值太多」。
  MatchCondition repeated =
      makeCondition(QStringLiteral("addr"),
                    QStringLiteral("exact"),
                    QStringList{QStringLiteral("1.2.3.4"), QStringLiteral("1.2.3.4")});
  QVERIFY(normalizeCondition(repeated).hasValue());
  QCOMPARE(repeated.values.size(), 1);
}

void RuleModelTest::valuesAreNormalizedAndDeduplicated() {
  // 地址域没有 set 方式，多个不连续的网段靠取值列表表达。
  MatchCondition subnets = makeCondition(QStringLiteral("addr"),
                                         QStringLiteral("cidr"),
                                         QStringList{QStringLiteral("2001:DB8::/32"),
                                                     QStringLiteral("2001:db8::/32"),
                                                     QStringLiteral("10.0.0.0/8")});
  QVERIFY(normalizeCondition(subnets).hasValue());
  QCOMPARE(subnets.values.size(), 2);
  QCOMPARE(subnets.values.at(0), QStringLiteral("2001:db8::/32"));
  QCOMPARE(subnets.values.at(1), QStringLiteral("10.0.0.0/8"));

  MatchCondition single = makeCondition(
      QStringLiteral("addr"), QStringLiteral("exact"), QStringList{QStringLiteral("2001:DB8::1")});
  QVERIFY(normalizeCondition(single).hasValue());
  QCOMPARE(single.values.join(QLatin1Char(',')), QStringLiteral("2001:db8::1"));

  MatchCondition port = makeCondition(
      QStringLiteral("port"), QStringLiteral("exact"), QStringList{QStringLiteral("080")});
  QVERIFY(normalizeCondition(port).hasValue());
  QCOMPARE(port.values.join(QLatin1Char(',')), QStringLiteral("80"));

  MatchCondition subnet = makeCondition(QStringLiteral("addr"),
                                        QStringLiteral("cidr"),
                                        QStringList{QStringLiteral("192.168.1.0/024")});
  QVERIFY(normalizeCondition(subnet).hasValue());
  QCOMPARE(subnet.values.join(QLatin1Char(',')), QStringLiteral("192.168.1.0/24"));

  MatchCondition range = makeCondition(QStringLiteral("addr"),
                                       QStringLiteral("range"),
                                       QStringList{QStringLiteral("2001:DB8::1-2001:DB8::FF")});
  QVERIFY(normalizeCondition(range).hasValue());
  QCOMPARE(range.values.join(QLatin1Char(',')), QStringLiteral("2001:db8::1-2001:db8::ff"));

  // 路径**原样保留**：用户写什么就存什么，不改分隔符、也不去尾部分隔符。
  // 曾经这里做过归一化，那是错的：归一是在「存储」层做了一件属于「比较」的事，
  // 而且有损（`..` 会被解开），存进去就回不到用户写的样子。
  MatchCondition directory = makeCondition(QStringLiteral("proc"),
                                           QStringLiteral("dir"),
                                           QStringList{QStringLiteral("C:\\Tools\\bin\\")});
  QVERIFY(normalizeCondition(directory).hasValue());
  QCOMPARE(directory.values.join(QLatin1Char(',')), QStringLiteral("C:\\Tools\\bin\\"));

  MatchCondition driveRoot = makeCondition(
      QStringLiteral("proc"), QStringLiteral("dir"), QStringList{QStringLiteral("C:\\")});
  QVERIFY(normalizeCondition(driveRoot).hasValue());
  QCOMPARE(driveRoot.values.join(QLatin1Char(',')), QStringLiteral("C:\\"));

  // 两种分隔符写法**不会**被合并成一个取值。合不合并都不影响语义：
  // 同一个字段的多个条件是「或」，两个等价条件不会改变判定结果；
  // 要不要把它们看成同一个文件，是 `sameProcessIdentity` 在比较时的事。
  MatchCondition twoSeparators = makeCondition(
      QStringLiteral("proc"),
      QStringLiteral("set"),
      QStringList{QStringLiteral("C:\\Tools\\a.exe"), QStringLiteral("C:/Tools/a.exe")});
  QVERIFY(normalizeCondition(twoSeparators).hasValue());
  QCOMPARE(twoSeparators.values.size(), 2);
}

void RuleModelTest::invalidValuesAreRejectedWithReasons() {
  struct Case {
    const char* domain;
    const char* mode;
    const char* value;
  };

  const Case cases[] = {
      {"addr", "exact", ""},
      {"addr", "exact", "not-an-ip"},
      {"addr", "exact", "1.2.3.256"},
      {"addr", "exact", "::1%12"},
      {"addr", "cidr", "192.168.1.0"},
      {"addr", "cidr", "192.168.1.0/33"},
      {"addr", "cidr", "2001:db8::/129"},
      {"addr", "cidr", "192.168.1.0/2a"},
      {"addr", "cidr", "192.168.1.0/0000024"},
      {"addr", "range", "192.168.1.9"},
      {"addr", "range", "192.168.1.9-192.168.1.1"},
      {"addr", "range", "192.168.1.1-2001:db8::1"},
      {"port", "exact", ""},
      {"port", "exact", "8a"},
      {"port", "exact", "65536"},
      {"port", "exact", "-1"},
      {"port", "range", "443-80"},
      {"port", "range", "80"},
      {"proc", "exact", "notepad.exe"},
      {"proc", "exact", "C:\\Windows\\"},
      {"proc", "exact", "C:\\Windows\\not*.exe"},
      {"proc", "dir", "games"},
      {"proc", "wildcard", ""},
  };

  for (const Case& one : cases) {
    MatchCondition condition = makeCondition(QString::fromLatin1(one.domain),
                                             QString::fromLatin1(one.mode),
                                             QStringList{QString::fromLatin1(one.value)});
    const auto result = validateCondition(condition);
    const QByteArray label = QByteArray(one.domain) + "/" + one.mode + " = " + one.value;
    QVERIFY2(!result.hasValue(), label.constData());
    QVERIFY2(result.error().code == ErrorCode::InvalidArgument, label.constData());
    // 「错误信息明确」是验收标准本身，所以空说明要判失败。
    QVERIFY2(!result.error().message.trimmed().isEmpty(), label.constData());
  }
}

void RuleModelTest::failedNormalizationLeavesConditionUntouched() {
  // 第一个值合法且会被规范化，第二个值非法。整体失败后原对象一个字段都不能变，
  // 半改的状态会让调用方无从知道该回滚什么。
  MatchCondition condition =
      makeCondition(QStringLiteral("ADDR"),
                    QStringLiteral("RANGE"),
                    QStringList{QStringLiteral("2001:DB8::1-2001:DB8::FF"), QStringLiteral("bad")});
  const MatchCondition before = condition;

  const auto result = normalizeCondition(condition);
  QVERIFY(!result.hasValue());
  QCOMPARE(condition.domain, before.domain);
  QCOMPARE(condition.mode, before.mode);
  QCOMPARE(condition.values, before.values);
  QCOMPARE(condition.negate, before.negate);

  Rule rule = makeRule(
      {makeCondition(
           QStringLiteral("PORT"), QStringLiteral("EXACT"), QStringList{QStringLiteral("80")}),
       makeCondition(
           QStringLiteral("ADDR"), QStringLiteral("EXACT"), QStringList{QStringLiteral("bad")})});
  const Rule ruleBefore = rule;
  QVERIFY(!normalizeRule(rule).hasValue());
  QCOMPARE(rule.conditions.size(), ruleBefore.conditions.size());
  QCOMPARE(rule.conditions.at(0).domain, ruleBefore.conditions.at(0).domain);
  QCOMPARE(rule.conditions.at(0).mode, ruleBefore.conditions.at(0).mode);
}

void RuleModelTest::duplicateDomainIsRejected() {
  Rule rule = makeRule(
      {makeCondition(
           QStringLiteral("port"), QStringLiteral("exact"), QStringList{QStringLiteral("80")}),
       makeCondition(
           QStringLiteral("port"), QStringLiteral("exact"), QStringList{QStringLiteral("443")})});
  const auto result = validateRule(rule);
  QVERIFY(!result.hasValue());
  QVERIFY(result.error().code == ErrorCode::InvalidArgument);
  // 提示要说清去哪儿改，否则用户只会知道"不行"。
  QVERIFY(result.error().message.contains(QStringLiteral("端口")));
  QVERIFY(result.error().message.contains(QStringLiteral("取值列表")));

  // 换成同一域的一个条件、两个取值就合法。
  QVERIFY(validateRule(
              makeRule({makeCondition(QStringLiteral("port"),
                                      QStringLiteral("set"),
                                      QStringList{QStringLiteral("80"), QStringLiteral("443")})}))
              .hasValue());
}

void RuleModelTest::schemaVersionMustMatch() {
  Rule rule = makeRule({makeCondition(QStringLiteral("proto"), QStringLiteral("tcp"))});
  QVERIFY(validateRule(rule).hasValue());

  rule.schema = kCurrentRuleSchema + 1;
  const auto tooNew = validateRule(rule);
  QVERIFY(!tooNew.hasValue());
  QVERIFY(tooNew.error().message.contains(QString::number(kCurrentRuleSchema + 1)));

  rule.schema = kCurrentRuleSchema - 1;
  QVERIFY(!validateRule(rule).hasValue());
}

void RuleModelTest::normalizeRuleRewritesCodesToCanonicalForm() {
  Rule rule = makeRule({makeCondition(QStringLiteral("PROC"),
                                      QStringLiteral("Exact"),
                                      QStringList{QStringLiteral("C:\\Windows\\notepad.exe")}),
                        makeCondition(QStringLiteral("PROTO"), QStringLiteral("TCP"))});
  QVERIFY(normalizeRule(rule).hasValue());
  QCOMPARE(rule.conditions.at(0).domain, QStringLiteral("proc"));
  QCOMPARE(rule.conditions.at(0).mode, QStringLiteral("exact"));
  QCOMPARE(rule.conditions.at(1).domain, QStringLiteral("proto"));
  QCOMPARE(rule.conditions.at(1).mode, QStringLiteral("tcp"));
}

void RuleModelTest::specificityFollowsDocumentedOrder() {
  const QList<MatchMode> order = {
      MatchMode::Exact, MatchMode::Set, MatchMode::Dir, MatchMode::Wildcard, MatchMode::Any};

  for (int index = 0; index + 1 < order.size(); ++index) {
    const auto higher = conditionSpecificity(
        makeCondition(QStringLiteral("proc"), QString::fromLatin1(modeCode(order.at(index)))));
    const auto lower = conditionSpecificity(
        makeCondition(QStringLiteral("proc"), QString::fromLatin1(modeCode(order.at(index + 1)))));
    QVERIFY2(higher.hasValue() && lower.hasValue(),
             qPrintable(QString::fromLatin1(modeCode(order.at(index)))));
    QVERIFY2(higher.value() > lower.value(),
             qPrintable(QStringLiteral("%1 应该比 %2 更具体")
                            .arg(modeCode(order.at(index)), modeCode(order.at(index + 1)))));
  }

  // 取反确实带来加成，否则「同具体度时取反略高」这条约定就没落地。
  const auto plain = conditionSpecificity(makeCondition(
      QStringLiteral("port"), QStringLiteral("exact"), QStringList{QStringLiteral("80")}));
  const auto negated = conditionSpecificity(makeCondition(
      QStringLiteral("port"), QStringLiteral("exact"), QStringList{QStringLiteral("80")}, true));
  QVERIFY(plain.hasValue() && negated.hasValue());
  QVERIFY(negated.value() >= plain.value());
}

void RuleModelTest::negationBonusNeverCrossesASpecificityBand() {
  // 加成必须小于最小档差，否则「加几个取反就比更具体的方式还优先」，
  // 规则看起来越具体反而越不优先。
  std::int32_t smallestGap = 0;
  for (const MatchDomain domain : kAllMatchDomains) {
    const QList<MatchMode> modes = modesOf(domain);
    for (const MatchMode left : modes) {
      for (const MatchMode right : modes) {
        const std::int32_t a = findModeSpec(domain, left).value().specificity;
        const std::int32_t b = findModeSpec(domain, right).value().specificity;
        const std::int32_t gap = a > b ? a - b : b - a;
        if (gap > 0 && (smallestGap == 0 || gap < smallestGap)) {
          smallestGap = gap;
        }
      }
    }
  }
  QVERIFY(smallestGap > 0);
  QVERIFY(smallestGap > kNegationBonus);

  // 一条规则最多每个域一个条件，所以加成总和也跨不过最小档差。
  const std::int32_t maxBonus =
      kNegationBonus * static_cast<std::int32_t>(std::size(kAllMatchDomains));
  QVERIFY(maxBonus < smallestGap);
}

void RuleModelTest::allowAlwaysOutranksBlock() {
  const QList<MatchCondition> mostSpecific = {
      makeCondition(QStringLiteral("proc"),
                    QStringLiteral("exact"),
                    QStringList{QStringLiteral("C:\\Windows\\notepad.exe")}),
      makeCondition(
          QStringLiteral("addr"), QStringLiteral("exact"), QStringList{QStringLiteral("1.2.3.4")}),
      makeCondition(
          QStringLiteral("port"), QStringLiteral("exact"), QStringList{QStringLiteral("443")}),
      makeCondition(QStringLiteral("proto"), QStringLiteral("tcp")),
      makeCondition(QStringLiteral("direction"), QStringLiteral("out")),
  };
  const QList<MatchCondition> widest = {
      makeCondition(QStringLiteral("proc"), QStringLiteral("any")),
      makeCondition(QStringLiteral("addr"), QStringLiteral("any")),
      makeCondition(QStringLiteral("port"), QStringLiteral("any")),
      makeCondition(QStringLiteral("proto"), QStringLiteral("any")),
      makeCondition(QStringLiteral("direction"), QStringLiteral("both")),
  };

  const auto blocking = computePriority(makeRule(mostSpecific, RuleAction::Block));
  const auto allowing = computePriority(makeRule(widest, RuleAction::Allow));
  QVERIFY(blocking.hasValue() && allowing.hasValue());

  // 这是白名单模式的地基：最宽的放行也必须压过最具体的阻断。
  QVERIFY(allowing.value() > blocking.value());

  const auto invalid = computePriority(makeRule({}));
  QVERIFY(!invalid.hasValue());
}

void RuleModelTest::riskLevelFollowsWideConditionCount() {
  QVERIFY(riskLevelOf(0) == RiskLevel::Normal);
  QVERIFY(riskLevelOf(1) == RiskLevel::Normal);
  QVERIFY(riskLevelOf(2) == RiskLevel::High);
  QVERIFY(riskLevelOf(3) == RiskLevel::Extreme);
  QVERIFY(riskLevelOf(9) == RiskLevel::Extreme);
}

void RuleModelTest::wideConditionsAreDetected() {
  // 一个宽条件：全匹配的协议。
  const auto single = assessWidth(makeRule(
      {makeCondition(QStringLiteral("proto"), QStringLiteral("any")),
       makeCondition(
           QStringLiteral("port"), QStringLiteral("exact"), QStringList{QStringLiteral("443")})}));
  QVERIFY(single.hasValue());
  QCOMPARE(single.value().count(), 1);
  QVERIFY(single.value().level == RiskLevel::Normal);
  QVERIFY(single.value().issues.at(0).conditionIndex == 0);
  QVERIFY(!single.value().issues.at(0).description.trimmed().isEmpty());

  // 两个宽条件：全匹配的协议 + 取反的地址。
  const auto pair =
      assessWidth(makeRule({makeCondition(QStringLiteral("proto"), QStringLiteral("any")),
                            makeCondition(QStringLiteral("addr"),
                                          QStringLiteral("cidr"),
                                          QStringList{QStringLiteral("10.0.0.0/8")},
                                          true)}));
  QVERIFY(pair.hasValue());
  QCOMPARE(pair.value().count(), 2);
  QVERIFY(pair.value().level == RiskLevel::High);

  // 三个宽条件：再加一个全匹配的程序域。
  const auto triple =
      assessWidth(makeRule({makeCondition(QStringLiteral("proc"), QStringLiteral("any")),
                            makeCondition(QStringLiteral("proto"), QStringLiteral("any")),
                            makeCondition(QStringLiteral("direction"), QStringLiteral("both"))}));
  QVERIFY(triple.hasValue());
  QCOMPARE(triple.value().count(), 3);
  QVERIFY(triple.value().level == RiskLevel::Extreme);

  // 一条全是具体条件的规则不该被标成有风险。
  const auto narrow =
      assessWidth(makeRule({makeCondition(QStringLiteral("proc"),
                                          QStringLiteral("exact"),
                                          QStringList{QStringLiteral("C:\\Windows\\notepad.exe")}),
                            makeCondition(QStringLiteral("proto"), QStringLiteral("tcp"))}));
  QVERIFY(narrow.hasValue());
  QCOMPARE(narrow.value().count(), 0);
  QVERIFY(narrow.value().level == RiskLevel::Normal);

  QVERIFY(!assessWidth(makeRule({})).hasValue());
}

void RuleModelTest::looseWildcardIsFlagged() {
  const auto anyFile = assessWidth(makeRule({makeCondition(
      QStringLiteral("proc"), QStringLiteral("wildcard"), QStringList{QStringLiteral("*")})}));
  QVERIFY(anyFile.hasValue());
  QCOMPARE(anyFile.value().count(), 1);

  // 只有文件名通配会命中所有目录下的同名文件，同样算宽。
  const auto anyDirectory = assessWidth(makeRule({makeCondition(
      QStringLiteral("proc"), QStringLiteral("wildcard"), QStringList{QStringLiteral("*.exe")})}));
  QVERIFY(anyDirectory.hasValue());
  QCOMPARE(anyDirectory.value().count(), 1);

  // 限定了目录的通配不算宽。
  const auto scoped =
      assessWidth(makeRule({makeCondition(QStringLiteral("proc"),
                                          QStringLiteral("wildcard"),
                                          QStringList{QStringLiteral("C:\\Tools\\*\\tool.exe")})}));
  QVERIFY(scoped.hasValue());
  QCOMPARE(scoped.value().count(), 0);
  QVERIFY(scoped.value().level == RiskLevel::Normal);
}

void RuleModelTest::valueComplementCoversWhatIsLeftOut() {
  // 求补是「取反」唯一的正确翻法（见 rule_model.h 里 complementAddressSpans 的说明），
  // 所以这段运算错了就直接变成「封得比预期宽」—— 最不容易被发现的那种错。
  const auto spanOf = [](const QString& literal) { return subnetToSpan(literal).value(); };

  // 一个 IPv4 地址之外：前后两段，不多不少。
  const auto single = complementAddressSpans({spanOf(QStringLiteral("1.2.3.4/32"))});
  QVERIFY(single.hasValue());
  QCOMPARE(single.value().size(), 2);
  QCOMPARE(single.value().at(0).lower.text, QStringLiteral("0.0.0.0"));
  QCOMPARE(single.value().at(0).upper.text, QStringLiteral("1.2.3.3"));
  QCOMPARE(single.value().at(1).lower.text, QStringLiteral("1.2.3.5"));
  QCOMPARE(single.value().at(1).upper.text, QStringLiteral("255.255.255.255"));

  // 两头最容易在边界上出错：起点全零时不该出现「空区间」，终点全一时不该溢出。
  const auto atZero = complementAddressSpans({spanOf(QStringLiteral("0.0.0.0/32"))});
  QVERIFY(atZero.hasValue());
  QCOMPARE(atZero.value().size(), 1);
  QCOMPARE(atZero.value().at(0).lower.text, QStringLiteral("0.0.0.1"));
  QCOMPARE(atZero.value().at(0).upper.text, QStringLiteral("255.255.255.255"));

  const auto atMax = complementAddressSpans({spanOf(QStringLiteral("255.255.255.255/32"))});
  QVERIFY(atMax.hasValue());
  QCOMPARE(atMax.value().size(), 1);
  QCOMPARE(atMax.value().at(0).lower.text, QStringLiteral("0.0.0.0"));
  QCOMPARE(atMax.value().at(0).upper.text, QStringLiteral("255.255.255.254"));

  // 相邻的两段要先合并，否则补集里会多出一个根本不存在的空洞。
  const auto merged = complementAddressSpans(
      {spanOf(QStringLiteral("1.2.3.0/24")), spanOf(QStringLiteral("1.2.4.0/24"))});
  QVERIFY(merged.hasValue());
  QCOMPARE(merged.value().size(), 2);
  QCOMPARE(merged.value().at(0).upper.text, QStringLiteral("1.2.2.255"));
  QCOMPARE(merged.value().at(1).lower.text, QStringLiteral("1.2.5.0"));

  // IPv6 走同一套 16 字节运算，跨字节进位的方向不能反。
  const auto v6 = complementAddressSpans({spanOf(QStringLiteral("2001:db8::/32"))});
  QVERIFY(v6.hasValue());
  QCOMPARE(v6.value().size(), 2);
  QCOMPARE(v6.value().at(0).lower.text, QStringLiteral("::"));
  QCOMPARE(v6.value().at(0).upper.text, QStringLiteral("2001:db7:ffff:ffff:ffff:ffff:ffff:ffff"));
  QCOMPARE(v6.value().at(1).lower.text, QStringLiteral("2001:db9::"));
  QCOMPARE(v6.value().at(1).upper.text, QStringLiteral("ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff"));

  // 覆盖全域与空输入都得到空集。**两者都不是「不限定」而是「没有取值」**，
  // 调用方必须自己区分（resolveNegation 就靠这个把「永不命中的规则」挖出来）。
  QVERIFY(complementAddressSpans({spanOf(QStringLiteral("0.0.0.0/0"))}).value().isEmpty());
  QVERIFY(complementAddressSpans({}).value().isEmpty());

  // 混族要报错：跨族的补集没有任何意义，算出来必定是错的。
  const auto mixed = complementAddressSpans(
      {spanOf(QStringLiteral("1.2.3.4/32")), spanOf(QStringLiteral("2001:db8::/32"))});
  QVERIFY(!mixed.hasValue());
  QCOMPARE(mixed.error().code, ErrorCode::InvalidArgument);

  // 端口同理：80 之外是 0-79 与 81-65535 两段。
  PortSpan port80;
  port80.lower = 80;
  port80.upper = 80;
  const QList<PortSpan> portComplement = complementPortSpans({port80});
  QCOMPARE(portComplement.size(), 2);
  QCOMPARE(static_cast<int>(portComplement.at(0).lower), 0);
  QCOMPARE(static_cast<int>(portComplement.at(0).upper), 79);
  QCOMPARE(static_cast<int>(portComplement.at(1).lower), 81);
  QCOMPARE(static_cast<int>(portComplement.at(1).upper), 65535);

  PortSpan allPorts;
  allPorts.lower = 0;
  allPorts.upper = 65535;
  QVERIFY(complementPortSpans({allPorts}).isEmpty());
  QVERIFY(complementPortSpans({}).isEmpty());
}

QTEST_GUILESS_MAIN(RuleModelTest)

#include "rule_model_test.moc"
