#include <QApplication>
#include <QDateTime>
#include <QDebug>
#include <QIcon>
#include <QMessageBox>
#include <QQmlApplicationEngine>
#include <QString>
#include <QTimer>
#include <QWindow>

#include <cstdlib>

#include "core/config_descriptors.h"
#include "core/database.h"
#include "core/json_config.h"
#include "core/log.h"
#include "core/platform_backend.h"
#include "core/rule_runtime.h"
#include "core/rule_store_sqlite.h"
#include "core/version.h"
#include "ui/app_shell.h"

using namespace baniphelper::core;
using baniphelper::ui::AppShell;

namespace {

int failWith(const QString& message) {
  qWarning().noquote() << message;
  return EXIT_FAILURE;
}

}  // namespace

int main(int argc, char* argv[]) {
  // 用 QApplication 而不是 QGuiApplication：托盘是 Widgets 里的 QSystemTrayIcon。
  //
  // 界面主体仍然是 Qt Quick（architecture.md 第 2.2 节）。托盘没有走「Qt.labs.platform 的
  // QML 版 SystemTrayIcon」，而是用 Widgets 里那个：labs 模块官方声明 API 可能随时变，
  // 而托盘是用户唯一的退出入口，不适合建在一个随时会改的实验性模块上。
  QApplication app(argc, argv);

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

  // 单实例判定要排在日志之前。
  //
  // 抢不到所有权的那次启动不是「第二个实例」，它是一个**唤醒器**：职责只有一句
  // 「请既有实例把界面拿出来」，然后退出。让它先开日志，就成了两个进程往同一份
  // 日志里写 —— 而 QFile 的追加在 Windows 上并不原子，实测把一整行日志写成了半行
  // （两个进程各自记住的写入位置撞在了一起）。日志是本程序唯一的现场，
  // 不能自己有概率丢行。
  //
  // 代价：这一段失败只能用 stderr 报（日志还没起来）。这是刻意的取舍 ——
  // 一个还没被判定为「本实例」的进程，本来也不该往主人的日志里写东西。
  //
  // 单实例的必要性：重复启动会各自下发一套过滤器，撤销时又互相不知道对方下过什么，
  // 最终在内核里留下一堆没人认领的过滤器。
  const Result<bool> acquired = backend.value().singleInstance->acquire();
  if (!acquired) {
    return failWith(QStringLiteral("取单实例所有权失败：%1").arg(acquired.error().message));
  }
  if (!acquired.value()) {
    const Result<void> signaled = backend.value().singleInstance->signalExisting();
    if (!signaled) {
      // 既唤不起既有实例，也不能再开一个。
      //
      // 这里必须让用户看得见：程序是 GUI 子系统、没有控制台，上面那句 qWarning
      // 在双击启动时无处可去。只留下「双击了但什么都没发生」正是这个功能要消灭的现象。
      const QString message = QStringLiteral(
                                  "已有实例在运行，但唤不起它的界面：%1\n\n"
                                  "请在系统托盘里找它的图标；找不到就先结束那个进程再启动。")
                                  .arg(signaled.error().message);
      qWarning().noquote() << message;
      QMessageBox::warning(nullptr, QStringLiteral("BanIPHelper"), message);
      return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
  }

  // 日志要尽早起来，唯一的例外是上面那次单实例判定（理由见上）。
  // 程序是 GUI 子系统、没有控制台，自己的记录与 Qt 内部消息除了文件没有第二个去处；
  // 晚一步起来，前面发生的事就永久丢了 —— 而启动阶段恰好是最容易出问题的一段。
  const Result<Paths> paths = backend.value().paths->resolve();
  if (!paths) {
    return failWith(QStringLiteral("解析应用目录失败：%1").arg(paths.error().message));
  }
  const Result<void> directories = backend.value().paths->ensureDirectories();
  if (!directories) {
    return failWith(QStringLiteral("准备应用目录失败：%1").arg(directories.error().message));
  }

  // 先读配置，再开日志：日志自己的可调项（级别、滚动阈值、保留天数、是否压缩）就住在配置里。
  // 读配置过程中发生的自愈动作不能当场写日志（那会儿还没有日志），所以先收集，等日志起来再补写。
  JsonConfig config;
  const Result<void> configured = config.open(paths.value().configFile, defaultConfigDescriptors());
  if (!configured) {
    return failWith(QStringLiteral("打开配置失败：%1").arg(configured.error().message));
  }

  QStringList logProblems;
  LogOptions logOptions;
  logOptions.directory = paths.value().logDirectory;
  {
    const Result<QJsonValue> level = config.value(QString::fromLatin1(kConfigKeyLogLevel));
    if (level) {
      const Result<LogLevel> parsed = parseLogLevel(level.value().toString());
      if (parsed) {
        logOptions.level = parsed.value();
      } else {
        logProblems.append(parsed.error().message);
      }
    } else {
      logProblems.append(level.error().message);
    }

    const Result<QJsonValue> rotate = config.value(QString::fromLatin1(kConfigKeyLogRotateBytes));
    if (rotate) {
      logOptions.rotateBytes = static_cast<qint64>(rotate.value().toDouble());
    } else {
      logProblems.append(rotate.error().message);
    }

    const Result<QJsonValue> retention =
        config.value(QString::fromLatin1(kConfigKeyLogRetentionDays));
    if (retention) {
      logOptions.retentionDays = static_cast<int>(retention.value().toDouble());
    } else {
      logProblems.append(retention.error().message);
    }

    const Result<QJsonValue> compress =
        config.value(QString::fromLatin1(kConfigKeyLogCompressRotated));
    if (compress) {
      logOptions.compressRotated = compress.value().toBool();
    } else {
      logProblems.append(compress.error().message);
    }
  }

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

  // 配置层的自愈动作与读失败都要写出来。不写的话，用户只会看到
  // 「设置怎么变回去了」或「日志级别改了没反应」，而找不到原因。
  for (const QString& note : config.recoveryNotes()) {
    logWrite(LogLevel::Warn, note);
  }
  for (const QString& problem : logProblems) {
    logWrite(LogLevel::Warn, QStringLiteral("配置里的日志项不可用，已退回默认值：%1").arg(problem));
  }

  // 数据存储。建库与迁移都在 open 里完成。
  // 失败必须显式退出：带着一个打不开的库继续跑，后面会以「规则存不上」这种更难懂的方式失败。
  Database database;
  const Result<void> storage = database.open(paths.value().databaseFile);
  if (!storage) {
    logWrite(LogLevel::Error, QStringLiteral("打开数据库失败：%1").arg(storage.error().message));
    return failWith(QStringLiteral("打开数据库失败：%1").arg(storage.error().message));
  }
  {
    const Result<int> version = database.schemaVersion();
    logWrite(LogLevel::Info,
             QStringLiteral("数据库就绪：%1（结构版本 %2）")
                 .arg(paths.value().databaseFile,
                      version ? QString::number(version.value()) : QStringLiteral("未知")));
  }

  // 日志级别是唯一标成「不必重启」的配置项，改动必须当场生效 ——
  // 否则为了抓一次现场得先关程序、改配置、再启动，现场早没了。
  const Result<SubscriptionId> subscribed = config.subscribe([&config](const ConfigChange& change) {
    const QString levelKey = QString::fromLatin1(kConfigKeyLogLevel);
    const bool levelTouched = change.changedKeys.isEmpty() || change.changedKeys.contains(levelKey);
    if (levelTouched) {
      const Result<QJsonValue> level = config.value(levelKey);
      if (level) {
        const Result<LogLevel> parsed = parseLogLevel(level.value().toString());
        if (parsed) {
          setLoggingLevel(parsed.value());
          logWrite(LogLevel::Info,
                   QStringLiteral("日志级别已切到 %1").arg(logLevelName(parsed.value())));
        }
      }
    }

    // 需要重启才生效的项要说清楚，否则用户会以为「改了没反应」是坏了。
    const QList<ConfigDescriptor> table = config.descriptors();
    for (const QString& key : change.changedKeys) {
      for (const ConfigDescriptor& descriptor : table) {
        if (descriptor.key == key && descriptor.requiresRestart) {
          logWrite(LogLevel::Warn, QStringLiteral("配置项 %1 已保存，需重启后生效").arg(key));
        }
      }
    }
  });
  if (!subscribed) {
    logWrite(LogLevel::Warn,
             QStringLiteral("订阅配置变更失败，运行期间改配置不会立即生效：%1")
                 .arg(subscribed.error().message));
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

  // 规则仓储与它的生命周期编排（S2.9）。
  //
  // 声明在这里是因为下面两段都要用它：一段把库里的规则装回引擎，
  // 另一段是运行期间的过期巡检。两者都是**同一个对象**，顺序才不会分叉。
  SqliteRuleStore ruleStore(database);
  RuleRuntime ruleRuntime(ruleStore, *backend.value().filterEngine);

  // 启动清理：收掉上次运行残留的自有过滤器。
  //
  // 过滤器在进程退出后仍然活在内核里，而崩溃与强杀都不会走退出路径，
  // 因此「启动时先清一遍」是唯一能保证不残留的地方。
  // 清理按自有 provider 限定，他方过滤器在结构上就不可能被误删。
  //
  // ⚠️ **清理必须排在「按库重建」之前。** 反过来的话，刚下发的规则会被当成
  // 「上次运行残留的过滤器」收掉 —— 规则永远不生效，而且清理还报成功。
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

    // 按库重建规则。到这一步为止引擎已经干净，所以下发上去的都是本次运行的东西。
    //
    // 报告的四类数字加起来就是库里的规则总数：单条失败**不阻断**其余规则生效，
    // 但每一条都要在日志里有交代 —— 「静默地少下发一条」是这一步最严重的失败方式。
    const Result<RestoreReport> restored = ruleRuntime.restore();
    if (!restored) {
      logWrite(LogLevel::Warn,
               QStringLiteral("读取已保存的规则失败，本次启动没有规则生效：%1")
                   .arg(restored.error().message));
    } else {
      logWrite(LogLevel::Info,
               QStringLiteral("规则已恢复：生效 %1 条，已过期未下发 %2 条，已停用 %3 条")
                   .arg(restored.value().applied)
                   .arg(restored.value().alreadyExpired)
                   .arg(restored.value().disabled));
      for (const RuleProblem& problem : restored.value().failures) {
        logWrite(LogLevel::Warn,
                 QStringLiteral("规则「%1」未能生效：%2").arg(problem.id, problem.reason));
      }
    }

    // 过期巡检：启动时先跑一趟（把「上次退出之后才到期」的规则收掉），之后每分钟一趟。
    //
    // 没有它的话，带过期时间的规则会在到期后继续生效到下次重启 ——
    // 而「到期」正是用户用来给自己兜底的手段，失效不准时等于兜底失效。
    auto expiring = new QTimer(&app);
    QObject::connect(expiring, &QTimer::timeout, &app, [&ruleRuntime]() {
      const Result<ExpireReport> expired = ruleRuntime.expireDue(QDateTime::currentDateTimeUtc());
      if (!expired) {
        logWrite(LogLevel::Warn,
                 QStringLiteral("检查已到期规则失败：%1").arg(expired.error().message));
        return;
      }
      for (const RuleId& id : expired.value().disabled) {
        logWrite(LogLevel::Info, QStringLiteral("规则「%1」已到期，已撤销并停用").arg(id));
      }
      for (const RuleProblem& problem : expired.value().failures) {
        logWrite(LogLevel::Warn,
                 QStringLiteral("规则「%1」的过期处理未完成：%2").arg(problem.id, problem.reason));
      }
    });
    expiring->start(60 * 1000);
  }

  // 托盘外壳。
  //
  // 先建外壳再加载界面：外壳要接收「既有实例被唤起」的通知，
  // 而那个通知随时可能来，包括界面还没出来的那几毫秒。
  AppShell shell(backend.value(), config);
  const bool trayReady = shell.start();

  // 接收端：二次启动的实例会往命名管道里发一句「show」。
  // 这一步把「双击了但什么都没有发生」变成「既有窗口被拿到前面」，
  // 也是 S1.3 留下的一处半成品（那时只有「发现已有实例」，没有接收端）。
  //
  // 建不起来只警告不阻断：程序本身仍然能用，只是二次启动唤不起它。
  const Result<void> listening =
      backend.value().singleInstance->listenForActivation([&shell]() { shell.showWindow(); });
  if (!listening) {
    logWrite(LogLevel::Warn,
             QStringLiteral("建立唤起接收端失败，二次启动将唤不起本实例：%1")
                 .arg(listening.error().message));
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

  // 根对象是 QML 的 Window，也就是一个 QWindow。
  // 拿不到就说明界面结构变了，托盘上的「显示主界面」会失效，因此要留痕。
  QWindow* mainWindow = nullptr;
  if (!engine.rootObjects().isEmpty()) {
    mainWindow = qobject_cast<QWindow*>(engine.rootObjects().constFirst());
  }
  shell.setMainWindow(mainWindow);

  // 任务栏与 Alt+Tab 用的图标。图标编在可执行文件里，不依赖外部文件。
  app.setWindowIcon(QIcon(QStringLiteral(":/baniphelper.svg")));

  // 托盘在的时候，窗口被关掉不等于要退出 —— 程序还要在托盘里活着。
  // 托盘不在的时候必须反过来，否则界面一关就只剩一个既没窗口也没图标、
  // 只能去任务管理器杀的进程。
  app.setQuitOnLastWindowClosed(!trayReady);

  logWrite(LogLevel::Info, QStringLiteral("启动完成，界面已就绪"));
  return app.exec();
}
