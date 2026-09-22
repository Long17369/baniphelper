// 规则展开器的检查（阶段二 S2.2）。
//
// 断言逐条对应验收标准「给定规则能输出确定的过滤器清单，含数量与条件」。
// 「确定」是这一步的全部要点：条数会被拿去与实际下发的条数对账，
// 所以这里的数字都写死，不写成「大于 0」这种无法发现漂移的判据。
//
// 这个模块最坏的失败方式不是崩溃，而是**拆少了**：少拆一族地址、少拆一个协议，
// 规则看上去下发成功，实际留着一个能穿过去的缝。

#include <QTest>

#include <QList>
#include <QString>
#include <QStringList>

#include "core/error.h"
#include "core/rule.h"
#include "core/rule_model.h"
#include "platform/win/filter_plan.h"

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

RuleSpec makeSpec(const QList<MatchCondition>& conditions,
                  RuleAction action = RuleAction::Block,
                  std::int32_t priority = 0) {
  RuleSpec rule;
  rule.id = QStringLiteral("test-rule");
  rule.action = action;
  rule.conditions = conditions;
  rule.priority = priority;
  return rule;
}

/// 展开失败时的原因。写成函数是因为 `QVERIFY2` 的描述参数**总是会被求值**，
/// 直接写 `plan.error()` 在成功路径上会撞上 `Result::error()` 的前置条件。
QString errorText(const Result<FilterPlan>& result) {
  return result.hasValue() ? QString() : result.error().message;
}

/// 同上，给取反解析用。
QString errorText(const Result<FilterPlanEntry>& result) {
  return result.hasValue() ? QString() : result.error().message;
}

/// 取条目里某个字段的条件，没有就返回 nullptr。
const FilterCondition* conditionOf(const FilterPlanEntry& entry, FilterField field) {
  for (const FilterCondition& condition : entry.conditions) {
    if (condition.field == field) {
      return &condition;
    }
  }
  return nullptr;
}

int countByStage(const FilterPlan& plan, FilterStage stage) {
  int count = 0;
  for (const FilterPlanEntry& entry : plan.entries) {
    if (entry.stage == stage) {
      ++count;
    }
  }
  return count;
}

int countByFamily(const FilterPlan& plan, AddressFamily family) {
  int count = 0;
  for (const FilterPlanEntry& entry : plan.entries) {
    if (entry.family == family) {
      ++count;
    }
  }
  return count;
}

}  // namespace

class FilterPlanTest : public QObject {
  Q_OBJECT

 private slots:
  void nameTablesCoverEveryValue();
  void singleRuleSplitsByStageAndFamily();
  void directionLimitsStages();
  void protocolLimitsStages();
  void addressFamilyFollowsTheAddress();
  void subnetBecomesASpan();
  void portBecomesASpan();
  void multipleValuesShareOneFilter();
  void negationIsCarriedNotComplemented();
  void conditionsAreOrderedAndUniquePerField();
  void negatedMultipleValuesStayOnOneFilter();
  void actionAndPriorityFollowTheRule();
  void wildcardAndDirectoryAreNotSupportedYet();
  void invalidConditionsAreRejected();
  void oversizedPlanIsRejected();
  void expansionIsDeterministic();

  void negationIsResolvedIntoComplements();
  void appIdNegationStaysForThePlatform();
  void unrepresentableNegationsAreRejected();
};

void FilterPlanTest::nameTablesCoverEveryValue() {
  for (const FilterStage stage :
       {FilterStage::OutboundConnect, FilterStage::InboundAccept, FilterStage::InboundDatagram}) {
    QVERIFY2(QByteArray(filterStageName(stage)).size() > 0, "落点缺英文代号");
  }
  for (const FilterField field : {FilterField::AppPath,
                                  FilterField::RemoteAddress,
                                  FilterField::Port,
                                  FilterField::Protocol}) {
    QVERIFY2(QByteArray(filterFieldName(field)).size() > 0, "字段缺英文代号");
  }
}

void FilterPlanTest::singleRuleSplitsByStageAndFamily() {
  // 地址定成了 IPv4，所以只该有 V4 条目；协议没写，所以 TCP 与 UDP 都要覆盖。
  RuleSpec rule = makeSpec({makeCondition(QStringLiteral("proc"),
                                          QStringLiteral("exact"),
                                          QStringList{QStringLiteral("C:\\a.exe")}),
                            makeCondition(QStringLiteral("addr"),
                                          QStringLiteral("exact"),
                                          QStringList{QStringLiteral("1.2.3.4")})});

  const auto plan = expandRule(rule);
  QVERIFY2(plan.hasValue(), qPrintable(errorText(plan)));

  // 地址定成了 IPv4，所以只该有 V4 条目；三个落点各一条，与取值个数无关。
  QCOMPARE(plan.value().lowerBoundCount(), 3);
  QCOMPARE(countByFamily(plan.value(), AddressFamily::V4), 3);
  QCOMPARE(countByFamily(plan.value(), AddressFamily::V6), 0);
  QCOMPARE(countByStage(plan.value(), FilterStage::OutboundConnect), 1);
  QCOMPARE(countByStage(plan.value(), FilterStage::InboundAccept), 1);
  QCOMPARE(countByStage(plan.value(), FilterStage::InboundDatagram), 1);
}

void FilterPlanTest::directionLimitsStages() {
  const auto outbound =
      expandRule(makeSpec({makeCondition(QStringLiteral("direction"), QStringLiteral("out"))}));
  QVERIFY(outbound.hasValue());
  // 只有出站一个落点，但两族的过滤器都得有。
  QCOMPARE(outbound.value().lowerBoundCount(), 2);
  QCOMPARE(countByStage(outbound.value(), FilterStage::OutboundConnect), 2);

  const auto inbound =
      expandRule(makeSpec({makeCondition(QStringLiteral("direction"), QStringLiteral("in"))}));
  QVERIFY(inbound.hasValue());
  // 入站两个落点（接收授权与数据报）：UDP 没有连接建立这一步。
  QCOMPARE(countByStage(inbound.value(), FilterStage::InboundAccept), 2);
  QCOMPARE(countByStage(inbound.value(), FilterStage::InboundDatagram), 2);
  QCOMPARE(inbound.value().lowerBoundCount(), 4);
}

void FilterPlanTest::protocolLimitsStages() {
  // 点名 UDP：入站要多一条数据报层，且条目上要带协议条件。
  const auto udp =
      expandRule(makeSpec({makeCondition(QStringLiteral("proto"), QStringLiteral("udp")),
                           makeCondition(QStringLiteral("direction"), QStringLiteral("in"))}));
  QVERIFY(udp.hasValue());
  QCOMPARE(udp.value().lowerBoundCount(), 4);
  QCOMPARE(countByStage(udp.value(), FilterStage::InboundAccept), 2);
  QCOMPARE(countByStage(udp.value(), FilterStage::InboundDatagram), 2);

  const FilterCondition* protocol = conditionOf(udp.value().entries.at(0), FilterField::Protocol);
  QVERIFY(protocol != nullptr);
  QVERIFY(protocol->protocol == TransportProtocol::Udp);

  // 点名 TCP：没有数据报层。
  const auto tcp =
      expandRule(makeSpec({makeCondition(QStringLiteral("proto"), QStringLiteral("tcp")),
                           makeCondition(QStringLiteral("direction"), QStringLiteral("in"))}));
  QVERIFY(tcp.hasValue());
  QCOMPARE(tcp.value().lowerBoundCount(), 2);
  QCOMPARE(countByStage(tcp.value(), FilterStage::InboundDatagram), 0);
}

void FilterPlanTest::addressFamilyFollowsTheAddress() {
  const auto v6 = expandRule(makeSpec({makeCondition(QStringLiteral("addr"),
                                                     QStringLiteral("exact"),
                                                     QStringList{QStringLiteral("2001:db8::1")})}));
  QVERIFY(v6.hasValue());
  QCOMPARE(countByFamily(v6.value(), AddressFamily::V6), v6.value().lowerBoundCount());
  QCOMPARE(countByFamily(v6.value(), AddressFamily::V4), 0);

  // 不限定地址不是「只做 IPv4」：两族都要出，否则 IPv6 那条路整个漏掉。
  const auto any =
      expandRule(makeSpec({makeCondition(QStringLiteral("addr"), QStringLiteral("any"))}));
  QVERIFY(any.hasValue());
  QCOMPARE(any.value().lowerBoundCount(), 6);
  QCOMPARE(countByFamily(any.value(), AddressFamily::V4), 3);
  QCOMPARE(countByFamily(any.value(), AddressFamily::V6), 3);
}

void FilterPlanTest::subnetBecomesASpan() {
  // 网段在平台上是一次匹配，要被算成区间而不是逐地址。
  const auto plan =
      expandRule(makeSpec({makeCondition(QStringLiteral("addr"),
                                         QStringLiteral("cidr"),
                                         QStringList{QStringLiteral("1.2.3.0/24")}),
                           makeCondition(QStringLiteral("direction"), QStringLiteral("out")),
                           makeCondition(QStringLiteral("proto"), QStringLiteral("tcp"))}));
  QVERIFY2(plan.hasValue(), qPrintable(errorText(plan)));
  QCOMPARE(plan.value().lowerBoundCount(), 1);

  const FilterCondition* address =
      conditionOf(plan.value().entries.at(0), FilterField::RemoteAddress);
  QVERIFY(address != nullptr);
  QCOMPARE(address->address.lower.text, QStringLiteral("1.2.3.0"));
  QCOMPARE(address->address.upper.text, QStringLiteral("1.2.3.255"));
  QVERIFY(address->address.lower.family == AddressFamily::V4);

  // IPv6 的网段同样要算对。
  const auto v6 =
      expandRule(makeSpec({makeCondition(QStringLiteral("addr"),
                                         QStringLiteral("cidr"),
                                         QStringList{QStringLiteral("2001:db8::/32")}),
                           makeCondition(QStringLiteral("direction"), QStringLiteral("out")),
                           makeCondition(QStringLiteral("proto"), QStringLiteral("tcp"))}));
  QVERIFY2(v6.hasValue(), qPrintable(errorText(v6)));
  const FilterCondition* address6 =
      conditionOf(v6.value().entries.at(0), FilterField::RemoteAddress);
  QVERIFY(address6 != nullptr);
  QCOMPARE(address6->address.lower.text, QStringLiteral("2001:db8::"));
  QCOMPARE(address6->address.upper.text, QStringLiteral("2001:db8:ffff:ffff:ffff:ffff:ffff:ffff"));
}

void FilterPlanTest::portBecomesASpan() {
  const auto single = expandRule(makeSpec(
      {makeCondition(
           QStringLiteral("port"), QStringLiteral("exact"), QStringList{QStringLiteral("80")}),
       makeCondition(QStringLiteral("direction"), QStringLiteral("out")),
       makeCondition(QStringLiteral("proto"), QStringLiteral("tcp"))}));
  QVERIFY(single.hasValue());
  const FilterCondition* port = conditionOf(single.value().entries.at(0), FilterField::Port);
  QVERIFY(port != nullptr);
  QCOMPARE(static_cast<int>(port->portLower), 80);
  QCOMPARE(static_cast<int>(port->portUpper), 80);

  const auto range =
      expandRule(makeSpec({makeCondition(QStringLiteral("port"),
                                         QStringLiteral("range"),
                                         QStringList{QStringLiteral("8000-8100")}),
                           makeCondition(QStringLiteral("direction"), QStringLiteral("out")),
                           makeCondition(QStringLiteral("proto"), QStringLiteral("tcp"))}));
  QVERIFY(range.hasValue());
  const FilterCondition* span = conditionOf(range.value().entries.at(0), FilterField::Port);
  QVERIFY(span != nullptr);
  QCOMPARE(static_cast<int>(span->portLower), 8000);
  QCOMPARE(static_cast<int>(span->portUpper), 8100);
}

void FilterPlanTest::multipleValuesShareOneFilter() {
  // 两个程序 × 两个端口 **不是**四条过滤器：同一条过滤器里同一字段可以放多个条件，
  // 语义是「或」。这一点已经实测确认，依据见 filter_plan.h 开头的说明。
  const auto plan = expandRule(makeSpec(
      {makeCondition(QStringLiteral("proc"),
                     QStringLiteral("set"),
                     QStringList{QStringLiteral("C:\\a.exe"), QStringLiteral("C:\\b.exe")}),
       makeCondition(QStringLiteral("port"),
                     QStringLiteral("set"),
                     QStringList{QStringLiteral("80"), QStringLiteral("443")}),
       makeCondition(QStringLiteral("direction"), QStringLiteral("out")),
       makeCondition(QStringLiteral("proto"), QStringLiteral("tcp")),
       makeCondition(QStringLiteral("addr"),
                     QStringLiteral("exact"),
                     QStringList{QStringLiteral("1.2.3.4")})}));
  QVERIFY2(plan.hasValue(), qPrintable(errorText(plan)));
  QCOMPARE(plan.value().lowerBoundCount(), 1);

  // 一条过滤器里：两个程序、一个地址、两个端口、一个协议。
  const FilterPlanEntry& entry = plan.value().entries.at(0);
  QCOMPARE(entry.conditions.size(), 6);

  int appPaths = 0;
  int ports = 0;
  for (const FilterCondition& condition : entry.conditions) {
    if (condition.field == FilterField::AppPath) {
      ++appPaths;
      // 路径原样穿过整条链路：用户写的是反斜杠形式，清单里就该是反斜杠形式。
      // 比较时才归一（`sameProcessIdentity`），存储与展开都不改写。
      QVERIFY(condition.appPath == QStringLiteral("C:\\a.exe") ||
              condition.appPath == QStringLiteral("C:\\b.exe"));
    } else if (condition.field == FilterField::Port) {
      ++ports;
      QVERIFY(condition.portLower == 80 || condition.portLower == 443);
    }
  }
  QCOMPARE(appPaths, 2);
  QCOMPARE(ports, 2);
}

void FilterPlanTest::negationIsCarriedNotComplemented() {
  // 取反**不求补**：算成正向区间是表达方式的选择，交给平台层挑最省的写法。
  // 所以条数不该因为取反而变化，标记要原样带到条件上。
  const auto plan =
      expandRule(makeSpec({makeCondition(QStringLiteral("addr"),
                                         QStringLiteral("cidr"),
                                         QStringList{QStringLiteral("1.2.3.0/24")},
                                         true),
                           makeCondition(QStringLiteral("direction"), QStringLiteral("out")),
                           makeCondition(QStringLiteral("proto"), QStringLiteral("tcp"))}));
  QVERIFY(plan.hasValue());
  QCOMPARE(plan.value().lowerBoundCount(), 1);

  const FilterCondition* address =
      conditionOf(plan.value().entries.at(0), FilterField::RemoteAddress);
  QVERIFY(address != nullptr);
  QVERIFY(address->negate);
  QCOMPARE(address->address.lower.text, QStringLiteral("1.2.3.0"));
  QCOMPARE(address->address.upper.text, QStringLiteral("1.2.3.255"));

  // 不取反时标记必须是假，否则平台会按反的意思下发。
  const auto plain =
      expandRule(makeSpec({makeCondition(QStringLiteral("addr"),
                                         QStringLiteral("cidr"),
                                         QStringList{QStringLiteral("1.2.3.0/24")}),
                           makeCondition(QStringLiteral("direction"), QStringLiteral("out")),
                           makeCondition(QStringLiteral("proto"), QStringLiteral("tcp"))}));
  QVERIFY(plain.hasValue());
  QVERIFY(!conditionOf(plain.value().entries.at(0), FilterField::RemoteAddress)->negate);
}

void FilterPlanTest::conditionsAreOrderedAndUniquePerField() {
  const auto plan = expandRule(makeSpec(
      {makeCondition(QStringLiteral("proc"),
                     QStringLiteral("exact"),
                     QStringList{QStringLiteral("C:\\a.exe")}),
       makeCondition(
           QStringLiteral("addr"), QStringLiteral("exact"), QStringList{QStringLiteral("1.2.3.4")}),
       makeCondition(
           QStringLiteral("port"), QStringLiteral("exact"), QStringList{QStringLiteral("443")}),
       makeCondition(QStringLiteral("proto"), QStringLiteral("tcp")),
       makeCondition(QStringLiteral("direction"), QStringLiteral("out"))}));
  QVERIFY2(plan.hasValue(), qPrintable(errorText(plan)));
  QCOMPARE(plan.value().lowerBoundCount(), 1);

  const FilterPlanEntry& entry = plan.value().entries.at(0);
  QCOMPARE(entry.conditions.size(), 4);

  // 顺序固定为程序、地址、端口、协议：清单要可比对，日志与测试都不该自己排序。
  QVERIFY(entry.conditions.at(0).field == FilterField::AppPath);
  QVERIFY(entry.conditions.at(1).field == FilterField::RemoteAddress);
  QVERIFY(entry.conditions.at(2).field == FilterField::Port);
  QVERIFY(entry.conditions.at(3).field == FilterField::Protocol);
}

void FilterPlanTest::negatedMultipleValuesStayOnOneFilter() {
  // 取反多值会生成同字段的多个 negate 条件。这个形状必须被锁定，因为
  // **它不能像正向多值那样翻译**：正向多值是「或」（对），而「≠A 或 ≠B」恒真，
  // 等于把整地址条件丢掉，规则会比预期**封得宽**。
  // 平台层看到同字段多个取反条件时必须先求补，这条约束写在 filter_plan_wfp.cpp 上；
  // 这里只锁定展开器交出去的形状。
  const auto plan = expandRule(makeSpec(
      {makeCondition(QStringLiteral("addr"),
                     QStringLiteral("cidr"),
                     QStringList{QStringLiteral("1.2.3.0/24"), QStringLiteral("10.0.0.0/8")},
                     true),
       makeCondition(QStringLiteral("direction"), QStringLiteral("out")),
       makeCondition(QStringLiteral("proto"), QStringLiteral("tcp"))}));
  QVERIFY2(plan.hasValue(), qPrintable(errorText(plan)));
  QCOMPARE(plan.value().lowerBoundCount(), 1);

  int negated = 0;
  for (const FilterCondition& condition : plan.value().entries.at(0).conditions) {
    if (condition.field == FilterField::RemoteAddress) {
      QVERIFY(condition.negate);
      ++negated;
    }
  }
  QCOMPARE(negated, 2);
}

void FilterPlanTest::actionAndPriorityFollowTheRule() {
  const auto plan =
      expandRule(makeSpec({makeCondition(QStringLiteral("direction"), QStringLiteral("out")),
                           makeCondition(QStringLiteral("proto"), QStringLiteral("tcp"))},
                          RuleAction::Allow,
                          12345));
  QVERIFY(plan.hasValue());
  QVERIFY(plan.value().lowerBoundCount() > 0);

  for (const FilterPlanEntry& entry : plan.value().entries) {
    QVERIFY(entry.action == RuleAction::Allow);
    QCOMPARE(entry.priority, 12345);
  }
}

void FilterPlanTest::wildcardAndDirectoryAreNotSupportedYet() {
  // 这两种要先把「系统里有哪些程序」落成确定的路径列表才能拆，属 S2.11 与 S2.12。
  // 关键是**明确报不支持**而不是跳过：跳过的后果是规则看着下发了、实际没封。
  struct Case {
    const char* mode;
    const char* value;
  };
  const Case cases[] = {
      {"wildcard", "C:\\Tools\\*\\tool.exe"},
      {"dir", "C:\\Tools\\bin"},
  };

  for (const Case& one : cases) {
    const auto plan =
        expandRule(makeSpec({makeCondition(QStringLiteral("proc"),
                                           QString::fromLatin1(one.mode),
                                           QStringList{QString::fromLatin1(one.value)})}));
    QVERIFY2(!plan.hasValue(), one.mode);
    QVERIFY2(plan.error().code == ErrorCode::NotSupported, one.mode);
    QVERIFY2(!plan.error().message.trimmed().isEmpty(), one.mode);
    // 说明里要指出这是「还没做」而不是「做不到」，否则用户会去找替代方案。
    QVERIFY2(plan.error().message.contains(QStringLiteral("S2.1")), one.mode);
  }
}

void FilterPlanTest::invalidConditionsAreRejected() {
  // 展开器不假设调用方一定校验过，自己再校一遍。
  const auto bad = expandRule(makeSpec({makeCondition(
      QStringLiteral("addr"), QStringLiteral("exact"), QStringList{QStringLiteral("not-an-ip")})}));
  QVERIFY(!bad.hasValue());
  QVERIFY(bad.error().code == ErrorCode::InvalidArgument);
  QVERIFY(!bad.error().message.trimmed().isEmpty());

  const auto empty = expandRule(makeSpec({}));
  QVERIFY(!empty.hasValue());
  QVERIFY(empty.error().code == ErrorCode::InvalidArgument);
}

void FilterPlanTest::oversizedPlanIsRejected() {
  // 同一条过滤器的同一个字段塞进几百个取值是不可维护的，而且这种规则
  // 几乎总是把一张表倒错了地方。宁可在展开期报错让人去改。
  QStringList ports;
  for (int index = 0; index < kMaxConditionsPerField + 10; ++index) {
    ports.append(QString::number(index));
  }

  const auto plan =
      expandRule(makeSpec({makeCondition(QStringLiteral("port"), QStringLiteral("set"), ports)}));
  QVERIFY(!plan.hasValue());
  QVERIFY(plan.error().code == ErrorCode::InvalidArgument);
  QVERIFY(plan.error().message.contains(QStringLiteral("超过")));

  // 刚好在上限上就不该拒。
  QStringList allowed;
  for (int index = 0; index < kMaxConditionsPerField; ++index) {
    allowed.append(QString::number(index));
  }
  QVERIFY(
      expandRule(makeSpec({makeCondition(QStringLiteral("port"), QStringLiteral("set"), allowed)}))
          .hasValue());
}

void FilterPlanTest::expansionIsDeterministic() {
  // 清单会被拿去与实际下发的条数对账，同一份规则必须每次都拆出同样的东西。
  const QList<MatchCondition> conditions = {
      makeCondition(QStringLiteral("proc"),
                    QStringLiteral("set"),
                    QStringList{QStringLiteral("C:\\a.exe"), QStringLiteral("C:\\b.exe")}),
      makeCondition(QStringLiteral("port"),
                    QStringLiteral("set"),
                    QStringList{QStringLiteral("80"), QStringLiteral("443")}),
      makeCondition(QStringLiteral("addr"),
                    QStringLiteral("cidr"),
                    QStringList{QStringLiteral("1.2.3.0/24"), QStringLiteral("2001:db8::/32")}),
  };

  const auto first = expandRule(makeSpec(conditions, RuleAction::Block, 100));
  const auto second = expandRule(makeSpec(conditions, RuleAction::Block, 100));
  QVERIFY(first.hasValue() && second.hasValue());
  QCOMPARE(first.value().lowerBoundCount(), second.value().lowerBoundCount());
  QVERIFY(first.value().lowerBoundCount() > 0);

  for (int index = 0; index < first.value().entries.size(); ++index) {
    const FilterPlanEntry& left = first.value().entries.at(index);
    const FilterPlanEntry& right = second.value().entries.at(index);
    QVERIFY(left.stage == right.stage);
    QVERIFY(left.family == right.family);
    QCOMPARE(left.conditions.size(), right.conditions.size());
    for (int condition = 0; condition < left.conditions.size(); ++condition) {
      QVERIFY(left.conditions.at(condition).field == right.conditions.at(condition).field);
    }
  }
}

// ---------------------------------------------------------------------------
// 取反的解析（S2.4）
// ---------------------------------------------------------------------------
//
// 这一组断言盯的是**最坏方向**：取反写错时，规则会比预期封得**宽**
// （`≠A 或 ≠B` 恒真，等于把条件整个丢掉），而验证「封得宽」比验证
// 「封得准」难得多 —— 用户只会发现「有些该通的连不上」，很难定位到是取反。

void FilterPlanTest::negationIsResolvedIntoComplements() {
  // 地址取反：一个地址补成两段，且**不再带取反标记**。
  const auto plan =
      expandRule(makeSpec({makeCondition(QStringLiteral("addr"),
                                         QStringLiteral("exact"),
                                         QStringList{QStringLiteral("1.2.3.4")},
                                         true),
                           makeCondition(QStringLiteral("direction"), QStringLiteral("out"))}));
  QVERIFY2(plan.hasValue(), qPrintable(errorText(plan)));
  QCOMPARE(plan.value().entries.size(), 1);

  const auto resolved = resolveNegation(plan.value().entries.at(0));
  QVERIFY2(resolved.hasValue(), qPrintable(errorText(resolved)));

  QCOMPARE(resolved.value().conditions.size(), 2);
  QVERIFY(!resolved.value().conditions.at(0).negate);
  QVERIFY(!resolved.value().conditions.at(1).negate);
  QCOMPARE(resolved.value().conditions.at(0).field, FilterField::RemoteAddress);
  QCOMPARE(resolved.value().conditions.at(0).address.lower.text, QStringLiteral("0.0.0.0"));
  QCOMPARE(resolved.value().conditions.at(0).address.upper.text, QStringLiteral("1.2.3.3"));
  QCOMPARE(resolved.value().conditions.at(1).address.lower.text, QStringLiteral("1.2.3.5"));
  QCOMPARE(resolved.value().conditions.at(1).address.upper.text, QStringLiteral("255.255.255.255"));

  // 多值取反：展开期是**两个条件**，这正是它必须求补的原因（两个取反条件是「或」，
  // 而「≠1.2.3.0 或 ≠5.6.7.0」恒为真）。求补之后是三段正向区间。
  //
  // 地址域没有 `set` 方式（只有 exact / range / cidr / any），多值要靠 cidr 或
  // range 的取值列表来表达，所以这里用两个 /32。
  const auto multi = expandRule(makeSpec(
      {makeCondition(QStringLiteral("addr"),
                     QStringLiteral("cidr"),
                     QStringList{QStringLiteral("1.2.3.0/32"), QStringLiteral("5.6.7.0/32")},
                     true),
       makeCondition(QStringLiteral("direction"), QStringLiteral("out"))}));
  QVERIFY2(multi.hasValue(), qPrintable(errorText(multi)));
  QCOMPARE(multi.value().entries.at(0).conditions.size(), 2);
  QVERIFY(multi.value().entries.at(0).conditions.at(0).negate);
  QVERIFY(multi.value().entries.at(0).conditions.at(1).negate);

  const auto resolvedMulti = resolveNegation(multi.value().entries.at(0));
  QVERIFY2(resolvedMulti.hasValue(), qPrintable(errorText(resolvedMulti)));
  QCOMPARE(resolvedMulti.value().conditions.size(), 3);
  for (const FilterCondition& condition : resolvedMulti.value().conditions) {
    QVERIFY(!condition.negate);
  }
  QCOMPARE(resolvedMulti.value().conditions.at(1).address.lower.text, QStringLiteral("1.2.3.1"));
  QCOMPARE(resolvedMulti.value().conditions.at(1).address.upper.text, QStringLiteral("5.6.6.255"));
  QCOMPARE(resolvedMulti.value().conditions.at(2).address.lower.text, QStringLiteral("5.6.7.1"));

  // 端口取反同理：80 变成 0-79 与 81-65535，原有的协议条件原样留着。
  const auto ports =
      expandRule(makeSpec({makeCondition(QStringLiteral("port"),
                                         QStringLiteral("exact"),
                                         QStringList{QStringLiteral("80")},
                                         true),
                           makeCondition(QStringLiteral("direction"), QStringLiteral("out")),
                           makeCondition(QStringLiteral("proto"), QStringLiteral("tcp"))}));
  QVERIFY2(ports.hasValue(), qPrintable(errorText(ports)));
  const auto resolvedPorts = resolveNegation(ports.value().entries.at(0));
  QVERIFY2(resolvedPorts.hasValue(), qPrintable(errorText(resolvedPorts)));
  QCOMPARE(resolvedPorts.value().conditions.size(), 3);
  QCOMPARE(static_cast<int>(resolvedPorts.value().conditions.at(0).portLower), 0);
  QCOMPARE(static_cast<int>(resolvedPorts.value().conditions.at(0).portUpper), 79);
  QCOMPARE(static_cast<int>(resolvedPorts.value().conditions.at(1).portLower), 81);
  QCOMPARE(static_cast<int>(resolvedPorts.value().conditions.at(1).portUpper), 65535);
  QCOMPARE(resolvedPorts.value().conditions.at(2).field, FilterField::Protocol);
  QVERIFY(!resolvedPorts.value().conditions.at(2).negate);

  // 不取反的条目要**逐字透传**：这一步不该顺手改动任何东西。
  const auto plain =
      expandRule(makeSpec({makeCondition(QStringLiteral("addr"),
                                         QStringLiteral("cidr"),
                                         QStringList{QStringLiteral("1.2.3.0/24")}),
                           makeCondition(QStringLiteral("direction"), QStringLiteral("out"))}));
  QVERIFY2(plain.hasValue(), qPrintable(errorText(plain)));
  const FilterPlanEntry& entry = plain.value().entries.at(0);
  const auto resolvedPlain = resolveNegation(entry);
  QVERIFY2(resolvedPlain.hasValue(), qPrintable(errorText(resolvedPlain)));
  QCOMPARE(resolvedPlain.value().conditions.size(), entry.conditions.size());
  QCOMPARE(resolvedPlain.value().conditions.at(0).address.lower.text,
           entry.conditions.at(0).address.lower.text);
  QCOMPARE(resolvedPlain.value().conditions.at(0).address.upper.text,
           entry.conditions.at(0).address.upper.text);
  QCOMPARE(resolvedPlain.value().stage, entry.stage);
  QCOMPARE(resolvedPlain.value().family, entry.family);
  QCOMPARE(resolvedPlain.value().priority, entry.priority);
}

void FilterPlanTest::appIdNegationStaysForThePlatform() {
  // 程序域是唯一求不出补集的字段：补集是「系统里除它之外的全部程序」，是个开放集合。
  // 单值还能靠平台的不等匹配表达，所以标记原样留着，由平台层翻成「不等于」。
  const auto plan =
      expandRule(makeSpec({makeCondition(QStringLiteral("proc"),
                                         QStringLiteral("exact"),
                                         QStringList{QStringLiteral("C:\\a.exe")},
                                         true),
                           makeCondition(QStringLiteral("direction"), QStringLiteral("out"))}));
  QVERIFY2(plan.hasValue(), qPrintable(errorText(plan)));

  const auto resolved = resolveNegation(plan.value().entries.at(0));
  QVERIFY2(resolved.hasValue(), qPrintable(errorText(resolved)));
  QCOMPARE(resolved.value().conditions.size(), 1);
  QVERIFY(resolved.value().conditions.at(0).negate);
  QCOMPARE(resolved.value().conditions.at(0).field, FilterField::AppPath);
  // 路径原样穿过：用户写反斜杠，清单里就是反斜杠。
  QCOMPARE(resolved.value().conditions.at(0).appPath, QStringLiteral("C:\\a.exe"));
}

void FilterPlanTest::unrepresentableNegationsAreRejected() {
  // 程序域多值取反：算不出补集，多条又只能是「或」，翻成「不等于 A 或 不等于 B」
  // 恒为真，程序条件会被整个丢掉 —— 封禁范围比预期大得多。宁可拒绝。
  const auto manyApps = expandRule(
      makeSpec({makeCondition(QStringLiteral("proc"),
                              QStringLiteral("set"),
                              QStringList{QStringLiteral("C:\\a.exe"), QStringLiteral("C:\\b.exe")},
                              true),
                makeCondition(QStringLiteral("direction"), QStringLiteral("out"))}));
  QVERIFY2(manyApps.hasValue(), qPrintable(errorText(manyApps)));
  const auto resolvedMany = resolveNegation(manyApps.value().entries.at(0));
  QVERIFY(!resolvedMany.hasValue());
  QCOMPARE(resolvedMany.error().code, ErrorCode::NotSupported);
  QVERIFY(resolvedMany.error().message.contains(QStringLiteral("程序")));

  // 覆盖全域的地址取反：补集是空集，也就是**永远不命中**。
  // 放它过去会被翻成「不限定地址」，与用户的意图正好相反。
  const auto everything =
      expandRule(makeSpec({makeCondition(QStringLiteral("addr"),
                                         QStringLiteral("cidr"),
                                         QStringList{QStringLiteral("0.0.0.0/0")},
                                         true),
                           makeCondition(QStringLiteral("direction"), QStringLiteral("out"))}));
  QVERIFY2(everything.hasValue(), qPrintable(errorText(everything)));
  const auto resolvedEverything = resolveNegation(everything.value().entries.at(0));
  QVERIFY(!resolvedEverything.hasValue());
  QCOMPARE(resolvedEverything.error().code, ErrorCode::InvalidArgument);
  QVERIFY(resolvedEverything.error().message.contains(QStringLiteral("永远")));

  // 端口全域取反：同上。
  const auto allPorts =
      expandRule(makeSpec({makeCondition(QStringLiteral("port"),
                                         QStringLiteral("range"),
                                         QStringList{QStringLiteral("0-65535")},
                                         true),
                           makeCondition(QStringLiteral("direction"), QStringLiteral("out")),
                           makeCondition(QStringLiteral("proto"), QStringLiteral("tcp"))}));
  QVERIFY2(allPorts.hasValue(), qPrintable(errorText(allPorts)));
  const auto resolvedAllPorts = resolveNegation(allPorts.value().entries.at(0));
  QVERIFY(!resolvedAllPorts.hasValue());
  QCOMPARE(resolvedAllPorts.error().code, ErrorCode::InvalidArgument);
}

QTEST_GUILESS_MAIN(FilterPlanTest)

#include "filter_plan_test.moc"
