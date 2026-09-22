// 规则生命周期编排的检查（阶段二 S2.9）。
//
// 断言全部落在**顺序与回滚**上，用两个记账用的假实现观察 ——
// 与 ban_action 那份同源。真实系统上的效果（重启后规则自动生效、到期自动失效）
// 由 tmp/drill-s2.9.cpp 验。
//
// 其中三条最要紧：
//   1. 先下发、后落库；
//   2. 落库失败要把引擎**恢复成原样**（更新场景是恢复成旧版本，不是撤销）；
//   3. 过期失效先撤、后停用 —— 撤销失败就不许改启用状态。

#include <QTest>

#include <QDateTime>
#include <QHash>
#include <QString>
#include <QStringList>

#include "core/error.h"
#include "core/rule_runtime.h"

using namespace baniphelper::core;

namespace {

/// 取失败原因，成功时给空串。
///
/// ⚠️ 不能在断言说明里直接写 `result.error().message`：`Result::error()` 带前置条件，
/// 而 `QVERIFY2` 的说明参数是**当场求值**的 —— 成功路径上也会算到它，于是抛
/// `std::bad_variant_access`，症状是「断言明明通过了，测试却报 unhandled exception」。
template <typename T>
QString why(const Result<T>& result) {
  return result.errorOrNull() != nullptr ? result.error().message : QString();
}

MatchCondition outbound() {
  MatchCondition condition;
  condition.domain = QStringLiteral("direction");
  condition.mode = QStringLiteral("out");
  return condition;
}

MatchCondition address(const QString& value) {
  MatchCondition condition;
  condition.domain = QStringLiteral("addr");
  condition.mode = QStringLiteral("exact");
  condition.values = QStringList{value};
  return condition;
}

Rule makeRule(const QString& id, const QString& addressValue = QStringLiteral("192.0.2.1")) {
  Rule rule;
  rule.id = id;
  rule.action = RuleAction::Block;
  rule.conditions = {address(addressValue), outbound()};
  rule.createdAt = QDateTime::currentDateTimeUtc();
  return rule;
}

// ---------------------------------------------------------------------------
// 记账用的假实现
// ---------------------------------------------------------------------------

class FakeEngine : public IFilterEngine {
 public:
  explicit FakeEngine(QStringList* log) : log_(log) {}

  /// 非空时 `applyRule` 失败。`Undo` 前缀的那些用来只让某一条规则失败。
  QString failApplyForId;
  QString failRevokeForId;

  /// 每次成功下发的条件取值，用来区分「新版本」与「旧版本」。
  QStringList appliedAddresses;
  QStringList revokedIds;

  [[nodiscard]] Result<void> open() override {
    return Result<void>::ok();
  }
  [[nodiscard]] Result<void> close() override {
    return Result<void>::ok();
  }
  [[nodiscard]] bool isOpen() const override {
    return true;
  }

  [[nodiscard]] Result<void> applyRule(const RuleSpec& rule) override {
    log_->append(QStringLiteral("apply:%1").arg(rule.id));
    if (rule.id == failApplyForId) {
      return Result<void>::fail(
          makeError(ErrorCode::Platform, QStringLiteral("平台拒绝了这次下发")));
    }
    if (!rule.conditions.isEmpty()) {
      appliedAddresses.append(rule.conditions.first().values.value(0));
    }
    return Result<void>::ok();
  }

  [[nodiscard]] Result<void> revokeRule(const RuleId& id) override {
    log_->append(QStringLiteral("revoke:%1").arg(id));
    if (id == failRevokeForId) {
      return Result<void>::fail(makeError(ErrorCode::Platform, QStringLiteral("撤销被平台拒绝")));
    }
    revokedIds.append(id);
    return Result<void>::ok();
  }

  [[nodiscard]] Result<void> revokeAll() override {
    return Result<void>::ok();
  }

  [[nodiscard]] Result<QList<AppliedRuleSummary>> appliedRules() const override {
    return Result<QList<AppliedRuleSummary>>::ok({});
  }

  [[nodiscard]] Result<void> applyDefaultBlock(std::int32_t) override {
    return Result<void>::ok();
  }
  [[nodiscard]] Result<bool> hasDefaultBlock() const override {
    return Result<bool>::ok(false);
  }
  [[nodiscard]] Result<void> revokeDefaultBlock() override {
    return Result<void>::ok();
  }
  [[nodiscard]] Result<CleanupReport> cleanupOrphans() override {
    return Result<CleanupReport>::ok(CleanupReport{});
  }

 private:
  QStringList* log_;
};

class FakeStore : public IRuleStore {
 public:
  explicit FakeStore(QStringList* log) : log_(log) {}

  QString failUpsertForId;
  QString failRemoveForId;

  [[nodiscard]] Result<QList<Rule>> list() const override {
    return Result<QList<Rule>>::ok(rules_.values());
  }

  [[nodiscard]] Result<Rule> find(const RuleId& id) const override {
    if (!rules_.contains(id)) {
      return Result<Rule>::fail(
          makeError(ErrorCode::NotFound, QStringLiteral("没有标识为「%1」的规则").arg(id)));
    }
    return Result<Rule>::ok(rules_.value(id));
  }

  [[nodiscard]] Result<void> upsert(const Rule& rule) override {
    log_->append(QStringLiteral("store.upsert:%1").arg(rule.id));
    if (rule.id == failUpsertForId) {
      return Result<void>::fail(makeError(ErrorCode::Io, QStringLiteral("磁盘写满了")));
    }
    rules_.insert(rule.id, rule);
    return Result<void>::ok();
  }

  [[nodiscard]] Result<void> remove(const RuleId& id) override {
    log_->append(QStringLiteral("store.remove:%1").arg(id));
    if (id == failRemoveForId) {
      return Result<void>::fail(makeError(ErrorCode::Io, QStringLiteral("磁盘写满了")));
    }
    rules_.remove(id);
    return Result<void>::ok();
  }

  [[nodiscard]] Result<void> clear() override {
    rules_.clear();
    return Result<void>::ok();
  }

  [[nodiscard]] Result<QList<Rule>> expired(const QDateTime& now) const override {
    QList<Rule> due;
    for (const Rule& rule : rules_) {
      if (rule.enabled && rule.expireAt.isValid() && rule.expireAt.toUTC() <= now.toUTC()) {
        due.append(rule);
      }
    }
    return Result<QList<Rule>>::ok(due);
  }

  [[nodiscard]] Result<SubscriptionId> subscribe(RuleChangeSink) override {
    return Result<SubscriptionId>::ok(1);
  }
  [[nodiscard]] Result<void> unsubscribe(SubscriptionId) override {
    return Result<void>::ok();
  }

  void seed(const Rule& rule) {
    rules_.insert(rule.id, rule);
  }
  [[nodiscard]] bool contains(const RuleId& id) const {
    return rules_.contains(id);
  }
  [[nodiscard]] Rule at(const RuleId& id) const {
    return rules_.value(id);
  }

 private:
  QStringList* log_;
  QHash<RuleId, Rule> rules_;
};

struct Harness {
  QStringList log;
  FakeEngine engine{&log};
  FakeStore store{&log};
  RuleRuntime runtime{store, engine};
};

}  // namespace

class RuleRuntimeTest : public QObject {
  Q_OBJECT

 private slots:
  void saveAppliesBeforePersisting();
  void failedApplyLeavesStoreUntouched();
  void failedPersistRollsEngineBackToNothing();
  void failedPersistRollsEngineBackToPreviousVersion();
  void disabledRuleIsRevokedInsteadOfApplied();
  void removeRevokesBeforeDeleting();
  void failedDeletePutsTheFilterBack();
  void restoreAccountsForEveryRule();
  void restoreKeepsGoingAfterOneFailure();
  void expireDueRevokesThenDisables();
  void expireDueKeepsRuleEnabledWhenRevokeFails();
};

void RuleRuntimeTest::saveAppliesBeforePersisting() {
  Harness harness;
  const Rule rule = makeRule(QStringLiteral("r1"));

  const Result<void> saved = harness.runtime.save(rule);

  QVERIFY2(saved.hasValue(), qPrintable(why(saved)));
  QCOMPARE(harness.log,
           QStringList({QStringLiteral("apply:r1"), QStringLiteral("store.upsert:r1")}));
  QVERIFY(harness.store.contains(QStringLiteral("r1")));
}

void RuleRuntimeTest::failedApplyLeavesStoreUntouched() {
  Harness harness;
  harness.engine.failApplyForId = QStringLiteral("r1");

  const Result<void> saved = harness.runtime.save(makeRule(QStringLiteral("r1")));

  QVERIFY2(!saved.hasValue(), "下发失败必须让整个保存失败");
  QCOMPARE(harness.log, QStringList({QStringLiteral("apply:r1")}));
  QVERIFY2(!harness.store.contains(QStringLiteral("r1")),
           "下发都没成功，不许在库里留下一条「启用中」的规则");
}

void RuleRuntimeTest::failedPersistRollsEngineBackToNothing() {
  Harness harness;
  harness.store.failUpsertForId = QStringLiteral("r1");

  const Result<void> saved = harness.runtime.save(makeRule(QStringLiteral("r1")));

  QVERIFY2(!saved.hasValue(), "落库失败必须报错");
  // 本来没有这条规则，所以「恢复原样」= 撤销。
  QCOMPARE(harness.log,
           QStringList({QStringLiteral("apply:r1"),
                        QStringLiteral("store.upsert:r1"),
                        QStringLiteral("revoke:r1")}));
  QVERIFY(harness.engine.revokedIds.contains(QStringLiteral("r1")));
}

void RuleRuntimeTest::failedPersistRollsEngineBackToPreviousVersion() {
  Harness harness;
  const Rule original = makeRule(QStringLiteral("r1"), QStringLiteral("192.0.2.1"));
  harness.store.seed(original);

  harness.store.failUpsertForId = QStringLiteral("r1");
  const Rule edited = makeRule(QStringLiteral("r1"), QStringLiteral("198.51.100.7"));

  const Result<void> saved = harness.runtime.save(edited);

  QVERIFY2(!saved.hasValue(), "落库失败必须报错");
  // 关键：恢复的是**旧版本**，不是撤销。撤销会把原来能用的规则也弄没。
  QCOMPARE(harness.engine.appliedAddresses,
           QStringList({QStringLiteral("198.51.100.7"), QStringLiteral("192.0.2.1")}));
  QVERIFY2(!harness.engine.revokedIds.contains(QStringLiteral("r1")), "更新失败不该把旧规则也撤掉");
  QCOMPARE(harness.store.at(QStringLiteral("r1")).conditions.first().values.value(0),
           QStringLiteral("192.0.2.1"));
}

void RuleRuntimeTest::disabledRuleIsRevokedInsteadOfApplied() {
  Harness harness;
  Rule rule = makeRule(QStringLiteral("r1"));
  rule.enabled = false;

  const Result<void> saved = harness.runtime.save(rule);

  QVERIFY2(saved.hasValue(), qPrintable(why(saved)));
  // 停用的规则不但不下发，还要把可能已经存在的过滤器撤掉 ——
  // 用户点「停用」之后的期待是它立刻不再生效，而不是等下次启动。
  QCOMPARE(harness.log,
           QStringList({QStringLiteral("revoke:r1"), QStringLiteral("store.upsert:r1")}));
  QVERIFY(!harness.store.at(QStringLiteral("r1")).enabled);
}

void RuleRuntimeTest::removeRevokesBeforeDeleting() {
  Harness harness;
  harness.store.seed(makeRule(QStringLiteral("r1")));

  const Result<void> removed = harness.runtime.remove(QStringLiteral("r1"));

  QVERIFY2(removed.hasValue(), qPrintable(why(removed)));
  QCOMPARE(harness.log,
           QStringList({QStringLiteral("revoke:r1"), QStringLiteral("store.remove:r1")}));
  QVERIFY(!harness.store.contains(QStringLiteral("r1")));
}

void RuleRuntimeTest::failedDeletePutsTheFilterBack() {
  Harness harness;
  harness.store.seed(makeRule(QStringLiteral("r1"), QStringLiteral("192.0.2.1")));
  harness.store.failRemoveForId = QStringLiteral("r1");

  const Result<void> removed = harness.runtime.remove(QStringLiteral("r1"));

  QVERIFY2(!removed.hasValue(), "删库失败必须报错");
  // 库里那条还在，所以过滤器也要装回去，否则「库里说启用、实际没生效」。
  QVERIFY2(harness.engine.appliedAddresses.contains(QStringLiteral("192.0.2.1")),
           "删库失败之后要把过滤器装回去");
  QVERIFY(harness.store.contains(QStringLiteral("r1")));
}

void RuleRuntimeTest::restoreAccountsForEveryRule() {
  Harness harness;
  const QDateTime now = QDateTime::currentDateTimeUtc();

  harness.store.seed(makeRule(QStringLiteral("enabled")));

  Rule disabled = makeRule(QStringLiteral("disabled"));
  disabled.enabled = false;
  harness.store.seed(disabled);

  Rule expired = makeRule(QStringLiteral("expired"));
  expired.expireAt = now.addSecs(-60);
  harness.store.seed(expired);

  const Result<RestoreReport> report = harness.runtime.restore();

  QVERIFY2(report.hasValue(), qPrintable(why(report)));
  QCOMPARE(report.value().applied, 1);
  QCOMPARE(report.value().disabled, 1);
  QCOMPARE(report.value().alreadyExpired, 1);
  QCOMPARE(report.value().failures.size(), 0);
  // 「每一条都要有交代」：四类加起来等于库里的条数。
  QCOMPARE(report.value().total(), harness.store.list().value().size());
  // 过期的规则**不下发**：下发一次再撤掉只会在内核里过一手。
  QVERIFY(!harness.engine.appliedAddresses.isEmpty());
  QCOMPARE(harness.log.size(), 1);
  QCOMPARE(harness.log.first(), QStringLiteral("apply:enabled"));
}

void RuleRuntimeTest::restoreKeepsGoingAfterOneFailure() {
  Harness harness;
  harness.store.seed(makeRule(QStringLiteral("broken")));
  harness.store.seed(makeRule(QStringLiteral("good")));
  harness.engine.failApplyForId = QStringLiteral("broken");

  const Result<RestoreReport> report = harness.runtime.restore();

  QVERIFY2(report.hasValue(), "单条失败不该让整个恢复失败");
  QCOMPARE(report.value().applied, 1);
  QCOMPARE(report.value().failures.size(), 1);
  QCOMPARE(report.value().failures.first().id, QStringLiteral("broken"));
  QVERIFY2(!report.value().failures.first().reason.trimmed().isEmpty(),
           "失败原因不允许为空，界面要直接显示它");
  QCOMPARE(report.value().total(), 2);
  QVERIFY(harness.log.contains(QStringLiteral("apply:good")));
}

void RuleRuntimeTest::expireDueRevokesThenDisables() {
  Harness harness;
  Rule due = makeRule(QStringLiteral("due"));
  due.expireAt = QDateTime::currentDateTimeUtc().addSecs(-60);
  harness.store.seed(due);

  const Result<ExpireReport> report = harness.runtime.expireDue(QDateTime::currentDateTimeUtc());

  QVERIFY2(report.hasValue(), qPrintable(why(report)));
  QCOMPARE(report.value().disabled, QStringList({QStringLiteral("due")}));
  QCOMPARE(report.value().failures.size(), 0);
  QCOMPARE(harness.log,
           QStringList({QStringLiteral("revoke:due"), QStringLiteral("store.upsert:due")}));
  QVERIFY(!harness.store.at(QStringLiteral("due")).enabled);
  // 再跑一趟不该重复处理：库里那条已经停用了。
  harness.log.clear();
  const Result<ExpireReport> again = harness.runtime.expireDue(QDateTime::currentDateTimeUtc());
  QVERIFY(again.hasValue());
  QCOMPARE(again.value().disabled.size(), 0);
  QCOMPARE(harness.log.size(), 0);
}

void RuleRuntimeTest::expireDueKeepsRuleEnabledWhenRevokeFails() {
  Harness harness;
  Rule due = makeRule(QStringLiteral("due"));
  due.expireAt = QDateTime::currentDateTimeUtc().addSecs(-60);
  harness.store.seed(due);
  harness.engine.failRevokeForId = QStringLiteral("due");

  const Result<ExpireReport> report = harness.runtime.expireDue(QDateTime::currentDateTimeUtc());

  QVERIFY2(report.hasValue(), "单条失败不该让整趟巡检失败");
  QCOMPARE(report.value().disabled.size(), 0);
  QCOMPARE(report.value().failures.size(), 1);
  // 规则还在生效，所以库里的记录必须还是「启用中」：说成停用就等于对用户撒谎，
  // 而且重启之后它会以「启用中」被重新下发。
  QVERIFY2(harness.store.at(QStringLiteral("due")).enabled, "撤销失败的规则不许被标成停用");
}

QTEST_APPLESS_MAIN(RuleRuntimeTest)

#include "rule_runtime_test.moc"
