#include <QDebug>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QString>

#include <cstdlib>

#include "core/log.h"
#include "core/platform_backend.h"
#include "core/version.h"

using namespace baniphelper::core;

namespace {

int failWith(const QString& message) {
  qWarning().noquote() << message;
  return EXIT_FAILURE;
}

}  // namespace

int main(int argc, char* argv[]) {
  QGuiApplication app(argc, argv);

  // 应用标识：配置目录与数据库目录都以它为准，必须在读取路径之前设好。
  app.setOrganizationName(QStringLiteral("BanIPHelper"));
  app.setApplicationName(QStringLiteral("BanIPHelper"));
  app.setApplicationVersion(QString::fromLatin1(versionString()));

  // 装配平台后端。装配失败必须显式退出：带着一个残缺的后端继续跑，
  // 后面每一步都会以更难懂的方式失败。
  Result<PlatformBackend> backend = createPlatformBackend();
  if (!backend) {
    return failWith(QStringLiteral("平台后端装配失败：%1").arg(backend.error().message));
  }
  if (!backend.value().isComplete()) {
    return failWith(QStringLiteral("平台后端装配不完整，缺少必需的接口实现"));
  }

  // 日志要在任何有意义的操作之前起来。程序是 GUI 子系统、没有控制台，
  // 自己的记录与 Qt 内部消息除了文件没有第二个去处；晚一步起来，
  // 前面发生的事就永久丢了 —— 而启动阶段恰好是最容易出问题的一段。
  const Result<Paths> paths = backend.value().paths->resolve();
  if (!paths) {
    return failWith(QStringLiteral("解析应用目录失败：%1").arg(paths.error().message));
  }
  const Result<void> directories = backend.value().paths->ensureDirectories();
  if (!directories) {
    return failWith(QStringLiteral("准备应用目录失败：%1").arg(directories.error().message));
  }

  LogOptions logOptions;
  logOptions.directory = paths.value().logDirectory;
  const Result<void> logging = openLogging(logOptions);
  if (!logging) {
    // 走到这里说明路径或权限有问题。继续跑就会变成「什么都记不下来却毫无提示」。
    return failWith(QStringLiteral("打开日志失败：%1").arg(logging.error().message));
  }
  installQtMessageHandler();

  logWrite(LogLevel::Info,
           QStringLiteral("BanIPHelper %1 启动，平台后端 %2")
               .arg(QString::fromLatin1(versionString()), backend.value().name));
  logWrite(LogLevel::Info, QStringLiteral("日志目录：%1").arg(paths.value().logDirectory));

  // 单实例。重复启动会各自下发一套过滤器，撤销时又互相不知道对方下过什么，
  // 最终在内核里留下一堆没人认领的过滤器。
  const Result<bool> acquired = backend.value().singleInstance->acquire();
  if (!acquired) {
    return failWith(QStringLiteral("取单实例所有权失败：%1").arg(acquired.error().message));
  }
  if (!acquired.value()) {
    const Result<void> signaled = backend.value().singleInstance->signalExisting();
    if (!signaled) {
      // 既唤不起既有实例，也不能再开一个。把原因说清楚再退出，
      // 否则用户看到的只是「双击了但什么都没发生」。
      qWarning().noquote()
          << QStringLiteral("已有实例在运行，但唤起它失败：%1").arg(signaled.error().message);
      return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
  }

  // 提权状态只查询、只报告，不在这里阻断启动：
  // 界面要能起来，用户才看得到「为什么某些功能不可用」。
  // 正常路径下提权由清单保证，走不到下面那个分支。
  const Result<bool> elevated = backend.value().privilege->isElevated();
  if (!elevated) {
    qWarning().noquote() << QStringLiteral("无法判断提权状态：%1").arg(elevated.error().message);
  } else if (!elevated.value()) {
    qWarning().noquote() << QStringLiteral("警告：当前未提权。%1")
                                .arg(backend.value().privilege->elevationRequirementText());
  }

  // 启动清理：收掉上次运行残留的自有过滤器。
  //
  // 过滤器在进程退出后仍然活在内核里，而崩溃与强杀都不会走退出路径，
  // 因此「启动时先清一遍」是唯一能保证不残留的地方。
  // 清理按自有 provider 限定，他方过滤器在结构上就不可能被误删。
  //
  // 失败只报告不阻断启动：带着残留过滤器启动的后果是旧规则仍在生效，
  // 比打不开界面轻，而且用户需要看到界面才知道发生了什么。
  const Result<void> engineOpened = backend.value().filterEngine->open();
  if (!engineOpened) {
    qWarning().noquote()
        << QStringLiteral("打开过滤器引擎失败：%1").arg(engineOpened.error().message);
  } else {
    const Result<CleanupReport> cleanup = backend.value().filterEngine->cleanupOrphans();
    if (!cleanup) {
      qWarning().noquote() << QStringLiteral("清理残留过滤器失败：%1").arg(cleanup.error().message);
    } else if (cleanup.value().removedOwn > 0) {
      qInfo().noquote()
          << QStringLiteral("已清掉 %1 条上次运行残留的过滤器").arg(cleanup.value().removedOwn);
    }
  }

  QQmlApplicationEngine engine;

  // QML 加载失败必须是显式失败，不允许留下一个「看起来启动了」的空进程。
  QObject::connect(
      &engine,
      &QQmlApplicationEngine::objectCreationFailed,
      &app,
      []() { QCoreApplication::exit(EXIT_FAILURE); },
      Qt::QueuedConnection);

  engine.loadFromModule("BanIPHelper", "Main");

  return app.exec();
}
