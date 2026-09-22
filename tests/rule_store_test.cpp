// 规则仓储的检查（阶段二 S2.9）。
//
// 跑在**真实的 SQLite** 上（QTemporaryDir 里的临时库），不碰 `%APPDATA%`。
// 断言的落点与 S2.1/S2.4 那两个模块同源：重点不在「存得进去」，
// 而在**该拒的有没有全拒**、以及**库里塞了坏行会不会被宽松解释**。
//
// 「读出来也校验」这一条尤其重要：库是可以被手工改的，而按错的语义封禁
// 比这条规则不生效危险得多。

#include <QTest>

#include <QDateTime>
#include <QDir>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QVariantList>

#include "core/database.h"
#include "core/error.h"
#include "core/rule.h"
#include "core/rule_model.h"
#include "core/rule_store_sqlite.h"

using namespace baniphelper::core;

namespace {

QString databasePathIn(const QTemporaryDir& dir) {
  return QDir(dir.path()).absoluteFilePath(QStringLiteral("baniphelper.db"));
}

MatchCondition outbound() {
  MatchCondition condition;
  condition.domain = QStringLiteral("direction");
  condition.mode = QStringLiteral("out");
  return condition;
}

MatchCondition address(const QString& value, bool negate = false) {
  MatchCondition condition;
  condition.domain = QStringLiteral("addr");
  condition.mode = QStringLiteral("exact");
  condition.values = QStringList{value};
  condition.negate = negate;
  return condition;
}

Rule makeRule(const QString& id) {
  Rule rule;
  rule.id = id;
  rule.action = RuleAction::Block;
  rule.conditions = {address(QStringLiteral("192.0.2.1")), outbound()};
  rule.note = QStringLiteral("测试用");
  return rule;
}

}  // namespace

class RuleStoreTest : public QObject {
  Q_OBJECT

 private slots:
  void upsertAndFindRoundTrip();
  void listIsOrderedByCreationTime();
  void updateKeepsOriginalCreationTime();
  void invalidRuleIsRejectedAndNothingIsWritten();
  void removeIsIdempotent();
  void clearNotifiesEveryRemovedRule();
  void expiredReturnsOnlyEnabledAndDue();
  void expireAtRoundTripsRegardlessOfTimeZone();
  void subscriptionsReceiveChanges();
  void emptySinkIsRejected();
  void corruptedRowIsRejectedOnRead();
  void missingRuleIsReportedAsNotFound();
};

void RuleStoreTest::upsertAndFindRoundTrip() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  Database database;
  QVERIFY(database.open(databasePathIn(dir)).hasValue());
  SqliteRuleStore store(database);

  Rule rule = makeRule(QStringLiteral("r1"));
  rule.action = RuleAction::Allow;
  rule.enabled = false;
  rule.expireAt = QDateTime::currentDateTimeUtc().addSecs(3600);
  rule.conditions = {address(QStringLiteral("2001:db8::1"), true), outbound()};

  const Result<void> written = store.upsert(rule);
  QVERIFY2(written.hasValue(),
           qPrintable(written.hasValue() ? QString() : written.error().message));

  const Result<Rule> loaded = store.find(QStringLiteral("r1"));
  QVERIFY(loaded.hasValue());
  QCOMPARE(loaded.value().id, QStringLiteral("r1"));
  QCOMPARE(loaded.value().action, RuleAction::Allow);
  QCOMPARE(loaded.value().enabled, false);
  QCOMPARE(loaded.value().note, QStringLiteral("测试用"));
  QCOMPARE(loaded.value().schema, kCurrentRuleSchema);
  QVERIFY(loaded.value().createdAt.isValid());
  QVERIFY2(loaded.value().expireAt.isValid(), "过期时间没能往返");
  QCOMPARE(loaded.value().conditions.size(), 2);
  QCOMPARE(loaded.value().conditions.at(0).domain, QStringLiteral("addr"));
  QCOMPARE(loaded.value().conditions.at(0).values, QStringList({QStringLiteral("2001:db8::1")}));
  QCOMPARE(loaded.value().conditions.at(0).negate, true);
  QCOMPARE(loaded.value().conditions.at(1).mode, QStringLiteral("out"));

  // 时刻要按「同一个瞬间」比对，而不是按字符串 —— 存的是 UTC，读回来可能带上时区表示。
  QCOMPARE(loaded.value().expireAt.toSecsSinceEpoch(), rule.expireAt.toSecsSinceEpoch());
}

void RuleStoreTest::listIsOrderedByCreationTime() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  Database database;
  QVERIFY(database.open(databasePathIn(dir)).hasValue());
  SqliteRuleStore store(database);

  const QDateTime base = QDateTime::currentDateTimeUtc();
  for (int index = 0; index < 3; ++index) {
    Rule rule = makeRule(QStringLiteral("r%1").arg(index));
    rule.createdAt = base.addSecs(index);
    QVERIFY(store.upsert(rule).hasValue());
  }

  const Result<QList<Rule>> rules = store.list();
  QVERIFY(rules.hasValue());
  QCOMPARE(rules.value().size(), 3);
  // 顺序稳定是「同具体度则先建的优先」这条优先级规则的地基，不能随查询计划漂。
  QCOMPARE(rules.value().at(0).id, QStringLiteral("r0"));
  QCOMPARE(rules.value().at(1).id, QStringLiteral("r1"));
  QCOMPARE(rules.value().at(2).id, QStringLiteral("r2"));
}

void RuleStoreTest::updateKeepsOriginalCreationTime() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  Database database;
  QVERIFY(database.open(databasePathIn(dir)).hasValue());
  SqliteRuleStore store(database);

  Rule first = makeRule(QStringLiteral("r1"));
  first.createdAt = QDateTime::currentDateTimeUtc().addSecs(-3600);
  QVERIFY(store.upsert(first).hasValue());
  const QDateTime original = store.find(QStringLiteral("r1")).value().createdAt;

  // 更新时故意传一个「现在」的时间：存储层必须忽略它。
  Rule edited = first;
  edited.createdAt = QDateTime::currentDateTimeUtc();
  edited.note = QStringLiteral("改过了");
  QVERIFY(store.upsert(edited).hasValue());

  const Result<Rule> loaded = store.find(QStringLiteral("r1"));
  QVERIFY(loaded.hasValue());
  QCOMPARE(loaded.value().note, QStringLiteral("改过了"));
  QCOMPARE(loaded.value().createdAt.toSecsSinceEpoch(), original.toSecsSinceEpoch());
}

void RuleStoreTest::invalidRuleIsRejectedAndNothingIsWritten() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  Database database;
  QVERIFY(database.open(databasePathIn(dir)).hasValue());
  SqliteRuleStore store(database);

  // 空条件：要表达「全部」必须显式写一个 any 条件。
  Rule empty = makeRule(QStringLiteral("empty"));
  empty.conditions.clear();
  const Result<void> rejectedEmpty = store.upsert(empty);
  QVERIFY2(!rejectedEmpty.hasValue(), "空条件规则必须被拒绝");
  QVERIFY2(!rejectedEmpty.error().message.trimmed().isEmpty(), "拒绝必须带原因");

  // 不认识的域：**绝不宽松解释**。
  Rule unknown = makeRule(QStringLiteral("unknown"));
  unknown.conditions = {address(QStringLiteral("192.0.2.1"))};
  unknown.conditions.first().domain = QStringLiteral("nonsense");
  const Result<void> rejectedUnknown = store.upsert(unknown);
  QVERIFY2(!rejectedUnknown.hasValue(), "不认识的域必须被拒绝");

  // 取值范围不合法。
  Rule badValue = makeRule(QStringLiteral("bad"));
  badValue.conditions = {address(QStringLiteral("192.0.2.999")), outbound()};
  QVERIFY2(!store.upsert(badValue).hasValue(), "不合法的取值必须被拒绝");

  // 同一域出现两个条件是交集语义，恒为假，必须拒绝。
  Rule duplicate = makeRule(QStringLiteral("dup"));
  duplicate.conditions = {
      address(QStringLiteral("192.0.2.1")), address(QStringLiteral("192.0.2.2")), outbound()};
  QVERIFY2(!store.upsert(duplicate).hasValue(), "同域重复条件必须被拒绝");

  // 一条都不许写进去。
  const Result<QList<Rule>> rules = store.list();
  QVERIFY(rules.hasValue());
  QCOMPARE(rules.value().size(), 0);

  // 空标识也要拒。
  Rule nameless = makeRule(QString());
  QVERIFY2(!store.upsert(nameless).hasValue(), "空标识必须被拒绝");
}

void RuleStoreTest::removeIsIdempotent() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  Database database;
  QVERIFY(database.open(databasePathIn(dir)).hasValue());
  SqliteRuleStore store(database);

  QVERIFY(store.upsert(makeRule(QStringLiteral("r1"))).hasValue());
  QVERIFY(store.remove(QStringLiteral("r1")).hasValue());
  // 第二次删同一个标识仍然成功 —— 撤销流程会被重复调用。
  QVERIFY2(store.remove(QStringLiteral("r1")).hasValue(), "删除不存在的规则要视为成功");
  QVERIFY(!store.find(QStringLiteral("r1")).hasValue());
}

void RuleStoreTest::clearNotifiesEveryRemovedRule() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  Database database;
  QVERIFY(database.open(databasePathIn(dir)).hasValue());
  SqliteRuleStore store(database);

  QStringList notified;
  const Result<SubscriptionId> subscribed =
      store.subscribe([&notified](const RuleId& id) { notified.append(id); });
  QVERIFY(subscribed.hasValue());

  QVERIFY(store.upsert(makeRule(QStringLiteral("r1"))).hasValue());
  QVERIFY(store.upsert(makeRule(QStringLiteral("r2"))).hasValue());
  notified.clear();

  QVERIFY(store.clear().hasValue());
  QCOMPARE(store.list().value().size(), 0);
  // 批量变更逐条通知：订阅者按标识刷新，给不出一个「全部」的假标识。
  QCOMPARE(notified.size(), 2);
  QVERIFY(notified.contains(QStringLiteral("r1")));
  QVERIFY(notified.contains(QStringLiteral("r2")));

  // 已经空了再清，不该再发通知。
  notified.clear();
  QVERIFY(store.clear().hasValue());
  QCOMPARE(notified.size(), 0);
}

void RuleStoreTest::expiredReturnsOnlyEnabledAndDue() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  Database database;
  QVERIFY(database.open(databasePathIn(dir)).hasValue());
  SqliteRuleStore store(database);

  const QDateTime now = QDateTime::currentDateTimeUtc();

  Rule due = makeRule(QStringLiteral("due"));
  due.expireAt = now.addSecs(-60);
  QVERIFY(store.upsert(due).hasValue());

  Rule future = makeRule(QStringLiteral("future"));
  future.expireAt = now.addSecs(3600);
  QVERIFY(store.upsert(future).hasValue());

  Rule never = makeRule(QStringLiteral("never"));
  QVERIFY(store.upsert(never).hasValue());

  Rule disabledDue = makeRule(QStringLiteral("disabledDue"));
  disabledDue.expireAt = now.addSecs(-60);
  disabledDue.enabled = false;
  QVERIFY(store.upsert(disabledDue).hasValue());

  const Result<QList<Rule>> expired = store.expired(now);
  QVERIFY(expired.hasValue());
  QCOMPARE(expired.value().size(), 1);
  QCOMPARE(expired.value().first().id, QStringLiteral("due"));
}

void RuleStoreTest::expireAtRoundTripsRegardlessOfTimeZone() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  Database database;
  QVERIFY(database.open(databasePathIn(dir)).hasValue());
  SqliteRuleStore store(database);

  // 存的是本地时间表示：落到库里必须是 UTC，读回来必须是同一个瞬间。
  // 用字符串比较做到期判断，所以「本地时间也能正确往返」这件事不能靠运气。
  Rule local = makeRule(QStringLiteral("local"));
  local.expireAt = QDateTime::currentDateTime().addSecs(600);
  QVERIFY(store.upsert(local).hasValue());

  const Result<Rule> loaded = store.find(QStringLiteral("local"));
  QVERIFY(loaded.hasValue());
  QCOMPARE(loaded.value().expireAt.toSecsSinceEpoch(), local.expireAt.toSecsSinceEpoch());
}

void RuleStoreTest::subscriptionsReceiveChanges() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  Database database;
  QVERIFY(database.open(databasePathIn(dir)).hasValue());
  SqliteRuleStore store(database);

  QStringList notified;
  const Result<SubscriptionId> kept =
      store.subscribe([&notified](const RuleId& id) { notified.append(id); });
  QVERIFY(kept.hasValue());
  SubscriptionId dropped = 0;
  const Result<SubscriptionId> temporary =
      store.subscribe([&dropped](const RuleId& id) { dropped += static_cast<int>(id.size()); });
  QVERIFY(temporary.hasValue());

  QVERIFY(store.upsert(makeRule(QStringLiteral("r1"))).hasValue());
  QCOMPARE(notified.size(), 1);

  QVERIFY(store.unsubscribe(temporary.value()).hasValue());
  // 重复退订视为成功。
  QVERIFY(store.unsubscribe(temporary.value()).hasValue());

  QVERIFY(store.upsert(makeRule(QStringLiteral("r2"))).hasValue());
  QCOMPARE(notified.size(), 2);
  QCOMPARE(dropped, 2);  // 只收到第一次通知（r1）
}

void RuleStoreTest::emptySinkIsRejected() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  Database database;
  QVERIFY(database.open(databasePathIn(dir)).hasValue());
  SqliteRuleStore store(database);

  // 传空处理函数会建出一个「收了通知但什么也不做」的黑洞，当场拒掉。
  const Result<SubscriptionId> subscribed = store.subscribe(RuleChangeSink());
  QVERIFY(!subscribed.hasValue());
  QVERIFY(subscribed.error().code == ErrorCode::InvalidArgument);
}

void RuleStoreTest::corruptedRowIsRejectedOnRead() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  Database database;
  QVERIFY(database.open(databasePathIn(dir)).hasValue());
  SqliteRuleStore store(database);

  // 绕开仓储直接写一行「域不认识」的规则，模拟手工改库或旧版本写入。
  const QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
  const Result<void> planted = database.execute(
      QStringLiteral("INSERT INTO rules (id, action, conditions, enabled, expire_at, note, "
                     "schema, created_at, updated_at) VALUES (?, ?, ?, ?, NULL, '', ?, ?, ?)"),
      QVariantList{
          QStringLiteral("planted"),
          static_cast<int>(RuleAction::Block),
          QStringLiteral(R"([{"domain":"nonsense","mode":"exact","negate":false,"values":["x"]}])"),
          1,
          kCurrentRuleSchema,
          now,
          now});
  QVERIFY2(planted.hasValue(),
           qPrintable(planted.hasValue() ? QString() : planted.error().message));

  const Result<QList<Rule>> rules = store.list();
  QVERIFY2(!rules.hasValue(), "读出一行坏规则必须报错，而不是跳过它");
  QVERIFY2(rules.error().message.contains(QStringLiteral("planted")),
           qPrintable(QStringLiteral("错误信息里要能看出是哪一条：%1").arg(rules.error().message)));
  QVERIFY2(rules.error().message.contains(QStringLiteral("nonsense")) ||
               rules.error().message.contains(QStringLiteral("域")),
           qPrintable(QStringLiteral("错误信息里要说清坏在哪：%1").arg(rules.error().message)));

  // 同样一条也要能从 find 里报出来。
  QVERIFY(!store.find(QStringLiteral("planted")).hasValue());
}

void RuleStoreTest::missingRuleIsReportedAsNotFound() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  Database database;
  QVERIFY(database.open(databasePathIn(dir)).hasValue());
  SqliteRuleStore store(database);

  const Result<Rule> missing = store.find(QStringLiteral("nope"));
  QVERIFY(!missing.hasValue());
  // 「没有这条规则」与「读失败了」是两件事：上层据此决定是提示还是报错。
  QVERIFY(missing.error().code == ErrorCode::NotFound);
}

QTEST_GUILESS_MAIN(RuleStoreTest)

#include "rule_store_test.moc"
