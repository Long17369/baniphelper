// 平台接口的契约测试（阶段一 S1.3 起，后续每实现一个平台接口就往下补）。
//
// 一套断言同时跑在**真实后端**与**内存后端**上。这是抽象层成立与否的唯一判据：
// 只测真实后端，出了分歧会分不清是接口用错了还是实现有 bug；
// 只测内存后端，测的又不是真正要上线的代码。
//
// 真实后端经装配点（createPlatformBackend）构造，因此本文件**不需要包含任何平台专有的头**，
// 将来加了新平台也不用改这里 —— 装配点会自动指向那个平台。

#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <QUdpSocket>
#include <QUuid>

#include <functional>

#include "core/platform_backend.h"
#include "platform/memory/backend.h"
#include "platform/memory/conn_monitor.h"
#include "platform/memory/filter_engine.h"
#include "platform/memory/privilege.h"
#include "platform/memory/traffic_stats.h"

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

/// 造一个格式合法的连接标识。
///
/// ⚠️ 本地地址刻意用 TEST-NET-3（`203.0.113.0/24`）—— 它不可能真的是本机地址，
/// 所以拿它去 `SetTcpEntry` 绝不可能误删一条真实连接。
/// 契约测试跑在开发机上，这一点不能只靠「应该不会那么巧」。
ConnectionKey makeConnectionKey(TransportProtocol protocol, AddressFamily family) {
  const bool v4 = family == AddressFamily::V4;
  ConnectionKey key;
  key.protocol = protocol;
  key.local.address.family = family;
  key.local.address.text = v4 ? QStringLiteral("203.0.113.9") : QStringLiteral("2001:db8::9");
  key.local.port = 51000;
  key.remote.address.family = family;
  key.remote.address.text = v4 ? QStringLiteral("223.5.5.5") : QStringLiteral("2001:db8::1");
  key.remote.port = 443;
  return key;
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
  void killerAnswersMatchDeclaredCapabilities();
  void killerWillNotSilentlySucceed();
  void connMonitorSnapshotIsWellFormed();
  void connMonitorFindsOwnSocketsWhenItCanEnumerate();
  void connMonitorEventsMatchDeclaredCapabilities();
  void connMonitorDeliversEmittedEvents();
  void trafficStatsRefusesReadBeforeCollection();
  void memoryTrafficStatsDeliversProgrammedCounters();
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
    QVERIFY2(backend.killer != nullptr, qPrintable(backends.label + " 后端没有装配断连模块"));
    QVERIFY2(backend.connMonitor != nullptr,
             qPrintable(backends.label + " 后端没有装配连接监视模块"));

    // 实现了某个接口，就必须把对应的能力位声明为支持。
    // 声明与实际不一致会让界面上出现「灰按钮但其实是能用的」，或反过来 ——
    // 后者更糟：用户以为能用，点下去才发现不行。
    QVERIFY2(
        backend.capabilities->isSupported(Capability::SingleInstance),
        qPrintable(backends.label + " 后端实现了 ISingleInstance，却没声明 SingleInstance 能力"));
    QVERIFY2(backend.capabilities->isSupported(Capability::Elevation),
             qPrintable(backends.label + " 后端实现了 IPrivilege，却没声明 Elevation 能力"));

    // 过滤器引擎的下发能力：声明必须与实现的进度一致，而**两个后端的进度不同**，
    // 所以这里给的是各自的期望值，不是一句对所有后端都成立的话 ——
    // 后者在能力落地时只能删掉重写，删掉之后就没人守着这件事了。
    //
    // - 真实后端：S2.4 起能下发与撤销规则（一次性端到端演练实测）；
    //   残留清理 S1.7 起就有，只是一直漏在声明之外，那同样是不一致。
    // - 内存后端：不追求完整的对照物，只保证界面能在没有管理员权限时跑起来。
    const bool real = backends.label == QStringLiteral("real");
    QVERIFY2(backend.capabilities->isSupported(Capability::FilterIPv4) == real,
             qPrintable(backends.label + " 后端的 FilterIPv4 声明与实现进度不一致"));
    QVERIFY2(backend.capabilities->isSupported(Capability::OrphanCleanup) == real,
             qPrintable(backends.label + " 后端的 OrphanCleanup 声明与实现进度不一致"));

    // 连接枚举：S3.1 落地后真实后端能枚举端点表并解出进程标识（实测见
    // docs/phases/03-observation.md 第 3.1 节），内存后端只能回放预置内容、枚举不了系统进程。
    QVERIFY2(backend.capabilities->isSupported(Capability::ProcessEnumeration) == real,
             qPrintable(backends.label + " 后端的 ProcessEnumeration 声明与实现进度不一致"));
    // 事件驱动：S3.3 落地后**两个后端都声明**，但理由不同 ——
    // 真实后端订的是 ETW 的 TCP 生命周期事件，内存后端是真的把投进去的事件送到订阅者手上。
    // 两个都声明，是因为「订上了就真收得到」这条契约在两边都成立；
    // 真实后端未提权时订不上，那属于权限不足（`NotPermitted`），
    // 由 connMonitorEventsMatchDeclaredCapabilities 单独守。
    QVERIFY2(backend.capabilities->isSupported(Capability::EventDrivenConnections),
             qPrintable(backends.label + " 后端实现了事件订阅却没声明 EventDrivenConnections，"
                                         "界面上会把它显示成「只能轮询」"));

    // 字节统计（S3.2）：TCP 两侧都声明（内存后端同样能「启用 → 读」），
    // 而 **UDP 两侧都不许声明** —— Windows 没有 UDP 版的按连接扩展统计，
    // 内存后端在这里假装能做到的话，界面就会按「UDP 也能看字节数」来布局。
    QVERIFY2(backend.capabilities->isSupported(Capability::TrafficStatsTcp),
             qPrintable(backends.label + " 后端实现了 TCP 字节统计却没声明 TrafficStatsTcp"));
    QVERIFY2(!backend.capabilities->isSupported(Capability::TrafficStatsUdp),
             qPrintable(backends.label + " 后端声明了 UDP 字节统计，但按连接的 UDP eStats 不存在"));
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

/// 断连实现给出的答案必须与能力声明一致（S2.8）。
///
/// 这里刻意**不写死**「IPv6 断不了」这件事：那是本平台的现状，不是契约。
/// 契约是「能断就得声明、声明了就得能断」—— 这样将来某个平台真的能断 IPv6 时，
/// 这条断言会跟着它一起成立，而不用回来改测试。
void PlatformContractTest::killerAnswersMatchDeclaredCapabilities() {
  forEachBackend([](const Backends& backends) {
    IKiller& killer = *backends.first.killer;
    const ICapabilities& capabilities = *backends.first.capabilities;

    const struct {
      const char* label;
      ConnectionKey key;
      Capability capability;
    } cases[] = {
        {"IPv4 TCP",
         makeConnectionKey(TransportProtocol::Tcp, AddressFamily::V4),
         Capability::KillTcpV4},
        {"IPv6 TCP",
         makeConnectionKey(TransportProtocol::Tcp, AddressFamily::V6),
         Capability::KillTcpV6},
    };

    for (const auto& one : cases) {
      const Result<bool> canKill = killer.canKill(one.key);
      QVERIFY2(canKill.hasValue(),
               qPrintable(QStringLiteral("%1 后端无法判断 %2 能不能断：%3")
                              .arg(backends.label,
                                   QString::fromLatin1(one.label),
                                   canKill.errorOrNull() ? canKill.error().message : QString())));
      QVERIFY2(canKill.value() == capabilities.isSupported(one.capability),
               qPrintable(
                   QStringLiteral("%1 后端说 %2 的 canKill=%3，能力声明却是 %4 —— "
                                  "声明与实际必须一致")
                       .arg(backends.label, QString::fromLatin1(one.label))
                       .arg(canKill.value() ? QStringLiteral("true") : QStringLiteral("false"))
                       .arg(capabilities.isSupported(one.capability) ? QStringLiteral("支持")
                                                                     : QStringLiteral("不支持"))));

      // 声明不支持时，真去断必须明确报「不支持」并带上说明，不许静默成功。
      if (!canKill.value()) {
        const Result<void> attempted = killer.kill(one.key);
        QVERIFY2(!attempted.hasValue(),
                 qPrintable(QStringLiteral("%1 后端声称断不了 %2，kill 却报了成功")
                                .arg(backends.label, QString::fromLatin1(one.label))));
        QCOMPARE(attempted.error().code, ErrorCode::NotSupported);
        QVERIFY2(!attempted.error().message.trimmed().isEmpty(),
                 qPrintable(backends.label + " 后端对不支持的断连没给出说明"));
      }
    }
  });
}

/// 批量断连必须逐条交代，而且不允许对做不到的事静默成功。
void PlatformContractTest::killerWillNotSilentlySucceed() {
  forEachBackend([](const Backends& backends) {
    IKiller& killer = *backends.first.killer;

    // UDP 没有连接可拆 —— 这一条与平台无关，任何实现都必须给 false。
    const ConnectionKey udp = makeConnectionKey(TransportProtocol::Udp, AddressFamily::V4);
    const Result<bool> udpCanKill = killer.canKill(udp);
    QVERIFY(udpCanKill.hasValue());
    QVERIFY2(!udpCanKill.value(),
             qPrintable(backends.label + " 后端说 UDP 的「连接」可以断，UDP 没有连接可拆"));

    const Result<void> udpKill = killer.kill(udp);
    QVERIFY2(!udpKill.hasValue(), qPrintable(backends.label + " 后端对 UDP 的断连静默报了成功"));
    QCOMPARE(udpKill.error().code, ErrorCode::NotSupported);
    QVERIFY2(!udpKill.error().message.trimmed().isEmpty(), "不支持必须给出非空说明");

    // 批量：混一条断不了的进去，每一条都要有交代。
    // 「断了几条 + 几条失败 == 请求条数」是把「悄悄少断一条」堵死的那条断言。
    const ConnectionKey v4 = makeConnectionKey(TransportProtocol::Tcp, AddressFamily::V4);
    const Result<KillReport> batch = killer.killMany({v4, udp});
    QVERIFY2(batch.hasValue(),
             qPrintable(QStringLiteral("%1 后端批量断连整体失败：%2")
                            .arg(backends.label,
                                 batch.errorOrNull() ? batch.error().message : QString())));
    QCOMPARE(batch.value().requested, 2);
    QCOMPARE(batch.value().killed + static_cast<int>(batch.value().failures.size()), 2);

    bool udpAccountedFor = false;
    for (const KillFailure& failure : batch.value().failures) {
      QVERIFY2(!failure.reason.trimmed().isEmpty(),
               qPrintable(backends.label + " 后端给出了一条没有原因的断连失败"));
      if (failure.connection == udp) {
        udpAccountedFor = true;
      }
    }
    QVERIFY2(udpAccountedFor, qPrintable(backends.label + " 后端没把断不了的 UDP 报成失败"));

    // 空清单不该报错，也不该凭空多出条目。
    const Result<KillReport> empty = killer.killMany({});
    QVERIFY(empty.hasValue());
    QCOMPARE(empty.value().requested, 0);
    QCOMPARE(empty.value().killed, 0);
    QCOMPARE(empty.value().failures.size(), 0);
  });
}

/// 连接快照的形状必须满足接口约定：
///
/// - 地址文本不能为空、端口不能为 0；
/// - 两端地址族一致（不一致的根本不是一条连接）；
/// - **UDP 行的对端与方向必须是「未知」**（公开端点表里没有对端字段，已实测）；
/// - **进程身份不允许编**：解不出可执行文件路径时就留空，
///   而不是拿 PID 拼一个看起来像名字的东西 —— 那会让界面把错的程序名显示成事实。
///
/// 真实后端这边是对它真枚举出来的几百行做检查；内存后端那边先预置几行代表形状，
/// 免得断言在空表上白白通过。
void PlatformContractTest::connMonitorSnapshotIsWellFormed() {
  forEachBackend([](const Backends& backends) {
    if (backends.label == QStringLiteral("memory")) {
      // 代表形状：一条 IPv4 TCP、一条 IPv6 TCP、一条对端不可知的 UDP，
      // 以及一条**解不出进程信息**的行（未提权时真实后端上这种行是多数）。
      auto* monitor = static_cast<MemoryConnMonitor*>(backends.first.connMonitor.get());
      QList<ConnectionSnapshot> seeded;

      ConnectionSnapshot tcp;
      tcp.key.protocol = TransportProtocol::Tcp;
      tcp.key.local.address = Address{QStringLiteral("127.0.0.1"), AddressFamily::V4};
      tcp.key.local.port = 50000;
      tcp.key.remote.address = Address{QStringLiteral("223.5.5.5"), AddressFamily::V4};
      tcp.key.remote.port = 443;
      tcp.process.identity.imagePath = QStringLiteral("C:\\Windows\\notepad.exe");
      tcp.process.identity.displayName = QStringLiteral("notepad.exe");
      tcp.process.pid = 1234;
      tcp.direction = Direction::Out;
      tcp.observedAt = QDateTime::currentDateTime();
      seeded.append(tcp);

      ConnectionSnapshot tcp6 = tcp;
      tcp6.key.local.address = Address{QStringLiteral("::1"), AddressFamily::V6};
      tcp6.key.remote.address = Address{QStringLiteral("2001:db8::1"), AddressFamily::V6};
      tcp6.direction = Direction::In;
      seeded.append(tcp6);

      ConnectionSnapshot udp;
      udp.key.protocol = TransportProtocol::Udp;
      udp.key.local.address = Address{QStringLiteral("0.0.0.0"), AddressFamily::V4};
      udp.key.local.port = 5353;
      udp.key.remote.address = Address{QStringLiteral("0.0.0.0"), AddressFamily::V4};
      udp.key.remote.port = 0;
      udp.direction = Direction::Unknown;
      udp.process.pid = 4321;  // 刻意不给进程身份：解不出来时必须留空
      udp.observedAt = QDateTime::currentDateTime();
      seeded.append(udp);

      monitor->setSnapshot(seeded);
    }

    const Result<QList<ConnectionSnapshot>> snap = backends.first.connMonitor->snapshot();
    QVERIFY2(snap.hasValue(),
             qPrintable(
                 QStringLiteral("%1 后端取连接快照失败：%2")
                     .arg(backends.label, snap.errorOrNull() ? snap.error().message : QString())));

    for (const ConnectionSnapshot& one : snap.value()) {
      const QString where =
          QStringLiteral("%1 后端的快照行 [%2 %3:%4 → %5:%6]")
              .arg(backends.label,
                   one.key.protocol == TransportProtocol::Tcp ? QStringLiteral("TCP")
                                                              : QStringLiteral("UDP"))
              .arg(one.key.local.address.text)
              .arg(one.key.local.port)
              .arg(one.key.remote.address.text)
              .arg(one.key.remote.port);

      QVERIFY2(!one.key.local.address.text.trimmed().isEmpty(),
               qPrintable(where + " 的本地地址是空的"));
      QVERIFY2(one.key.local.port != 0, qPrintable(where + " 的本地端口是 0"));
      QVERIFY2(one.key.local.address.family == one.key.remote.address.family,
               qPrintable(where + " 的两端地址族不一致，这不可能是一条连接"));
      QVERIFY2(one.observedAt.isValid(), qPrintable(where + " 没有采样时刻"));

      if (one.key.protocol == TransportProtocol::Udp) {
        QVERIFY2(one.key.remote.port == 0,
                 qPrintable(where + " 是 UDP 行却带着非零对端端口 —— 公开数据源里没有这个字段，"
                                    "带出来的一定是别的东西"));
        QVERIFY2(one.direction == Direction::Unknown,
                 qPrintable(where + " 是 UDP 行却没把方向标成未知：对端都不知道，方向无从谈起"));
      } else {
        QVERIFY2(one.key.remote.port != 0, qPrintable(where + " 是 TCP 行但对端端口是 0"));
        QVERIFY2(
            one.direction != Direction::Unknown,
            qPrintable(where +
                       " 是 TCP 行却报了个未知方向：四元组与监听端口集都在，方向是判得出来的"));
      }

      // 进程身份不允许编：有路径就必须有显示名，没路径就不许有显示名。
      if (one.process.identity.imagePath.isEmpty()) {
        QVERIFY2(one.process.identity.displayName.isEmpty(),
                 qPrintable(where + " 解不出可执行文件路径，却给了显示名「" +
                            one.process.identity.displayName + "」—— 那是编的"));
      } else {
        QVERIFY2(!one.process.identity.displayName.isEmpty(),
                 qPrintable(where + " 有可执行文件路径却没有显示名"));
      }
    }
  });
}

/// 会枚举连接的后端，必须能找到**刚刚亲手建立的**那几条连接。
///
/// 这是 S3.1 唯一真正的行为断言：整机条数与系统工具对照属于一次性演练（未入库），
/// 而「我造的这条在不在、方向对不对」必须在单元测试里就成立。
void PlatformContractTest::connMonitorFindsOwnSocketsWhenItCanEnumerate() {
  forEachBackend([](const Backends& backends) {
    if (!backends.first.capabilities->isSupported(Capability::ProcessEnumeration)) {
      qInfo().noquote() << QStringLiteral(
                               "后端 %1：不声明 ProcessEnumeration，"
                               "本次跳过行为核对（它只回放预置内容）")
                               .arg(backends.label);
      return;
    }

    // 造一对可控的 TCP 连接：一个在 127.0.0.1 上监听，一个主动连过去。
    // 同一对四元组因此会有两条记录、方向相反 —— 一条必须判入站、一条必须判出站。
    QTcpServer server;
    QVERIFY2(server.listen(QHostAddress::LocalHost, 0),
             qPrintable(QStringLiteral("监听 127.0.0.1 失败：%1").arg(server.errorString())));
    const quint16 listenPort = server.serverPort();
    QVERIFY(listenPort != 0);

    QTcpSocket client;
    client.connectToHost(QHostAddress::LocalHost, listenPort);
    QVERIFY2(client.waitForConnected(5000),
             qPrintable(QStringLiteral("本机 loopback 连接建不起来，测试前提不成立：%1")
                            .arg(client.errorString())));
    QVERIFY2(server.waitForNewConnection(5000), "监听套接字没等到连接，测试前提不成立");
    QVERIFY(server.nextPendingConnection() != nullptr);
    const quint16 clientPort = client.localPort();
    QVERIFY(clientPort != 0);

    // 只绑定的 UDP 端点，两个地址族各一个。
    QUdpSocket udp4;
    QVERIFY2(udp4.bind(QHostAddress::LocalHost, 0),
             qPrintable(QStringLiteral("绑定 UDP IPv4 失败：%1").arg(udp4.errorString())));
    const quint16 udp4Port = udp4.localPort();
    QUdpSocket udp6;
    const bool udp6Bound = udp6.bind(QHostAddress::LocalHostIPv6, 0);

    const Result<QList<ConnectionSnapshot>> snap = backends.first.connMonitor->snapshot();
    QVERIFY2(snap.hasValue(),
             qPrintable(
                 QStringLiteral("%1 后端取连接快照失败：%2")
                     .arg(backends.label, snap.errorOrNull() ? snap.error().message : QString())));

    bool foundOutbound = false;
    bool foundInbound = false;
    bool foundUdp4 = false;
    bool foundUdp6 = false;
    for (const ConnectionSnapshot& one : snap.value()) {
      const ConnectionKey& key = one.key;
      if (key.protocol == TransportProtocol::Tcp) {
        if (key.local.port == clientPort && key.remote.port == listenPort) {
          foundOutbound = true;
          QVERIFY2(one.direction == Direction::Out,
                   "主动连出去的那条被判成了入站：它的本地端口是临时口、不在监听端口集里");
        }
        if (key.local.port == listenPort && key.remote.port == clientPort) {
          foundInbound = true;
          QVERIFY2(one.direction == Direction::In,
                   "被接受的那条被判成了出站：它的本地端口就是监听端口，"
                   "判成出站多半是按地址而不是按端口比的（监听常绑在 0.0.0.0）");
        }
      } else {
        if (key.local.port == udp4Port) {
          foundUdp4 = true;
          QVERIFY2(key.remote.port == 0 && one.direction == Direction::Unknown,
                   "刚绑定的 UDP 端点不该带对端或方向");
        }
        if (udp6Bound && key.local.port == udp6.localPort() &&
            key.local.address.family == AddressFamily::V6) {
          foundUdp6 = true;
          QVERIFY2(key.remote.port == 0 && one.direction == Direction::Unknown,
                   "刚绑定的 IPv6 UDP 端点不该带对端或方向");
        }
      }
    }

    QVERIFY2(
        foundOutbound,
        qPrintable(QStringLiteral("%1 后端的快照里没有刚刚建立的出站连接").arg(backends.label)));
    QVERIFY2(foundInbound,
             qPrintable(QStringLiteral("%1 后端的快照里没有被接受的入站连接").arg(backends.label)));
    QVERIFY2(
        foundUdp4,
        qPrintable(QStringLiteral("%1 后端的快照里没有刚绑定的 UDP 端点").arg(backends.label)));
    if (udp6Bound) {
      QVERIFY2(
          foundUdp6,
          qPrintable(
              QStringLiteral("%1 后端的快照里没有刚绑定的 IPv6 UDP 端点").arg(backends.label)));
    }
  });
}

/// 事件订阅：没声明能力就必须**明确报不支持**，声明了就必须真的订上。
///
/// 这条守的是「静默降级」：返回一个永远不触发的事件句柄，界面会安安静静地
/// 等一个不会来的事件，而「已退化成轮询」以及它带来的「短连接会漏」就不会被标注出来。
///
/// 真实后端声明了这个能力，而它的数据源（ETW 实时会话）**需要提权**，
/// 所以未提权时唯一允许的失败是 `NotPermitted` 加非空说明；
/// 「声明了却报 NotSupported」属于自相矛盾，必须失败。
void PlatformContractTest::connMonitorEventsMatchDeclaredCapabilities() {
  forEachBackend([](const Backends& backends) {
    const bool declared =
        backends.first.capabilities->isSupported(Capability::EventDrivenConnections);
    const Result<bool> elevatedResult = backends.first.privilege->isElevated();
    QVERIFY2(elevatedResult.hasValue(),
             qPrintable(QStringLiteral("%1 后端答不出自己是不是提权，订阅契约就无从判断")
                            .arg(backends.label)));
    const bool elevated = elevatedResult.value();

    const Result<SubscriptionId> subscription =
        backends.first.connMonitor->subscribe([](const ConnectionEvent&) {});

    if (!declared) {
      QVERIFY2(
          !subscription.hasValue(),
          qPrintable(backends.label + " 后端没声明 EventDrivenConnections，subscribe 却成功了 —— "
                                      "上层会去等一个永远不会来的事件"));
      QCOMPARE(subscription.error().code, ErrorCode::NotSupported);
      QVERIFY2(!subscription.error().message.trimmed().isEmpty(),
               "报不支持时必须给出非空说明，否则界面上只剩一个没有任何解释的灰按钮");
    } else if (subscription.hasValue()) {
      QVERIFY2(subscription.value() != kInvalidSubscription,
               qPrintable(QStringLiteral("%1 后端发了一个无效的订阅句柄（0）")
                              .arg(backends.label)));
      QVERIFY2(backends.first.connMonitor->unsubscribe(subscription.value()).hasValue(),
               "声明了事件驱动却退不掉，平台侧订阅会泄漏");
    } else {
      QVERIFY2(!elevated,
               qPrintable(QStringLiteral("%1 后端在已提权状态下仍然订不上连接事件：%2")
                              .arg(backends.label, subscription.error().message)));
      // ⚠️ 未提权时这条会失败，而失败信息里必须带上原生码：ETW 的失败原因
      // （权限不足 / 会话名冲突 / 提供程序订不上）给的都是不同的 Win32 码，
      // 只报「不是 NotPermitted」等于没说。
      QVERIFY2(subscription.error().code == ErrorCode::NotPermitted,
               qPrintable(QStringLiteral("%1 后端未提权却报的不是「权限不足」：%2（原生码 %3）")
                              .arg(backends.label, subscription.error().message)
                              .arg(subscription.error().nativeCode)));
      QVERIFY2(!subscription.error().message.trimmed().isEmpty(),
               "报权限不足时必须说清怎么办（需要管理员还是需要哪个组）");
    }

    // 退订必须幂等且对未知句柄成功：退出路径会无条件调一次收尾。
    QVERIFY(backends.first.connMonitor->unsubscribe(kInvalidSubscription).hasValue());
    QVERIFY(backends.first.connMonitor->unsubscribe(0x5EED1234).hasValue());
  });
}

/// 「订上了就真收得到」：用内存后端验到底。
///
/// 真实后端的事件来自 ETW，既要提权又依赖机器上真有流量，做不成无条件可重复的断言；
/// 而这条契约本身（订了就能收到、退订就不再收到）必须由**受控输入**来验。
/// 真实后端的行为证据在阶段三的演练里（真建一条连接看有没有事件）。
void PlatformContractTest::connMonitorDeliversEmittedEvents() {
  MemoryConnMonitor monitor;
  QList<ConnectionEvent> first;
  QList<ConnectionEvent> second;

  const Result<SubscriptionId> firstId =
      monitor.subscribe([&first](const ConnectionEvent& event) { first.append(event); });
  QVERIFY2(firstId.hasValue(), qPrintable(firstId.errorOrNull() ? firstId.error().message
                                                               : QString()));
  const Result<SubscriptionId> secondId =
      monitor.subscribe([&second](const ConnectionEvent& event) { second.append(event); });
  QVERIFY2(secondId.hasValue(), "同一个监视器要能同时挂多个订阅者，两个 id 不能撞车");
  QVERIFY(firstId.value() != secondId.value());

  ConnectionEvent appeared;
  appeared.kind = ConnectionEventKind::Appeared;
  appeared.snapshot.key = makeConnectionKey(TransportProtocol::Tcp, AddressFamily::V4);
  appeared.snapshot.direction = Direction::Out;
  monitor.emitEvent(appeared);

  QCOMPARE(first.size(), 1);
  QCOMPARE(second.size(), 1);
  QCOMPARE(first.first().kind, ConnectionEventKind::Appeared);
  QVERIFY(first.first().snapshot.key == appeared.snapshot.key);

  // 退订之后**不再收到**：收尾路径靠这条保证「不会往已经销毁的对象上回调」。
  QVERIFY(monitor.unsubscribe(firstId.value()).hasValue());
  ConnectionEvent disappeared;
  disappeared.kind = ConnectionEventKind::Disappeared;
  disappeared.snapshot.key = appeared.snapshot.key;
  monitor.emitEvent(disappeared);
  QCOMPARE(first.size(), 1);
  QCOMPARE(second.size(), 2);
  QCOMPARE(second.last().kind, ConnectionEventKind::Disappeared);

  QVERIFY(monitor.unsubscribe(secondId.value()).hasValue());
  monitor.emitEvent(appeared);
  QCOMPARE(second.size(), 2);

  // 空回调要当场拒掉，不能给一个「成功但永远不会有人收到」的订阅。
  const Result<SubscriptionId> empty = monitor.subscribe(ConnectionEventSink());
  QVERIFY2(!empty.hasValue(), "空回调的订阅被接受了：界面会以为在等事件，实际永远等不到");
  QCOMPARE(empty.error().code, ErrorCode::InvalidArgument);
}

/// 字节统计的契约：**没启用采集就绝不许给出值**。
///
/// 真实实现里这条不是防御性代码，而是硬要求：实测
/// `GetPerTcpConnectionEStats` 在未启用采集时会**返回成功并给出未初始化内存里的垃圾值**。
/// 实现不自己记账的话，界面就会把垃圾当流量显示出去，而且永远不会报错。
void PlatformContractTest::trafficStatsRefusesReadBeforeCollection() {
  forEachBackend([](const Backends& backends) {
    ITrafficStats& stats = *backends.first.trafficStats;
    const ConnectionKey ghost = makeConnectionKey(TransportProtocol::Tcp, AddressFamily::V4);

    const Result<ConnectionCounters> cold = stats.read(ghost);
    QVERIFY2(!cold.hasValue(),
             qPrintable(backends.label + " 后端在没启用采集时就读出了值："
                                         "真实实现的这条路径拿到的是垃圾值，不能信"));
    QVERIFY2(!cold.errorOrNull()->message.trimmed().isEmpty(),
             "报「还不能读」时必须说清要先 enableCollection");

    // 对一条根本不存在的连接启用：提权后的真实后端报「找不到」，
    // 未提权时连第一关都过不去（`SetPerTcpConnectionEStats` 返回拒绝访问）。
    const Result<bool> elevatedResult = backends.first.privilege->isElevated();
    QVERIFY(elevatedResult.hasValue());
    const Result<void> enabled = stats.enableCollection(ghost);
    QVERIFY2(!enabled.hasValue(), "对一条不存在的连接启用采集居然成功了");
    if (elevatedResult.value()) {
      QCOMPARE(enabled.error().code, ErrorCode::NotFound);
    } else {
      QVERIFY2(enabled.error().code == ErrorCode::NotPermitted ||
                   enabled.error().code == ErrorCode::NotFound,
               qPrintable(QStringLiteral("%1 后端未提权时的失败分类不对：%2")
                              .arg(backends.label, enabled.error().message)));
    }
    QVERIFY(!enabled.error().message.trimmed().isEmpty());

    // UDP 一律明确不支持：按连接的 UDP 扩展统计在 Windows 上不存在。
    const Result<void> udp =
        stats.enableCollection(makeConnectionKey(TransportProtocol::Udp, AddressFamily::V4));
    QVERIFY2(!udp.hasValue(), "UDP 的按连接统计被接受了，但那个接口根本不存在");
    QCOMPARE(udp.error().code, ErrorCode::NotSupported);
    QVERIFY(!udp.error().message.trimmed().isEmpty());

    // 停止采集是幂等的：退出路径会无条件调一次收尾。
    QVERIFY(stats.disableCollection(ghost).hasValue());
    QVERIFY(stats.disableCollection(makeConnectionKey(TransportProtocol::Tcp, AddressFamily::V6))
                .hasValue());

    // 批量读**不整体失败**：连接一直在建立与消失，「快照里有、读的时候没了」是常态。
    // 拿不到的那一条用值类型自己的语义表示 —— 空 `since` = 该连接拿不到计数。
    const QList<ConnectionKey> batch = {ghost,
                                        makeConnectionKey(TransportProtocol::Tcp, AddressFamily::V6)};
    const Result<QList<ConnectionCounters>> many = stats.readMany(batch);
    QVERIFY2(many.hasValue(),
             qPrintable(QStringLiteral("%1 后端的批量读整批失败了：%2")
                            .arg(backends.label,
                                 many.errorOrNull() ? many.error().message : QString())));
    QCOMPARE(many.value().size(), batch.size());
    for (const ConnectionCounters& one : many.value()) {
      QVERIFY2(one.since.isNull(),
               "拿不到计数的条目必须留空 since（无有效起点），不能拿一个 0 冒充流量");
    }
  });
}

/// 「预置了就真读得到、启用了才读得到」：用内存后端验到底。
///
/// 真实后端要提权、还依赖机器上真有一条连接，做不成无条件可重复的断言；
/// 而这几条语义（启用前拒读、重复启用不重置起点、连接消失后的两种表现）
/// 必须由**受控输入**来验。真实后端的行为证据在阶段三的演练里。
void PlatformContractTest::memoryTrafficStatsDeliversProgrammedCounters() {
  MemoryTrafficStats stats;
  const ConnectionKey key = makeConnectionKey(TransportProtocol::Tcp, AddressFamily::V4);

  // 没预置就启用 → 找不到：内存后端不凭一个四元组凭空造出一条连接。
  const Result<void> tooEarly = stats.enableCollection(key);
  QVERIFY2(!tooEarly.hasValue(), "内存后端对没预置的连接启用成功了");
  QCOMPARE(tooEarly.error().code, ErrorCode::NotFound);

  ConnectionCounters programmed;
  programmed.bytesOut = 123456;
  programmed.bytesIn = 654321;
  programmed.segmentsOut = 100;
  programmed.segmentsIn = 200;
  stats.setCounters(key, programmed);

  QVERIFY(stats.enableCollection(key).hasValue());
  QVERIFY2(stats.enableCollection(key).hasValue(), "对已启用的连接重复启用必须安全");

  const Result<ConnectionCounters> first = stats.read(key);
  QVERIFY2(first.hasValue(),
           qPrintable(first.errorOrNull() ? first.error().message : QString()));
  QCOMPARE(first.value().bytesOut, std::uint64_t(123456));
  QCOMPARE(first.value().bytesIn, std::uint64_t(654321));
  QCOMPARE(first.value().segmentsOut, std::uint64_t(100));
  QCOMPARE(first.value().segmentsIn, std::uint64_t(200));
  QVERIFY2(!first.value().since.isNull(),
           "since 必须填上：界面靠它说明计数从哪一刻起有效");

  // 重复启用**不能把起点往后挪**，否则那个下限会每次采样都往后漂。
  const QDateTime since = first.value().since;
  QVERIFY(stats.enableCollection(key).hasValue());
  const Result<ConnectionCounters> second = stats.read(key);
  QVERIFY(second.hasValue());
  QCOMPARE(second.value().since, since);

  // 连接消失：单条读要明确报「找不到」，批量读则用空 since 表示。
  stats.forget(key);
  const Result<ConnectionCounters> gone = stats.read(key);
  QVERIFY2(!gone.hasValue(), "连接已经被 forget，单条读却给了值");
  QCOMPARE(gone.error().code, ErrorCode::NotFound);
  const Result<QList<ConnectionCounters>> batch = stats.readMany(QList<ConnectionKey>{key});
  QVERIFY(batch.hasValue());
  QCOMPARE(batch.value().size(), 1);
  QVERIFY(batch.value().first().since.isNull());

  // 停止采集之后**又回到「不许读」**：不然就又变成「没启用却给值」。
  stats.setCounters(key, programmed);
  QVERIFY(stats.disableCollection(key).hasValue());
  QCOMPARE(stats.trackedCount(), 0);
  const Result<ConnectionCounters> afterDisable = stats.read(key);
  QVERIFY2(!afterDisable.hasValue(), "停掉采集之后还读得出值");
  QCOMPARE(afterDisable.error().code, ErrorCode::InvalidArgument);
}

QTEST_GUILESS_MAIN(PlatformContractTest)

#include "platform_contract_test.moc"
