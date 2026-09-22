// 「先封后断」这条时序不变量的测试（阶段二 S2.8）。
//
// 这一步的代码只有二十行，但它是整个封禁链路里唯一一条**时序**要求，
// 而写反的症状是「点封禁之后连接闪一下又回来了」—— 只在少数时序下复现，
// 靠手工点几乎测不出来。所以断言全部落在**调用顺序**上，用两个记账用的假实现来观察。
//
// 用假实现而不是真实后端：这里要验的是「编排」，与平台无关。
// 真实后端能不能真的断掉一条连接，由 tmp/drill-s2.8.cpp 对着系统验。

#include <QTest>
#include <QStringList>

#include "core/ban_action.h"
#include "core/rule_model.h"

using namespace baniphelper::core;

namespace {

/// 取失败原因，成功时给空串。
///
/// ⚠️ **不能**在断言说明里直接写 `result.error().message`：`Result::error()`
/// 带前置条件（只能在一件失败的结果上取），而 `QVERIFY2` 的说明参数是**当场求值**的
/// —— 成功路径上也会算到它，于是抛 `std::bad_variant_access`。
/// 症状是「断言明明通过了，测试却报 unhandled exception」。
template <typename T>
QString why(const Result<T>& result) {
  return result.errorOrNull() != nullptr ? result.error().message : QString();
}

// ---------------------------------------------------------------------------
// 两个记账用的假实现
// ---------------------------------------------------------------------------

/// 只记录「被要求做什么」，并按开关决定成功还是失败。
///
/// 所有调用都追加到**同一个**日志里 —— 顺序正是被测对象，
/// 两个实现各记一份的话就看不出先后。
class FakeEngine : public IFilterEngine {
 public:
  explicit FakeEngine(QStringList* log) : log_(log) {}

  /// 非空时 `applyRule` 直接失败，用来验证「下发失败就不许断」。
  QString failApplyWith;

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
    log_->append(QStringLiteral("apply:") + rule.id);
    if (!failApplyWith.isEmpty()) {
      return Result<void>::fail(makeError(ErrorCode::InvalidArgument, failApplyWith));
    }
    applied_.append(rule);
    return Result<void>::ok();
  }

  [[nodiscard]] Result<void> revokeRule(const RuleId& id) override {
    log_->append(QStringLiteral("revoke:") + id);
    return Result<void>::ok();
  }

  [[nodiscard]] Result<void> revokeAll() override {
    return Result<void>::ok();
  }

  [[nodiscard]] Result<QList<AppliedRuleSummary>> appliedRules() const override {
    QList<AppliedRuleSummary> summaries;
    for (const RuleSpec& rule : applied_) {
      AppliedRuleSummary summary;
      summary.id = rule.id;
      summary.action = rule.action;
      summary.filterCount = 1;
      summary.active = true;
      summaries.append(summary);
    }
    return Result<QList<AppliedRuleSummary>>::ok(summaries);
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
  QList<RuleSpec> applied_;
};

class FakeKiller : public IKiller {
 public:
  explicit FakeKiller(QStringList* log) : log_(log) {}

  /// 断连实现整体失败（例如引擎没打开）。
  QString failAllWith;

  /// 这些连接断不掉，会出现在 `KillReport::failures` 里。
  QList<ConnectionKey> refuseThese;

  int killManyCalls = 0;

  [[nodiscard]] Result<bool> canKill(const ConnectionKey&) const override {
    return Result<bool>::ok(true);
  }

  [[nodiscard]] Result<void> kill(const ConnectionKey& connection) override {
    log_->append(QStringLiteral("kill:%1").arg(connection.remote.port));
    if (refuseThese.contains(connection)) {
      return Result<void>::fail(
          makeError(ErrorCode::Platform, QStringLiteral("对端不响应，这条断不掉")));
    }
    return Result<void>::ok();
  }

  [[nodiscard]] Result<KillReport> killMany(const QList<ConnectionKey>& connections) override {
    ++killManyCalls;
    log_->append(QStringLiteral("killMany:%1").arg(connections.size()));
    if (!failAllWith.isEmpty()) {
      return Result<KillReport>::fail(makeError(ErrorCode::Busy, failAllWith));
    }

    KillReport report;
    report.requested = static_cast<int>(connections.size());
    for (const ConnectionKey& connection : connections) {
      const Result<void> outcome = kill(connection);
      if (outcome) {
        ++report.killed;
        continue;
      }
      KillFailure failure;
      failure.connection = connection;
      failure.reason = outcome.error().message;
      report.failures.append(failure);
    }
    return Result<KillReport>::ok(report);
  }

 private:
  QStringList* log_;
};

// ---------------------------------------------------------------------------
// 夹具
// ---------------------------------------------------------------------------

/// 一次编排要用到的全套：一个共同的日志、两个假实现，外加构造好的规则与连接。
struct Harness {
  QStringList log;
  FakeEngine engine{&log};
  FakeKiller killer{&log};

  static RuleSpec rule(const QString& id) {
    RuleSpec spec;
    spec.id = id;
    spec.action = RuleAction::Block;
    spec.priority = 1200;
    MatchCondition condition;
    condition.domain = QStringLiteral("direction");
    condition.mode = QStringLiteral("out");
    spec.conditions = {condition};
    return spec;
  }

  static ConnectionKey connection(std::uint16_t localPort, std::uint16_t remotePort) {
    ConnectionKey key;
    key.protocol = TransportProtocol::Tcp;
    key.local.address.family = AddressFamily::V4;
    key.local.address.text = QStringLiteral("192.168.137.1");
    key.local.port = localPort;
    key.remote.address.family = AddressFamily::V4;
    key.remote.address.text = QStringLiteral("223.5.5.5");
    key.remote.port = remotePort;
    return key;
  }
};

}  // namespace

class BanActionTest : public QObject {
  Q_OBJECT

 private slots:
  void applyComesBeforeKill();
  void failedApplyKillsNothing();
  void emptyConnectionListLeavesKillerAlone();
  void perConnectionFailuresAreKept();
  void killerFailureSaysRuleIsAlreadyInEffect();
};

/// 最核心的一条：顺序必须是「先 applyRule，再断」。
void BanActionTest::applyComesBeforeKill() {
  Harness harness;
  const QList<ConnectionKey> established{Harness::connection(51000, 443),
                                         Harness::connection(51001, 8443)};

  const Result<KillReport> outcome = applyBanAndKill(
      harness.engine, harness.killer, Harness::rule(QStringLiteral("r1")), established);

  QVERIFY2(outcome.hasValue(), qPrintable(why(outcome)));
  // 日志是**两个实现共用的**，所以它能证明先后，而不只是「两边都被调过」。
  // `killMany` 内部又逐条调了 `kill`，那两条也一并记下来。
  QCOMPARE(harness.log,
           QStringList({QStringLiteral("apply:r1"),
                        QStringLiteral("killMany:2"),
                        QStringLiteral("kill:443"),
                        QStringLiteral("kill:8443")}));
  QCOMPARE(outcome.value().requested, 2);
  QCOMPARE(outcome.value().killed, 2);
  QCOMPARE(outcome.value().failures.size(), 0);
}

/// 下发失败时，**一条连接都不许断**：断了却封不住，等于白白打断用户的连接。
void BanActionTest::failedApplyKillsNothing() {
  Harness harness;
  harness.engine.failApplyWith = QStringLiteral("规则里有取值不合法");
  const QList<ConnectionKey> established{Harness::connection(51000, 443)};

  const Result<KillReport> outcome = applyBanAndKill(
      harness.engine, harness.killer, Harness::rule(QStringLiteral("r1")), established);

  QVERIFY2(!outcome.hasValue(), "下发失败时整体必须失败");
  QCOMPARE(outcome.error().message, QStringLiteral("规则里有取值不合法"));
  QCOMPARE(harness.log, QStringList({QStringLiteral("apply:r1")}));
  QCOMPARE(harness.killer.killManyCalls, 0);
}

/// 没有已建立的连接时，不去碰断连实现 —— 「一次都没被碰过」比「用空列表调一次」更容易核对。
void BanActionTest::emptyConnectionListLeavesKillerAlone() {
  Harness harness;

  const Result<KillReport> outcome =
      applyBanAndKill(harness.engine, harness.killer, Harness::rule(QStringLiteral("r1")), {});

  QVERIFY2(outcome.hasValue(), qPrintable(why(outcome)));
  QCOMPARE(harness.log, QStringList({QStringLiteral("apply:r1")}));
  QCOMPARE(harness.killer.killManyCalls, 0);
  QCOMPARE(outcome.value().requested, 0);
  QCOMPARE(outcome.value().killed, 0);
}

/// 某一条断不掉不影响别的，而且必须逐条给出原因（不允许聚合成「部分失败」）。
void BanActionTest::perConnectionFailuresAreKept() {
  Harness harness;
  const ConnectionKey stubborn = Harness::connection(51001, 443);
  harness.killer.refuseThese = {stubborn};
  const QList<ConnectionKey> established{Harness::connection(51000, 443), stubborn};

  const Result<KillReport> outcome = applyBanAndKill(
      harness.engine, harness.killer, Harness::rule(QStringLiteral("r1")), established);

  QVERIFY2(outcome.hasValue(), "个别连接断不掉不算整体失败");
  QCOMPARE(outcome.value().requested, 2);
  QCOMPARE(outcome.value().killed, 1);
  QCOMPARE(outcome.value().failures.size(), 1);
  QCOMPARE(outcome.value().failures.at(0).connection.remote.port, static_cast<std::uint16_t>(443));
  QCOMPARE(outcome.value().failures.at(0).connection.local.port, static_cast<std::uint16_t>(51001));
  QVERIFY2(!outcome.value().failures.at(0).reason.trimmed().isEmpty(),
           "失败原因不允许为空，界面要直接显示它");
}

/// 规则已经生效、断连整体失败时，错误信息必须说清「规则已下发，只需重试断连」，
/// 否则用户会以为封禁也失败了，去重新下发一遍（甚至以为整个功能坏了）。
void BanActionTest::killerFailureSaysRuleIsAlreadyInEffect() {
  Harness harness;
  harness.killer.failAllWith = QStringLiteral("引擎句柄已关闭");
  const QList<ConnectionKey> established{Harness::connection(51000, 443)};

  const Result<KillReport> outcome = applyBanAndKill(
      harness.engine, harness.killer, Harness::rule(QStringLiteral("r1")), established);

  QVERIFY2(!outcome.hasValue(), "断连整体失败要报错");
  QCOMPARE(outcome.error().code, ErrorCode::Busy);
  QVERIFY2(
      outcome.error().message.contains(QStringLiteral("已下发")),
      qPrintable(QStringLiteral("错误信息里没说清规则已经生效：%1").arg(outcome.error().message)));
  QVERIFY2(outcome.error().message.contains(QStringLiteral("引擎句柄已关闭")),
           qPrintable(QStringLiteral("错误信息里丢了底层原因：%1").arg(outcome.error().message)));
  // 顺序仍然是「先封后断」，只是断那一步整体失败了。
  QCOMPARE(harness.log, QStringList({QStringLiteral("apply:r1"), QStringLiteral("killMany:1")}));
}

QTEST_APPLESS_MAIN(BanActionTest)

#include "ban_action_test.moc"
