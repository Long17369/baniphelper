#include <QDebug>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QString>

#include <cstdlib>

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
