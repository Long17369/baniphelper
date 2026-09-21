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
  void memoryPrivilegeCoversUnelevatedBranch();
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

    // 实现了某个接口，就必须把对应的能力位声明为支持。
    // 声明与实际不一致会让界面上出现「灰按钮但其实是能用的」，或反过来 ——
    // 后者更糟：用户以为能用，点下去才发现不行。
    QVERIFY2(
        backend.capabilities->isSupported(Capability::SingleInstance),
        qPrintable(backends.label + " 后端实现了 ISingleInstance，却没声明 SingleInstance 能力"));
    QVERIFY2(backend.capabilities->isSupported(Capability::Elevation),
             qPrintable(backends.label + " 后端实现了 IPrivilege，却没声明 Elevation 能力"));
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
    // 实例名是刚生成的，没有任何人持有它。
    const Result<void> signaled = backends.first.singleInstance->signalExisting();

    QVERIFY2(!signaled.hasValue(),
             qPrintable(backends.label +
                        " 后端在没有既有实例时报了成功 —— 调用方会以为唤起了界面然后自己退出，"
                        "结果是新旧两个窗口都不见了"));
    QVERIFY(signaled.error().code == ErrorCode::NotFound);
    QVERIFY2(!signaled.error().message.trimmed().isEmpty(), "失败必须带非空说明");
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

QTEST_GUILESS_MAIN(PlatformContractTest)

#include "platform_contract_test.moc"
