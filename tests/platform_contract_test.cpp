// 平台接口的契约测试（阶段一 S1.3 起，后续每实现一个平台接口就往下补）。
//
// 一套断言同时跑在**真实后端**与**内存后端**上。这是抽象层成立与否的唯一判据：
// 只测真实后端，出了分歧会分不清是接口用错了还是实现有 bug；
// 只测内存后端，测的又不是真正要上线的代码。
//
// 真实后端经装配点（createPlatformBackend）构造，因此本文件**不需要包含任何平台专有的头**，
// 将来加了新平台也不用改这里 —— 装配点会自动指向那个平台。

#include <QTest>
#include <QUuid>

#include <functional>

#include "core/platform_backend.h"
#include "platform/memory/backend.h"
#include "platform/memory/filter_engine.h"
#include "platform/memory/privilege.h"

using namespace baniphelper::core;

namespace {

/// 一个被测后端：一个标签，加上「用指定单实例名字造出它」的工厂。
struct BackendCase {
  QString label;
  std::function<Result<PlatformBackend>(const QString& name)> create;
};

QList<BackendCase> backendCases() {
  return {
      BackendCase{QStringLiteral("real"),
                  [](const QString& name) { return createPlatformBackend(name); }},
      BackendCase{
          QStringLiteral("memory"),
          [](const QString& name) { return Result<PlatformBackend>::ok(makeMemoryBackend(name)); }},
  };
}

/// 每次测试都换一个新名字，免得与正在运行的程序抢同一个内核对象 ——
/// 那样测试结果会取决于当时有没有开着程序。
QString freshInstanceName() {
  return QStringLiteral("BanIPHelper.contract.") +
         QUuid::createUuid().toString(QUuid::WithoutBraces);
}

/// 同一实例名下的两份后端。
///
/// 单实例的契约必须拿两份才能验证，而两份都要能独立构造，
/// 所以在协议里就把它们都准备好。
///
/// 持有**非 const** 引用是必要的：`acquire` 与 `relaunchElevated` 这类方法
/// 会改变对象状态，本来就不是 const 方法。
struct Backends {
  QString label;
  QString instanceName;
  PlatformBackend& first;
  PlatformBackend& second;
};

/// 对每个后端跑一遍给定断言，失败信息里带后端标签。
template <typename Fn>
void forEachBackend(Fn&& fn) {
  for (const BackendCase& testCase : backendCases()) {
    const QString name = freshInstanceName();
    Result<PlatformBackend> first = testCase.create(name);
    Result<PlatformBackend> second = testCase.create(name);

    if (!first || !second) {
      const QString reason = !first ? first.error().message : second.error().message;
      QFAIL(qPrintable(QStringLiteral("后端 %1 构造失败：%2").arg(testCase.label, reason)));
      return;
    }

    const Backends backends{testCase.label, name, first.value(), second.value()};
    fn(backends);
  }
}

}  // namespace

class PlatformContractTest : public QObject {
  Q_OBJECT

 private slots:
  void backendIsComplete();
  void capabilityDeclarationMatchesImplementation();
  void privilegeAnswersClearly();
  void elevatedProcessDoesNotRelaunchItself();
  void singleInstanceRejectsSecondHolder();
  void singleInstanceRecoversAfterRelease();
  void releaseIsIdempotent();
  void signalWithoutHolderIsNotSilentSuccess();
  void activationReachesTheHolder();
  void activationHandlerCanBeReplaced();
  void activationStopsAfterStopListening();
  void memoryPrivilegeCoversUnelevatedBranch();
  void filterEngineOpensAndClosesIdempotently();
  void filterEngineCleansUpOwnFiltersOnly();
  void filterEngineRefusesWhatItCannotDo();
  void memoryFilterEngineRemovesSeededOrphans();
};

void PlatformContractTest::backendIsComplete() {
  forEachBackend([](const Backends& backends) {
    const PlatformBackend& backend = backends.first;
    QVERIFY2(!backend.name.isEmpty(), qPrintable(backends.label + " 后端没有名字"));
    QVERIFY2(backend.isComplete(), qPrintable(backends.label + " 后端装配不完整"));
  });
}

void PlatformContractTest::capabilityDeclarationMatchesImplementation() {
  forEachBackend([](const Backends& backends) {
    const PlatformBackend& backend = backends.first;

    QVERIFY2(backend.filterEngine != nullptr,
             qPrintable(backends.label + " 后端没有装配过滤器引擎"));

    // 实现了某个接口，就必须把对应的能力位声明为支持。
    // 声明与实际不一致会让界面上出现「灰按钮但其实是能用的」，或反过来 ——
    // 后者更糟：用户以为能用，点下去才发现不行。
    QVERIFY2(
        backend.capabilities->isSupported(Capability::SingleInstance),
        qPrintable(backends.label + " 后端实现了 ISingleInstance，却没声明 SingleInstance 能力"));
    QVERIFY2(backend.capabilities->isSupported(Capability::Elevation),
             qPrintable(backends.label + " 后端实现了 IPrivilege，却没声明 Elevation 能力"));

    // 过滤器引擎存在，但下发能力还没实现，因此 FilterIPv4 必须**不**被声明。
    // 这一条与 filterEngineRefusesWhatItCannotDo 是一对：
    // 一个管声明，一个管行为，两边必须同时成立。
    QVERIFY2(!backend.capabilities->isSupported(Capability::FilterIPv4),
             qPrintable(backends.label + " 后端声明了 IPv4 过滤能力，但下发还没实现"));
  });
}

void PlatformContractTest::privilegeAnswersClearly() {
  forEachBackend([](const Backends& backends) {
    IPrivilege& privilege = *backends.first.privilege;

    // 提权状态必须能问出明确答案，而且重复问答案稳定。
    const Result<bool> first = privilege.isElevated();
    QVERIFY2(first.hasValue(),
             qPrintable(QStringLiteral("%1 后端无法给出提权状态：%2")
                            .arg(backends.label,
                                 first.errorOrNull() ? first.error().message : QString())));
    const Result<bool> second = privilege.isElevated();
    QVERIFY(second.hasValue());
    QCOMPARE(first.value(), second.value());

    // 缺权限的说明不允许为空：界面要直接把它显示给用户。
    QVERIFY2(
        !privilege.elevationRequirementText().trimmed().isEmpty(),
        qPrintable(backends.label + " 后端没给出缺权限时的说明，界面只能显示一句没有信息的话"));
  });
}

void PlatformContractTest::elevatedProcessDoesNotRelaunchItself() {
  forEachBackend([](const Backends& backends) {
    IPrivilege& privilege = *backends.first.privilege;

    const Result<bool> elevated = privilege.isElevated();
    QVERIFY(elevated.hasValue());

    // 把本次运行覆盖到哪条分支打出来：以提权与非提权两种方式各跑一次，
    // 才能说这条契约被完整验证过，否则「测试通过」可能只是把断言跳过了。
    qInfo().noquote() << QStringLiteral("后端 %1：当前%2，本次运行%3覆盖「已提权时不再拉起自己」")
                             .arg(backends.label,
                                  elevated.value() ? QStringLiteral("已提权")
                                                   : QStringLiteral("未提权"),
                                  elevated.value() ? QString() : QStringLiteral("未"));

    if (!elevated.value()) {
      // 未提权时调用 relaunchElevated 会真的拉起一个进程并弹出提权确认框。
      // 契约测试不允许有这种副作用，因此这一条只在已提权的前提下验证；
      // 未提权那条分支由 memoryPrivilegeCoversUnelevatedBranch 覆盖。
      return;
    }

    const Result<void> relaunch = privilege.relaunchElevated();
    QVERIFY2(!relaunch.hasValue(),
             qPrintable(backends.label +
                        " 后端明明已提权却仍然拉起了新实例，会留下两个进程各下一套过滤器"));
    QVERIFY(relaunch.error().code == ErrorCode::AlreadyExists);
  });
}

void PlatformContractTest::singleInstanceRejectsSecondHolder() {
  forEachBackend([](const Backends& backends) {
    const Result<bool> first = backends.first.singleInstance->acquire();
    QVERIFY2(first.hasValue(),
             qPrintable(QStringLiteral("%1 后端取所有权时报错：%2")
                            .arg(backends.label,
                                 first.errorOrNull() ? first.error().message : QString())));
    QVERIFY2(first.value(), qPrintable(backends.label + " 后端首个实例没能取得所有权"));

    // 同一个对象重复取得所有权是幂等的。
    const Result<bool> repeat = backends.first.singleInstance->acquire();
    QVERIFY(repeat.hasValue());
    QVERIFY2(repeat.value(), qPrintable(backends.label + " 后端重复取得所有权失败"));

    // 第二个同名实例必须被识别为「已有实例」，而**不是**报成错误：
    // 调用方正是靠这个 false 去走「唤起既有实例然后退出」那条路径的。
    const Result<bool> second = backends.second.singleInstance->acquire();
    QVERIFY2(second.hasValue(),
             qPrintable(QStringLiteral("%1 后端把「已有实例」报成了错误：%2")
                            .arg(backends.label,
                                 second.errorOrNull() ? second.error().message : QString())));
    QVERIFY2(!second.value(), qPrintable(backends.label + " 后端没认出第二个实例"));
  });
}

void PlatformContractTest::singleInstanceRecoversAfterRelease() {
  forEachBackend([](const Backends& backends) {
    const Result<bool> acquired = backends.first.singleInstance->acquire();
    QVERIFY(acquired.hasValue() && acquired.value());

    backends.first.singleInstance->release();

    // 释放之后必须能重新取得。做不到的话，程序崩一次就再也启动不起来了 ——
    // 用残留文件判断实例的实现正是这么坏的。
    const Result<bool> reacquired = backends.second.singleInstance->acquire();
    QVERIFY2(reacquired.hasValue(), qPrintable(backends.label + " 后端释放后重新取得时报错"));
    QVERIFY2(reacquired.value(),
             qPrintable(backends.label + " 后端在释放之后仍然认为已有实例在跑"));
  });
}

void PlatformContractTest::releaseIsIdempotent() {
  forEachBackend([](const Backends& backends) {
    // 退出路径可能被走两次，重复释放不允许崩，也不允许把状态搅乱。
    backends.first.singleInstance->release();
    backends.first.singleInstance->release();
    backends.first.singleInstance->release();

    const Result<bool> acquired = backends.first.singleInstance->acquire();
    QVERIFY(acquired.hasValue());
    QVERIFY2(acquired.value(), qPrintable(backends.label + " 后端在重复释放之后无法再取得所有权"));
  });
}

void PlatformContractTest::signalWithoutHolderIsNotSilentSuccess() {
  forEachBackend([](const Backends& backends) {
    // 实例名是刚生成的，没有任何人持有它，也没有任何人开接收端。
    const Result<void> signaled = backends.first.singleInstance->signalExisting();

    QVERIFY2(!signaled.hasValue(),
             qPrintable(backends.label +
                        " 后端在没有既有实例时报了成功 —— 调用方会以为唤起了界面然后自己退出，"
                        "结果是新旧两个窗口都不见了"));
    QVERIFY(signaled.error().code == ErrorCode::NotFound);
    QVERIFY2(!signaled.error().message.trimmed().isEmpty(), "失败必须带非空说明");
  });
}

void PlatformContractTest::activationReachesTheHolder() {
  forEachBackend([](const Backends& backends) {
    ISingleInstance& holder = *backends.first.singleInstance;
    ISingleInstance& latecomer = *backends.second.singleInstance;

    const Result<bool> acquired = holder.acquire();
    QVERIFY(acquired.hasValue() && acquired.value());

    int activations = 0;
    const Result<void> listening = holder.listenForActivation([&activations]() { ++activations; });
    QVERIFY2(listening.hasValue(),
             qPrintable(QStringLiteral("%1 后端建立接收端失败：%2")
                            .arg(backends.label,
                                 listening.errorOrNull() ? listening.error().message : QString())));
    QCOMPARE(activations, 0);

    const Result<void> signaled = latecomer.signalExisting();
    QVERIFY2(signaled.hasValue(),
             qPrintable(QStringLiteral("%1 后端唤不起已经开了接收端的实例：%2")
                            .arg(backends.label,
                                 signaled.errorOrNull() ? signaled.error().message : QString())));

    // 投递是异步的（实现负责送到主线程），所以等而不是当场断言。
    QTRY_COMPARE_WITH_TIMEOUT(activations, 1, 5000);

    holder.stopListening();
  });
}

void PlatformContractTest::activationHandlerCanBeReplaced() {
  forEachBackend([](const Backends& backends) {
    ISingleInstance& holder = *backends.first.singleInstance;
    ISingleInstance& latecomer = *backends.second.singleInstance;

    const Result<bool> acquired = holder.acquire();
    QVERIFY(acquired.hasValue() && acquired.value());

    int oldHandlerCalls = 0;
    int newHandlerCalls = 0;
    QVERIFY(holder.listenForActivation([&oldHandlerCalls]() { ++oldHandlerCalls; }).hasValue());
    QVERIFY(holder.listenForActivation([&newHandlerCalls]() { ++newHandlerCalls; }).hasValue());

    QVERIFY(latecomer.signalExisting().hasValue());
    QTRY_COMPARE_WITH_TIMEOUT(newHandlerCalls, 1, 5000);

    // 旧的处理函数必须已经被换掉。两个都活着的话，一次唤起会把界面拿起来两遍。
    QCOMPARE(oldHandlerCalls, 0);

    holder.stopListening();
  });
}

void PlatformContractTest::activationStopsAfterStopListening() {
  forEachBackend([](const Backends& backends) {
    ISingleInstance& holder = *backends.first.singleInstance;
    ISingleInstance& latecomer = *backends.second.singleInstance;

    const Result<bool> acquired = holder.acquire();
    QVERIFY(acquired.hasValue() && acquired.value());

    int activations = 0;
    QVERIFY(holder.listenForActivation([&activations]() { ++activations; }).hasValue());

    holder.stopListening();
    // 重复停止同样是幂等的，退出路径可能被走两次。
    holder.stopListening();

    const Result<void> signaled = latecomer.signalExisting();
    QVERIFY2(
        !signaled.hasValue(),
        qPrintable(backends.label +
                   " 后端在接收端已停之后仍然报唤起成功 —— 调用方会以为界面出来了然后自己退出"));
    QVERIFY(signaled.error().code == ErrorCode::NotFound);

    // 手工放行一次事件循环，确认没有哪一次投递被压在队列里。
    QTest::qWait(50);
    QCOMPARE(activations, 0);

    // 空处理函数要当场拒掉，不能建出一个「收了请求但什么也不做」的黑洞。
    const Result<void> empty = holder.listenForActivation(ActivationHandler());
    QVERIFY(!empty.hasValue());
    QVERIFY(empty.error().code == ErrorCode::InvalidArgument);
  });
}

void PlatformContractTest::memoryPrivilegeCoversUnelevatedBranch() {
  // 真实后端测不了这条分支：测试进程的权限是外部给定的，测试里没法既当已提权又当未提权。
  // 这正是「同一套契约测试」需要内存实现的原因。
  MemoryPrivilege privilege(false);

  const Result<bool> before = privilege.isElevated();
  QVERIFY(before.hasValue());
  QVERIFY(!before.value());

  const Result<void> relaunch = privilege.relaunchElevated();
  QVERIFY2(relaunch.hasValue(), "未提权时拉起提权实例不应报 AlreadyExists");
  QCOMPARE(privilege.relaunchCount(), 1);

  // 拉起之后状态变为已提权，此时再拉一次必须被拒 —— 否则会留下两个进程。
  const Result<bool> after = privilege.isElevated();
  QVERIFY(after.hasValue());
  QVERIFY(after.value());

  const Result<void> again = privilege.relaunchElevated();
  QVERIFY(!again.hasValue());
  QVERIFY(again.error().code == ErrorCode::AlreadyExists);
  QCOMPARE(privilege.relaunchCount(), 1);
}

void PlatformContractTest::filterEngineOpensAndClosesIdempotently() {
  forEachBackend([](const Backends& backends) {
    IFilterEngine& engine = *backends.first.filterEngine;
    const Result<bool> elevated = backends.first.privilege->isElevated();
    QVERIFY(elevated.hasValue());

    QVERIFY2(!engine.isOpen(), qPrintable(backends.label + " 后端刚构造出来就报告引擎已打开"));

    const Result<void> opened = engine.open();
    if (!opened.hasValue()) {
      // 未提权时引擎必然打不开。但**已提权却打不开就是缺陷**，
      // 不能因为「反正另一条路能过」就把它放过去。
      QVERIFY2(!elevated.value(),
               qPrintable(QStringLiteral("%1 后端已提权却打不开过滤器引擎：%2")
                              .arg(backends.label, opened.error().message)));
      QVERIFY2(!opened.error().message.trimmed().isEmpty(), "打开引擎失败必须给出非空说明");
      qInfo().noquote() << QStringLiteral(
                               "后端 %1：当前未提权，引擎打开后的行为本次未覆盖，"
                               "用 sudo 再跑一遍才算完整")
                               .arg(backends.label);
      return;
    }

    QVERIFY2(engine.isOpen(), qPrintable(backends.label + " 后端打开之后仍然报告未打开"));

    // 重复打开是幂等的，接口契约要求如此。
    QVERIFY2(engine.open().hasValue(), qPrintable(backends.label + " 后端重复打开引擎失败"));

    QVERIFY2(engine.close().hasValue(), qPrintable(backends.label + " 后端关闭引擎失败"));
    QVERIFY2(!engine.isOpen(), qPrintable(backends.label + " 后端关闭之后仍然报告已打开"));

    // 重复关闭同样幂等。
    QVERIFY2(engine.close().hasValue(), qPrintable(backends.label + " 后端重复关闭引擎失败"));
  });
}

void PlatformContractTest::filterEngineCleansUpOwnFiltersOnly() {
  forEachBackend([](const Backends& backends) {
    IFilterEngine& engine = *backends.first.filterEngine;
    const Result<bool> elevated = backends.first.privilege->isElevated();
    QVERIFY(elevated.hasValue());

    if (!engine.open().hasValue()) {
      QVERIFY2(!elevated.value(),
               qPrintable(backends.label + " 后端已提权却打不开过滤器引擎，清理无从验证"));
      return;
    }

    // 第一次可能清掉上次运行残留的自家过滤器，条数取决于机器状态，因此不做断言。
    const Result<CleanupReport> first = engine.cleanupOrphans();
    QVERIFY2(first.hasValue(),
             qPrintable(QStringLiteral("%1 后端清理残留过滤器失败：%2")
                            .arg(backends.label,
                                 first.errorOrNull() ? first.error().message : QString())));
    QVERIFY(first.value().removedOwn >= 0);

    // 把「本次清掉了几条」打出来：以提权方式跑时，这一行就是「清理真的生效」的证据，
    // 否则测试通过只能说明「调用没报错」。
    if (first.value().removedOwn > 0) {
      qInfo().noquote() << QStringLiteral("后端 %1：本次清掉 %2 条上次运行残留的过滤器")
                               .arg(backends.label)
                               .arg(first.value().removedOwn);
    }

    // 他方过滤器在结构上不可能被选中（枚举按自有 provider 限定），所以这个数必须是 0。
    // 它一旦非零，说明有人改成了全量枚举再逐个判断 —— 那正是要防的写法。
    QCOMPARE(first.value().skippedForeign, 0);

    // 第二次必须一条都不剩：这才叫「清干净了」，而不是「清掉了一部分」。
    const Result<CleanupReport> second = engine.cleanupOrphans();
    QVERIFY(second.hasValue());
    QCOMPARE(second.value().removedOwn, 0);

    QVERIFY(engine.close().hasValue());
  });
}

void PlatformContractTest::filterEngineRefusesWhatItCannotDo() {
  forEachBackend([](const Backends& backends) {
    IFilterEngine& engine = *backends.first.filterEngine;

    // 引擎没打开时清理必须报错，而不是静默成功：
    // 静默成功会让上层以为「已经清干净了」，带着一堆残留继续跑。
    const Result<CleanupReport> closedState = engine.cleanupOrphans();
    QVERIFY2(!closedState.hasValue(),
             qPrintable(backends.label + " 后端在引擎未打开时清理却报了成功"));

    const Result<bool> elevated = backends.first.privilege->isElevated();
    QVERIFY(elevated.hasValue());
    if (!engine.open().hasValue()) {
      QVERIFY2(!elevated.value(), qPrintable(backends.label + " 后端已提权却打不开过滤器引擎"));
      return;
    }

    // 声明与实际必须一致：能力位说不支持，接口就必须明确报「不支持」，
    // 不允许悄悄成功，也不允许悄悄什么都不做。
    if (!backends.first.capabilities->isSupported(Capability::FilterIPv4)) {
      RuleSpec rule;
      rule.id = QStringLiteral("contract-test-rule");
      rule.conditions.append(
          MatchCondition{QStringLiteral("proc"), QStringLiteral("any"), {}, false});

      const Result<void> applied = engine.applyRule(rule);
      QVERIFY2(!applied.hasValue(),
               qPrintable(backends.label + " 后端声明不支持 IPv4 过滤，applyRule 却成功了"));
      QVERIFY(applied.error().code == ErrorCode::NotSupported);
      QVERIFY2(!applied.error().message.trimmed().isEmpty(), "不支持必须给出非空说明");
    }

    QVERIFY(engine.close().hasValue());
  });
}

void PlatformContractTest::memoryFilterEngineRemovesSeededOrphans() {
  // 真实后端在测试里造不出残留：那需要真的下发过滤器，而下发还没实现。
  // 所以「清理确实把残留清掉了」只能在内存后端上验证 ——
  // 这正是契约测试需要一个可控对照物的原因。
  MemoryFilterEngine engine;
  QVERIFY(engine.open().hasValue());

  engine.seedOrphans(3);
  QCOMPARE(engine.storedFilterCount(), 3);

  const Result<CleanupReport> report = engine.cleanupOrphans();
  QVERIFY(report.hasValue());
  QCOMPARE(report.value().removedOwn, 3);
  QCOMPARE(report.value().skippedForeign, 0);
  QCOMPARE(engine.storedFilterCount(), 0);

  // revokeAll 与清理走同一个原语，同样要清干净。
  engine.seedOrphans(2);
  QVERIFY(engine.revokeAll().hasValue());
  QCOMPARE(engine.storedFilterCount(), 0);
}

QTEST_GUILESS_MAIN(PlatformContractTest)

#include "platform_contract_test.moc"
